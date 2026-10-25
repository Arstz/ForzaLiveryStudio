namespace {

struct PenTargetCheck {
    QString error;
    int points = 0;
    int soft = 0;
};

PathsD prunePolygonExcursions(PathsD shape, double limit, int &removed) {
    const auto original = shape;
    double spent = 0;
    int count = 0;
    for (auto &loop : shape) {
        for (bool changed = true; changed;) {
            changed = false;
            std::map<std::pair<qint64, qint64>, size_t> seen;
            for (size_t index = 0; index < loop.size(); ++index) {
                const auto key = std::pair(qRound64(loop[index].x * booleanScale()), qRound64(loop[index].y * booleanScale()));
                const auto [entry, inserted] = seen.emplace(key, index);
                if (inserted || index - entry->second < 2 || index - entry->second > loop.size() - 2) continue;
                const PathD excursion(loop.begin() + entry->second, loop.begin() + index);
                const double value = std::abs(Area(excursion));
                if (spent + value > limit) continue;
                loop.erase(loop.begin() + entry->second + 1, loop.begin() + index + 1);
                spent += value;
                ++count;
                changed = true;
                break;
            }
        }
    }
    if (count && std::abs(Area(Xor(original, shape, FillRule::NonZero, kBooleanPrecision))) <= limit) {
        removed += count;
        return shape;
    }
    return original;
}

PenTargetCheck checkPenTarget(const QPainterPath &path, const Options &options) {
    gui::RegionPenLoopConversionOptions conversionOptions;
    conversionOptions.preserveInputCurves = true;
    const auto converted = gui::regionOutlineToPenLoops(path, conversionOptions);
    PenTargetCheck result;
    if (!converted.valid()) {
        result.error = converted.error.isEmpty() ? "Empty Pen contour" : converted.error;
        return result;
    }
    if (path.fillRule() == Qt::WindingFill && converted.loops.size() > 1) {
        auto parity = path;
        parity.setFillRule(Qt::OddEvenFill);
        if (std::abs(Area(Xor(polygons(path, options.flattenTolerance), polygons(parity, options.flattenTolerance),
                              FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance) {
            result.error = "SVG fill rule and Pen cutouts describe different filled geometry";
            return result;
        }
    }
    for (const auto &loop : converted.loops) for (const auto &point : loop.points) {
        ++result.points;
        result.soft += point.kind == gui::PenPointKind::Soft;
    }
    return result;
}

} // namespace

QVector<gui::SvgVectorObject> clipToViewport(const QVector<gui::SvgVectorObject> &input,
                                           QSize size, const Options &options) {
    const QRectF viewport{QPointF(), QSizeF(size)};
    const auto frame = polygons(rectangle(viewport), options.flattenTolerance);
    const CurveBank curves(makeItems(input), options.flattenTolerance);
    QVector<gui::SvgVectorObject> result;
    for (const auto &object : input) {
        if (viewport.contains(object.path.boundingRect())) { result.push_back(object); continue; }
        const auto shape = Intersect(polygons(object.path, options.flattenTolerance), frame,
            FillRule::NonZero, kBooleanPrecision);
        if (shape.empty()) continue;
        auto restored = curves.restore(shape, viewport);
        if (!viewport.contains(restored.boundingRect()) || std::abs(Area(Xor(shape,
            polygons(restored, options.flattenTolerance), FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance)
            restored = fromPolygons(shape);
        result.push_back({restored, object.color});
    }
    return result;
}

QString penTargetError(const QPainterPath &path, const Options &options) {
    return checkPenTarget(path, options).error;
}

QJsonObject auditPenTargets(const QVector<gui::SvgVectorObject> &input, const Options &options) {
    QJsonArray failures;
    int valid = 0, points = 0, soft = 0;
    for (int index = 0; index < input.size(); ++index) {
        const auto check = checkPenTarget(input[index].path, options);
        if (!check.error.isEmpty() || input[index].color.alpha() != 255) {
            failures.append(QJsonObject{{"path", index}, {"error", check.error.isEmpty()
                ? "Nonopaque fill" : check.error}});
            continue;
        }
        ++valid;
        points += check.points;
        soft += check.soft;
    }
    return {{"passed", valid == input.size()}, {"paths", input.size()}, {"valid_targets", valid},
        {"invalid_targets", input.size() - valid}, {"pen_anchors", points},
        {"hard_anchors", points - soft}, {"soft_anchors", soft}, {"failures", failures}};
}

PenBaseline preparePenTargets(const QVector<Item> &input, const Options &options, const Renderer &renderer) {
    PenBaseline result;
    const auto before = renderer(objects(input));
    const CurveBank curves(input, options.flattenTolerance);
    const auto domain = polygons(filledUnion(objects(input), options.flattenTolerance), options.flattenTolerance);
    QJsonArray changes, failures;
    QJsonObject failedPart;
    int cuts = 0, restored = 0, flattened = 0, triangles = 0, junctionRepairs = 0, removedExcursions = 0, filledHoles = 0;
    double junctionArea = 0, filledHoleArea = 0;
    double discardedArea = 0;
    std::function<bool(const PathsD &, QColor, int)> append;
    append = [&](const PathsD &shape, QColor color, int depth) {
        const auto parts = components(shape);
        for (auto part : parts) {
            PathsD retained;
            double available = std::max(0.0, options.areaTolerance - discardedArea - junctionArea - filledHoleArea);
            int removedHoles = 0;
            for (const auto &loop : part) {
                const double value = -Area(loop);
                if (value >= 0 && value <= available && !penTargetError(fromPolygons({loop}), options).isEmpty()) {
                    available -= value;
                    ++removedHoles;
                } else retained.push_back(loop);
            }
            const double holeDelta = std::abs(Area(Xor(part, retained, FillRule::NonZero, kBooleanPrecision)));
            if (holeDelta + discardedArea + junctionArea + filledHoleArea <= options.areaTolerance) {
                filledHoles += removedHoles;
                filledHoleArea += holeDelta;
                part = std::move(retained);
            }
            const auto cleaned = prunePolygonExcursions(part,
                std::max(0.0, options.areaTolerance - discardedArea - junctionArea - filledHoleArea), removedExcursions);
            const double removed = std::abs(Area(Xor(part, cleaned, FillRule::NonZero, kBooleanPrecision)));
            discardedArea += removed;
            part = cleaned;
            auto path = curves.restore(part);
            const auto equivalent = [&](const QPainterPath &candidate) {
                return std::abs(Area(Xor(part, polygons(candidate, options.flattenTolerance),
                    FillRule::NonZero, kBooleanPrecision))) <= options.areaTolerance;
            };
            if (equivalent(path) && penTargetError(path, options).isEmpty()) {
                result.items.push_back({path, color, {}});
                ++restored;
                continue;
            }
            path = fromPolygons(part);
            const auto polygonError = penTargetError(path, options);
            if (polygonError.isEmpty()) {
                result.items.push_back({path, color, {}});
                ++flattened;
                continue;
            }
            const auto junction = junctionCompletion(part, domain, options);
            if (!junction.isEmpty()) {
                const auto repairedShape = polygons(junction, options.flattenTolerance);
                const double delta = std::abs(Area(Xor(part, repairedShape, FillRule::NonZero, kBooleanPrecision)));
                if (discardedArea + junctionArea + filledHoleArea + delta <= options.areaTolerance) {
                    auto restoredPath = curves.restore(repairedShape);
                    if (std::abs(Area(Xor(repairedShape, polygons(restoredPath, options.flattenTolerance),
                        FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance
                        || !penTargetError(restoredPath, options).isEmpty()) restoredPath = junction;
                    result.items.push_back({restoredPath, color, {}});
                    junctionArea += delta;
                    ++junctionRepairs;
                    continue;
                }
            }
            const auto box = path.boundingRect();
            const double partArea = std::abs(Area(part));
            if (partArea + discardedArea + junctionArea + filledHoleArea <= options.areaTolerance) {
                discardedArea += partArea;
                continue;
            }
            gui::PolygonMeshRequest meshRequest;
            meshRequest.sources.triangle = QPolygonF{{0, 0}, {1, 0}, {0, 1}};
            meshRequest.mergeSquares = false;
            for (const auto &loop : part) {
                QVector<QPointF> points;
                for (const auto &point : loop) points.push_back({point.x, point.y});
                meshRequest.contours.push_back(points);
            }
            const auto mesh = gui::meshPolygon(meshRequest);
            if (mesh.error.isEmpty() && !mesh.placements.isEmpty()) {
                QVector<Item> pieces;
                PathsD coverage;
                for (const auto &placement : mesh.placements) {
                    QPainterPath triangle;
                    triangle.addPolygon(placement.transform.map(meshRequest.sources.triangle));
                    triangle.closeSubpath();
                    if (!penTargetError(triangle, options).isEmpty()) { pieces.clear(); break; }
                    const auto shape = polygons(triangle, options.flattenTolerance);
                    coverage.insert(coverage.end(), shape.begin(), shape.end());
                    pieces.push_back({triangle, color, {}});
                }
                if (!pieces.isEmpty() && std::abs(Area(Xor(part, Union(coverage, FillRule::NonZero, kBooleanPrecision),
                    FillRule::NonZero, kBooleanPrecision))) <= options.areaTolerance) {
                    result.items += pieces;
                    triangles += pieces.size();
                    continue;
                }
            }
            failedPart = {{"native_error", polygonError}, {"depth", depth}, {"area", partArea},
                {"bounds", QJsonArray{box.x(), box.y(), box.width(), box.height()}}, {"mesh_error", mesh.error}};
            QJsonArray diagnosticLoops;
            for (const auto &loop : part) {
                QJsonArray points;
                for (size_t vertex = 0; vertex < std::min(size_t(256), loop.size()); ++vertex)
                    points.append(QJsonArray{loop[vertex].x, loop[vertex].y});
                diagnosticLoops.append(points);
                if (diagnosticLoops.size() == 16) break;
            }
            failedPart.insert("polygon_vertices", diagnosticLoops);
            if (depth >= 16 || std::max(box.width(), box.height()) < 1e-7) return false;
            auto lower = box, upper = box;
            bool vertical = box.width() >= box.height();
            double cut = vertical ? box.center().x() : box.center().y();
            double bestDistance = std::numeric_limits<double>::infinity();
            const double margin = 16.0 / booleanScale();
            for (const auto &point : polygonJunctions(part)) {
                for (const bool axis : {true, false}) {
                    const double coordinate = axis ? point.x() : point.y();
                    const double minimum = axis ? box.left() : box.top();
                    const double maximum = axis ? box.right() : box.bottom();
                    if (coordinate <= minimum + margin || coordinate >= maximum - margin) continue;
                    const double distance = std::abs(coordinate - (minimum + maximum) * .5) / (maximum - minimum);
                    if (distance >= bestDistance) continue;
                    vertical = axis;
                    cut = coordinate;
                    bestDistance = distance;
                }
            }
            if (vertical) {
                lower.setRight(cut);
                upper.setLeft(cut);
            } else {
                lower.setBottom(cut);
                upper.setTop(cut);
            }
            ++cuts;
            const auto left = Intersect(part, rawPolygons(rectangle(lower), options.flattenTolerance),
                FillRule::NonZero, kBooleanPrecision);
            const auto right = Intersect(part, rawPolygons(rectangle(upper), options.flattenTolerance),
                FillRule::NonZero, kBooleanPrecision);
            if (left.empty() || right.empty() || !append(left, color, depth + 1) || !append(right, color, depth + 1))
                return false;
        }
        return true;
    };
    for (int index = 0; index < input.size(); ++index) {
        const auto &item = input[index];
        const auto error = penTargetError(item.path, options);
        if (error.isEmpty()) {
            result.items.push_back(item);
            continue;
        }
        const int first = result.items.size();
        result.changed = true;
        if (item.color.alpha() != 255 || !append(polygons(item.path, options.flattenTolerance), item.color, 0)) {
            if (result.error.isEmpty())
                result.error = QString("Cannot decompose source path %1 into valid Bucket targets: %2").arg(index).arg(error);
            failedPart.insert("source_path", index);
            failures.append(failedPart);
            result.items.resize(first);
            result.items.push_back(item);
            continue;
        }
        changes.append(QJsonObject{{"source_path", index}, {"reason", error},
            {"first_target", first}, {"target_count", result.items.size() - first}});
    }
    result.items = makeItems(objects(result.items));
    const auto serialized = svg(objects(result.items), before.size());
    const auto captured = gui::extractSvgVectorObjects(serialized, before.size());
    const auto audit = auditPenTargets(captured.objects, options);
    const auto error = imageError(before, renderer(captured.objects));
    const double silhouette = silhouetteDifference(objects(input), captured.objects, options.flattenTolerance);
    const bool passed = result.error.isEmpty() && audit.value("passed").toBool()
        && captured.objects.size() == result.items.size() && withinBudget(error, options)
        && silhouette <= options.silhouetteTolerance;
    if (result.error.isEmpty() && !passed)
        result.error = "Bucket baseline conversion exceeds geometry, rendering or Pen compatibility limits";
    result.report = {{"passed", passed}, {"changed", result.changed}, {"before_paths", input.size()},
        {"after", audit}, {"image_error", error}, {"silhouette_difference_area", silhouette},
        {"restored_components", restored}, {"flattened_components", flattened}, {"decomposition_cuts", cuts},
        {"fallback_triangles", triangles}, {"discarded_unconvertible_area", discardedArea},
        {"junction_repairs", junctionRepairs}, {"junction_difference_area", junctionArea},
        {"removed_excursions", removedExcursions},
        {"filled_unconvertible_holes", filledHoles}, {"filled_unconvertible_hole_area", filledHoleArea},
        {"normalization_difference_area_bound", discardedArea + junctionArea + filledHoleArea},
        {"changes", changes}, {"decomposition_failures", failures}, {"error", result.error}};
    return result;
}
