PathsD appearanceRepairDomain(const QImage &target, const QImage &actual,
                              const ColorOptions &colors, double alphaLimit) {
    const auto left = target.convertToFormat(QImage::Format_ARGB32);
    const auto right = actual.convertToFormat(QImage::Format_ARGB32);
    const int cell = colors.localWindow, columns = (left.width() + cell - 1) / cell;
    const int rows = (left.height() + cell - 1) / cell, stride = left.width() + 1;
    std::vector<bool> tiles(columns * rows);
    std::vector<double> integral(stride * (left.height() + 1));
    const auto mark = [&](QRect box) {
        box = box.adjusted(-2, -2, 2, 2).intersected(left.rect());
        if (box.isEmpty()) return;
        for (int y = box.top() / cell; y <= box.bottom() / cell; ++y)
            for (int x = box.left() / cell; x <= box.right() / cell; ++x) tiles[y * columns + x] = true;
    };
    for (int y = 0; y < left.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        double row = 0;
        for (int x = 0; x < left.width(); ++x) {
            double maximum = 0;
            for (const int shift : {0, 8, 16}) {
                const double ca = ((a[x] >> shift) & 255) * qAlpha(a[x]) / 255.0 + 255 - qAlpha(a[x]);
                const double cb = ((b[x] >> shift) & 255) * qAlpha(b[x]) / 255.0 + 255 - qAlpha(b[x]);
                row += (ca - cb) * (ca - cb);
                maximum = std::max(maximum, std::abs(ca - cb));
            }
            integral[(y + 1) * stride + x + 1] = integral[y * stride + x + 1] + row;
            if (maximum > colors.renderMaxError || std::abs(qAlpha(a[x]) - qAlpha(b[x])) > alphaLimit) mark({x, y, 1, 1});
        }
    }
    const int width = std::min(cell, left.width()), height = std::min(cell, left.height());
    const double limit = colors.localRmse * colors.localRmse * width * height * 3;
    for (int y = height; y <= left.height(); ++y)
        for (int x = width; x <= left.width(); ++x) {
            const double squared = integral[y * stride + x] - integral[(y - height) * stride + x]
                - integral[y * stride + x - width] + integral[(y - height) * stride + x - width];
            if (squared > limit + 1e-9) mark({x - width, y - height, width, height});
        }
    QRegion selected;
    for (int y = 0; y < rows; ++y) for (int x = 0; x < columns;) {
        if (!tiles[y * columns + x]) { ++x; continue; }
        const int first = x++;
        while (x < columns && tiles[y * columns + x]) ++x;
        selected += QRect(first * cell, y * cell, (x - first) * cell, cell).intersected(left.rect());
    }
    PathsD rectangles, outer;
    for (const auto &box : selected) {
        const auto polygon = rawPolygons(rectangle(box), 0.05);
        rectangles.insert(rectangles.end(), polygon.begin(), polygon.end());
    }
    for (const auto &part : components(Union(rectangles, FillRule::NonZero, kBooleanPrecision))) outer.push_back(part.front());
    return Union(outer, FillRule::NonZero, kBooleanPrecision);
}

