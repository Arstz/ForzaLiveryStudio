struct ColorPlan {
    QVector<Item> items;
    Graph graph;
    QJsonArray steps;
    QJsonObject diagnostics;
    QString error;
};

ColorPlan planColorStack(const std::map<QRgb, PathsD> &visible, const PathsD &domain,
                         const std::vector<PathsD> &sourceShapes,
                         const std::vector<ColorRegion> &regions, const std::vector<QColor> &fills,
                         const Options &options, const ColorOptions &colorOptions,
                         const std::function<QPainterPath(const PathsD &)> &restore,
                         const QVector<QRgb> &preferredOrder = {}, bool connectedCosts = false) {
    ColorPlan output;
    std::vector<QRgb> colors;
    std::vector<PathsD> required;
    std::vector<double> mass, density;
    std::map<QRgb, int> indices;
    std::vector<PolygonIndex> ownership;
    for (const auto &[color, shape] : visible) {
        indices[color] = int(colors.size());
        colors.push_back(color);
        required.push_back(Union(shape, FillRule::NonZero, kBooleanPrecision));
        ownership.emplace_back(required.back());
        mass.push_back(std::abs(Area(required.back())));
        density.push_back(geometryCost(pathMetrics({{restore(required.back()), QColor(color)}}, options.flattenTolerance), options)
            / std::max(mass.back(), 1e-12));
    }
    std::vector<std::set<int>> sourceIds(colors.size());
    for (int owner = 0; owner < int(regions.size()); ++owner) sourceIds[indices.at(fills[owner].rgb())].insert(regions[owner].source);
    std::vector<int> order;
    for (int color = 0; color < int(colors.size()); ++color) order.push_back(color);
    int base = *std::max_element(order.begin(), order.end(), [&](int a, int b) { return mass[a] < mass[b]; });
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return std::tuple(a == base ? 0 : 1, density[a], a) < std::tuple(b == base ? 0 : 1, density[b], b);
    });
    if (!preferredOrder.isEmpty()) {
        std::set<int> seen;
        std::vector<int> selected;
        for (const QRgb fill : preferredOrder)
            if (indices.contains(fill) && seen.insert(indices.at(fill)).second) selected.push_back(indices.at(fill));
        if (selected.size() != colors.size()) { output.error = "Incomplete color order"; return output; }
        order = std::move(selected);
        base = order.front();
    }
    const auto filled = [&](const PathsD &shape) {
        const auto normalized = Union(shape, FillRule::NonZero, kBooleanPrecision);
        PathsD outer;
        for (const auto &loop : normalized) if (Area(loop) > 0) outer.push_back(loop);
        return Intersect(Union(outer, FillRule::NonZero, kBooleanPrecision), domain, FillRule::NonZero, kBooleanPrecision);
    };
    std::map<QByteArray, double> costCache;
    const auto costOf = [&](const QPainterPath &path, int color) {
        QByteArray encoded;
        QDataStream stream(&encoded, QIODevice::WriteOnly);
        stream << path;
        const auto key = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
        if (const auto found = costCache.find(key); found != costCache.end()) return found->second;
        auto metrics = pathMetrics({{path, QColor(colors[color])}}, options.flattenTolerance);
        if (connectedCosts) metrics.paths = metrics.components;
        const double value = geometryCost(metrics, options);
        costCache.emplace(key, value);
        return value;
    };
    struct Patch {
        QVector<Item> items;
        PathsD support;
        double cost = std::numeric_limits<double>::infinity();
    };
    std::map<QByteArray, Patch> correctionCache;
    std::vector<bool> committed(colors.size());
    int splitCorrections = 0, nestingRejected = 0, geometryRejected = 0, boundedRejected = 0, holeRejected = 0;
    int exactFallbacks = 0;
    const auto diagnostics = [&]() {
        return QJsonObject{{"restoration_draw_items", splitCorrections}, {"nesting_rejections", nestingRejected},
            {"geometry_rejections", geometryRejected}, {"bounded_cost_rejections", boundedRejected},
            {"cap_holes_detected", holeRejected}, {"exact_visible_fallbacks", exactFallbacks},
            {"ownership_source", "Fixed original color regions partitioned across final draw positions"}};
    };
    const auto exactPatch = [&](const PathsD &territory, int color) {
        auto path = restore(territory);
        auto support = polygons(path, options.flattenTolerance);
        if (std::abs(Area(Xor(support, territory, FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance) {
            path = fromPolygons(territory);
            support = territory;
        }
        return Patch{{{path, QColor(colors[color]), {}}}, support, costOf(path, color)};
    };
    const int domainHoles = pathMetrics({{fromPolygons(domain), Qt::black}}, options.flattenTolerance).holes;
    std::vector<bool> active(colors.size());
    std::function<Patch(PathsD, int, int, double)> correction;
    correction = [&](PathsD territory, int color, int depth, double ceiling) -> Patch {
        if (std::abs(Area(territory)) <= options.areaTolerance) return {{}, {}, 0};
        auto fallback = exactPatch(territory, color);
        const auto retainExact = [&]() {
            ++exactFallbacks;
            return fallback.cost < ceiling ? fallback : Patch{};
        };
        if (depth > 16 || active[color]) { ++nestingRejected; return retainExact(); }
        QByteArray encoded;
        QDataStream stream(&encoded, QIODevice::WriteOnly);
        stream << qint32(color) << quint32(territory.size());
        for (const auto &loop : territory) {
            stream << quint32(loop.size());
            for (const auto &point : loop) stream << point.x << point.y;
        }
        const auto key = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
        if (const auto found = correctionCache.find(key); found != correctionCache.end())
            return found->second.cost < ceiling ? found->second : Patch{};
        const double limit = std::min(ceiling, fallback.cost);
        auto support = filled(territory);
        auto path = restore(support);
        double cost = costOf(path, color);
        if (pathMetrics({{path, QColor(colors[color])}}, options.flattenTolerance).holes > domainHoles) {
            ++holeRejected;
        }
        if (PolygonIndex(polygons(path, options.flattenTolerance)).differenceArea(territory,
            fromPolygons(territory).boundingRect()) > options.areaTolerance) { ++geometryRejected; return retainExact(); }
        const auto simplify = [&](const PathsD &proposal) {
            const auto candidate = filled(Union(support, proposal, FillRule::NonZero, kBooleanPrecision));
            const auto added = Difference(candidate, support, FillRule::NonZero, kBooleanPrecision);
            const auto box = fromPolygons(added).boundingRect();
            for (int other = 0; other < int(colors.size()); ++other)
                if (other != color && committed[other]
                    && ownership[other].intersectArea(added, box) > options.areaTolerance) return;
            const auto alternative = restore(candidate);
            const double value = costOf(alternative, color);
            if (value >= cost - 1e-9) return;
            support = candidate;
            path = alternative;
            cost = value;
        };
        simplify(rawPolygons(rectangle(path.boundingRect()), options.flattenTolerance));
        simplify(rawPolygons(hull(path, options.flattenTolerance), options.flattenTolerance));
        if (cost >= limit) { ++boundedRejected; return retainExact(); }
        const auto loops = spans(path);
        if (options.maximumChordSpans > 0) for (const auto &loop : loops) for (const auto &span : loop) {
            if (!span.curved) continue;
            QPainterPath curve;
            curve.moveTo(span.start);
            span.appendTo(curve);
            simplify(rawPolygons(rectangle(curve.boundingRect()), options.flattenTolerance));
        }
        if (cost >= limit) { ++boundedRejected; return retainExact(); }
        Patch result{{{path, QColor(colors[color]), {}}}, support, cost};
        struct Child { int color; PathsD region; double enclosing; };
        std::vector<Child> children;
        const auto box = path.boundingRect();
        for (int other = 0; other < int(colors.size()); ++other) {
            if (other == color || !committed[other] || ownership[other].intersectArea(support, box) <= options.areaTolerance) continue;
            const auto region = Intersect(required[other], support, FillRule::NonZero, kBooleanPrecision);
            double enclosing = 0;
            for (const auto &loop : region) if (Area(loop) > 0) enclosing += Area(loop);
            children.push_back({other, region, enclosing});
        }
        std::stable_sort(children.begin(), children.end(), [](const auto &a, const auto &b) {
            return std::tuple(-a.enclosing, a.color) < std::tuple(-b.enclosing, b.color);
        });
        PathsD restored;
        active[color] = true;
        for (const auto &child : children) {
            const auto needed = Difference(child.region, restored, FillRule::NonZero, kBooleanPrecision);
            auto patch = correction(needed, child.color, depth + 1, limit - result.cost);
            if (!std::isfinite(patch.cost)) { active[color] = false; return retainExact(); }
            result.items += patch.items;
            result.cost += patch.cost;
            restored = Union(restored, patch.support, FillRule::NonZero, kBooleanPrecision);
        }
        active[color] = false;
        if (result.cost >= fallback.cost) result = retainExact();
        correctionCache.emplace(key, result);
        return result;
    };
    for (const int color : order) {
        correctionCache.clear();
        if (color == base && !connectedCosts) {
            output.items.push_back({restore(domain), QColor(colors[color]), {}});
            committed[color] = true;
            output.steps.append(QJsonObject{{"fill", QColor(colors[color]).name()}, {"kind", "domain-base"}, {"correction_paths", 0}});
            continue;
        }
        Patch best = exactPatch(required[color], color);
        QString bestKind = "visible-exact";
        committed[color] = true;
        const auto consider = [&](const PathsD &proposal, const QString &kind, bool fillHoles = true) {
            const auto support = fillHoles ? filled(proposal)
                : Intersect(Union(proposal, FillRule::NonZero, kBooleanPrecision), domain, FillRule::NonZero, kBooleanPrecision);
            const auto path = restore(support);
            const auto shape = polygons(path, options.flattenTolerance);
            if (PolygonIndex(shape).differenceArea(required[color], fromPolygons(required[color]).boundingRect()) > options.areaTolerance) {
                ++geometryRejected; return;
            }
            const double initial = costOf(path, color);
            if (initial >= best.cost) return;
            Patch trial{{{path, QColor(colors[color]), {}}}, shape, initial};
            PathsD restored;
            std::vector<std::pair<double, int>> parents;
            for (int other = 0; other < int(colors.size()); ++other)
                if (other != color && committed[other]) parents.push_back({-mass[other], other});
            std::sort(parents.begin(), parents.end());
            for (const auto &[weight, other] : parents) {
                if (ownership[other].intersectArea(shape, path.boundingRect()) <= options.areaTolerance) continue;
                const auto overlap = Intersect(required[other], shape, FillRule::NonZero, kBooleanPrecision);
                const auto needed = Difference(overlap, restored, FillRule::NonZero, kBooleanPrecision);
                auto patch = correction(needed, other, 0, best.cost - trial.cost);
                if (!std::isfinite(patch.cost)) return;
                trial.cost += patch.cost;
                trial.items += patch.items;
                restored = Union(restored, patch.support, FillRule::NonZero, kBooleanPrecision);
                if (trial.cost >= best.cost) return;
            }
            best = std::move(trial);
            bestKind = kind;
        };
        consider(required[color], "visible-completion");
        if (color == base && connectedCosts) consider(domain, "domain-base");
        if (colorOptions.coverageOverlap > 0) {
            QPainterPathStroker stroker;
            stroker.setWidth(colorOptions.coverageOverlap * 2);
            stroker.setJoinStyle(Qt::MiterJoin);
            const auto overlap = polygons(stroker.createStroke(fromPolygons(required[color])), options.flattenTolerance);
            consider(Union(required[color], overlap, FillRule::NonZero, kBooleanPrecision), "opaque-overlap");
        }
        PathsD original;
        for (const int source : sourceIds[color]) original.insert(original.end(), sourceShapes[source].begin(), sourceShapes[source].end());
        original = Union(original, FillRule::NonZero, kBooleanPrecision);
        consider(original, "source-support", false);
        consider(original, "source-hole-completion");
        if (connectedCosts) {
            const auto bridges = connectedColorBridges(required[color], domain, options);
            consider(bridges, "component-bridges", false);
            consider(bridges, "bridge-hole-completion");
        }
        const auto path = fromPolygons(required[color]);
        consider(rawPolygons(rectangle(path.boundingRect()), options.flattenTolerance), "rectangle");
        consider(rawPolygons(hull(path, options.flattenTolerance), options.flattenTolerance), "hull");
        PathsD rectangles, hulls;
        for (const auto &part : components(required[color])) {
            const auto shape = fromPolygons(part);
            auto rect = rawPolygons(rectangle(shape.boundingRect()), options.flattenTolerance);
            rectangles.insert(rectangles.end(), rect.begin(), rect.end());
            auto convex = rawPolygons(hull(shape, options.flattenTolerance), options.flattenTolerance);
            hulls.insert(hulls.end(), convex.begin(), convex.end());
        }
        consider(Union(rectangles, FillRule::NonZero, kBooleanPrecision), "component-rectangles");
        consider(Union(hulls, FillRule::NonZero, kBooleanPrecision), "component-hulls");
        if (!std::isfinite(best.cost)) {
            output.error = "No opaque completion for fixed color ownership";
            output.diagnostics = diagnostics();
            return output;
        }
        splitCorrections += best.items.size() - 1;
        output.items += best.items;
        output.steps.append(QJsonObject{{"fill", QColor(colors[color]).name()}, {"kind", bestKind},
            {"correction_paths", best.items.size() - 1}, {"combined_geometry_cost", best.cost}});
        if (options.progress) std::fprintf(stderr, "reconstruction: %lld draw items after %lld colors\n",
            static_cast<long long>(output.items.size()), static_cast<long long>(output.steps.size()));
    }
    QVector<QPainterPath> assigned(output.items.size());
    std::vector<PathsD> supports(output.items.size());
    std::vector<PathsD> claimed(colors.size());
    for (int draw = output.items.size() - 1; draw >= 0; --draw) {
        const int color = indices.at(output.items[draw].color.rgb());
        const auto shape = polygons(output.items[draw].path, options.flattenTolerance);
        supports[draw] = shape;
        const auto requiredPart = Difference(Intersect(shape, required[color], FillRule::NonZero, kBooleanPrecision),
            claimed[color], FillRule::NonZero, kBooleanPrecision);
        assigned[draw] = fromPolygons(requiredPart);
        claimed[color] = Union(claimed[color], requiredPart, FillRule::NonZero, kBooleanPrecision);
    }
    const double visibleLimit = options.visibleAreaTolerance > 0 ? options.visibleAreaTolerance : options.areaTolerance;
    for (int color = 0; color < int(colors.size()); ++color) {
        const double missing = std::abs(Area(Difference(required[color], claimed[color], FillRule::NonZero, kBooleanPrecision)));
        if (missing > visibleLimit) {
            output.error = "Completion removed fixed original color ownership";
            output.diagnostics = diagnostics();
            output.diagnostics.insert("unassigned_fill", QColor(colors[color]).name());
            output.diagnostics.insert("unassigned_visible_area", missing);
            return output;
        }
    }
    int opaqueBase = -1;
    for (int draw = output.items.size() - 1; draw >= 0; --draw)
        if (std::abs(Area(Xor(supports[draw], domain, FillRule::NonZero, kBooleanPrecision))) <= options.areaTolerance) {
            opaqueBase = draw;
            break;
        }
    const int previousCount = output.items.size();
    QVector<Item> retained;
    QVector<QPainterPath> retainedOwnership;
    std::vector<PathsD> retainedSupports;
    for (int draw = 0; draw < output.items.size(); ++draw) {
        if (draw != opaqueBase && area(assigned[draw], options.flattenTolerance) <= options.areaTolerance) continue;
        retained.push_back(output.items[draw]);
        retainedOwnership.push_back(assigned[draw]);
        retainedSupports.push_back(std::move(supports[draw]));
    }
    output.items = retained;
    assigned = retainedOwnership;
    supports = std::move(retainedSupports);
    std::vector<PathsD> assignedShapes;
    for (const auto &path : assigned) assignedShapes.push_back(polygons(path, options.flattenTolerance));
    const OwnerIndex spatial(assigned, fromPolygons(domain).boundingRect());
    std::set<std::pair<int, int>> edges;
    QJsonArray backwardEdges;
    double maximumBackwardArea = 0;
    int backwardCount = 0;
    for (int draw = 0; draw < output.items.size(); ++draw) {
        const PolygonIndex support(supports[draw]);
        for (const int other : spatial.query(output.items[draw].path.boundingRect())) {
            if (draw == other || (connectedCosts && output.items[draw].color == output.items[other].color)) continue;
            const double overlap = support.intersectArea(assignedShapes[other], assigned[other].boundingRect());
            if (overlap <= options.areaTolerance) continue;
            edges.insert({draw, other});
            if (other < draw) {
                ++backwardCount;
                maximumBackwardArea = std::max(maximumBackwardArea, overlap);
                if (backwardEdges.size() < 64) backwardEdges.append(QJsonObject{{"from", draw}, {"to", other},
                    {"from_fill", output.items[draw].color.name()}, {"to_fill", output.items[other].color.name()},
                    {"overlap_area", overlap}});
            }
        }
    }
    auto graph = topologicalOrder(output.items.size(), edges);
    output.diagnostics = diagnostics();
    output.diagnostics.insert("backward_dependency_count", backwardCount);
    output.diagnostics.insert("maximum_backward_overlap_area", maximumBackwardArea);
    output.diagnostics.insert("backward_dependency_samples", backwardEdges);
    if (!graph.error.isEmpty()) { output.error = graph.error; output.graph = graph; return output; }
    QVector<Item> sorted;
    QVector<int> positions(output.items.size());
    for (const int draw : graph.order) { positions[draw] = sorted.size(); sorted.push_back(output.items[draw]); }
    output.items = sorted;
    std::set<std::pair<int, int>> remapped;
    for (const auto &[from, to] : edges) remapped.insert({positions[from], positions[to]});
    output.graph = topologicalOrder(output.items.size(), remapped);
    output.diagnostics.insert("redundant_draw_items_removed", previousCount - output.items.size());
    return output;
}
