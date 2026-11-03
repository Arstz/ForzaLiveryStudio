QVector<gui::catalog::Polygons> constructiveComponents(const gui::catalog::Polygons &polygons) {
    QVector<gui::catalog::Polygons> components;
    for (const auto &polygon : polygons)
        if (gui::catalog::signedArea(polygon) > 0)
            components.push_back({polygon});
    for (const auto &polygon : polygons) {
        if (gui::catalog::signedArea(polygon) >= 0)
            continue;
        int owner = -1;
        double smallest = std::numeric_limits<double>::infinity();
        for (int index = 0; index < components.size(); ++index) {
            const double area = gui::catalog::signedArea(components[index].front());
            if (area < smallest && components[index].front().containsPoint(polygon.front(), Qt::OddEvenFill)) {
                smallest = area;
                owner = index;
            }
        }
        require(owner >= 0, QStringLiteral("Cannot assign a hole to a component"));
        components[owner].push_back(polygon);
    }
    return components;
}

gui::catalog::Region canonicalConstructiveRegion(const gui::PenFillRequest &request) {
    gui::catalog::Polygons outside, holes;
    for (auto loop : request.loops) {
        gui::materializePenHandles(loop.points);
        QPolygonF polygon;
        std::function<void(const gui::PenBoundarySegment &, int)> append;
        append = [&](const auto &segment, int depth) {
            require(depth < 24 && polygon.size() < 12000, QStringLiteral("Canonical boundary work limit exceeded"));
            if (!segment.curved || segment.flatness() <= request.boundaryTolerance * 0.2) {
                polygon.push_back(segment.start);
                return;
            }
            const auto [first, second] = segment.split();
            append(first, depth + 1);
            append(second, depth + 1);
        };
        for (const auto &segment : gui::penSegments(loop.points))
            append(segment, 0);
        const auto filled = gui::catalog::unite({polygon});
        if (loop.kind == gui::PenLoopKind::Outer)
            outside += filled;
        else
            holes += filled;
    }
    gui::catalog::Region region;
    region.required = gui::catalog::subtract(gui::catalog::unite(outside), gui::catalog::unite(holes));
    require(!region.required.isEmpty(), QStringLiteral("Canonical region has no fillable area"));
    region.visible = region.spillFree = region.required;
    region.requiredPath = region.spillFreePath = gui::catalog::painterPath(region.required);
    region.bounds = region.requiredPath.boundingRect();
    region.area = region.originalArea = gui::catalog::area(region.required);
    region.tolerance = request.boundaryTolerance;
    return gui::catalog::leewayAdjustedRegion(region, {}, 2.0);
}

