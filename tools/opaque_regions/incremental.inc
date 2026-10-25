Result Optimizer::runIncremental() {
    struct Node {
        Item item;
        PathsD shape;
        std::vector<int> above;
        QString kind;
        int id = 0;
        int holes = 0;
        double cost = 0.0;
    };
    using Pointer = std::shared_ptr<Node>;
    using Scene = QVector<Pointer>;
    struct Edit {
        std::set<int> removed;
        Scene added;
        QString kind;
    };
    struct State {
        Scene scene;
        QImage image;
        Graph graph;
        QVector<int> ownerPositions;
        double squared = 0.0;
        double sourceSquared = 0.0;
        double cost = 0.0;
        int accepted = 0;
        int diagnostics = 0;
        int benchmarks = 0;
    };

    Result result;
    result.items = baseline_;
    if (absorptionEnabled_) {
        result.ownershipBefore = absorptionOwnership(baseline_, ownership_, options_.flattenTolerance);
        result.ownershipAfter = result.ownershipBefore;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto reference = renderer_(objects(baseline_));
    const auto appearanceValid = [&](const QImage &image, QRect rectangle) {
        if (appearanceReference_.isNull()) return true;
        const auto error = appearanceError(appearanceReference_.copy(rectangle), image, appearanceLimits_.localWindow);
        const bool global = !absorptionEnabled_ || rectangle == appearanceReference_.rect();
        return (!global || error.value("rmse").toDouble() <= appearanceLimits_.renderRmse)
            && error.value("max_channel_error").toDouble() <= appearanceLimits_.renderMaxError
            && (absorptionEnabled_ || error.value("max_local_rmse").toDouble() <= appearanceLimits_.localRmse)
            && error.value("max_alpha_error").toDouble() <= options_.renderMaxError;
    };
    if (!appearanceValid(reference, reference.rect())) {
        result.error = "Baseline appearance exceeds original-input limits";
        return result;
    }
    const double samples = reference.width() * double(reference.height()) * 3;
    if (absorptionEnabled_ && absorptionReference_.size() != reference.size()) {
        result.error = "Color absorption target must match the native guide dimensions";
        return result;
    }
    const double sourceBaselineRmse = absorptionEnabled_ ? imageError(absorptionReference_, reference).value("rmse").toDouble() : 0;
    const double sourceMaximumSquared = std::pow(sourceBaselineRmse + absorptionSourceGrowth_, 2) * samples;
    const auto domain = polygons(domain_, options_.flattenTolerance);
    const auto domainParts = components(domain);
    const CurveBank curves(baseline_, options_.flattenTolerance);
    const OwnerIndex spatial(ownership_, domain_.boundingRect());
    std::vector<PathsD> required;
    std::vector<PolygonIndex> regions;
    std::map<std::set<int>, PathsD> territoryCache;
    std::map<QString, int> rejected;
    int serial = 0, generated = 0, audits = 0, repairedBatches = 0;
    const auto territory = [&](const std::set<int> &owners) -> const PathsD & {
        const auto found = territoryCache.find(owners);
        if (found != territoryCache.end()) return found->second;
        PathsD combined;
        for (const int owner : owners) combined.insert(combined.end(), required[owner].begin(), required[owner].end());
        return territoryCache.emplace(owners, Union(combined, FillRule::NonZero, kBooleanPrecision)).first->second;
    };
    for (const auto &path : ownership_) {
        required.push_back(polygons(path, options_.flattenTolerance));
        regions.emplace_back(required.back());
    }
    const auto items = [](const Scene &scene) {
        QVector<Item> output;
        for (const auto &node : scene) output.push_back(node->item);
        return output;
    };
    const auto makeNode = [&](Item item, const QString &kind, double ceiling) -> Pointer {
        ++generated;
        if (item.path.isEmpty() || item.owners.empty() || item.color.alpha() != 255) return {};
        for (int index = 0; index < item.path.elementCount(); ++index) {
            const auto element = item.path.elementAt(index);
            if (!std::isfinite(element.x) || !std::isfinite(element.y)) return {};
        }
        for (const int owner : item.owners)
            if (owner < 0 || owner >= ownership_.size()
                || (!absorptionEnabled_ && item.color != baseline_[ownerSources_[owner]].color)) return {};
        Metrics metrics;
        metrics.paths = 1;
        const auto loops = spans(item.path);
        metrics.subpaths = loops.size();
        double totalArea = 0, maximumArea = 0;
        for (const auto &loop : loops) {
            double loopArea = 0;
            for (const auto &span : loop) {
                metrics.bezierSegments += span.curved;
                metrics.lineSegments += !span.curved;
                loopArea += span.signedArea();
            }
            totalArea += std::abs(loopArea);
            maximumArea = std::max(maximumArea, std::abs(loopArea));
        }
        metrics.components = 1;
        if (geometryCost(metrics, options_) >= ceiling - 1e-9) return {};
        if (!metrics.bezierSegments
            && 2 * maximumArea - totalArea + options_.areaTolerance < std::abs(Area(territory(item.owners)))) {
            ++rejected["Insufficient support area"]; return {};
        }
        const auto shape = polygons(item.path, options_.flattenTolerance);
        if (shape.empty()) return {};
        metrics.components = metrics.holes = 0;
        for (const auto &loop : shape) {
            metrics.holes += Area(loop) < 0;
            metrics.components += Area(loop) > 0;
        }
        const double cost = geometryCost(metrics, options_);
        if (cost >= ceiling - 1e-9) return {};
        if (metrics.components != 1) { ++rejected["Disconnected Bucket target"]; return {}; }
        const PolygonIndex support(shape);
        for (const int owner : item.owners) {
            if (support.differenceArea(required[owner], ownership_[owner].boundingRect()) > options_.areaTolerance) {
                ++rejected["Required territory removed"]; return {};
            }
        }
        if (std::abs(Area(Difference(shape, domain, FillRule::NonZero, kBooleanPrecision))) > options_.silhouetteTolerance) {
            ++rejected["Outside silhouette"]; return {};
        }
        const auto penError = penTargetError(item.path, options_);
        if (!penError.isEmpty()) { ++rejected["Invalid Bucket target: " + penError]; return {}; }
        auto node = std::make_shared<Node>();
        node->id = serial++;
        node->item = std::move(item);
        node->shape = shape;
        node->kind = kind;
        node->cost = cost;
        node->holes = metrics.holes;
        for (const int owner : spatial.query(node->item.path.boundingRect())) {
            if (node->item.owners.contains(owner)) continue;
            if (!absorptionEnabled_ && options_.colorOwnership && node->item.color == baseline_[ownerSources_[owner]].color) continue;
            if (support.intersectArea(required[owner], ownership_[owner].boundingRect()) > options_.areaTolerance)
                node->above.push_back(owner);
        }
        return node;
    };
    const auto graphFor = [&](const Scene &scene) {
        QVector<int> ownerNode(ownership_.size(), -1);
        for (int index = 0; index < scene.size(); ++index) for (const int owner : scene[index]->item.owners) {
            if (ownerNode[owner] >= 0) return Graph{{}, {}, "Duplicated owner"};
            ownerNode[owner] = index;
        }
        for (int owner = 0; owner < ownerNode.size(); ++owner)
            if (ownerNode[owner] < 0 && std::abs(Area(required[owner])) > options_.areaTolerance)
                return Graph{{}, {}, "Missing required owner"};
        std::set<std::pair<int, int>> edges;
        for (int index = 0; index < scene.size(); ++index) for (const int owner : scene[index]->above)
            if (ownerNode[owner] >= 0 && ownerNode[owner] != index) edges.insert({index, ownerNode[owner]});
        return topologicalOrder(scene.size(), edges);
    };
    const auto sceneMetrics = [&](const Scene &scene, const Graph &graph) {
        auto metrics = pathMetrics(objects(items(scene)), options_.flattenTolerance);
        metrics.orderingEdges = graph.edges.size();
        metrics.orderingMeasured = metrics.costMeasured = true;
        QVector<QVector<int>> next(scene.size());
        QVector<int> depths(scene.size(), 1);
        for (const auto &[from, to] : graph.edges) next[from].push_back(to);
        for (const int index : graph.order) {
            metrics.orderingDepth = std::max(metrics.orderingDepth, depths[index]);
            for (const int to : next[index]) depths[to] = std::max(depths[to], depths[index] + 1);
        }
        metrics.cost = geometryCost(metrics, options_);
        return metrics;
    };
    State state;
    state.image = reference;
    state.sourceSquared = sourceBaselineRmse * sourceBaselineRmse * samples;
    for (const auto &item : baseline_) {
        auto node = makeNode(item, "source", std::numeric_limits<double>::infinity());
        if (!node) {
            result.error = QString("Baseline component ownership failed validation at item %1").arg(state.scene.size());
            QJsonObject causes;
            for (const auto &[cause, count] : rejected) causes.insert(cause, count);
            result.diagnostics.append(QJsonObject{{"baseline_rejections", causes}});
            return result;
        }
        state.scene.push_back(node);
    }
    state.graph = graphFor(state.scene);
    if (!state.graph.error.isEmpty()) { result.error = state.graph.error; return result; }
    result.before = sceneMetrics(state.scene, state.graph);
    result.after = result.before;
    if (options_.maximumPasses == 0) { result.graph = state.graph; return result; }
    state.ownerPositions.fill(-1, ownership_.size());
    for (int index = 0; index < state.scene.size(); ++index)
        for (const int owner : state.scene[index]->item.owners) state.ownerPositions[owner] = index;
    state.cost = result.before.cost;
    const State originalState = state;
    State checkpoint = state;
    std::vector<Edit> pending;
    std::set<int> cycleNodes;
    QVector<quint64> dirtyTiles(4096, 0);
    quint64 dirtySerial = 0;
    std::map<int, std::pair<int, quint64>> failedLocal;
    const auto visitTiles = [&](QRect rectangle, const std::function<void(quint64 &)> &visit) {
        if (rectangle.isEmpty()) return;
        const int left = std::clamp(rectangle.left() * 64 / reference.width(), 0, 63);
        const int right = std::clamp(rectangle.right() * 64 / reference.width(), 0, 63);
        const int top = std::clamp(rectangle.top() * 64 / reference.height(), 0, 63);
        const int bottom = std::clamp(rectangle.bottom() * 64 / reference.height(), 0, 63);
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) visit(dirtyTiles[y * 64 + x]);
    };
    const auto audit = [&]() -> QString {
        ++audits;
        PathsD combined;
        for (const auto &node : state.scene) combined.insert(combined.end(), node->shape.begin(), node->shape.end());
        const auto merged = Union(combined, FillRule::NonZero, kBooleanPrecision);
        const double silhouette = std::abs(Area(Xor(domain, merged, FillRule::NonZero, kBooleanPrecision)));
        if (silhouette > options_.silhouetteTolerance) return "Full silhouette budget exceeded";
        const auto actual = renderer_(objects(items(state.scene)));
        const auto error = imageError(reference, actual);
        if (!withinBudget(error, options_)) return "Full rendering budget exceeded";
        if (!appearanceValid(actual, actual.rect())) return "Original-input appearance budget exceeded";
        if (absorptionEnabled_) {
            const auto source = imageError(absorptionReference_, actual);
            const double squared = std::pow(source.value("rmse").toDouble(), 2) * samples;
            if (squared > sourceMaximumSquared + 1e-6) return "Source reconstruction RMSE growth exceeded";
            const auto detail = absorptionDetailError(absorptionReference_, reference, actual, absorptionLimits_, absorptionEdgeContrast_);
            if (!detail.value("passed").toBool()) return "Source edge or local added-error budget exceeded";
            state.sourceSquared = squared;
        }
        state.image = actual;
        state.squared = std::pow(error.value("rmse").toDouble(), 2) * samples;
        return {};
    };
    std::function<void()> flush;
    std::function<bool(const Edit &, bool, bool)> attempt;
    attempt = [&](const Edit &edit, bool record, bool fullCheck) {
        cycleNodes.clear();
        Scene trial;
        QRectF bounds;
        std::map<int, int> oldPositions;
        int removed = 0;
        for (int index = 0; index < state.scene.size(); ++index) {
            const auto &node = state.scene[index];
            oldPositions[node->id] = index;
            if (edit.removed.contains(node->id)) {
                if (removed++ == 0) for (const auto &replacement : edit.added) trial.push_back(replacement);
                bounds = bounds.united(node->item.path.boundingRect());
            } else trial.push_back(node);
        }
        if (removed != edit.removed.size()) return false;
        if (edit.kind.contains("group")) {
            int beforeHoles = 0, afterHoles = 0;
            for (const auto &node : state.scene) if (edit.removed.contains(node->id)) beforeHoles += node->holes;
            for (const auto &node : edit.added) afterHoles += node->holes;
            if (afterHoles > beforeHoles) { ++rejected["Grouping introduced holes"]; return false; }
        }
        for (const auto &node : edit.added) bounds = bounds.united(node->item.path.boundingRect());
        bool stableLocal = edit.removed.size() == 1 && edit.added.size() == 1 && options_.orderingWeight == 0;
        int localPosition = -1;
        if (stableLocal) {
            localPosition = oldPositions[*edit.removed.begin()];
            stableLocal = state.scene[localPosition]->item.owners == edit.added.front()->item.owners;
        }
        if (stableLocal) {
            QVector<bool> forward(state.scene.size(), false), backward(state.scene.size(), false);
            for (const int owner : edit.added.front()->above) {
                const int target = state.ownerPositions[owner];
                if (target >= 0 && target < localPosition) { forward[target] = true; stableLocal = false; }
            }
            if (!stableLocal) {
                backward[localPosition] = true;
                for (const auto &[from, to] : state.graph.edges)
                    if (from < localPosition && to <= localPosition && forward[from]) forward[to] = true;
                for (auto edge = state.graph.edges.rbegin(); edge != state.graph.edges.rend(); ++edge)
                    if (edge->first < localPosition && edge->second <= localPosition && backward[edge->second]) backward[edge->first] = true;
                for (int index = 0; index < localPosition; ++index)
                    if (forward[index] && backward[index]) cycleNodes.insert(state.scene[index]->id);
                if (!cycleNodes.empty()) { ++rejected["Dependency cycle"]; return false; }
            }
        }
        Graph graph;
        if (!stableLocal) graph = graphFor(trial);
        if (!graph.error.isEmpty()) {
            ++rejected[graph.error];
            if (graph.error == "Dependency cycle") {
                for (const auto &added : edit.added) {
                    int start = -1;
                    for (int index = 0; index < trial.size(); ++index) if (trial[index]->id == added->id) start = index;
                    std::set<int> forward{start}, backward{start};
                    for (bool changed = true; changed;) {
                        changed = false;
                        for (const auto &[from, to] : graph.edges) {
                            if (forward.contains(from)) changed |= forward.insert(to).second;
                            if (backward.contains(to)) changed |= backward.insert(from).second;
                        }
                    }
                    for (const int index : forward)
                        if (backward.contains(index) && !edit.added.contains(trial[index])) cycleNodes.insert(trial[index]->id);
                }
            }
            return false;
        }
        double cost = options_.orderingWeight * graph.edges.size();
        for (const auto &node : trial) cost += node->cost;
        if (cost >= state.cost - 1e-9) { ++rejected["No combined cost reduction"]; return false; }
        Scene ordered;
        if (stableLocal) ordered = trial;
        else for (const int index : graph.order) ordered.push_back(trial[index]);
        QVector<Pointer> common;
        for (const auto &node : ordered) if (oldPositions.contains(node->id)) common.push_back(node);
        QVector<int> suffix(common.size() + 1, std::numeric_limits<int>::max());
        for (int index = common.size() - 1; index >= 0; --index)
            suffix[index] = std::min(suffix[index + 1], oldPositions[common[index]->id]);
        int maximum = -1;
        for (int index = 0; index < common.size(); ++index) {
            const int position = oldPositions[common[index]->id];
            if (position < maximum || position > suffix[index + 1]) bounds = bounds.united(common[index]->item.path.boundingRect());
            maximum = std::max(maximum, position);
        }
        const int margin = appearanceReference_.isNull() ? 2 : appearanceLimits_.localWindow + 1;
        QRect rectangle = bounds.adjusted(-margin, -margin, margin, margin).toAlignedRect().intersected(reference.rect());
        quint64 stamp = 0;
        if (stableLocal) {
            visitTiles(rectangle, [&](quint64 &value) { stamp = std::max(stamp, value); });
            const auto found = failedLocal.find(edit.added.front()->id);
            if (found != failedLocal.end() && found->second == std::pair(state.scene[localPosition]->id, stamp)) {
                ++rejected["Cached local rendering maximum exceeded"]; return false;
            }
        }
        QImage crop;
        double squared = state.squared;
        double sourceSquared = state.sourceSquared;
        if (!rectangle.isEmpty()) {
            crop = renderCrop(objects(items(ordered)), rectangle);
            if (absorptionEnabled_) {
                auto previous = state.image.copy(rectangle);
                if (previous.format() != crop.format()) previous = previous.convertToFormat(crop.format());
                int left = crop.width(), top = crop.height(), right = -1, bottom = -1;
                for (int y = 0; y < crop.height(); ++y) {
                    const auto *a = reinterpret_cast<const QRgb *>(previous.constScanLine(y));
                    const auto *b = reinterpret_cast<const QRgb *>(crop.constScanLine(y));
                    for (int x = 0; x < crop.width(); ++x) if (a[x] != b[x]) {
                        left = std::min(left, x); right = std::max(right, x);
                        top = std::min(top, y); bottom = std::max(bottom, y);
                    }
                }
                if (right < left) { rectangle = {}; crop = {}; }
                else {
                    const auto changed = QRect(QPoint(left, top), QPoint(right, bottom))
                        .translated(rectangle.topLeft()).adjusted(-margin, -margin, margin, margin).intersected(rectangle);
                    crop = crop.copy(QRect(changed.topLeft() - rectangle.topLeft(), changed.size()));
                    rectangle = changed;
                }
            }
        }
        if (!rectangle.isEmpty()) {
            const auto target = reference.copy(rectangle);
            const auto error = imageError(target, crop);
            if (error.value("max_channel_error").toDouble() > options_.renderMaxError
                || error.value("max_alpha_error").toDouble() > options_.renderMaxError) {
                if (stableLocal) failedLocal[edit.added.front()->id] = {state.scene[localPosition]->id, stamp};
                ++rejected["Local rendering maximum exceeded"]; return false;
            }
            const auto previous = imageError(target, state.image.copy(rectangle));
            squared += (std::pow(error.value("rmse").toDouble(), 2) - std::pow(previous.value("rmse").toDouble(), 2))
                * rectangle.width() * double(rectangle.height()) * 3;
            squared = std::max(0.0, squared);
            if (squared > options_.renderRmse * options_.renderRmse * samples + 1e-9) {
                ++rejected["Cumulative rendering RMSE exceeded"]; return false;
            }
            if (!appearanceValid(crop, rectangle)) { ++rejected["Original-input local appearance exceeded"]; return false; }
            if (absorptionEnabled_) {
                const auto targetSource = absorptionReference_.copy(rectangle);
                const auto sourceError = imageError(targetSource, crop);
                const auto previousSource = imageError(targetSource, state.image.copy(rectangle));
                sourceSquared += (std::pow(sourceError.value("rmse").toDouble(), 2)
                    - std::pow(previousSource.value("rmse").toDouble(), 2)) * rectangle.width() * double(rectangle.height()) * 3;
                sourceSquared = std::max(0.0, sourceSquared);
                if (sourceSquared > sourceMaximumSquared + 1e-6) { ++rejected["Source reconstruction RMSE growth exceeded"]; return false; }
                if (absorptionSourceGrowth_ == 0
                    && sourceSquared > state.sourceSquared + std::max(1e-6, state.sourceSquared * 1e-12)) {
                    ++rejected["Source reconstruction would worsen after an earlier improvement"]; return false;
                }
                const auto detail = absorptionDetailError(targetSource, reference.copy(rectangle), crop, absorptionLimits_, absorptionEdgeContrast_);
                if (!detail.value("passed").toBool()) { ++rejected["Source edge or local added-error budget exceeded"]; return false; }
            }
        }
        const State previous = state;
        state.scene = ordered;
        if (stableLocal) {
            auto edge = state.graph.edges.lower_bound({localPosition, 0});
            while (edge != state.graph.edges.end() && edge->first == localPosition) edge = state.graph.edges.erase(edge);
            for (const int owner : edit.added.front()->above) {
                const int target = state.ownerPositions[owner];
                if (target >= 0 && target != localPosition) state.graph.edges.insert({localPosition, target});
            }
        } else {
            state.graph = graphFor(ordered);
            state.ownerPositions.fill(-1, ownership_.size());
            for (int index = 0; index < ordered.size(); ++index)
                for (const int owner : ordered[index]->item.owners) state.ownerPositions[owner] = index;
        }
        state.cost = cost;
        state.squared = squared;
        state.sourceSquared = sourceSquared;
        if (!rectangle.isEmpty()) {
            QPainter painter(&state.image);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(rectangle.topLeft(), crop);
        }
        if (fullCheck) {
            const QString error = audit();
            if (!error.isEmpty()) { ++rejected[error]; state = previous; return false; }
        }
        ++dirtySerial;
        visitTiles(rectangle, [&](quint64 &value) { value = dirtySerial; });
        QJsonArray ownerIds, sources, replaced, removedFills, addedFills;
        std::set<int> sourceIds;
        for (const auto &node : edit.added) for (const int owner : node->item.owners) {
            ownerIds.append(owner); sourceIds.insert(ownerSources_[owner]);
        }
        for (const auto &node : previous.scene) if (edit.removed.contains(node->id)) {
            replaced.append(node->id);
            removedFills.append(node->item.color.name());
            for (const int owner : node->item.owners) sourceIds.insert(ownerSources_[owner]);
        }
        for (const int source : sourceIds) sources.append(source);
        for (const auto &node : edit.added) addedFills.append(node->item.color.name());
        result.diagnostics.append(QJsonObject{{"kind", edit.kind}, {"owners", ownerIds}, {"sources", sources},
            {"removed_fills", removedFills}, {"added_fills", addedFills},
            {"removed_nodes", replaced}, {"paths_before", previous.scene.size()}, {"paths_after", ordered.size()},
            {"cost_before", previous.cost}, {"cost_after", cost}, {"render_rectangle", QJsonArray{rectangle.x(), rectangle.y(), rectangle.width(), rectangle.height()}}});
        if (absorptionEnabled_) {
            auto entry = result.diagnostics.last().toObject();
            entry.insert("source_rmse_before", std::sqrt(previous.sourceSquared / samples));
            entry.insert("source_rmse_after", std::sqrt(state.sourceSquared / samples));
            result.diagnostics.replace(result.diagnostics.size() - 1, entry);
        }
        if (!edit.added.isEmpty() && edit.removed.size() <= 8 && edit.added.size() <= 16) {
            Result::Benchmark sample;
            sample.sources = sources;
            sample.saving = previous.cost - cost;
            for (const auto &node : previous.scene)
                if (edit.removed.contains(node->id)) sample.before.push_back({node->item.path, node->item.color});
            Scene paintOrder;
            for (const auto &node : ordered) if (edit.added.contains(node)) paintOrder.push_back(node);
            sample.after = objects(items(paintOrder));
            result.benchmarks.push_back(sample);
        }
        ++state.accepted;
        state.diagnostics = result.diagnostics.size();
        state.benchmarks = result.benchmarks.size();
        if (record) {
            pending.push_back(edit);
            if (pending.size() >= size_t(options_.checkpointEdits)) flush();
        }
        return true;
    };
    flush = [&]() {
        if (pending.empty()) return;
        const auto error = audit();
        if (!error.isEmpty()) {
            ++repairedBatches;
            ++rejected[error];
            const auto replay = pending;
            state = checkpoint;
            dirtyTiles.fill(++dirtySerial);
            while (result.diagnostics.size() > checkpoint.diagnostics) result.diagnostics.removeLast();
            while (result.benchmarks.size() > checkpoint.benchmarks) result.benchmarks.removeLast();
            for (const auto &edit : replay) attempt(edit, false, true);
        }
        pending.clear();
        checkpoint = state;
        if (options_.progress) {
            std::fprintf(stderr, "incremental: %d edits, %d paths, cost %.0f, %d proposals, %.1f seconds\n",
                state.accepted, int(state.scene.size()), state.cost, generated,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
            std::fflush(stderr);
        }
    };
    std::map<int, Scene> simpleCache;
    const auto variants = [&](const Item &item, double ceiling, const QString &prefix) {
        Scene output;
        std::set<QByteArray> seen;
        const auto add = [&](const QPainterPath &path, const QString &kind) {
            QByteArray key;
            QDataStream stream(&key, QIODevice::WriteOnly);
            stream << path;
            if (!seen.insert(key).second) return;
            if (auto node = makeNode({path, item.color, item.owners}, prefix + kind, ceiling)) output.push_back(node);
        };
        add(item.path, "source");
        const auto &visible = territory(item.owners);
        const auto visiblePath = curves.restore(visible);
        add(visiblePath, "visible");
        add(rectangle(visiblePath.boundingRect()), "rectangle");
        add(hull(visiblePath, options_.flattenTolerance), "hull");
        const auto addClipped = [&](const QPainterPath &path, const QString &kind) {
            const auto shape = Intersect(polygons(path, options_.flattenTolerance), domain,
                FillRule::NonZero, kBooleanPrecision);
            auto restored = curves.restore(shape);
            if (std::abs(Area(Xor(shape, polygons(restored, options_.flattenTolerance),
                FillRule::NonZero, kBooleanPrecision))) > options_.areaTolerance) restored = fromPolygons(shape);
            add(restored, kind);
        };
        addClipped(rectangle(visiblePath.boundingRect()), "clipped-rectangle");
        addClipped(hull(visiblePath, options_.flattenTolerance), "clipped-hull");
        if (prefix.startsWith("group-")) for (const auto &part : domainParts)
            add(curves.restore(part), "domain-support");
        PathsD outer;
        for (const auto &loop : visible) if (Area(loop) > 0) outer.push_back(loop);
        add(curves.restore(Union(outer, FillRule::NonZero, kBooleanPrecision)), "fill-holes");
        std::stable_sort(output.begin(), output.end(), [](const auto &a, const auto &b) {
            if (a->cost != b->cost) return a->cost < b->cost;
            return a->id < b->id;
        });
        return output;
    };
    const auto simplify = [&](const Pointer &node) -> const Scene & {
        const auto found = simpleCache.find(node->id);
        if (found != simpleCache.end()) return found->second;
        Scene output = variants(node->item, node->cost, "");
        const auto loops = spans(node->item.path);
        const auto requiredPath = fromPolygons(territory(node->item.owners));
        QPainterPathStroker boundaryStroker;
        boundaryStroker.setWidth(options_.flattenTolerance * 2);
        const auto domainBoundary = boundaryStroker.createStroke(domain_);
        for (int loop = 0; loop < loops.size(); ++loop) {
            auto filled = loops;
            filled.removeAt(loop);
            if (!filled.isEmpty())
                if (auto candidate = makeNode({fromSpans(filled, node->item.path.fillRule()), node->item.color, node->item.owners},
                                              "remove-loop", node->cost)) output.push_back(candidate);
            const auto &chain = loops[loop];
            std::vector<int> lengths;
            for (int length = 1; length <= options_.maximumChordSpans && length < chain.size(); ++length) lengths.push_back(length);
            if (options_.maximumChordSpans > 0)
                for (int length = std::max(2, options_.maximumChordSpans * 2); length < chain.size(); length *= 2) lengths.push_back(length);
            std::reverse(lengths.begin(), lengths.end());
            for (const int length : lengths) {
                const int stride = length <= options_.maximumChordSpans ? 1 : std::max(1, length / 8);
                for (int start = 0; start < chain.size(); start += stride) {
                    if (length == 1 && !chain[start].curved) continue;
                    const auto first = chain[start].start, last = chain[(start + length - 1) % chain.size()].end;
                    bool possible = true;
                    for (const double fraction : {.25, .5, .75}) {
                        const auto point = first + (last - first) * fraction;
                        if (requiredPath.contains(point) || (!domain_.contains(point) && !domainBoundary.contains(point))) {
                            possible = false; break;
                        }
                    }
                    if (!possible) continue;
                    Loop shortened{{first, {}, last, false, {}}};
                    for (int offset = length; offset < chain.size(); ++offset) shortened.push_back(chain[(start + offset) % chain.size()]);
                    auto changed = loops;
                    changed[loop] = shortened;
                    if (auto candidate = makeNode({fromSpans(changed, node->item.path.fillRule()), node->item.color, node->item.owners},
                                                  "hidden-chain", node->cost)) output.push_back(candidate);
                    if (output.size() >= options_.completionCandidates) break;
                }
                if (output.size() >= options_.completionCandidates) break;
            }
        }
        std::stable_sort(output.begin(), output.end(), [](const auto &a, const auto &b) {
            if (a->cost != b->cost) return a->cost < b->cost;
            return a->id < b->id;
        });
        return simpleCache.emplace(node->id, std::move(output)).first->second;
    };
    const auto locate = [&](int id) -> Pointer {
        for (const auto &node : state.scene) if (node->id == id) return node;
        return {};
    };
    const auto groupVariants = [&](const Scene &members) {
        Item item;
        item.color = members.front()->item.color;
        item.path.setFillRule(members.front()->item.path.fillRule());
        PathsD combined;
        double ceiling = 0;
        int holes = 0;
        for (const auto &node : members) {
            item.owners.insert(node->item.owners.begin(), node->item.owners.end());
            item.path.addPath(node->item.path);
            combined.insert(combined.end(), node->shape.begin(), node->shape.end());
            ceiling += node->cost;
            holes += node->holes;
        }
        auto output = variants(item, ceiling, "group-");
        if (auto node = makeNode({curves.restore(Union(combined, FillRule::NonZero, kBooleanPrecision)), item.color, item.owners},
                                  "group-union", ceiling)) output.push_back(node);
        std::set<int> memberPositions, ancestors;
        for (int position = 0; position < state.scene.size(); ++position)
            if (members.contains(state.scene[position])) memberPositions.insert(position);
        std::vector<int> pendingAncestors(memberPositions.begin(), memberPositions.end());
        for (size_t next = 0; next < pendingAncestors.size(); ++next) {
            const int target = pendingAncestors[next];
            for (const auto &[from, to] : state.graph.edges)
                if (to == target && !memberPositions.contains(from) && ancestors.insert(from).second)
                    pendingAncestors.push_back(from);
        }
        if (!ancestors.empty()) {
            PathsD forbidden;
            for (const int position : ancestors) for (const int owner : state.scene[position]->item.owners)
                forbidden.insert(forbidden.end(), required[owner].begin(), required[owner].end());
            const auto allowed = Difference(domain, Union(forbidden, FillRule::NonZero, kBooleanPrecision),
                FillRule::NonZero, kBooleanPrecision);
            const auto addAllowed = [&](const PathsD &shape, const QString &kind) {
                auto path = curves.restore(shape);
                if (std::abs(Area(Xor(shape, polygons(path, options_.flattenTolerance),
                    FillRule::NonZero, kBooleanPrecision))) > options_.areaTolerance) path = fromPolygons(shape);
                if (auto node = makeNode({path, item.color, item.owners}, kind, ceiling)) output.push_back(node);
            };
            const auto visible = fromPolygons(territory(item.owners));
            addAllowed(Intersect(polygons(rectangle(visible.boundingRect()), options_.flattenTolerance), allowed,
                FillRule::NonZero, kBooleanPrecision), "group-ordered-rectangle");
            addAllowed(Intersect(polygons(hull(visible, options_.flattenTolerance), options_.flattenTolerance), allowed,
                FillRule::NonZero, kBooleanPrecision), "group-ordered-hull");
            for (const auto &part : components(allowed)) addAllowed(part, "group-ordered-support");
        }
        output.erase(std::remove_if(output.begin(), output.end(), [&](const auto &node) {
            if (node->holes <= holes) return false;
            ++rejected["Grouping introduced holes"];
            return true;
        }), output.end());
        std::stable_sort(output.begin(), output.end(), [](const auto &a, const auto &b) { return a->cost < b->cost; });
        return output;
    };
    const auto tryEdit = [&](const Edit &edit) {
        if (attempt(edit, true, false)) return true;
        const auto conflicts = cycleNodes;
        if (conflicts.empty()) return false;
        auto trimmedEdit = edit;
        std::set<int> editOwners;
        for (const auto &node : edit.added) editOwners.insert(node->item.owners.begin(), node->item.owners.end());
        bool trimmedValid = true, trimmedAny = false;
        for (const int id : conflicts) {
            const auto other = locate(id);
            if (!other) { trimmedValid = false; break; }
            std::set<int> cutOwners;
            for (const int owner : other->above) if (editOwners.contains(owner)) cutOwners.insert(owner);
            if (cutOwners.empty()) continue;
            const auto retained = Difference(other->shape, territory(cutOwners), FillRule::NonZero, kBooleanPrecision);
            std::set<int> assigned;
            Scene pieces;
            for (const auto &part : components(retained)) {
                const PolygonIndex support(part);
                std::set<int> owners;
                for (const int owner : other->item.owners) {
                    if (required[owner].empty()) continue;
                    const auto box = ownership_[owner].boundingRect();
                    if (support.intersectArea(required[owner], box) > 0
                        && support.differenceArea(required[owner], box) <= options_.areaTolerance)
                        owners.insert(owner);
                }
                if (owners.empty()) continue;
                for (const int owner : owners) if (!assigned.insert(owner).second) trimmedValid = false;
                auto path = curves.restore(part);
                if (std::abs(Area(Xor(part, polygons(path, options_.flattenTolerance),
                    FillRule::NonZero, kBooleanPrecision))) > options_.areaTolerance
                    || !penTargetError(path, options_).isEmpty()) path = fromPolygons(part);
                auto node = makeNode({path, other->item.color, owners}, "trimmed-occluder", std::numeric_limits<double>::infinity());
                if (!node) { trimmedValid = false; break; }
                pieces.push_back(node);
            }
            for (const int owner : other->item.owners)
                if (!required[owner].empty() && !assigned.contains(owner)) trimmedValid = false;
            if (!trimmedValid) break;
            trimmedEdit.removed.insert(id);
            trimmedEdit.added += pieces;
            trimmedAny = true;
        }
        trimmedEdit.kind = "joint-" + edit.kind + "-trim-occluders";
        if (trimmedValid && trimmedAny && attempt(trimmedEdit, true, false)) return true;
        if (!trimmedValid) ++rejected["Cycle trim split fixed ownership"];
        int removedHoles = 0, addedHoles = 0;
        for (const auto &node : state.scene) if (edit.removed.contains(node->id)) removedHoles += node->holes;
        for (const auto &node : edit.added) addedHoles += node->holes;
        if (addedHoles < removedHoles || edit.kind.contains("fill-holes")) {
            auto joint = edit;
            bool changed = false;
            for (const int id : conflicts) {
                const auto other = locate(id);
                if (!other) continue;
                std::set<int> selected;
                for (const auto &candidate : edit.added) for (const int owner : candidate->above)
                    if (other->item.owners.contains(owner)) selected.insert(owner);
                if (selected.empty()) continue;
                auto remaining = other->item.owners;
                for (const int owner : selected) remaining.erase(owner);
                auto cap = makeNode({curves.restore(territory(selected)), other->item.color, selected},
                                    "enclosed-occluder", std::numeric_limits<double>::infinity());
                if (!cap) continue;
                joint.removed.insert(id);
                if (!remaining.empty()) {
                    auto rest = std::make_shared<Node>(*other);
                    rest->id = serial++;
                    rest->item.owners = std::move(remaining);
                    rest->kind = "enclosed-retained-source";
                    rest->above.insert(rest->above.end(), selected.begin(), selected.end());
                    joint.added.push_back(rest);
                }
                joint.added.push_back(cap);
                changed = true;
            }
            joint.kind = "joint-" + edit.kind + "-enclosed-occluders";
            if (changed && attempt(joint, true, false)) return true;
        }
        if (conflicts.size() > 2) return false;
        std::vector<Edit> alternatives{edit};
        for (const int id : conflicts) {
            const auto other = locate(id);
            if (!other) return false;
            std::set<int> selected;
            for (const auto &candidate : edit.added) for (const int owner : candidate->above)
                if (other->item.owners.contains(owner)) selected.insert(owner);
            if (selected.empty() || selected.size() > size_t(options_.componentLimit)) return false;
            auto remaining = other->item.owners;
            for (const int owner : selected) remaining.erase(owner);
            Pointer rest;
            if (!remaining.empty()) {
                const double minimum = options_.pathWeight + options_.componentWeight
                    + std::min(3 * options_.segmentWeight, options_.segmentWeight + 2 * options_.controlPointWeight);
                double maximumAvailable = 0;
                for (const auto &base : alternatives) {
                    double available = 0;
                    for (const auto &node : state.scene) if (base.removed.contains(node->id)) available += node->cost;
                    for (const auto &node : base.added) available -= node->cost;
                    maximumAvailable = std::max(maximumAvailable, available);
                }
                if (maximumAvailable <= minimum + 1e-9) return false;
                rest = std::make_shared<Node>(*other);
                rest->id = serial++;
                rest->item.owners = remaining;
                rest->kind = "split-retained-source";
                for (const int owner : selected)
                    if (regions[owner].intersectArea(other->shape, other->item.path.boundingRect()) > options_.areaTolerance)
                        rest->above.push_back(owner);
            }
            std::vector<Edit> expanded;
            for (const auto &base : alternatives) {
                double available = other->cost - (rest ? rest->cost : 0.0);
                for (const auto &node : state.scene) if (base.removed.contains(node->id)) available += node->cost;
                for (const auto &node : base.added) available -= node->cost;
                if (available <= options_.pathWeight + options_.componentWeight) continue;
                const auto choices = variants({curves.restore(territory(selected)), other->item.color, selected}, available, "split-occluder-");
                for (const auto &choice : choices) {
                    auto joint = base;
                    joint.removed.insert(id);
                    if (rest) joint.added.push_back(rest);
                    joint.added.push_back(choice);
                    joint.kind = "joint-" + edit.kind + "-split-occluder";
                    expanded.push_back(joint);
                }
            }
            const auto editCost = [&](const Edit &proposal) {
                double cost = 0;
                for (const auto &node : proposal.added) cost += node->cost;
                for (const auto &node : state.scene) if (proposal.removed.contains(node->id)) cost -= node->cost;
                return cost;
            };
            std::stable_sort(expanded.begin(), expanded.end(), [&](const Edit &a, const Edit &b) { return editCost(a) < editCost(b); });
            if (expanded.size() > size_t(options_.shortlistSize)) expanded.resize(options_.shortlistSize);
            alternatives = std::move(expanded);
            if (alternatives.empty()) return false;
        }
        for (const auto &alternative : alternatives) if (attempt(alternative, true, false)) return true;
        return false;
    };
    #include "absorption_search.inc"
    for (int round = 0; !absorptionEnabled_ && round < options_.maximumPasses; ++round) {
        const int acceptedBefore = state.accepted;
        const auto groupSnapshot = state.scene;
        std::set<QRgb> wholeColors;
        for (const auto &seed : groupSnapshot) {
            if (!locate(seed->id)) continue;
            std::vector<std::pair<double, Pointer>> neighbors;
            for (const auto &other : state.scene) {
                if (other->id == seed->id || other->item.color != seed->item.color) continue;
                const auto a = seed->item.path.boundingRect(), b = other->item.path.boundingRect();
                const double dx = std::max({0.0, a.left() - b.right(), b.left() - a.right()});
                const double dy = std::max({0.0, a.top() - b.bottom(), b.top() - a.bottom()});
                neighbors.push_back({dx * dx + dy * dy, other});
            }
            std::stable_sort(neighbors.begin(), neighbors.end(), [](const auto &a, const auto &b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second->id < b.second->id;
            });
            std::vector<Scene> groups;
            if (wholeColors.insert(seed->item.color.rgb()).second && neighbors.size() > 1) {
                Scene whole{seed};
                for (const auto &[distance, other] : neighbors) whole.push_back(other);
                groups.push_back(whole);
                Scene compatible{seed};
                std::set<int> combinedOwners = seed->item.owners;
                std::set<int> combinedAbove(seed->above.begin(), seed->above.end());
                for (const auto &[distance, other] : neighbors) {
                    auto probe = std::make_shared<Node>();
                    probe->item.owners = combinedOwners;
                    probe->item.owners.insert(other->item.owners.begin(), other->item.owners.end());
                    std::set<int> above = combinedAbove;
                    above.insert(other->above.begin(), other->above.end());
                    for (const int owner : above)
                        if (!probe->item.owners.contains(owner)) probe->above.push_back(owner);
                    Scene trial;
                    for (const auto &node : state.scene)
                        if (node != other && !compatible.contains(node)) trial.push_back(node);
                    trial.push_back(probe);
                    if (!graphFor(trial).error.isEmpty()) continue;
                    compatible.push_back(other);
                    combinedOwners = probe->item.owners;
                    combinedAbove = std::move(above);
                }
                if (compatible.size() > 2 && compatible.size() < whole.size()) groups.push_back(compatible);
            }
            Scene cluster{seed};
            for (int index = 0; index < std::min(int(neighbors.size()), options_.groupNeighbors); ++index) {
                groups.push_back({seed, neighbors[index].second});
                cluster.push_back(neighbors[index].second);
                if (cluster.size() > 2) groups.push_back(cluster);
            }
            bool merged = false;
            std::function<bool(Scene)> mergeBatch;
            mergeBatch = [&](Scene members) {
                members.erase(std::remove_if(members.begin(), members.end(), [&](const auto &member) {
                    return !locate(member->id);
                }), members.end());
                if (members.size() < 2) return false;
                std::set<int> removed;
                for (const auto &member : members) removed.insert(member->id);
                for (const auto &candidate : groupVariants(members)) {
                    if (tryEdit({removed, {candidate}, candidate->kind})) return true;
                }
                if (members.size() < 4) return false;
                std::map<int, int> positions;
                for (int position = 0; position < state.scene.size(); ++position) positions[state.scene[position]->id] = position;
                std::stable_sort(members.begin(), members.end(), [&](const auto &a, const auto &b) {
                    return positions.at(a->id) < positions.at(b->id);
                });
                const int middle = members.size() / 2;
                const bool lower = mergeBatch(members.mid(0, middle));
                const bool upper = mergeBatch(members.mid(middle));
                return lower || upper;
            };
            for (const auto &members : groups) {
                merged = mergeBatch(members);
                if (merged) break;
            }
        }
        flush();
        const auto snapshot = state.scene;
        for (auto node : snapshot) {
            if (!locate(node->id)) continue;
            bool hidden = true;
            for (const int owner : node->item.owners) hidden &= std::abs(Area(required[owner])) <= options_.areaTolerance;
            if (hidden && attempt({{node->id}, {}, "remove-hidden"}, true, false)) continue;
            for (int step = 0; step < options_.maximumPasses; ++step) {
                const auto alternatives = simplify(node);
                bool accepted = false;
                for (const auto &alternative : alternatives) {
                    if (!tryEdit({{node->id}, {alternative}, alternative->kind})) continue;
                    simpleCache.erase(node->id);
                    node = alternative;
                    accepted = true;
                    break;
                }
                if (!accepted || !locate(node->id)) break;
            }
        }
        flush();
        const auto splitSnapshot = state.scene;
        for (const auto &node : splitSnapshot) {
            if (!locate(node->id) || node->item.owners.size() < 2 || node->item.owners.size() > size_t(options_.componentLimit)) continue;
            Scene parts;
            bool valid = true;
            for (const int owner : node->item.owners) {
                if (required[owner].empty()) continue;
                const auto choices = variants({curves.restore(required[owner]), node->item.color, {owner}}, node->cost, "component-");
                if (choices.empty()) { valid = false; break; }
                parts.push_back(choices.front());
            }
            if (valid && parts.size() > 1 && attempt({{node->id}, parts, "component-completion"}, true, false)) continue;
            bool changed = false;
            for (const auto &other : state.scene) {
                if (other->id == node->id || other->item.color != node->item.color) continue;
                for (const int owner : node->item.owners) {
                    if (required[owner].empty()) continue;
                    auto restOwners = node->item.owners;
                    restOwners.erase(owner);
                    const auto remaining = variants({node->item.path, node->item.color, restOwners}, node->cost + other->cost, "split-rest-");
                    const auto part = makeNode({curves.restore(required[owner]), node->item.color, {owner}}, "split-part", std::numeric_limits<double>::infinity());
                    if (!part || remaining.empty()) continue;
                    for (const auto &merged : groupVariants({part, other})) {
                        if (attempt({{node->id, other->id}, {remaining.front(), merged}, "split-group"}, true, false)) { changed = true; break; }
                    }
                    if (changed) break;
                }
                if (changed) break;
            }
        }
        flush();
        if (state.accepted == acceptedBefore) break;
    }
    flush();
    result.items = items(state.scene);
    result.error = audit();
    if (!result.error.isEmpty()) {
        result.diagnostics.append(QJsonObject{{"final_validation_error", result.error}, {"baseline_retained", true}});
        result.items = baseline_;
        result.error.clear();
        state = originalState;
    }
    result.after = state.accepted ? sceneMetrics(state.scene, state.graph) : result.before;
    if (absorptionEnabled_) result.ownershipAfter = absorptionOwnership(result.items, ownership_, options_.flattenTolerance);
    const auto finalDependencies = state.graph;
    result.graph = topologicalOrder(result.items.size(), finalDependencies.edges);
    if (!finalDependencies.error.isEmpty()) result.graph.error = finalDependencies.error;
    result.accepted = state.accepted;
    result.selectedStrategy = state.accepted ? (absorptionEnabled_ ? "color-absorption" : "incremental") : "baseline";
    const auto finalDomain = filledUnion(objects(result.items), options_.flattenTolerance);
    result.silhouetteDifference = area(booleanPath(domain_, finalDomain, ClipType::Xor, options_.flattenTolerance), options_.flattenTolerance);
    const auto finalError = imageError(reference, renderer_(objects(result.items)));
    QJsonObject rejections;
    for (const auto &[cause, count] : rejected) rejections.insert(cause, count);
    result.diagnostics.prepend(QJsonObject{{"generated", generated}, {"ownership_components", ownership_.size()},
        {"accepted_edits", result.accepted}, {"full_audits", audits}, {"repaired_batches", repairedBatches},
        {"rejected", rejections}, {"hidden_area_penalty", false}, {"compact_fit_leeway", false}});
    result.strategies.append(QJsonObject{{"strategy", absorptionEnabled_ ? "color-absorption" : "incremental"}, {"metrics", result.after.json()},
        {"image_error", finalError}, {"error", result.error},
        {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}});

    if (result.accepted == 0) result.benchmarks.clear();
    std::stable_sort(result.benchmarks.begin(), result.benchmarks.end(), [](const auto &a, const auto &b) { return a.saving > b.saving; });

    return result;
}
