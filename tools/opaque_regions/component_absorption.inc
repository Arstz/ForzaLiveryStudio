            {
                using Request = std::tuple<double, int, QRgb>;
                std::vector<Request> requests;
                for (int owner = 0; owner < ownership_.size(); ++owner) {
                    const int position = state.ownerPositions[owner];
                    if (position < 0 || state.scene[position]->item.owners.size() < 2) continue;
                    const auto donor = state.scene[position];
                    std::map<QRgb, double> fills;
                    for (const auto &[neighbor, length] : contacts[owner]) {
                        const int adjacent = state.ownerPositions[neighbor];
                        if (adjacent >= 0 && adjacent != position)
                            fills[state.scene[adjacent]->item.color.rgb()] += length;
                    }
                    for (const auto &[fill, length] : fills) {
                        if (fill == donor->item.color.rgb()) continue;
                        const double change = colorError(moments[owner], QColor(fill)) - colorError(moments[owner], donor->item.color);
                        const double support = absorptionLimits_.detailScale * length
                            + absorptionLimits_.detailScale * absorptionLimits_.detailScale;
                        const double score = colorDistance(perceptualColor(donor->item.color), perceptualColor(QColor(fill)))
                            * moments[owner].area / std::max(1e-12, moments[owner].area + support);
                        if (score > threshold && change >= 0) continue;
                        if (absorptionSourceGrowth_ == 0 && change > 1e-6) continue;
                        requests.push_back({change, owner, fill});
                    }
                }
                std::stable_sort(requests.begin(), requests.end());
                int visited = 0;
                for (const auto &[estimate, owner, fillRgb] : requests) {
                    const int position = state.ownerPositions[owner];
                    if (position < 0) continue;
                    const auto donor = state.scene[position];
                    const QColor fill(fillRgb);
                    if (donor->item.owners.size() < 2 || donor->item.color == fill) continue;
                    const double change = colorError(moments[owner], fill) - colorError(moments[owner], donor->item.color);
                    const double available = absorptionSourceGrowth_ == 0 ? 0 : sourceMaximumSquared - state.sourceSquared;
                    if (change > available + 1e-6) continue;
                    Scene compatible, exact;
                    std::set<int> neighbors;
                    for (const auto &[neighbor, length] : contacts[owner]) {
                        const int adjacent = state.ownerPositions[neighbor];
                        if (adjacent >= 0 && adjacent != position) neighbors.insert(adjacent);
                    }
                    for (const int adjacent : neighbors) {
                        const auto node = state.scene[adjacent];
                        if (node->item.color == fill) exact.push_back(node);
                        if (colorDistance(perceptualColor(node->item.color), perceptualColor(fill)) <= threshold)
                            compatible.push_back(node);
                    }
                    if (exact.empty()) continue;
                    std::vector<Scene> groups{compatible, exact};
                    for (const auto &node : exact) groups.push_back({node});
                    struct Choice { Edit edit; double saving; double added; int dependencies; };
                    std::vector<Choice> choices;
                    std::set<std::set<int>> seenGroups;
                    const auto record = [&](Edit edit, double beforeCost) {
                        double afterCost = 0, added = 0;
                        int dependencies = 0;
                        for (const auto &node : edit.added) afterCost += node->cost;
                        if (afterCost >= beforeCost - 1e-9) return;
                        for (const auto &node : edit.added) {
                            added += std::max(0.0, std::abs(Area(node->shape)) - momentOf(node).area);
                            dependencies += int(node->above.size());
                        }
                        choices.push_back({std::move(edit), beforeCost - afterCost, added, dependencies});
                    };
                    for (const auto &group : groups) {
                        if (group.empty()) continue;
                        std::set<int> removed{donor->id}, owners{owner}, groupIds;
                        PathsD support;
                        double beforeCost = donor->cost, predicted = change;
                        for (const auto &node : group) {
                            groupIds.insert(node->id);
                            removed.insert(node->id);
                            owners.insert(node->item.owners.begin(), node->item.owners.end());
                            support.insert(support.end(), node->shape.begin(), node->shape.end());
                            beforeCost += node->cost;
                            predicted += colorError(momentOf(node), fill) - colorError(momentOf(node), node->item.color);
                        }
                        if (!seenGroups.insert(groupIds).second || predicted > available + 1e-6) continue;
                        support.insert(support.end(), required[owner].begin(), required[owner].end());
                        const auto visible = fromPolygons(territory(owners));
                        const std::vector<PathsD> proposals{Union(support, FillRule::NonZero, kBooleanPrecision), territory(owners),
                            polygons(rectangle(visible.boundingRect()), options_.flattenTolerance),
                            polygons(hull(visible, options_.flattenTolerance), options_.flattenTolerance)};
                        std::set<QByteArray> seen;
                        for (const auto &shape : proposals) {
                            auto path = curves.restore(shape);
                            if (std::abs(Area(Xor(shape, polygons(path, options_.flattenTolerance), FillRule::NonZero,
                                kBooleanPrecision))) > options_.areaTolerance) path = fromPolygons(shape);
                            if (!seen.insert(svg({{path, fill}}, reference.size())).second) continue;
                            const auto prototype = makeNode({path, fill, owners}, "component-absorption-group", beforeCost);
                            if (!prototype) continue;
                            const PolygonIndex coverage(prototype->shape);
                            const auto touched = spatial.query(prototype->item.path.boundingRect());
                            std::set<int> expanded = owners;
                            std::set<int> allCovered = owners;
                            for (const int part : touched) {
                                if (part == owner || !donor->item.owners.contains(part)) continue;
                                if (coverage.differenceArea(required[part], ownership_[part].boundingRect()) != 0) continue;
                                allCovered.insert(part);
                                const double error = colorError(moments[part], fill) - colorError(moments[part], donor->item.color);
                                if (error <= 1e-6) expanded.insert(part);
                            }
                            std::vector<std::set<int>> ownershipPlans{allCovered};
                            if (expanded != allCovered) ownershipPlans.push_back(expanded);
                            if (expanded != owners) ownershipPlans.push_back(owners);
                            for (const auto &assigned : ownershipPlans) {
                                auto target = std::make_shared<Node>(*prototype);
                                target->id = serial++;
                                target->item.owners = assigned;
                                ++generated;
                                std::erase_if(target->above, [&](int part) { return assigned.contains(part); });
                                Scene upper;
                                auto base = donor->item.owners;
                                for (const int part : assigned) base.erase(part);
                                bool valid = true;
                                double upperCost = 0;
                                for (const int part : touched) {
                                    if (!base.contains(part)) continue;
                                    if (coverage.intersectArea(required[part], ownership_[part].boundingRect()) <= options_.areaTolerance
                                        && coverage.differenceArea(required[part], ownership_[part].boundingRect()) != 0) continue;
                                    base.erase(part);
                                    auto restored = curves.restore(required[part]);
                                    if (std::abs(Area(Xor(required[part], polygons(restored, options_.flattenTolerance), FillRule::NonZero,
                                        kBooleanPrecision))) > options_.areaTolerance) restored = fromPolygons(required[part]);
                                    auto node = makeNode({restored, donor->item.color, {part}}, "component-restoration",
                                        beforeCost - target->cost - upperCost);
                                    if (!node) {
                                        node = makeNode({fromPolygons(required[part]), donor->item.color, {part}}, "component-restoration",
                                            beforeCost - target->cost - upperCost);
                                    }
                                    if (!node) { valid = false; break; }
                                    upperCost += node->cost;
                                    upper.push_back(node);
                                }
                                if (!valid) continue;
                                Scene additions{target};
                                additions += upper;
                                if (base.empty()) {
                                    record({removed, additions, "component-absorption-group"}, beforeCost);
                                    continue;
                                }
                                auto retained = std::make_shared<Node>(*donor);
                                retained->id = serial++;
                                retained->kind = "component-base";
                                retained->item.owners = base;
                                ++generated;
                                for (const int part : donor->item.owners) if (!base.contains(part)) retained->above.push_back(part);
                                std::sort(retained->above.begin(), retained->above.end());
                                retained->above.erase(std::unique(retained->above.begin(), retained->above.end()), retained->above.end());
                                auto unchanged = additions;
                                unchanged.prepend(retained);
                                record({removed, unchanged, "component-absorption-group"}, beforeCost);
                                Metrics rectangleMetrics;
                                rectangleMetrics.paths = rectangleMetrics.components = 1;
                                rectangleMetrics.lineSegments = 4;
                                if (donor->cost <= geometryCost(rectangleMetrics, options_) + 1e-9) continue;
                                PathsD baseParts;
                                for (const int part : base) baseParts.insert(baseParts.end(), required[part].begin(), required[part].end());
                                baseParts = Union(baseParts, FillRule::NonZero, kBooleanPrecision);
                                const auto baseVisible = fromPolygons(baseParts);
                                const std::vector<PathsD> baseShapes{baseParts,
                                    polygons(rectangle(baseVisible.boundingRect()), options_.flattenTolerance),
                                    polygons(hull(baseVisible, options_.flattenTolerance), options_.flattenTolerance)};
                                for (const auto &baseShape : baseShapes) {
                                    auto basePath = curves.restore(baseShape);
                                    if (std::abs(Area(Xor(baseShape, polygons(basePath, options_.flattenTolerance), FillRule::NonZero,
                                        kBooleanPrecision))) > options_.areaTolerance) basePath = fromPolygons(baseShape);
                                    const auto simplified = makeNode({basePath, donor->item.color, base}, "component-base",
                                        beforeCost - target->cost - upperCost);
                                    if (!simplified) continue;
                                    auto updated = additions;
                                    updated.prepend(simplified);
                                    record({removed, updated, "component-absorption-group"}, beforeCost);
                                }
                            }
                        }
                    }
                    std::stable_sort(choices.begin(), choices.end(), [](const auto &a, const auto &b) {
                        return std::tuple(-a.saving, a.added, a.dependencies, a.edit.added.front()->id)
                            < std::tuple(-b.saving, b.added, b.dependencies, b.edit.added.front()->id);
                    });
                    for (const auto &choice : choices) if (attempt(choice.edit, true, false)) break;
                    if (options_.progress && ++visited % 256 == 0) {
                        std::fprintf(stderr, "components: round %d, %d/%d proposals, %d paths\n", round + 1,
                            visited, int(requests.size()), int(state.scene.size()));
                        std::fflush(stderr);
                    }
                }
                flush();
            }
