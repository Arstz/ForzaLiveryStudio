namespace {

struct SupportScene {
    QVector<Item> items;
    QVector<QPainterPath> ownership;
    Graph graph;
    QJsonObject geometry;
    QString error;
};

PathsD regularizeSupportDomain(const PathsD &input, double limit, QJsonObject &diagnostics) {
    std::vector<std::pair<double, int>> holes;
    for (int index = 0; index < int(input.size()); ++index)
        if (Area(input[index]) < 0) holes.push_back({-Area(input[index]), index});
    std::stable_sort(holes.begin(), holes.end());
    std::set<int> removed;
    double spent = 0;
    for (const auto &[value, index] : holes) {
        if (spent + value > limit) break;
        spent += value;
        removed.insert(index);
    }
    PathsD result;
    for (int index = 0; index < int(input.size()); ++index)
        if (!removed.contains(index)) result.push_back(input[index]);
    int excursions = 0;
    result = prunePolygonExcursions(result, std::max(0.0, limit - spent), excursions);
    result = Union(result, FillRule::NonZero, kBooleanPrecision);
    const double delta = std::abs(Area(Xor(input, result, FillRule::NonZero, kBooleanPrecision)));
    if (delta > limit) return input;
    diagnostics = {{"filled_sub_tolerance_domain_holes", int(removed.size())},
        {"removed_domain_excursions", excursions}, {"difference_area", delta}, {"area_limit", limit}};
    return result;
}

SupportScene orderSupportScene(QVector<Item> items, const std::map<QRgb, PathsD> &required,
                              const PathsD &domain, const Options &options) {
    SupportScene output;
    std::vector<PathsD> shapes, assigned;
    QVector<QPainterPath> ownership;
    std::map<QRgb, PathsD> claimed;
    PathsD combined;
    for (const auto &item : items) {
        if (item.color.alpha() != 255 || !required.contains(item.color.rgb())) {
            output.error = "Support changed the opaque palette";
            return output;
        }
        shapes.push_back(polygons(item.path, options.flattenTolerance));
        combined.insert(combined.end(), shapes.back().begin(), shapes.back().end());
    }
    assigned.resize(items.size());
    ownership.resize(items.size());
    for (int draw = items.size() - 1; draw >= 0; --draw) {
        const QRgb fill = items[draw].color.rgb();
        const auto owned = Intersect(shapes[draw], required.at(fill), FillRule::NonZero, kBooleanPrecision);
        assigned[draw] = Difference(owned, claimed[fill], FillRule::NonZero, kBooleanPrecision);
        ownership[draw] = fromPolygons(assigned[draw]);
        claimed[fill] = Union(claimed[fill], assigned[draw], FillRule::NonZero, kBooleanPrecision);
    }
    double missing = 0, maximumMissing = 0, hiddenArea = 0;
    for (int draw = 0; draw < items.size(); ++draw)
        hiddenArea += std::max(0.0, std::abs(Area(shapes[draw])) - std::abs(Area(assigned[draw])));
    for (const auto &[fill, shape] : required) {
        const double delta = std::abs(Area(Difference(shape, claimed[fill], FillRule::NonZero, kBooleanPrecision)));
        missing += delta;
        maximumMissing = std::max(maximumMissing, delta);
    }
    const auto painted = Union(combined, FillRule::NonZero, kBooleanPrecision);
    const double silhouette = std::abs(Area(Xor(painted, domain, FillRule::NonZero, kBooleanPrecision)));
    output.geometry = {{"missing_fixed_color_area", missing}, {"maximum_missing_color_area", maximumMissing},
        {"silhouette_difference_area", silhouette},
        {"hidden_support_area", hiddenArea},
        {"ownership_basis", "Fixed original visible colors partitioned across final draw positions"}};
    if (maximumMissing > options.areaTolerance || silhouette > options.silhouetteTolerance) {
        output.error = "Support removed required color territory or changed the external silhouette";
        return output;
    }
    const OwnerIndex spatial(ownership, fromPolygons(domain).boundingRect());
    std::set<std::pair<int, int>> edges;
    QJsonArray backward;
    for (int draw = 0; draw < items.size(); ++draw) {
        const PolygonIndex support(shapes[draw]);
        for (const int owner : spatial.query(items[draw].path.boundingRect())) {
            if (items[draw].color == items[owner].color) continue;
            const double overlap = support.intersectArea(assigned[owner], ownership[owner].boundingRect());
            if (overlap > options.areaTolerance) {
                edges.insert({draw, owner});
                if (owner < draw && backward.size() < 32)
                    backward.append(QJsonObject{{"from", draw}, {"to", owner}, {"overlap_area", overlap},
                        {"from_fill", items[draw].color.name()}, {"to_fill", items[owner].color.name()}});
            }
        }
    }
    const auto graph = topologicalOrder(items.size(), edges);
    output.geometry.insert("backward_dependency_samples", backward);
    if (!graph.error.isEmpty()) { output.error = graph.error; output.graph = graph; return output; }
    QVector<int> positions(items.size());
    for (const int draw : graph.order) {
        positions[draw] = output.items.size();
        items[draw].owners = {int(output.items.size())};
        output.items.push_back(items[draw]);
        output.ownership.push_back(ownership[draw]);
    }
    std::set<std::pair<int, int>> remapped;
    for (const auto &[from, to] : edges) remapped.insert({positions[from], positions[to]});
    output.graph = topologicalOrder(output.items.size(), remapped);
    std::map<QRgb, PathsD> visible;
    for (int draw = 0; draw < items.size(); ++draw) {
        PathsD above;
        for (int later = positions[draw] + 1; later < output.items.size(); ++later) {
            const int original = graph.order[later];
            if (!items[draw].path.boundingRect().intersects(items[original].path.boundingRect())) continue;
            above.insert(above.end(), shapes[original].begin(), shapes[original].end());
        }
        const auto exposed = Difference(shapes[draw], Union(above, FillRule::NonZero, kBooleanPrecision),
            FillRule::NonZero, kBooleanPrecision);
        auto &color = visible[items[draw].color.rgb()];
        color.insert(color.end(), exposed.begin(), exposed.end());
    }
    double colorDifference = 0, maximumColorDifference = 0;
    for (const auto &[fill, shape] : required) {
        const double delta = std::abs(Area(Xor(shape, Union(visible[fill], FillRule::NonZero, kBooleanPrecision),
            FillRule::NonZero, kBooleanPrecision)));
        colorDifference += delta;
        maximumColorDifference = std::max(maximumColorDifference, delta);
    }
    output.geometry.insert("sum_visible_color_difference_area", colorDifference);
    output.geometry.insert("maximum_visible_color_difference_area", maximumColorDifference);
    const double visibleLimit = options.visibleAreaTolerance > 0 ? options.visibleAreaTolerance : options.areaTolerance;
    output.geometry.insert("visible_color_area_limit", visibleLimit);
    if (maximumColorDifference > visibleLimit) output.error = "Complete visible color geometry exceeds the area tolerance";
    return output;
}

Metrics supportMetrics(const QVector<Item> &items, const Graph &graph, const Options &options) {
    auto metrics = pathMetrics(objects(items), options.flattenTolerance);
    QVector<QVector<int>> next(items.size());
    QVector<int> depths(items.size(), 1);
    metrics.orderingEdges = graph.edges.size();
    metrics.orderingMeasured = metrics.costMeasured = true;
    for (const auto &[from, to] : graph.edges) next[from].push_back(to);
    for (const int draw : graph.order) {
        metrics.orderingDepth = std::max(metrics.orderingDepth, depths[draw]);
        for (const int above : next[draw]) depths[above] = std::max(depths[above], depths[draw] + 1);
    }
    metrics.cost = geometryCost(metrics, options);
    return metrics;
}

ColorPlan peelColorSupports(const std::map<QRgb, PathsD> &required, const PathsD &domain,
                           const std::map<QRgb, PathsD> &sources, const QVector<QRgb> &order,
                           const Options &options, const std::function<QPainterPath(const PathsD &)> &restore) {
    ColorPlan output;
    PathsD ownership;
    for (const auto &[fill, shape] : required) ownership.insert(ownership.end(), shape.begin(), shape.end());
    PathsD covered = Difference(domain, Union(ownership, FillRule::NonZero, kBooleanPrecision), FillRule::NonZero, kBooleanPrecision);
    int placed = 0;
    for (auto current = order.crbegin(); current != order.crend(); ++current) {
        const QRgb fill = *current;
        const auto &visible = required.at(fill);
        const auto allowed = Union(visible, covered, FillRule::NonZero, kBooleanPrecision);
        const PolygonIndex available(allowed);
        QPainterPath best;
        QString kind;
        double bestCost = std::numeric_limits<double>::infinity();
        std::set<QByteArray> seen;
        const auto consider = [&](const PathsD &proposal, const QString &name) {
            if (proposal.empty()) return;
            const auto proposalBounds = fromPolygons(proposal).boundingRect();
            const bool permitted = available.differenceArea(proposal, proposalBounds) <= options.areaTolerance
                && std::abs(Area(Difference(proposal, domain, FillRule::NonZero, kBooleanPrecision))) <= options.silhouetteTolerance;
            const auto clipped = permitted ? proposal : Intersect(proposal, allowed, FillRule::NonZero, kBooleanPrecision);
            const auto path = restore(clipped);
            const auto shape = polygons(path, options.flattenTolerance);
            if (PolygonIndex(shape).differenceArea(visible, fromPolygons(visible).boundingRect()) > options.areaTolerance
                || available.differenceArea(shape, path.boundingRect()) > options.areaTolerance) return;
            const auto encoded = svg({{path, QColor(fill)}}, {1, 1});
            if (!seen.insert(encoded).second) return;
            auto metrics = pathMetrics({{path, QColor(fill)}}, options.flattenTolerance);
            metrics.paths = metrics.components;
            const double cost = geometryCost(metrics, options);
            if (cost >= bestCost - 1e-9) return;
            best = path;
            bestCost = cost;
            kind = name;
        };
        if (placed + 1 == order.size()) consider(domain, "domain-underlay");
        consider(visible, "visible-color");
        consider(allowed, "covered-color-bridge");
        const auto visiblePath = restore(visible);
        consider(rawPolygons(rectangle(visiblePath.boundingRect()), options.flattenTolerance), "covered-rectangle");
        consider(polygons(hull(visiblePath, options.flattenTolerance), options.flattenTolerance), "covered-hull");
        if (sources.contains(fill)) consider(sources.at(fill), "source-color-support");
        const auto bridged = connectedColorBridges(visible, allowed, options);
        consider(bridged, "covered-bridges");
        PathsD bridgedOuter;
        for (const auto &loop : bridged) if (Area(loop) > 0) bridgedOuter.push_back(loop);
        consider(Union(bridgedOuter, FillRule::NonZero, kBooleanPrecision), "bridged-hole-fill");
        PathsD outer;
        for (const auto &loop : visible) if (Area(loop) > 0) outer.push_back(loop);
        consider(Union(outer, FillRule::NonZero, kBooleanPrecision), "covered-hole-fill");
        if (!std::isfinite(bestCost)) { output.error = "No fixed-color support below the selected top layers"; return output; }
        output.items.push_back({best, QColor(fill), {}});
        output.steps.append(QJsonObject{{"fill", QColor(fill).name()}, {"kind", kind}, {"connected_geometry_cost", bestCost}});
        covered = Union(covered, visible, FillRule::NonZero, kBooleanPrecision);
        ++placed;
        if (options.progress && (placed % 16 == 0 || placed == order.size())) {
            std::fprintf(stderr, "supports: peeled %d/%d colors\n", placed, int(order.size()));
            std::fflush(stderr);
        }
    }
    std::reverse(output.items.begin(), output.items.end());
    const auto scene = orderSupportScene(output.items, required, domain, options);
    if (scene.error.isEmpty()) output.items = scene.items;
    output.graph = scene.graph;
    output.error = scene.error;
    output.diagnostics = scene.geometry;
    return output;
}

} // namespace