void constructiveCoverBenchmark(const QStringList &arguments,
                                const QVector<gui::catalog::Primitive> &primitives) {
    const auto request = readRequest(arguments[2]);
    const QString mode = arguments.size() > 3 ? arguments[3] : QStringLiteral("legacy");
    const bool slabs = mode.contains(QStringLiteral("slabs"));
    const bool canonical = mode.contains(QStringLiteral("canonical"));
    const bool rawMode = mode.startsWith(QStringLiteral("raw")) || slabs;
    const double epsilon = arguments.size() > 4 ? arguments[4].toDouble() : 0.0;
    const int refinementBudget = arguments.size() > 5 ? arguments[5].toInt() : 0;
    QElapsedTimer timer;
    timer.start();
    const auto phase = [&](const QString &name) {
        if (arguments.contains(QStringLiteral("trace")))
            QTextStream(stderr) << timer.elapsed() << " ms " << name << '\n' << Qt::flush;
    };
    gui::catalog::FillResult fitted;
    QString constructionError;
    qint64 constructionMilliseconds = 0;
    bool verified = false;
    int vertices = 0;
    int initialCount = 0;
    qint64 refinementMilliseconds = 0;
    bool refinementVerified = false;
    QVector<QPolygonF> idealTriangles;
    double maximumVertexError = 0;
    const auto cancelled = [&] { return timer.elapsed() > 5000; };
    try {
        const auto authored = canonical ? canonicalConstructiveRegion(request) : gui::catalog::buildRegion(request, cancelled);
        phase(QStringLiteral("source region"));
        auto region = gui::catalog::leewayAdjustedRegion(authored, {}, 2.0);
        if (epsilon > 0) {
            gui::catalog::Polygons simplified;
            for (const auto &polygon : region.required) {
                Clipper2Lib::PathD path;
                for (const auto &point : polygon)
                    path.emplace_back(point.x(), point.y());
                const auto reduced = Clipper2Lib::SimplifyPath(path, epsilon, true);
                QPolygonF result;
                for (const auto &point : reduced)
                    result.push_back({point.x, point.y});
                if (result.size() >= 3)
                    simplified.push_back(result);
            }
            region.required = gui::catalog::unite(simplified);
        }
        for (const auto &polygon : region.required)
            vertices += polygon.size();
        const auto nativeTriangle = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
            return primitive.shape.shapeId == 103;
        });
        require(nativeTriangle != primitives.end(), QStringLiteral("Native triangle is unavailable"));
        const auto appendRawTriangle = [&](const QPolygonF &target) {
            const auto &source = nativeTriangle->shape.contours.front();
            std::array<int, 3> order{0, 1, 2};
            double bestError = std::numeric_limits<double>::infinity();
            QTransform best;
            do {
                const auto transform = gui::catalog::emittedTransform(gui::catalog::affineFromAnchors(
                    {source[0], source[1], source[2]}, {target[order[0]], target[order[1]], target[order[2]]}));
                double error = 0;
                for (int index = 0; index < 3; ++index) {
                    const auto delta = transform.map(source[index]) - target[order[index]];
                    error = std::max(error, QPointF::dotProduct(delta, delta));
                }
                if (error < bestError) {
                    bestError = error;
                    best = transform;
                }
            } while (std::next_permutation(order.begin(), order.end()));
            gui::PenPlacement result;
            result.shapeId = 103;
            result.transform = best;
            fitted.fill.placements.push_back(result);
            idealTriangles.push_back(target);
            maximumVertexError = std::max(maximumVertexError, std::sqrt(bestError));
        };
        if (slabs) {
            QVector<double> levels;
            for (const auto &polygon : region.required)
                for (const auto &point : polygon)
                    levels.push_back(point.y());
            std::sort(levels.begin(), levels.end());
            levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
            struct Crossing { QPointF start, end; double x; };
            const auto at = [](const Crossing &edge, double y) {
                return QPointF(edge.start.x() + (edge.end.x() - edge.start.x())
                    * ((y - edge.start.y()) / (edge.end.y() - edge.start.y())), y);
            };
            const auto square = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
                return primitive.shape.shapeId == 101;
            });
            require(square != primitives.end(), QStringLiteral("Native square is unavailable"));
            for (int slab = 1; slab < levels.size(); ++slab) {
                require(!cancelled(), QStringLiteral("Construction time budget reached"));
                const double top = levels[slab - 1], bottom = levels[slab], middle = (top + bottom) * 0.5;
                QVector<Crossing> crossings;
                for (const auto &polygon : region.required)
                    for (int edge = 0; edge < polygon.size(); ++edge) {
                        const auto first = polygon[edge], second = polygon[(edge + 1) % polygon.size()];
                        if (middle > std::min(first.y(), second.y()) && middle < std::max(first.y(), second.y())) {
                            Crossing crossing{first, second, 0};
                            crossing.x = at(crossing, middle).x();
                            crossings.push_back(crossing);
                        }
                    }
                std::sort(crossings.begin(), crossings.end(), [](const auto &a, const auto &b) { return a.x < b.x; });
                require(crossings.size() % 2 == 0, QStringLiteral("Unpaired region boundary crossing"));
                for (int index = 0; index < crossings.size(); index += 2) {
                    const auto a = at(crossings[index], top), b = at(crossings[index + 1], top);
                    const auto c = at(crossings[index + 1], bottom), d = at(crossings[index], bottom);
                    const double firstWidth = b.x() - a.x(), secondWidth = c.x() - d.x();
                    if (firstWidth <= 0 && secondWidth <= 0)
                        continue;
                    if (std::abs(firstWidth - secondWidth) < 1e-8 && firstWidth > 0) {
                        const QPolygonF target({a, b, c, d});
                        const auto bounds = square->shape.bounds;
                        const auto transform = gui::catalog::emittedTransform(gui::catalog::affineFromAnchors(
                            {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft()}, {a, b, d}));
                        const QPolygonF source({bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()});
                        for (int vertex = 0; vertex < 4; ++vertex)
                            maximumVertexError = std::max(maximumVertexError, QLineF(transform.map(source[vertex]), target[vertex]).length());
                        gui::PenPlacement placement;
                        placement.shapeId = 101;
                        placement.transform = transform;
                        fitted.fill.placements.push_back(placement);
                        idealTriangles.push_back(target);
                    } else {
                        if (firstWidth > 0)
                            appendRawTriangle({a, b, d});
                        if (secondWidth > 0)
                            appendRawTriangle({b, c, d});
                    }
                }
            }
        } else if (mode == QStringLiteral("centered") || rawMode) {
            const auto triangle = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
                return primitive.shape.shapeId == 103;
            });
            require(triangle != primitives.end(), QStringLiteral("Native triangle is unavailable"));
            const auto components = constructiveComponents(region.required);
            for (const auto &component : components) {
                const auto center = component.front().boundingRect().center();
                QTransform toLocal;
                toLocal.translate(-center.x(), -center.y());
                QTransform toWorld;
                toWorld.translate(center.x(), center.y());
                gui::PolygonMeshRequest meshRequest;
                meshRequest.mergeSquares = false;
                meshRequest.sources.triangle = triangle->shape.contours.front();
                for (const auto &polygon : component)
                    meshRequest.contours.push_back(toLocal.map(polygon));
                const auto mesh = gui::meshPolygon(meshRequest, cancelled);
                require(mesh.error.isEmpty(), mesh.error);
                for (const auto &placement : mesh.placements) {
                    require(!cancelled(), QStringLiteral("Construction time budget reached"));
                    auto piece = region;
                    piece.required = gui::catalog::mapped(triangle->shape, placement.transform * toWorld);
                    if (piece.required.isEmpty() || piece.required.front().size() != 3)
                        continue;
                    if (rawMode) {
                        const auto &target = piece.required.front();
                        appendRawTriangle(target);
                    } else {
                        const auto candidates = gui::catalog::completeCover(piece, primitives, cancelled);
                        for (const auto &candidate : candidates)
                            fitted.fill.placements.push_back(candidate.placement);
                    }
                }
            }
        } else {
            const auto candidates = gui::catalog::completeCover(region, primitives, cancelled);
            for (const auto &candidate : candidates)
                fitted.fill.placements.push_back(candidate.placement);
        }
        const bool rawAccepted = rawMode && fitted.fill.placements.size() <= 8
            && gui::compact::verifyEmittedRegion(authored, primitives, {}, &fitted);
        if ((mode == QStringLiteral("rawedges") || slabs) && !rawAccepted) {
            phase(QStringLiteral("edge repair"));
            const auto triangle = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
                return primitive.shape.shapeId == 103;
            });
            const auto square = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
                return primitive.shape.shapeId == 101;
            });
            require(square != primitives.end(), QStringLiteral("Native square is unavailable"));
            gui::catalog::Polygons coverage;
            for (const auto &placement : fitted.fill.placements) {
                const auto primitive = std::find_if(primitives.begin(), primitives.end(), [&](const auto &candidate) {
                    return candidate.shape.shapeId == placement.shapeId;
                });
                coverage += gui::catalog::mapped(primitive->shape, placement.transform);
            }
            coverage = gui::catalog::unite(coverage);
            auto missing = gui::catalog::subtract(region.required, coverage);
            fitted.diagnostics.insert("rawCount", fitted.fill.placements.size());
            fitted.diagnostics.insert("maximumVertexError", maximumVertexError);
            const double radius = maximumVertexError + gui::catalog::kVerificationClearance * 4;
            QSet<QString> edges;
            for (const auto &polygon : idealTriangles) {
                for (int edge = 0; edge < polygon.size(); ++edge) {
                    require(!cancelled(), QStringLiteral("Construction time budget reached"));
                    auto start = polygon[edge], end = polygon[(edge + 1) % polygon.size()];
                    if (start.x() > end.x() || (start.x() == end.x() && start.y() > end.y()))
                        std::swap(start, end);
                    const QString key = QStringLiteral("%1,%2,%3,%4").arg(start.x(), 0, 'f', 6).arg(start.y(), 0, 'f', 6)
                        .arg(end.x(), 0, 'f', 6).arg(end.y(), 0, 'f', 6);
                    if (edges.contains(key))
                        continue;
                    edges.insert(key);
                    const double length = QLineF(start, end).length();
                    if (length <= 0)
                        continue;
                    const auto horizontal = (end - start) / length;
                    const QPointF vertical(-horizontal.y(), horizontal.x());
                    const auto rectangle = [&](double padding) {
                        return QPolygonF({start - horizontal * padding - vertical * padding,
                            end + horizontal * padding - vertical * padding,
                            end + horizontal * padding + vertical * padding,
                            start - horizontal * padding + vertical * padding});
                    };
                    const auto required = gui::catalog::unite({rectangle(radius)});
                    if (gui::catalog::intersect(missing, required).isEmpty())
                        continue;
                    bool covered = false;
                    for (int attempt = 0; attempt < 12; ++attempt) {
                        const auto target = rectangle(std::ldexp(radius, attempt + 1));
                        const auto bounds = square->shape.bounds;
                        const auto transform = gui::catalog::emittedTransform(gui::catalog::affineFromAnchors(
                            {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft()}, {target[0], target[1], target[3]}));
                        const auto support = gui::catalog::mapped(square->shape, transform);
                        if (gui::catalog::subtract(required, support).isEmpty()
                            && gui::catalog::subtract(support, region.permitted).isEmpty()) {
                            gui::PenPlacement placement;
                            placement.shapeId = 101;
                            placement.transform = transform;
                            fitted.fill.placements.push_back(placement);
                            missing = gui::catalog::subtract(missing, support);
                            covered = true;
                            break;
                        }
                    }
                    require(covered, QStringLiteral("Cannot cover a native rounding seam"));
                }
            }
            fitted.diagnostics.insert("remainingSeamArea", gui::catalog::area(missing));
            phase(QStringLiteral("edge repair complete"));
        } else if (rawMode && !rawAccepted) {
            phase(QStringLiteral("raw mesh"));
            gui::catalog::Polygons coverage;
            const auto triangle = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
                return primitive.shape.shapeId == 103;
            });
            for (const auto &placement : fitted.fill.placements)
                coverage += gui::catalog::mapped(triangle->shape, placement.transform);
            coverage = gui::catalog::unite(coverage);
            phase(QStringLiteral("raw union"));
            fitted.diagnostics.insert("rawCount", fitted.fill.placements.size());
            const auto inner = mode == QStringLiteral("rawfull") ? region.required
                : gui::catalog::interiorSupport(authored.required, 0.5);
            const auto bounds = gui::catalog::painterPath(region.required).boundingRect();
            const double magnitude = std::max({1.0, std::abs(bounds.left()), std::abs(bounds.right()),
                std::abs(bounds.top()), std::abs(bounds.bottom())});
            const auto patchAllowed = mode == QStringLiteral("rawfull")
                ? gui::catalog::intersect(gui::catalog::expanded(region.required,
                    std::max(0.001, magnitude * std::numeric_limits<float>::epsilon() * 16)), region.permitted)
                : authored.required;
            auto missing = gui::catalog::subtract(inner, coverage);
            phase(QStringLiteral("interior gaps"));
            fitted.diagnostics.insert("rawMissingInterior", gui::catalog::area(missing));
            if (!missing.isEmpty()) {
                const auto square = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
                    return primitive.shape.shapeId == 101;
                });
                require(square != primitives.end(), QStringLiteral("Native square is unavailable"));
                int work = 0;
                std::function<void(const gui::catalog::Polygons &, int)> patchInterior;
                patchInterior = [&](const gui::catalog::Polygons &pending, int depth) {
                    if (pending.isEmpty())
                        return;
                    require(!cancelled(), QStringLiteral("Construction time budget reached"));
                    require(++work < 12000 && depth < 32, QStringLiteral("Interior patch work limit exceeded"));
                    const auto hull = gui::catalog::convexHull([&] {
                        QPolygonF points;
                        for (const auto &polygon : pending)
                            points += polygon;
                        return points;
                    }());
                    for (int edge = 0; edge < hull.size(); ++edge) {
                        require(!cancelled(), QStringLiteral("Construction time budget reached"));
                        const auto direction = hull[(edge + 1) % hull.size()] - hull[edge];
                        const double length = std::hypot(direction.x(), direction.y());
                        if (length <= 0)
                            continue;
                        const QPointF horizontal = direction / length;
                        const QPointF vertical(-horizontal.y(), horizontal.x());
                        double minimumX = std::numeric_limits<double>::infinity(), minimumY = minimumX;
                        double maximumX = -minimumX, maximumY = -minimumX;
                        for (const auto &point : hull) {
                            const auto relative = point - hull[edge];
                            const double x = QPointF::dotProduct(relative, horizontal);
                            const double y = QPointF::dotProduct(relative, vertical);
                            minimumX = std::min(minimumX, x);
                            maximumX = std::max(maximumX, x);
                            minimumY = std::min(minimumY, y);
                            maximumY = std::max(maximumY, y);
                        }
                        for (double padding : {0.001, 0.004, 0.016, 0.064}) {
                            const auto origin = hull[edge] + horizontal * (minimumX - padding) + vertical * (minimumY - padding);
                            const auto right = origin + horizontal * (maximumX - minimumX + padding * 2);
                            const auto bottom = origin + vertical * (maximumY - minimumY + padding * 2);
                            const auto bounds = square->shape.bounds;
                            const auto transform = gui::catalog::emittedTransform(gui::catalog::affineFromAnchors(
                                {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft()}, {origin, right, bottom}));
                            const auto support = gui::catalog::mapped(square->shape, transform);
                            if (gui::catalog::subtract(pending, support).isEmpty()
                                && gui::catalog::subtract(support, patchAllowed).isEmpty()) {
                                gui::PenPlacement placement;
                                placement.shapeId = 101;
                                placement.transform = transform;
                                fitted.fill.placements.push_back(placement);
                                return;
                            }
                        }
                    }
                    const auto bounds = gui::catalog::painterPath(pending).boundingRect();
                    auto first = bounds, second = bounds;
                    if (bounds.width() >= bounds.height()) {
                        first.setRight(bounds.center().x());
                        second.setLeft(bounds.center().x());
                    } else {
                        first.setBottom(bounds.center().y());
                        second.setTop(bounds.center().y());
                    }
                    for (const auto &half : {first, second}) {
                        const QPolygonF clip({half.topLeft(), half.topRight(), half.bottomRight(), half.bottomLeft()});
                        patchInterior(gui::catalog::intersect(pending, {clip}), depth + 1);
                    }
                };
                for (int round = 0; round < 3 && !missing.isEmpty(); ++round) {
                    phase(QStringLiteral("patch round %1").arg(round));
                    for (const auto &component : constructiveComponents(missing))
                        patchInterior(component, 0);
                    coverage.clear();
                    for (const auto &placement : fitted.fill.placements) {
                        const auto primitive = std::find_if(primitives.begin(), primitives.end(), [&](const auto &candidate) {
                            return candidate.shape.shapeId == placement.shapeId;
                        });
                        coverage += gui::catalog::mapped(primitive->shape, placement.transform);
                    }
                    coverage = gui::catalog::unite(coverage);
                    phase(QStringLiteral("patched union"));
                    missing = gui::catalog::subtract(inner, coverage);
                }
                fitted.diagnostics.insert("patchWork", work);
            }
        }
        constructionMilliseconds = timer.elapsed();
        phase(QStringLiteral("original verification"));
        verified = rawAccepted || gui::compact::verifyEmittedRegion(authored, primitives, {}, &fitted);
        phase(QStringLiteral("verification complete"));
        initialCount = fitted.fill.placements.size();
        if (verified && refinementBudget > 0) {
            gui::compact::FillOptions options;
            options.initialPlacements = fitted.fill.placements;
            options.evaluationBudget = refinementBudget;
            options.shapeBudget = initialCount;
            options.retainFailedFill = true;
            options.useGpu = !arguments.contains(QStringLiteral("cpu"));
            const qint64 start = timer.elapsed();
            auto refined = gui::compact::fillRegion(request, primitives, options,
                [&] { return timer.elapsed() - start > 5000; });
            refinementMilliseconds = timer.elapsed() - start;
            refinementVerified = !refined.fill.placements.isEmpty()
                && gui::compact::verifyEmittedFill(request, primitives, {}, &refined);
            if (refinementVerified && refined.fill.placements.size() < fitted.fill.placements.size())
                fitted = std::move(refined);
        }
    } catch (const std::exception &error) {
        constructionError = QString::fromUtf8(error.what());
        constructionMilliseconds = timer.elapsed();
    }
    QJsonArray placements;
    for (const auto &placement : fitted.fill.placements) {
        const auto &transform = placement.transform;
        placements.push_back(QJsonObject{{"shapeId", placement.shapeId},
            {"transform", QJsonArray{transform.m11(), transform.m12(), transform.m21(), transform.m22(), transform.dx(), transform.dy()}}});
    }
    const QJsonObject output{{"verified", verified}, {"count", fitted.fill.placements.size()},
        {"total_ms", timer.elapsed()}, {"construction_ms", constructionMilliseconds},
        {"vertices", vertices}, {"mode", mode}, {"epsilon", epsilon},
        {"initial_count", initialCount}, {"refinement_budget", refinementBudget},
        {"refinement_ms", refinementMilliseconds}, {"refinement_verified", refinementVerified},
        {"error", constructionError}, {"diagnostics", fitted.diagnostics}, {"placements", placements}};
    QTextStream(stdout) << QJsonDocument(output).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
}