QVector<Item> reconstructColors(const QVector<Item> &baseline, const QVector<Item> &paint,
                                const QVector<Item> &restoration,
                                const std::vector<ColorRegion> &regions,
                                const std::vector<QColor> &fills, const Options &options,
                                const ColorOptions &colors, const Renderer &renderer,
                                const QImage &reference, QJsonObject &report,
                                QVector<Item> *topologyCandidate, QVector<Item> *reconstructionCandidate) {
    const auto started = std::chrono::steady_clock::now();
    *topologyCandidate = *reconstructionCandidate = paint;
    const CurveBank curves(baseline, options.flattenTolerance);
    int curveFallbacks = 0;
    const auto sameTopology = [](const PathsD &a, const PathsD &b) {
        const auto count = [](const PathsD &shape) {
            int outer = 0, holes = 0;
            for (const auto &loop : shape) { outer += Area(loop) > 0; holes += Area(loop) < 0; }
            return std::pair(outer, holes);
        };
        return count(a) == count(b);
    };
    const auto restore = [&](const PathsD &shape) {
        QPainterPath path;
        path.setFillRule(Qt::WindingFill);
        const double limit = options.areaTolerance / std::max(size_t(1), shape.size());
        for (const auto &loop : shape) {
            const PathsD part{loop};
            const auto restored = curves.restore(part);
            const auto actual = polygons(restored, options.flattenTolerance);
            const auto expected = Union(part, FillRule::NonZero, kBooleanPrecision);
            if (sameTopology(actual, expected) && std::abs(Area(Xor(actual, expected,
                FillRule::NonZero, kBooleanPrecision))) <= limit) path.addPath(restored);
            else { path.addPath(fromPolygons(part)); ++curveFallbacks; }
        }
        const auto actual = polygons(path, options.flattenTolerance);
        const auto expected = Union(shape, FillRule::NonZero, kBooleanPrecision);
        if (sameTopology(actual, expected) && std::abs(Area(Xor(actual, expected,
            FillRule::NonZero, kBooleanPrecision))) <= options.areaTolerance) return path;
        ++curveFallbacks;
        return fromPolygons(shape);
    };
    PathsD domain;
    std::vector<PathsD> sourceShapes;
    std::map<QRgb, PathsD> byFill;
    for (int owner = 0; owner < int(regions.size()); ++owner) {
        const auto &shape = regions[owner].shape;
        auto &combined = byFill[fills[owner].rgb()];
        combined.insert(combined.end(), shape.begin(), shape.end());
    }
    domain.clear();
    for (const auto &item : baseline) {
        const auto shape = polygons(item.path, options.flattenTolerance);
        sourceShapes.push_back(shape);
        domain.insert(domain.end(), shape.begin(), shape.end());
    }
    domain = Union(domain, FillRule::NonZero, kBooleanPrecision);
    const auto plan = planColorStack(byFill, domain, sourceShapes, regions, fills, options, colors, restore);
    if (!plan.error.isEmpty()) {
        if (!plan.graph.edges.empty()) *topologyCandidate = *reconstructionCandidate = plan.items;
        report = {{"accepted", false}, {"rejection", plan.error}, {"planning", plan.diagnostics},
            {"ordering", plan.graph.json()}, {"paint_groups", plan.steps}};
        return paint;
    }
    auto candidate = plan.items;
    const auto finalGraph = plan.graph;
    const auto before = pathMetrics(objects(paint), options.flattenTolerance);
    const auto baseCandidate = candidate;
    *topologyCandidate = baseCandidate;
    auto actual = renderer(objects(candidate));
    auto image = appearanceError(reference, actual, colors.localWindow);
    const auto topologyMetrics = pathMetrics(objects(candidate), options.flattenTolerance);
    const auto topologyError = image;
    PathsD repair;
    QVector<QPainterPath> restoredGeometry(restoration.size());
    std::set<int> wholeSources;
    int restoredPaths = 0, repairAttempts = 0;
    for (; repairAttempts < 3; ++repairAttempts) {
        if (image.value("rmse").toDouble() <= colors.renderRmse
            && image.value("max_channel_error").toDouble() <= colors.renderMaxError
            && image.value("max_local_rmse").toDouble() <= colors.localRmse
            && image.value("max_alpha_error").toDouble() <= options.renderMaxError) break;
        const auto added = appearanceRepairDomain(reference, actual, colors, options.renderMaxError);
        if (added.empty()) break;
        PathsD expanded = added;
        if (!repair.empty()) {
            const auto repeated = Intersect(added, repair, FillRule::NonZero, kBooleanPrecision);
            const auto badBounds = fromPolygons(repeated).boundingRect().toAlignedRect().intersected(reference.rect());
            QImage badMask(reference.size(), QImage::Format_ARGB32);
            badMask.fill(Qt::transparent);
            {
                QPainter painter(&badMask);
                painter.setPen(Qt::NoPen);
                painter.setBrush(Qt::white);
                painter.drawPath(fromPolygons(repeated));
            }
            for (int source = 0; source < restoration.size(); ++source) {
                const auto &item = restoration[source];
                const auto box = item.path.boundingRect().adjusted(-2, -2, 2, 2).toAlignedRect().intersected(badBounds);
                if (box.isEmpty()) continue;
                const auto original = renderCrop({{item.path, item.color}}, box).convertToFormat(QImage::Format_ARGB32);
                const auto clipped = renderCrop({{restoredGeometry[source], item.color}}, box).convertToFormat(QImage::Format_ARGB32);
                bool changed = false;
                for (int y = 0; y < box.height() && !changed; ++y) {
                    const auto *maskRow = reinterpret_cast<const QRgb *>(badMask.constScanLine(y + box.top()));
                    const auto *a = reinterpret_cast<const QRgb *>(original.constScanLine(y));
                    const auto *b = reinterpret_cast<const QRgb *>(clipped.constScanLine(y));
                    for (int x = 0; x < box.width(); ++x)
                        if (qAlpha(maskRow[x + box.left()]) && std::abs(qAlpha(a[x]) - qAlpha(b[x])) > 2) { changed = true; break; }
                }
                if (!changed) continue;
                const auto boundary = item.path.boundingRect().adjusted(-2, -2, 2, 2).toAlignedRect().intersected(reference.rect());
                const auto polygon = rawPolygons(rectangle(boundary), options.flattenTolerance);
                expanded.insert(expanded.end(), polygon.begin(), polygon.end());
                wholeSources.insert(source);
            }
        }
        repair = Union(repair, Union(expanded, FillRule::NonZero, kBooleanPrecision), FillRule::NonZero, kBooleanPrecision);
        PathsD repairOuter;
        for (const auto &part : components(repair)) repairOuter.push_back(part.front());
        repair = Union(repairOuter, FillRule::NonZero, kBooleanPrecision);
        const PolygonIndex mask(repair);
        candidate = baseCandidate;
        restoredGeometry.fill({});
        restoredPaths = 0;
        for (int source = 0; source < restoration.size(); ++source) {
            const auto &item = restoration[source];
            if (mask.intersectArea(sourceShapes[source], item.path.boundingRect()) <= 1e-12) continue;
            if (mask.differenceArea(sourceShapes[source], item.path.boundingRect()) <= options.areaTolerance) {
                candidate.push_back(item);
                restoredGeometry[source] = item.path;
                ++restoredPaths;
            } else {
                const auto clipped = Intersect(sourceShapes[source], repair, FillRule::NonZero, kBooleanPrecision);
                if (!clipped.empty()) {
                    const auto path = curves.restore(clipped);
                    candidate.push_back({path, item.color, {}});
                    restoredGeometry[source] = path;
                    ++restoredPaths;
                }
            }
        }
        actual = renderer(objects(candidate));
        image = appearanceError(reference, actual, colors.localWindow);
        if (options.progress)
            std::fprintf(stderr, "reconstruction: %d local source paths, RMSE %.3f, worst local %.3f, peak %.0f\n",
                restoredPaths, image.value("rmse").toDouble(), image.value("max_local_rmse").toDouble(), image.value("max_channel_error").toDouble());
    }
    const auto after = pathMetrics(objects(candidate), options.flattenTolerance);
    *reconstructionCandidate = candidate;
    const auto geometric = appearanceError(renderer(objects(paint)), actual, colors.localWindow);
    const double silhouette = silhouetteDifference(objects(baseline), objects(candidate), options.flattenTolerance);
    const auto safeMetrics = pathMetrics(objects(restoration), options.flattenTolerance);
    const bool valid = finalGraph.error.isEmpty() && geometryCost(after, options)
            < std::min(geometryCost(before, options), geometryCost(safeMetrics, options))
        && after.holes <= safeMetrics.holes
        && image.value("rmse").toDouble() <= colors.renderRmse
        && image.value("max_channel_error").toDouble() <= colors.renderMaxError
        && image.value("max_local_rmse").toDouble() <= colors.localRmse
        && image.value("max_alpha_error").toDouble() <= options.renderMaxError
        && silhouette <= options.silhouetteTolerance;
    report = {{"accepted", valid}, {"before", before.json()}, {"after", after.json()},
        {"before_local_restoration", topologyMetrics.json()}, {"before_local_restoration_image_error", topologyError},
        {"image_error", geometric}, {"opaque_image_error", image}, {"silhouette_difference_area", silhouette},
        {"ordering", finalGraph.json()}, {"paint_groups", plan.steps}, {"planning", plan.diagnostics},
        {"coverage_overlap_pixels", colors.coverageOverlap},
        {"coverage_completions", 0},
        {"curve_restore_geometry_fallbacks", curveFallbacks},
        {"restored_source_paths", restoredPaths}, {"repair_attempts", repairAttempts},
        {"whole_source_bounds_restored", int(wholeSources.size())},
        {"repair_domain", QString::fromUtf8(svg({{fromPolygons(repair), Qt::black}}, reference.size()))},
        {"ordering_covers_prefix_paths", baseCandidate.size()},
        {"appearance_passed", image.value("rmse").toDouble() <= colors.renderRmse
            && image.value("max_channel_error").toDouble() <= colors.renderMaxError
            && image.value("max_local_rmse").toDouble() <= colors.localRmse},
        {"geometry_simpler", geometryCost(after, options) < std::min(geometryCost(before, options), geometryCost(safeMetrics, options))},
        {"holes_passed", after.holes <= safeMetrics.holes},
        {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
    if (options.progress)
        std::fprintf(stderr, "reconstruction: %d paths, %d holes, RMSE %.3f, worst local %.3f, peak %.0f%s\n",
            after.paths, after.holes, image.value("rmse").toDouble(), image.value("max_local_rmse").toDouble(),
            image.value("max_channel_error").toDouble(), valid ? " accepted" : " rejected; source supports retained");
    return valid ? makeItems(objects(candidate)) : paint;
}