Result Optimizer::runSupports() {
    Result result;
    result.items = baseline_;
    if (!auditPenTargets(objects(baseline_), options_).value("passed").toBool()) {
        result.error = "Color-support baseline contains an incompatible Bucket target";
        return result;
    }
    result.graph = dependencies(baseline_);
    result.before = supportMetrics(baseline_, result.graph, options_);
    result.after = result.before;
    result.error = result.graph.error;
    if (!result.error.isEmpty() || options_.maximumPasses == 0) return result;
    const auto reference = renderer_(objects(baseline_));
    const auto lockedDomain = polygons(domain_, options_.flattenTolerance);
    QJsonObject domainRegularization;
    const auto domain = regularizeSupportDomain(lockedDomain, options_.silhouetteTolerance, domainRegularization);
    const CurveBank curves(baseline_, options_.flattenTolerance);
    int curveFallbacks = 0;
    std::map<QByteArray, QPainterPath> curveCache;
    const auto restore = [&](const PathsD &shape) {
        QByteArray encoded;
        QDataStream stream(&encoded, QIODevice::WriteOnly);
        stream << quint32(shape.size());
        for (const auto &loop : shape) {
            stream << quint32(loop.size());
            for (const auto &point : loop) stream << point.x << point.y;
        }
        const auto key = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
        if (const auto found = curveCache.find(key); found != curveCache.end()) return found->second;
        auto path = curves.restore(shape);
        const auto normalized = Union(shape, FillRule::NonZero, kBooleanPrecision);
        const auto actual = polygons(path, options_.flattenTolerance);
        if (std::abs(Area(Xor(actual, normalized, FillRule::NonZero, kBooleanPrecision))) > options_.areaTolerance) {
            ++curveFallbacks;
            path = fromPolygons(normalized);
        }
        if (curveCache.size() >= 512) curveCache.clear();
        curveCache.emplace(key, path);
        return path;
    };
    std::map<QRgb, PathsD> required, sourceColors;
    std::vector<PathsD> sourceShapes;
    std::vector<ColorRegion> regions;
    std::vector<QColor> fills;
    for (int owner = 0; owner < ownership_.size(); ++owner) {
        const auto shape = polygons(ownership_[owner], options_.flattenTolerance);
        if (shape.empty()) continue;
        const auto color = baseline_[ownerSources_[owner]].color;
        auto &territory = required[color.rgb()];
        territory.insert(territory.end(), shape.begin(), shape.end());
        ColorRegion region;
        region.shape = shape;
        region.source = ownerSources_[owner];
        regions.push_back(std::move(region));
        fills.push_back(color);
    }
    for (auto &[fill, shape] : required) shape = Union(shape, FillRule::NonZero, kBooleanPrecision);
    if (options_.onSupportTrial) {
        QVector<gui::SvgVectorObject> fixed;
        for (const auto &[fill, shape] : required) fixed.push_back({fromPolygons(shape), QColor(fill)});
        options_.onSupportTrial(QJsonObject{{"artifact", "fixed_color_ownership.svg"},
            {"svg", QString::fromUtf8(svg(fixed, reference.size()))}});
    }
    for (const auto &item : baseline_) {
        auto shape = polygons(item.path, options_.flattenTolerance);
        sourceShapes.push_back(shape);
        auto &combined = sourceColors[item.color.rgb()];
        combined.insert(combined.end(), shape.begin(), shape.end());
    }
    for (auto &[fill, shape] : sourceColors) shape = Union(shape, FillRule::NonZero, kBooleanPrecision);
    if (required.empty()) return result;
    QVector<QRgb> colors;
    std::map<QRgb, double> mass, density;
    for (const auto &[fill, shape] : required) {
        colors.push_back(fill);
        mass[fill] = std::abs(Area(shape));
        auto metrics = pathMetrics({{restore(shape), QColor(fill)}}, options_.flattenTolerance);
        metrics.paths = metrics.components;
        density[fill] = geometryCost(metrics, options_) / std::max(1e-12, mass[fill]);
    }
    std::vector<std::pair<QString, QVector<QRgb>>> orders;
    const auto addOrder = [&](const QString &name, QVector<QRgb> order) {
        if (order.size() != colors.size()) return;
        for (const auto &[existingName, existing] : orders) if (existing == order) return;
        orders.push_back({name, std::move(order)});
    };
    auto order = colors;
    std::stable_sort(order.begin(), order.end(), [&](QRgb a, QRgb b) { return std::pair(density[a], a) < std::pair(density[b], b); });
    addOrder("difficulty", order);
    order = colors;
    std::stable_sort(order.begin(), order.end(), [&](QRgb a, QRgb b) { return std::pair(-mass[a], a) < std::pair(-mass[b], b); });
    addOrder("area", order);
    order.clear();
    std::set<QRgb> seen;
    for (const auto &item : baseline_)
        if (required.contains(item.color.rgb()) && seen.insert(item.color.rgb()).second) order.push_back(item.color.rgb());
    addOrder("source-first", order);
    order.clear();
    seen.clear();
    for (auto item = baseline_.crbegin(); item != baseline_.crend(); ++item)
        if (required.contains(item->color.rgb()) && seen.insert(item->color.rgb()).second) order.push_back(item->color.rgb());
    std::reverse(order.begin(), order.end());
    addOrder("source-last", order);
    std::stable_sort(orders.begin(), orders.end(), [](const auto &a, const auto &b) {
        return (a.first == "source-first" ? 0 : 1) < (b.first == "source-first" ? 0 : 1);
    });
    if (orders.size() > size_t(options_.shortlistSize)) orders.resize(options_.shortlistSize);
    std::pair<double, int> bestTie{std::numeric_limits<double>::infinity(), std::numeric_limits<int>::max()};
    struct RefinementCandidate {
        QVector<Item> items;
        QVector<QPainterPath> ownership;
        Metrics metrics;
        QJsonArray steps;
        QString name;
    };
    std::vector<RefinementCandidate> refinementPool;
    const auto assess = [&](ColorPlan plan, const QString &name, const auto &started) {
        if (options_.onSupportTrial && !plan.items.isEmpty())
            options_.onSupportTrial(QJsonObject{{"artifact", "support_latest_plan.svg"},
                {"svg", QString::fromUtf8(svg(objects(plan.items), reference.size()))}, {"strategy", name}});
        QString error = plan.error;
        QJsonObject native;
        auto candidate = plan.items;
        Graph graph = plan.graph;
        QJsonObject geometry = plan.diagnostics;
        QVector<QPainterPath> fixedOwnership;
        if (error.isEmpty()) {
            const auto prepared = preparePenTargets(candidate, options_, renderer_);
            native = prepared.report;
            error = prepared.error;
            candidate = prepared.items;
            if (error.isEmpty()) {
                const auto scene = orderSupportScene(candidate, required, lockedDomain, options_);
                candidate = scene.items;
                graph = scene.graph;
                geometry = scene.geometry;
                fixedOwnership = scene.ownership;
                error = scene.error;
            }
        }
        Metrics metrics;
        QJsonObject image;
        if (error.isEmpty()) {
            metrics = supportMetrics(candidate, graph, options_);
            const auto rendered = renderer_(objects(candidate));
            image = imageError(reference, rendered);
            if (!withinBudget(image, options_)) error = "Color-support reconstruction exceeds the rendering budget";
            if (!appearanceReference_.isNull()) {
                const auto appearance = appearanceError(appearanceReference_, rendered, appearanceLimits_.localWindow);
                if (appearance.value("rmse").toDouble() > appearanceLimits_.renderRmse
                    || appearance.value("max_channel_error").toDouble() > appearanceLimits_.renderMaxError
                    || appearance.value("max_local_rmse").toDouble() > appearanceLimits_.localRmse)
                    error = "Color-support reconstruction exceeds original-input appearance limits";
            }
        }
        const QJsonObject entry{{"strategy", name}, {"metrics", metrics.json()}, {"image_error", image},
            {"error", error}, {"geometry", geometry}, {"native_conversion", native},
            {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
        result.strategies.append(entry);
        if (options_.onSupportTrial) options_.onSupportTrial(entry);
        if (error.isEmpty() && metrics.cost < result.before.cost - 1e-9
            && (metrics.paths < result.before.paths || metrics.holes < result.before.holes
            || metrics.lineSegments + 3 * metrics.bezierSegments < result.before.lineSegments + 3 * result.before.bezierSegments))
            refinementPool.push_back({candidate, fixedOwnership, metrics, plan.steps, name});
        if (metrics.costMeasured && (result.candidateItems.isEmpty()
            || metrics.cost < result.candidateReport.value("metrics").toObject().value("cost").toDouble())) {
            result.candidateItems = candidate;
            result.candidateReport = entry;
        }
        const auto tie = std::pair(geometry.value("hidden_support_area").toDouble(), int(graph.edges.size()));
        if (error.isEmpty() && (metrics.cost < result.after.cost - 1e-9
            || (result.accepted && std::abs(metrics.cost - result.after.cost) <= 1e-9 && tie < bestTie))) {
            result.items = candidate;
            result.after = metrics;
            result.graph = graph;
            result.accepted = 1;
            result.selectedStrategy = "supports";
            result.silhouetteDifference = geometry.value("silhouette_difference_area").toDouble();
            result.diagnostics = plan.steps;
            bestTie = tie;
        }
        if (options_.progress) {
            std::fprintf(stderr, "supports: %s, %d targets, cost %.0f, %s\n", qPrintable(name), metrics.paths, metrics.cost,
                error.isEmpty() ? "validated" : qPrintable(error));
            std::fflush(stderr);
        }
    };
    for (const auto &[name, preferred] : orders) {
        const auto started = std::chrono::steady_clock::now();
        if (options_.progress) std::fprintf(stderr, "supports: starting peel-%s\n", qPrintable(name));
        assess(peelColorSupports(required, domain, sourceColors, preferred, options_, restore), "peel-" + name, started);
    }
    ColorOptions exact;
    exact.coverageOverlap = 0;
    for (int index = 0; index < std::min(int(orders.size()), std::max(1, options_.maximumPasses)); ++index) {
        const auto &[name, preferred] = orders[index];
        const auto started = std::chrono::steady_clock::now();
        if (options_.progress) std::fprintf(stderr, "supports: starting repaint-%s\n", qPrintable(name));
        assess(planColorStack(required, domain, sourceShapes, regions, fills, options_, exact, restore, preferred, true),
            "repaint-" + name, started);
    }
    QVector<int> selected;
    const auto select = [&](const auto &less) {
        if (refinementPool.empty() || selected.size() >= options_.shortlistSize) return;
        int best = 0;
        for (int index = 1; index < int(refinementPool.size()); ++index)
            if (less(refinementPool[index].metrics, refinementPool[best].metrics)) best = index;
        if (!selected.contains(best)) selected.push_back(best);
    };
    select([](const Metrics &a, const Metrics &b) { return a.cost < b.cost; });
    select([](const Metrics &a, const Metrics &b) { return std::pair(a.paths, a.cost) < std::pair(b.paths, b.cost); });
    select([](const Metrics &a, const Metrics &b) { return std::pair(a.holes, a.cost) < std::pair(b.holes, b.cost); });
    select([](const Metrics &a, const Metrics &b) {
        return std::pair(a.lineSegments + 3 * a.bezierSegments, a.cost) < std::pair(b.lineSegments + 3 * b.bezierSegments, b.cost);
    });
    QJsonArray refinements;
    for (const int index : selected) {
        const auto &candidate = refinementPool[index];
        const auto started = std::chrono::steady_clock::now();
        Options refinement = options_;
        refinement.strategy = "incremental";
        refinement.maximumPasses = options_.maximumPasses;
        refinement.maximumChordSpans = options_.maximumChordSpans;
        refinement.componentLimit = 1;
        refinement.colorOwnership = true;
        refinement.onSupportTrial = {};
        Optimizer refiner(candidate.items, refinement, {}, renderer_, candidate.ownership, domain_);
        ColorOptions limits;
        limits.renderRmse = options_.renderRmse;
        limits.renderMaxError = limits.localRmse = options_.renderMaxError;
        limits.localWindow = 1;
        refiner.setAppearanceGuard(appearanceReference_.isNull() ? reference : appearanceReference_,
            appearanceReference_.isNull() ? limits : appearanceLimits_);
        if (options_.progress) std::fprintf(stderr, "supports: refining %s, %d connected targets with fixed original color ownership\n",
            qPrintable(candidate.name), int(candidate.items.size()));
        const auto refined = refiner.run();
        auto scene = orderSupportScene(refined.items, required, lockedDomain, options_);
        const auto image = imageError(reference, renderer_(objects(scene.items)));
        const auto metrics = scene.error.isEmpty() ? supportMetrics(scene.items, scene.graph, options_) : Metrics{};
        const bool improved = refined.error.isEmpty() && scene.error.isEmpty()
            && withinBudget(image, options_) && metrics.cost < result.after.cost - 1e-9;
        const QJsonObject entry{{"strategy", "refine-" + candidate.name}, {"metrics", metrics.json()},
            {"image_error", image}, {"geometry", scene.geometry}, {"accepted", improved},
            {"error", refined.error.isEmpty() ? scene.error : refined.error},
            {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
        result.strategies.append(entry);
        if (options_.onSupportTrial) options_.onSupportTrial(entry);
        if (improved) {
            result.items = scene.items;
            result.graph = scene.graph;
            result.after = metrics;
            result.silhouetteDifference = scene.geometry.value("silhouette_difference_area").toDouble();
            result.diagnostics = candidate.steps;
            result.accepted = 1;
            result.selectedStrategy = "supports";
        }
        refinements.append(QJsonObject{{"construction", candidate.name}, {"details", refined.diagnostics}, {"accepted", improved}});
    }
    result.diagnostics.prepend(QJsonObject{{"candidate_refinements", refinements},
        {"fixed_original_color_ownership", true}, {"refinement_chord_spans", options_.maximumChordSpans},
        {"refinement_passes", options_.maximumPasses}, {"refinement_component_limit", 1}});
    result.diagnostics.prepend(QJsonObject{{"strategy", "supports"}, {"unchanged_palette", true},
        {"primary_color_count", int(required.size())}, {"tested_orders", int(orders.size())},
        {"curve_fallbacks", curveFallbacks}, {"region_count_target", QJsonValue()},
        {"domain_regularization", domainRegularization},
        {"connected_target_cost", true}, {"joint_hole_repainting", true}});
    return result;
}
