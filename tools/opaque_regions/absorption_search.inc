    if (absorptionEnabled_) {
        if (ownership_.size() >= 0xffffff) throw std::runtime_error("Too many ownership regions for native absorption adjacency");
        struct Moment { ColorVector sum{}; double pixels = 0; double area = 0; };
        std::vector<Moment> moments(ownership_.size());
        std::vector<std::map<int, double>> contacts(ownership_.size());
        QVector<int> pixels(ownership_.size(), 0);
        QImage labels(reference.size(), QImage::Format_RGB32);
        labels.fill(Qt::black);
        {
            QPainter painter(&labels);
            painter.setPen(Qt::NoPen);
            for (int owner = 0; owner < ownership_.size(); ++owner) {
                painter.setBrush(QColor::fromRgb(QRgb(owner + 1)));
                painter.drawPath(ownership_[owner]);
                moments[owner].area = std::abs(Area(required[owner]));
            }
        }
        const auto target = absorptionReference_.convertToFormat(QImage::Format_ARGB32);
        const auto connect = [&](int a, int b, double length) {
            if (a < 0 || b < 0 || a == b) return;
            contacts[a][b] += length;
            contacts[b][a] += length;
        };
        for (int y = 0; y < labels.height(); ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(labels.constScanLine(y));
            const auto *source = reinterpret_cast<const QRgb *>(target.constScanLine(y));
            const auto *next = y + 1 < labels.height() ? reinterpret_cast<const QRgb *>(labels.constScanLine(y + 1)) : nullptr;
            for (int x = 0; x < labels.width(); ++x) {
                const int owner = int(row[x] & 0xffffff) - 1;
                if (owner >= 0) {
                    const auto rgb = compositeColor(source[x]);
                    for (int channel = 0; channel < 3; ++channel) moments[owner].sum[channel] += rgb[channel];
                    ++moments[owner].pixels;
                    ++pixels[owner];
                }
                if (x + 1 < labels.width()) connect(owner, int(row[x + 1] & 0xffffff) - 1, 1);
                if (next) connect(owner, int(next[x] & 0xffffff) - 1, 1);
            }
        }
        for (int owner = 0; owner < ownership_.size(); ++owner) {
            if (pixels[owner] && !contacts[owner].empty()) continue;
            if (moments[owner].area <= 0) continue;
            QPainterPathStroker stroker;
            stroker.setWidth(std::max(options_.flattenTolerance * 4, 1e-5));
            const auto band = stroker.createStroke(ownership_[owner]);
            const auto shape = polygons(band, options_.flattenTolerance);
            for (const int neighbor : spatial.query(band.boundingRect())) {
                if (neighbor == owner || contacts[owner].contains(neighbor)) continue;
                const double overlap = regions[neighbor].intersectArea(shape, band.boundingRect());
                if (overlap > 0) connect(owner, neighbor, overlap / stroker.width());
            }
        }
        std::map<int, Moment> momentCache;
        const auto momentOf = [&](const Pointer &node) {
            if (const auto found = momentCache.find(node->id); found != momentCache.end()) return found->second;
            Moment result;
            for (const int owner : node->item.owners) {
                result.area += moments[owner].area;
                result.pixels += moments[owner].pixels;
                for (int channel = 0; channel < 3; ++channel) result.sum[channel] += moments[owner].sum[channel];
            }
            momentCache.emplace(node->id, result);
            return result;
        };
        const auto colorError = [](const Moment &moment, QColor fill) {
            const ColorVector rgb{double(fill.red()), double(fill.green()), double(fill.blue())};
            double result = 0;
            for (int channel = 0; channel < 3; ++channel)
                result += moment.pixels * rgb[channel] * rgb[channel] - 2 * rgb[channel] * moment.sum[channel];
            return result;
        };
        const auto paletteChoices = [&](const Pointer &a, const Pointer &b) {
            const auto x = momentOf(a), y = momentOf(b);
            Moment joined;
            joined.pixels = x.pixels + y.pixels;
            for (int channel = 0; channel < 3; ++channel) joined.sum[channel] = x.sum[channel] + y.sum[channel];
            QVector<QColor> fills{a->item.color};
            if (a->item.color != b->item.color) fills.push_back(b->item.color);
            if (joined.pixels <= 0) return fills;
            std::set<QRgb> palette;
            for (const auto &node : state.scene) palette.insert(node->item.color.rgb());
            std::vector<std::pair<double, QRgb>> ranked;
            for (const QRgb color : palette) ranked.push_back({colorError(joined, QColor(color)), color});
            std::stable_sort(ranked.begin(), ranked.end());
            const double before = colorError(x, a->item.color) + colorError(y, b->item.color);
            for (int index = 0; index < std::min(2, int(ranked.size())); ++index) {
                if (ranked[index].first >= before - 1e-9) continue;
                const QColor color(ranked[index].second);
                if (!fills.contains(color)) fills.push_back(color);
            }
            return fills;
        };
        const auto sourceImproves = [&](const Pointer &a, const Pointer &b) {
            const auto x = momentOf(a), y = momentOf(b);
            Moment joined;
            joined.pixels = x.pixels + y.pixels;
            for (int channel = 0; channel < 3; ++channel) joined.sum[channel] = x.sum[channel] + y.sum[channel];
            const double before = colorError(x, a->item.color) + colorError(y, b->item.color);
            for (const auto fill : paletteChoices(a, b))
                if (colorError(joined, fill) < before - 1e-9) return true;
            return false;
        };
        const auto eligibility = [&](const Pointer &a, const Pointer &b, double length) {
            const auto x = momentOf(a), y = momentOf(b);
            const double effective = x.area * y.area / std::max(1e-12, x.area + y.area);
            const double support = absorptionLimits_.detailScale * length
                + absorptionLimits_.detailScale * absorptionLimits_.detailScale;
            return colorDistance(perceptualColor(a->item.color), perceptualColor(b->item.color))
                * effective / std::max(1e-12, effective + support);
        };
        int adjacentPairs = 0;
        for (const auto &neighbors : contacts) adjacentPairs += int(neighbors.size());
        result.diagnostics.append(QJsonObject{{"color_absorption", true}, {"fixed_adjacency_edges", adjacentPairs / 2},
            {"palette_averaging", false}, {"area_deletion", false}, {"region_count_target", QJsonValue()},
            {"source_rmse_before", sourceBaselineRmse}, {"source_rmse_growth_limit", absorptionSourceGrowth_},
            {"source_edge_contrast", absorptionEdgeContrast_}});
        const double threshold = std::pow(absorptionLimits_.tolerance / 100, 2);
        for (int round = 0; round < options_.maximumPasses; ++round) {
            const int acceptedBefore = state.accepted;
            #include "component_absorption.inc"
            std::map<std::pair<int, int>, double> pairs;
            for (int owner = 0; owner < ownership_.size(); ++owner) {
                const int a = state.ownerPositions[owner];
                if (a < 0) continue;
                for (const auto &[neighbor, length] : contacts[owner]) {
                    if (owner >= neighbor) continue;
                    const int b = state.ownerPositions[neighbor];
                    if (b < 0 || a == b) continue;
                    pairs[std::minmax(a, b)] += length;
                }
            }
            using Pair = std::tuple<double, int, int, double>;
            std::vector<Pair> queue;
            for (const auto &[pair, length] : pairs) {
                const auto &a = state.scene[pair.first], &b = state.scene[pair.second];
                const double score = eligibility(a, b, length);
                if (score > threshold && !sourceImproves(a, b)) continue;
                queue.push_back({score, *a->item.owners.begin(), *b->item.owners.begin(), length});
            }
            std::stable_sort(queue.begin(), queue.end());
            std::set<std::pair<int, int>> tried;
            std::set<std::pair<QRgb, QRgb>> triedClasses;
            int visited = 0;
            for (const auto &[oldScore, anchorA, anchorB, length] : queue) {
                const int positionA = state.ownerPositions[anchorA], positionB = state.ownerPositions[anchorB];
                if (positionA < 0 || positionB < 0 || positionA == positionB) continue;
                const auto a = state.scene[positionA], b = state.scene[positionB];
                if (!tried.insert(std::minmax(a->id, b->id)).second) continue;
                if (eligibility(a, b, length) > threshold && !sourceImproves(a, b)) continue;
                ++visited;
                std::set<int> owners = a->item.owners;
                owners.insert(b->item.owners.begin(), b->item.owners.end());
                const double ceiling = a->cost + b->cost;
                auto joined = Union(a->shape, b->shape, FillRule::NonZero, kBooleanPrecision);
                std::vector<PathsD> proposals{std::move(joined), territory(owners)};
                const auto visible = fromPolygons(territory(owners));
                proposals.push_back(polygons(rectangle(visible.boundingRect()), options_.flattenTolerance));
                proposals.push_back(polygons(hull(visible, options_.flattenTolerance), options_.flattenTolerance));
                const double requiredArea = std::abs(Area(territory(owners)));
                const auto x = momentOf(a), y = momentOf(b);
                Moment combined;
                combined.area = x.area + y.area;
                combined.pixels = x.pixels + y.pixels;
                for (int channel = 0; channel < 3; ++channel) combined.sum[channel] = x.sum[channel] + y.sum[channel];
                const double beforeError = colorError(x, a->item.color) + colorError(y, b->item.color);
                struct Choice { Pointer node; double sourceError; double addedArea; };
                std::vector<Choice> choices;
                std::set<QByteArray> seen;
                const auto fills = paletteChoices(a, b);
                for (const auto &shape : proposals) {
                    auto path = curves.restore(shape);
                    if (std::abs(Area(Xor(shape, polygons(path, options_.flattenTolerance), FillRule::NonZero,
                        kBooleanPrecision))) > options_.areaTolerance) path = fromPolygons(shape);
                    Pointer geometry;
                    for (const auto fill : fills) {
                        if (!seen.insert(svg({{path, fill}}, reference.size())).second) continue;
                        Pointer node;
                        if (geometry) {
                            node = std::make_shared<Node>(*geometry);
                            node->id = serial++;
                            node->item.color = fill;
                            ++generated;
                        } else {
                            node = makeNode({path, fill, owners}, "color-absorption-group", ceiling);
                            if (!node || node->holes > a->holes + b->holes) break;
                            geometry = node;
                        }
                        choices.push_back({node, colorError(combined, fill) - beforeError,
                            std::max(0.0, std::abs(Area(node->shape)) - requiredArea)});
                    }
                }
                std::stable_sort(choices.begin(), choices.end(), [&](const auto &a, const auto &b) {
                    const double savingA = ceiling - a.node->cost, savingB = ceiling - b.node->cost;
                    return std::tuple(-savingA, a.addedArea, a.node->above.size(), a.sourceError, a.node->item.color.rgb(), a.node->id)
                        < std::tuple(-savingB, b.addedArea, b.node->above.size(), b.sourceError, b.node->item.color.rgb(), b.node->id);
                });
                std::set<QRgb> classTargets;
                if (!choices.empty() && a->item.color != b->item.color) {
                    if (triedClasses.insert({b->item.color.rgb(), a->item.color.rgb()}).second) classTargets.insert(a->item.color.rgb());
                    if (triedClasses.insert({a->item.color.rgb(), b->item.color.rgb()}).second) classTargets.insert(b->item.color.rgb());
                }
                for (const auto &choice : choices) {
                    bool accepted = false;
                    const auto fill = choice.node->item.color;
                    if (a->item.color != b->item.color && (fill == a->item.color || fill == b->item.color)) {
                        const auto discarded = fill == a->item.color ? b->item.color : a->item.color;
                        if (classTargets.contains(fill.rgb())) {
                            Scene members;
                            Moment remainder;
                            for (const auto &node : state.scene) {
                                if (node->id == a->id || node->id == b->id || node->item.color != discarded) continue;
                                members.push_back(node);
                                const auto moment = momentOf(node);
                                remainder.pixels += moment.pixels;
                                for (int channel = 0; channel < 3; ++channel) remainder.sum[channel] += moment.sum[channel];
                            }
                            const double predicted = choice.sourceError + colorError(remainder, fill) - colorError(remainder, discarded);
                            const double available = absorptionSourceGrowth_ == 0 ? 0 : sourceMaximumSquared - state.sourceSquared;
                            const bool nearby = colorDistance(perceptualColor(discarded), perceptualColor(fill)) <= threshold;
                            if (!members.empty() && (nearby || predicted < 0) && predicted <= available + 1e-6) {
                                Edit entire{{a->id, b->id}, {choice.node}, "color-class-absorption-group"};
                                for (const auto &node : members) {
                                    auto recolored = std::make_shared<Node>(*node);
                                    recolored->id = serial++;
                                    recolored->kind = entire.kind;
                                    recolored->item.color = fill;
                                    ++generated;
                                    entire.removed.insert(node->id);
                                    entire.added.push_back(std::move(recolored));
                                }
                                auto covered = std::make_shared<Node>(*choice.node);
                                const PolygonIndex coverage(covered->shape);
                                Scene retained;
                                bool simplified = false;
                                for (int index = 0; index < members.size(); ++index) {
                                    const auto &member = members[index];
                                    const bool contained = std::all_of(member->item.owners.begin(), member->item.owners.end(),
                                        [&](int owner) { return coverage.differenceArea(required[owner], ownership_[owner].boundingRect()) == 0; });
                                    if (contained) {
                                        covered->item.owners.insert(member->item.owners.begin(), member->item.owners.end());
                                        simplified = true;
                                    } else retained.push_back(entire.added[index + 1]);
                                }
                                if (simplified) {
                                    covered->id = serial++;
                                    covered->kind = "color-class-support-group";
                                    ++generated;
                                    std::erase_if(covered->above, [&](int owner) { return covered->item.owners.contains(owner); });
                                    retained.prepend(covered);
                                    accepted = attempt({entire.removed, retained, covered->kind}, true, false);
                                }
                                if (!accepted) accepted = attempt(entire, true, false);
                            }
                        }
                    }
                    const Edit edit{{a->id, b->id}, {choice.node}, "color-absorption-group"};
                    if (!accepted) accepted = attempt(edit, true, false);
                    if (!accepted && !cycleNodes.empty() && cycleNodes.size() <= size_t(options_.groupNeighbors))
                        accepted = tryEdit(edit);
                    if (!accepted) continue;
                    momentCache.emplace(choice.node->id, combined);
                    break;
                }
                if (options_.progress && visited % 256 == 0) {
                    std::fprintf(stderr, "absorption: round %d, %d/%d pairs, %d accepted merges, %d paths\n",
                        round + 1, visited, int(queue.size()), state.accepted, int(state.scene.size()));
                    std::fflush(stderr);
                }
            }
            flush();
            if (state.accepted == acceptedBefore) break;
        }
    }
