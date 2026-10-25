ColorResult mergeColors(const QVector<Item> &baseline, const Options &options,
                        const ColorOptions &colors, const Renderer &renderer,
                        const std::vector<ColorRegion> &regions,
                        const std::vector<std::map<int, double>> &adjacency,
                        ColorResult result, const QImage &reference, Metrics before,
                        std::chrono::steady_clock::time_point started) {
    const CurveBank curves(baseline, options.flattenTolerance);
    const auto measure = [&](const QVector<Item> &items) {
        auto metrics = pathMetrics(objects(items), options.flattenTolerance);
        metrics.cost = geometryCost(metrics, options);
        metrics.costMeasured = true;
        return metrics;
    };
    const auto acceptable = [&](const QJsonObject &error) {
        return error.value("rmse").toDouble() <= colors.renderRmse
            && error.value("max_channel_error").toDouble() <= colors.renderMaxError
            && error.value("max_alpha_error").toDouble() <= options.renderMaxError
            && error.value("max_local_rmse").toDouble() <= colors.localRmse;
    };
    const auto rounded = [](const ColorVector &rgb) {
        return QColor(qRound(rgb[0]), qRound(rgb[1]), qRound(rgb[2]));
    };
    std::vector<std::vector<int>> sourceMembers(baseline.size());
    for (int owner = 0; owner < int(regions.size()); ++owner) sourceMembers[regions[owner].source].push_back(owner);
    QJsonArray attempts;
    for (int attempt = 0; attempt < 6; ++attempt) {
        const double tolerance = colors.tolerance / 100 * std::pow(0.75, attempt);
        const double threshold = tolerance * tolerance;
        const double retentionSquared = colors.contrastRetention * colors.contrastRetention;
        std::vector<int> membership(regions.size());
        int stamp = 0;
        std::map<QString, int> rejections;
        const auto fits = [&](const std::vector<int> &members, QColor fill, double drift,
                              const std::vector<ColorVector> &current, bool footprint = true) -> QString {
            const auto mean = perceptualColor(fill);
            ++stamp;
            for (const int owner : members) membership[owner] = stamp;
            for (const int owner : members) {
                const double coverage = footprint ? regions[owner].coverage : 1;
                if (colorDistance(regions[owner].mean, mean) * coverage * coverage > drift * drift + 1e-12) return "Original color drift";
                double rgbError = 0;
                const ColorVector rgb{double(fill.red()), double(fill.green()), double(fill.blue())};
                for (int channel = 0; channel < 3; ++channel)
                    rgbError += std::pow(rgb[channel] - regions[owner].rgb[channel], 2);
                if (rgbError * coverage * coverage > 3 * colors.localRmse * colors.localRmse + 1e-9) return "Original regional RGB error";
                for (const auto &[neighbor, length] : adjacency[owner]) {
                    const double contrast = colorDistance(regions[owner].mean, regions[neighbor].mean);
                    const double contactCoverage = footprint ? std::min(coverage, regions[neighbor].coverage) : 1;
                    if (contrast * contactCoverage * contactCoverage <= threshold) continue;
                    const auto &other = membership[neighbor] == stamp ? mean : current[neighbor];
                    if (colorDistance(mean, other) + 1e-12 < retentionSquared * contrast)
                        return "Original boundary contrast";
                }
            }
            return {};
        };
        const auto average = [&](const std::vector<int> &members) {
            ColorVector rgb{};
            double weight = 0;
            for (const int owner : members) {
                for (int channel = 0; channel < 3; ++channel) rgb[channel] += regions[owner].rgb[channel] * regions[owner].area;
                weight += regions[owner].area;
            }
            for (auto &channel : rgb) channel /= weight;
            return rounded(rgb);
        };
        struct PalettePlan {
            std::vector<QColor> fills;
            std::vector<int> labels;
        };
        const auto reuse = [&](std::vector<std::vector<int>> clusters, double drift,
                               const std::vector<QColor> &unchanged) {
            struct Entry { QColor fill; std::vector<int> members; };
            std::vector<Entry> entries;
            std::vector<ColorVector> current;
            for (const auto fill : unchanged) current.push_back(perceptualColor(fill));
            const auto mass = [&](const std::vector<int> &members) {
                double area = 0;
                for (const int owner : members) area += regions[owner].area;
                return area;
            };
            clusters.erase(std::remove_if(clusters.begin(), clusters.end(), [](const auto &members) { return members.empty(); }), clusters.end());
            std::stable_sort(clusters.begin(), clusters.end(), [&](const auto &a, const auto &b) { return mass(a) > mass(b); });
            for (const auto &cluster : clusters) {
                const auto fill = average(cluster);
                int nearest = -1;
                double distance = drift * drift;
                QColor best;
                for (int entry = 0; entry < int(entries.size()); ++entry) {
                    const double candidate = colorDistance(perceptualColor(fill), perceptualColor(entries[entry].fill));
                    if (candidate > distance + 1e-12) continue;
                    auto joined = entries[entry].members;
                    joined.insert(joined.end(), cluster.begin(), cluster.end());
                    const auto color = average(joined);
                    if (fits(joined, color, drift, current, false).isEmpty()) {
                        nearest = entry;
                        distance = candidate;
                        best = color;
                    }
                }
                if (nearest < 0) {
                    const auto chosen = fits(cluster, fill, drift, current, false).isEmpty() ? fill : unchanged[cluster.front()];
                    nearest = int(entries.size());
                    entries.push_back({chosen, cluster});
                } else {
                    auto &entry = entries[nearest];
                    entry.fill = best;
                    entry.members.insert(entry.members.end(), cluster.begin(), cluster.end());
                }
                for (const int owner : entries[nearest].members) current[owner] = perceptualColor(entries[nearest].fill);
            }
            PalettePlan plan;
            plan.labels.resize(regions.size(), -1);
            for (const auto &entry : entries) {
                const int label = int(plan.fills.size());
                plan.fills.push_back(entry.fill);
                for (const int owner : entry.members) plan.labels[owner] = label;
            }
            return plan;
        };
        std::vector<QColor> original;
        for (const auto &region : regions) original.push_back(baseline[region.source].color);
        const auto palette = reuse(sourceMembers, tolerance, original);
        auto paletteItems = baseline;
        for (int source = 0; source < baseline.size(); ++source)
            if (!sourceMembers[source].empty()) paletteItems[source].color = palette.fills[palette.labels[sourceMembers[source].front()]];

        struct State {
            ColorVector mean;
            ColorVector rgb;
            std::vector<int> members;
            double area = 0;
            int parent = 0;
            int version = 0;
        };
        auto neighbors = adjacency;
        std::vector<State> states;
        std::vector<ColorVector> current;
        std::vector<QColor> currentFills;
        for (int owner = 0; owner < int(regions.size()); ++owner) {
            const auto mean = perceptualColor(palette.fills[palette.labels[owner]]);
            states.push_back({mean, regions[owner].rgb, {owner}, regions[owner].area, owner});
            current.push_back(mean);
            currentFills.push_back(palette.fills[palette.labels[owner]]);
        }
        using Trial = std::tuple<double, int, int, int, int>;
        std::priority_queue<Trial, std::vector<Trial>, std::greater<>> pending;
        const auto offer = [&](int a, int b) {
            if (a > b) std::swap(a, b);
            const double effective = states[a].area * states[b].area / (states[a].area + states[b].area);
            const double support = colors.detailScale * neighbors[a].at(b) + colors.detailScale * colors.detailScale;
            const double score = colorDistance(states[a].mean, states[b].mean) * effective / (effective + support);
            pending.emplace(score, a, b, states[a].version, states[b].version);
        };
        for (int owner = 0; owner < int(regions.size()); ++owner)
            for (const auto &[neighbor, length] : neighbors[owner]) if (owner < neighbor) offer(owner, neighbor);
        int merges = 0;
        while (!pending.empty()) {
            const auto [score, a, b, versionA, versionB] = pending.top();
            pending.pop();
            if (states[a].parent != a || states[b].parent != b || states[a].version != versionA || states[b].version != versionB) continue;
            if (score > threshold) break;
            auto members = states[a].members;
            members.insert(members.end(), states[b].members.begin(), states[b].members.end());
            const double total = states[a].area + states[b].area;
            ColorVector rgb{};
            for (int channel = 0; channel < 3; ++channel)
                rgb[channel] = (states[a].rgb[channel] * states[a].area + states[b].rgb[channel] * states[b].area) / total;
            const QColor aFill = currentFills[a], bFill = currentFills[b];
            const auto rgbError = [&](QColor fill) {
                double error = 0;
                const ColorVector value{double(fill.red()), double(fill.green()), double(fill.blue())};
                for (int channel = 0; channel < 3; ++channel) error += std::pow(value[channel] - rgb[channel], 2);
                return error;
            };
            const auto fill = rgbError(aFill) <= rgbError(bFill) ? aFill : bFill;
            const auto rejection = fits(members, fill, tolerance, current);
            if (!rejection.isEmpty()) { ++rejections[rejection]; continue; }
            states[a].mean = perceptualColor(fill);
            states[a].rgb = rgb;
            states[a].members = std::move(members);
            states[a].area = total;
            ++states[a].version;
            states[b].parent = a;
            states[b].members.clear();
            ++merges;
            for (const int owner : states[a].members) {
                current[owner] = states[a].mean;
                currentFills[owner] = fill;
            }
            neighbors[a].erase(b);
            for (const auto &[neighbor, length] : neighbors[b]) {
                if (neighbor == a) continue;
                neighbors[neighbor].erase(b);
                neighbors[neighbor][a] += length;
                neighbors[a][neighbor] += length;
            }
            neighbors[b].clear();
            for (const auto &[neighbor, length] : neighbors[a]) offer(a, neighbor);
        }
        PalettePlan regional;
        std::map<QRgb, int> regionalEntries;
        for (const auto fill : currentFills) {
            const auto [entry, inserted] = regionalEntries.emplace(fill.rgb(), int(regional.fills.size()));
            if (inserted) regional.fills.push_back(fill);
            regional.labels.push_back(entry->second);
        }
        QVector<Item> regionalItems;
        for (int source = 0; source < baseline.size(); ++source) {
            std::map<QRgb, PathsD> groups;
            std::map<QRgb, double> weights;
            for (const int owner : sourceMembers[source]) {
                const auto fill = regional.fills[regional.labels[owner]].rgb();
                groups[fill].insert(groups[fill].end(), regions[owner].shape.begin(), regions[owner].shape.end());
                weights[fill] += regions[owner].area;
            }
            if (groups.empty()) continue;
            const auto primary = std::max_element(weights.begin(), weights.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;
            regionalItems.push_back({baseline[source].path, QColor(primary), {}});
            if (groups.size() == 1) continue;
            const auto sourceShape = polygons(baseline[source].path, options.flattenTolerance);
            for (const auto &[fill, shape] : groups) {
                if (fill == primary) continue;
                const auto required = Union(shape, FillRule::NonZero, kBooleanPrecision);
                PathsD blocked;
                for (const auto &[other, region] : groups)
                    if (other != fill) blocked.insert(blocked.end(), region.begin(), region.end());
                const auto allowed = Difference(sourceShape, Union(blocked, FillRule::NonZero, kBooleanPrecision), FillRule::NonZero, kBooleanPrecision);
                QPainterPath best = curves.restore(required);
                double cost = geometryCost(pathMetrics({{best, QColor(fill)}}, options.flattenTolerance), options);
                const auto consider = [&](const PathsD &proposal) {
                    const auto clipped = Intersect(proposal, allowed, FillRule::NonZero, kBooleanPrecision);
                    const auto path = curves.restore(clipped);
                    const double candidate = geometryCost(pathMetrics({{path, QColor(fill)}}, options.flattenTolerance), options);
                    if (candidate >= cost || path.isEmpty()) return;
                    if (std::abs(Area(Difference(required, polygons(path, options.flattenTolerance), FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance) return;
                    best = path;
                    cost = candidate;
                };
                consider(allowed);
                consider(rawPolygons(rectangle(best.boundingRect()), options.flattenTolerance));
                PathsD outer;
                for (const auto &loop : required) if (Area(loop) > 0) outer.push_back(loop);
                consider(outer);
                if (colors.boundaryTolerance > 0)
                    consider(Union(SimplifyPaths(allowed, colors.boundaryTolerance), required, FillRule::NonZero, kBooleanPrecision));
                regionalItems.push_back({best, QColor(fill), {}});
            }
        }
        const auto paletteMetrics = measure(paletteItems), regionalMetrics = measure(regionalItems);
        const auto colorStats = [&](const PalettePlan &plan, bool footprint) {
            double drift = 0, weightedDrift = 0, retention = 1;
            int protectedEdges = 0;
            for (int owner = 0; owner < int(regions.size()); ++owner) {
                const auto fill = perceptualColor(plan.fills[plan.labels[owner]]);
                drift = std::max(drift, colorDistance(regions[owner].mean, fill));
                const double coverage = footprint ? regions[owner].coverage : 1;
                weightedDrift = std::max(weightedDrift, colorDistance(regions[owner].mean, fill) * coverage * coverage);
                for (const auto &[neighbor, length] : adjacency[owner]) {
                    if (owner >= neighbor) continue;
                    const double original = colorDistance(regions[owner].mean, regions[neighbor].mean);
                    const double contactCoverage = footprint ? std::min(coverage, regions[neighbor].coverage) : 1;
                    if (original * contactCoverage * contactCoverage <= threshold) continue;
                    ++protectedEdges;
                    const double current = colorDistance(fill, perceptualColor(plan.fills[plan.labels[neighbor]]));
                    retention = std::min(retention, std::sqrt(current / original));
                }
            }
            return QJsonObject{{"max_original_color_drift", std::sqrt(drift) * 100},
                {"max_coverage_weighted_color_drift", std::sqrt(weightedDrift) * 100}, {"coverage_weighted", footprint},
                {"minimum_significant_boundary_contrast_retention", retention}, {"protected_edges", protectedEdges}};
        };
        const auto paletteStats = colorStats(palette, false), regionalStats = colorStats(regional, true);
        const auto paletteError = appearanceError(reference, renderer(objects(paletteItems)), colors.localWindow);
        const auto regionalError = appearanceError(reference, renderer(objects(regionalItems)), colors.localWindow);
        const double silhouette = silhouetteDifference(objects(baseline), objects(regionalItems), options.flattenTolerance);
        const auto statsPassed = [&](const QJsonObject &stats, double drift) {
            return stats.value("max_coverage_weighted_color_drift").toDouble() <= drift * 100 + 1e-8
                && stats.value("minimum_significant_boundary_contrast_retention").toDouble() + 1e-8 >= colors.contrastRetention;
        };
        const bool palettePassed = acceptable(paletteError) && statsPassed(paletteStats, tolerance);
        const bool regionalPassed = acceptable(regionalError) && statsPassed(regionalStats, tolerance)
            && silhouette <= options.silhouetteTolerance;
        const bool regionalSimpler = regionalMetrics.cost < before.cost - 1e-9 || regionalMetrics.fills < before.fills;
        const bool paletteSimpler = paletteMetrics.fills < before.fills;
        QJsonObject regionalReconstruction;
        QVector<Item> regionalScene = regionalItems;
        if (colors.reconstruct && regionalSimpler && statsPassed(regionalStats, tolerance)) {
            std::vector<QColor> ownerFills;
            for (int owner = 0; owner < int(regions.size()); ++owner) ownerFills.push_back(regional.fills[regional.labels[owner]]);
            regionalScene = reconstructColors(baseline, regionalItems, paletteItems, regions, ownerFills,
                options, colors, renderer, reference, regionalReconstruction,
                &result.topologyCandidate, &result.reconstructionCandidate);
        }
        const bool regionalGeometryPassed = regionalMetrics.cost <= paletteMetrics.cost + 1e-9
            && regionalMetrics.holes <= paletteMetrics.holes;
        const bool useRegional = regionalSimpler && ((regionalPassed && regionalGeometryPassed)
            || regionalReconstruction.value("accepted").toBool());
        const bool valid = useRegional || (palettePassed && paletteSimpler);
        QJsonObject rejected;
        for (const auto &[reason, count] : rejections) rejected.insert(reason, count);
        attempts.append(QJsonObject{{"color_tolerance", tolerance * 100}, {"merges", merges},
            {"palette_only", QJsonObject{{"metrics", paletteMetrics.json()}, {"image_error", paletteError}, {"color_guards", paletteStats}, {"appearance_passed", palettePassed}}},
            {"regional", QJsonObject{{"metrics", regionalMetrics.json()}, {"image_error", regionalError}, {"color_guards", regionalStats}, {"appearance_passed", regionalPassed}}},
            {"regional_reconstruction", regionalReconstruction},
            {"regional_source_geometry_passed", regionalGeometryPassed},
            {"discarded_low_impact_components", 0}, {"rejected", rejected},
            {"silhouette_difference_area", silhouette}, {"accepted", valid}});
        if (options.progress) {
            const auto error = useRegional ? appearanceError(reference, renderer(objects(regionalScene)), colors.localWindow) : paletteError;
            std::fprintf(stderr, "colors: tolerance %.3f, %d bounded merges, %d/%d palette/regional colors, %s RMSE %.3f, worst local %.3f%s\n",
                tolerance * 100, merges, paletteMetrics.fills, regionalMetrics.fills,
                useRegional ? "regional" : "palette", error.value("rmse").toDouble(), error.value("max_local_rmse").toDouble(), valid ? " accepted" : " rejected");
        }
        if (!valid) continue;
        result.paletteItems = paletteItems;
        result.regionalItems = regionalItems;
        QJsonObject reconstruction = regionalReconstruction;
        result.items = regionalScene;
        if (!useRegional) {
            std::vector<QColor> ownerFills;
            for (int owner = 0; owner < int(regions.size()); ++owner) ownerFills.push_back(palette.fills[palette.labels[owner]]);
            result.items = colors.reconstruct ? reconstructColors(baseline, paletteItems, paletteItems, regions, ownerFills,
                options, colors, renderer, reference, reconstruction,
                &result.topologyCandidate, &result.reconstructionCandidate) : paletteItems;
        }
        result.report.insert("reconstruction", reconstruction);
        result.changed = true;
        result.report.insert("status", "accepted");
        result.report.insert("selected_stage", QString(useRegional ? "regional-source-support" : "palette-only")
            + (reconstruction.value("accepted").toBool() ? "+occlusion-reconstruction" : ""));
        result.report.insert("after", measure(result.items).json());
        result.report.insert("palette_only", QJsonObject{{"metrics", measure(result.paletteItems).json()},
            {"image_error", paletteError}, {"accepted", palettePassed}});
        result.report.insert("regional", QJsonObject{{"metrics", measure(result.regionalItems).json()},
            {"image_error", regionalError}, {"geometry_passed", regionalGeometryPassed},
            {"accepted", useRegional && regionalPassed && regionalGeometryPassed}, {"colors_selected", useRegional}});
        result.report.insert("image_error", appearanceError(reference, renderer(objects(result.items)), colors.localWindow));
        result.report.insert("color_guards", useRegional ? regionalStats : paletteStats);
        result.report.insert("silhouette_difference_area", silhouetteDifference(objects(baseline), objects(result.items), options.flattenTolerance));
        result.report.insert("discarded_low_impact_components", 0);
        result.report.insert("ownership_assignment_stage", "Bounded regional colors on original source supports; final ownership rebuilt from emitted paint stack");
        QJsonArray assignment;
        for (int owner = 0; owner < int(regions.size()); ++owner)
            assignment.append(QJsonObject{{"source", regions[owner].source}, {"visible_area", regions[owner].area},
                {"native_coverage_bound", regions[owner].coverage},
                {"fill", useRegional ? regional.fills[regional.labels[owner]].name() : palette.fills[palette.labels[owner]].name()},
                {"merged_region", useRegional ? regional.labels[owner] : palette.labels[owner]}});
        result.report.insert("ownership_assignment", assignment);
        break;
    }
    if (!result.changed) {
        result.report.insert("status", "rejected");
        result.report.insert("after", before.json());
        result.report.insert("image_error", appearanceError(reference, reference, colors.localWindow));
    }
    result.report.insert("attempts", attempts);
    result.report.insert("runtime_seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return result;
}
