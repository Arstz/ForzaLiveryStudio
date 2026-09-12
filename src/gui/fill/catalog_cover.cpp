#include "catalog_cover_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace gui::catalog {
namespace {

constexpr int kTargetSamples = 16;
constexpr int kSourceSamples = 8;
constexpr std::array<int, 4> kTargetSpans = {2, 4, 8, 12};
constexpr int kSeedRetention = 2;
constexpr int kBoundaryProbeCount = 12;
constexpr int kMaximumPolishedSeeds = 192;
constexpr int kWitnessGridSize = 9;
constexpr int kMaximumMeshWitnesses = 96;
constexpr int kMaximumWitnessRounds = 6;
constexpr int kWitnessesPerRound = 12;
constexpr int kMaximumSearchDepth = 128;
constexpr int kPruningBoundarySamples = 12;
constexpr std::array<double, 3> kPruningInsetFractions = {0.001, 0.03, 0.2};
constexpr double kSpillPenalty = 32.0;
constexpr double kInitialParameterStep = 0.04;
constexpr double kMinimumCandidateAreaRatio = 1e-9;
constexpr double kInitialExpansion = 1.000002;
constexpr double kRankingQuantum = 1e-12;
constexpr double kBoundarySeedOffsetFraction = 0.25;

struct Seed {
    QTransform transform;
    int primitive = -1;
    double score = -std::numeric_limits<double>::infinity();
};

struct SearchState {
    QVector<QPointF> witnesses;
    QVector<QVector<int>> choices;
    QVector<QBitArray> memberships;
    QVector<int> best;
    QVector<int> selected;
    QVector<bool> forbidden;
    Polygons discoveredResidual;
    int nodes = 0;
    double spill = std::numeric_limits<double>::infinity();
};

bool isCancelled(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

bool finiteTransform(const QTransform &transform) {
    const std::array<double, 6> values = {transform.m11(), transform.m12(),
        transform.m21(), transform.m22(), transform.dx(), transform.dy()};

    return std::all_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value) && std::abs(value) < kMaximumCoordinate;
    }) && std::abs(transform.determinant()) > kMinimumDeterminant;
}

bool transformLess(const QTransform &left, const QTransform &right) {
    return std::array<double, 6>{left.m11(), left.m12(), left.m21(), left.m22(), left.dx(), left.dy()}
        < std::array<double, 6>{right.m11(), right.m12(), right.m21(), right.m22(), right.dx(), right.dy()};
}

double rankValue(double value, const Region &region) {
    const double quantum = region.area * kRankingQuantum;

    return std::round(value / quantum) * quantum;
}

int witnessCount(const QBitArray &bits) {
    const int byteCount = bits.size() / 8;
    const int remainder = bits.size() % 8;
    int result = 0;
    for (int index = 0; index < byteCount; ++index) {
        result += std::popcount(static_cast<unsigned char>(bits.bits()[index]));
    }
    if (remainder > 0) {
        result += std::popcount(static_cast<unsigned char>(bits.bits()[byteCount])
                                & ((1U << remainder) - 1));
    }

    return result;
}

QPolygonF sampledBoundary(const QPolygonF &polygon, int count, double outwardOffset = 0.0) {
    QPolygonF result;
    QVector<double> lengths{0.0};
    for (int index = 0; index < polygon.size(); ++index) {
        lengths.push_back(lengths.back()
            + QLineF(polygon[index], polygon[(index + 1) % polygon.size()]).length());
    }
    if (lengths.back() <= 0.0) {
        return result;
    }
    for (int sample = 0; sample < count; ++sample) {
        const double distance = lengths.back() * sample / count;
        const int edge = std::clamp(static_cast<int>(std::upper_bound(
            lengths.begin(), lengths.end(), distance) - lengths.begin()) - 1,
            0, static_cast<int>(polygon.size()) - 1);
        const double fraction = (distance - lengths[edge]) / (lengths[edge + 1] - lengths[edge]);
        const QPointF tangent = polygon[(edge + 1) % polygon.size()] - polygon[edge];
        const QPointF offset(tangent.y(), -tangent.x());
        result.push_back(polygon[edge] * (1.0 - fraction)
                         + polygon[(edge + 1) % polygon.size()] * fraction
                         + offset * (outwardOffset / (lengths[edge + 1] - lengths[edge])));
    }

    return result;
}

double screenSeed(const PenPrimitive &shape, const QPolygonF &probes,
                  const QTransform &transform, const Region &region) {
    if (!finiteTransform(transform)) {
        return -1.0;
    }
    const QRectF bounds = transform.mapRect(shape.bounds);
    const QRectF overlap = bounds.intersected(region.bounds);
    const double shapeArea = shape.area * std::abs(transform.determinant());
    if (!QRectF(-kMaximumCoordinate, -kMaximumCoordinate,
                2.0 * kMaximumCoordinate, 2.0 * kMaximumCoordinate).contains(bounds)
        || overlap.isEmpty() || shapeArea <= region.area * kMinimumCandidateAreaRatio
        || shapeArea > region.area * 4.0) {
        return -1.0;
    }
    int outside = 0;
    for (const QPointF &point : probes) {
        if (!region.permittedPath.contains(transform.map(point))) {
            ++outside;
        }
    }
    if (outside > probes.size() / 2) {
        return -1.0;
    }

    return rankValue(std::min(shapeArea, overlap.width() * overlap.height()) / (1.0 + outside * outside), region);
}

void retainSeed(QVector<Seed> *seeds, const Seed &seed, int limit) {
    if (seed.score <= 0.0) {
        return;
    }
    seeds->push_back(seed);
    std::stable_sort(seeds->begin(), seeds->end(), [](const Seed &left, const Seed &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.primitive != right.primitive) {
            return left.primitive < right.primitive;
        }
        return transformLess(left.transform, right.transform);
    });
    if (seeds->size() > limit) {
        seeds->resize(limit);
    }
}

QVector<Seed> generateSeeds(const Region &region, const QVector<Primitive> &primitives,
                           const std::function<bool()> &cancelled, int *evaluations) {
    QVector<Seed> result;
    QVector<Seed> secondarySeeds;
    QVector<QPolygonF> targetLoops;
    QVector<QVector<Seed>> boundarySeeds;
    for (const QPolygonF &polygon : region.required) {
        targetLoops.push_back(sampledBoundary(polygon, kTargetSamples,
                                             region.tolerance * kBoundarySeedOffsetFraction));
    }
    boundarySeeds.resize(targetLoops.size() * kTargetSamples * kTargetSpans.size());
    for (int primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
        QVector<Seed> wholeSeeds;
        const Primitive &primitive = primitives[primitiveIndex];
        const PenPrimitive &shape = primitive.shape;
        const QPolygonF &sourceLoop = shape.contours.front();
        const QPolygonF probes = sampledBoundary(sourceLoop, kBoundaryProbeCount);
        const QPolygonF source = sampledBoundary(sourceLoop, kSourceSamples);
        if (isCancelled(cancelled)) {
            return {};
        }
        for (int rotation = 0; rotation < kSourceSamples; ++rotation) {
            for (int mirror : {-1, 1}) {
                QTransform rotate;
                rotate.rotate(rotation * 360.0 / kSourceSamples);
                rotate.scale(mirror, 1.0);
                const QRectF rotated = rotate.mapRect(shape.bounds);
                QTransform place;
                place.translate(region.bounds.center().x(), region.bounds.center().y());
                place.scale(region.bounds.width() / rotated.width() * kInitialExpansion,
                            region.bounds.height() / rotated.height() * kInitialExpansion);
                place.translate(-rotated.center().x(), -rotated.center().y());
                const QTransform transform = rotate * place;
                ++*evaluations;
                retainSeed(&wholeSeeds, {transform, primitiveIndex,
                    screenSeed(shape, probes, transform, region)}, primitive.reserve ? 1 : kSeedRetention);
            }
        }
        if (!wholeSeeds.isEmpty()) {
            result.push_back(wholeSeeds.front());
            for (int index = 1; index < wholeSeeds.size(); ++index) {
                secondarySeeds.push_back(wholeSeeds[index]);
            }
        }
        for (int loopIndex = 0; loopIndex < targetLoops.size(); ++loopIndex) {
            const QPolygonF &target = targetLoops[loopIndex];
            for (int targetIndex = 0; targetIndex < target.size(); ++targetIndex) {
                if (isCancelled(cancelled)) {
                    return {};
                }
                for (int spanIndex = 0; spanIndex < kTargetSpans.size(); ++spanIndex) {
                    const int targetSpan = kTargetSpans[spanIndex];
                    const std::array<QPointF, 3> targetAnchors = {
                        target[targetIndex], target[(targetIndex + targetSpan / 2) % target.size()],
                        target[(targetIndex + targetSpan) % target.size()],
                    };
                    for (int sourceIndex = 0; sourceIndex < source.size(); sourceIndex += primitive.reserve ? 2 : 1) {
                        for (int sourceSpan : {kSourceSamples / 4, kSourceSamples / 2, 3 * kSourceSamples / 4}) {
                            for (int direction : {-1, 1}) {
                                const auto sourceAt = [&](int offset) {
                                    return source[(sourceIndex + direction * offset + source.size()) % source.size()];
                                };
                                const QTransform transform = affineFromAnchors(
                                    {sourceAt(0), sourceAt(sourceSpan / 2), sourceAt(sourceSpan)}, targetAnchors);
                                ++*evaluations;
                                retainSeed(&boundarySeeds[(loopIndex * kTargetSamples + targetIndex) * kTargetSpans.size() + spanIndex],
                                    {transform, primitiveIndex, screenSeed(shape, probes, transform, region)},
                                    1);
                            }
                        }
                    }
                }
            }
        }
    }
    for (const QVector<Seed> &seeds : boundarySeeds) {
        result += seeds;
    }
    std::stable_sort(secondarySeeds.begin(), secondarySeeds.end(), [](const Seed &left, const Seed &right) {
        return left.score > right.score;
    });
    result += secondarySeeds;
    if (result.size() > kMaximumPolishedSeeds) {
        result.resize(kMaximumPolishedSeeds);
    }

    return result;
}

Candidate evaluatedCandidate(const PenPrimitive &shape, const QTransform &transform,
                             const Region &region, double *score, bool *legal) {
    Candidate result;
    result.placement.shapeId = shape.shapeId;
    result.placement.transform = emittedTransform(transform);
    *legal = false;
    *score = -std::numeric_limits<double>::infinity();
    if (!finiteTransform(result.placement.transform)) {
        return result;
    }
    if (!QRectF(-kMaximumCoordinate, -kMaximumCoordinate,
                2.0 * kMaximumCoordinate, 2.0 * kMaximumCoordinate)
             .contains(result.placement.transform.mapRect(shape.bounds))) {
        return result;
    }
    result.polygons = mapped(shape, result.placement.transform);
    if (result.polygons.isEmpty()) {
        return result;
    }
    const Polygons outside = subtract(result.polygons, region.permitted);
    result.gain = rankValue(area(intersect(result.polygons, region.required)), region);
    result.spill = rankValue(area(result.polygons) - result.gain, region);
    *score = rankValue(result.gain - kSpillPenalty * area(outside) - result.spill * 0.01, region);
    *legal = outside.isEmpty() && result.gain > region.area * kMinimumCandidateAreaRatio;

    return result;
}

std::optional<Candidate> polishSeed(const Seed &seed, const PenPrimitive &shape,
                                   const Region &region, const FillOptions &options,
                                   const std::function<bool()> &cancelled, int *evaluations) {
    std::optional<Candidate> best;
    QTransform current = seed.transform;
    double currentScore = -std::numeric_limits<double>::infinity();
    double step = kInitialParameterStep;
    const auto evaluate = [&](const QTransform &transform, double *score) {
        bool legal = false;
        Candidate candidate = evaluatedCandidate(shape, transform, region, score, &legal);
        ++*evaluations;
        if (legal && (!best || candidate.gain > best->gain
                      || (candidate.gain == best->gain && candidate.spill < best->spill))) {
            best = std::move(candidate);
        }
    };
    evaluate(current, &currentScore);
    const QRectF initialBounds = current.mapRect(shape.bounds);
    QTransform unexpand;
    unexpand.translate(initialBounds.center().x(), initialBounds.center().y());
    unexpand.scale(1.0 / kInitialExpansion, 1.0 / kInitialExpansion);
    unexpand.translate(-initialBounds.center().x(), -initialBounds.center().y());
    const QTransform unexpanded = current * unexpand;
    double unexpandedScore = 0.0;
    evaluate(unexpanded, &unexpandedScore);
    if (unexpandedScore > currentScore) {
        current = unexpanded;
        currentScore = unexpandedScore;
    }
    for (int iteration = 0; iteration < options.refinementSteps; ++iteration) {
        if (isCancelled(cancelled)) {
            return {};
        }
        QTransform next = current;
        double nextScore = currentScore;
        const QRectF bounds = current.mapRect(shape.bounds);
        for (int parameter = 0; parameter < 6; ++parameter) {
            for (int direction : {-1, 1}) {
                QTransform adjust;
                adjust.translate(bounds.center().x(), bounds.center().y());
                switch (parameter) {
                case 0: adjust.translate(direction * step * bounds.width(), 0.0); break;
                case 1: adjust.translate(0.0, direction * step * bounds.height()); break;
                case 2: adjust.scale(1.0 + direction * step, 1.0); break;
                case 3: adjust.scale(1.0, 1.0 + direction * step); break;
                case 4: adjust.rotate(direction * step * 90.0); break;
                case 5: adjust.shear(direction * step, 0.0); break;
                }
                adjust.translate(-bounds.center().x(), -bounds.center().y());
                const QTransform proposal = current * adjust;
                double score = 0.0;
                evaluate(proposal, &score);
                if (score > nextScore) {
                    next = proposal;
                    nextScore = score;
                }
            }
        }
        if (nextScore > currentScore) {
            current = next;
            currentScore = nextScore;
        } else {
            step *= 0.5;
        }
    }
    if (best) {
        best->path = painterPath(best->polygons);
        best->bounds = best->path.boundingRect();
    }

    return best;
}

Polygons selectedUnion(const QVector<Candidate> &candidates, const QVector<int> &selected) {
    Polygons parts;
    for (int index : selected) {
        parts += candidates[index].polygons;
    }

    return unite(parts);
}

QVector<QPointF> interiorWitnesses(const Polygons &polygons, int maximum) {
    QVector<QPointF> result;
    const QPainterPath path = painterPath(polygons);
    for (const QPolygonF &polygon : polygons) {
        if (signedArea(polygon) <= 0.0) {
            continue;
        }
        QVector<double> heights;
        for (const QPointF &point : polygon) {
            heights.push_back(point.y());
        }
        std::sort(heights.begin(), heights.end());
        heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
        for (int row = 0; row + 1 < heights.size() && result.size() < maximum; ++row) {
            QVector<double> intersections;
            const double height = (heights[row] + heights[row + 1]) * 0.5;
            for (const QPolygonF &loop : polygons) {
                for (int index = 0; index < loop.size(); ++index) {
                    const QPointF &left = loop[index];
                    const QPointF &right = loop[(index + 1) % loop.size()];
                    if ((left.y() > height) != (right.y() > height)) {
                        intersections.push_back(left.x() + (right.x() - left.x())
                            * (height - left.y()) / (right.y() - left.y()));
                    }
                }
            }
            std::sort(intersections.begin(), intersections.end());
            for (int index = 0; index + 1 < intersections.size() && result.size() < maximum; ++index) {
                const QPointF point((intersections[index] + intersections[index + 1]) * 0.5, height);
                if (path.contains(point)) {
                    result.push_back(point);
                }
            }
        }
        if (result.size() >= maximum) {
            break;
        }
    }

    return result;
}

bool containsWitness(const Candidate &candidate, const QPointF &point) {
    if (!candidate.bounds.contains(point)) {
        return false;
    }
    if (candidate.path.contains(point)) {
        return true;
    }
    for (const QPolygonF &polygon : candidate.polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF edge = polygon[(index + 1) % polygon.size()] - polygon[index];
            const QPointF offset = point - polygon[index];
            const double length = QPointF::dotProduct(edge, edge);
            const double position = QPointF::dotProduct(offset, edge);
            const double determinant = edge.x() * offset.y() - edge.y() * offset.x();
            if (position >= 0.0 && position <= length
                && determinant == 0.0) {
                return true;
            }
        }
    }

    return false;
}

void buildWitnessConstraints(SearchState *state, const QVector<Candidate> &candidates) {
    state->choices = QVector<QVector<int>>(state->witnesses.size());
    state->memberships = QVector<QBitArray>(candidates.size(), QBitArray(state->witnesses.size()));
    for (int candidate = 0; candidate < candidates.size(); ++candidate) {
        for (int witness = 0; witness < state->witnesses.size(); ++witness) {
            if (containsWitness(candidates[candidate], state->witnesses[witness])) {
                state->choices[witness].push_back(candidate);
                state->memberships[candidate].setBit(witness);
            }
        }
    }
}

void retainSelection(SearchState *state, const QVector<int> &selected,
                     const QVector<Candidate> &candidates, const Region &region) {
    const Polygons coverage = selectedUnion(candidates, selected);
    const Polygons residual = subtract(region.required, coverage);
    if (!residual.isEmpty()) {
        if (state->discoveredResidual.isEmpty()) {
            state->discoveredResidual = residual;
        }
        return;
    }
    const double spill = rankValue(area(coverage) - region.area, region);
    QVector<int> canonical = selected;
    std::sort(canonical.begin(), canonical.end());
    if (selected.size() < state->best.size()
        || (selected.size() == state->best.size()
            && (spill < state->spill || (spill == state->spill && canonical < state->best)))) {
        state->best = std::move(canonical);
        state->spill = spill;
    }
}

bool hasExclusiveWitness(int candidateIndex, const QVector<int> &selected,
                         const QVector<Candidate> &candidates, const Region &region) {
    const Candidate &candidate = candidates[candidateIndex];
    const QPolygonF &polygon = candidate.polygons.front();
    QPointF center;
    for (const QPointF &point : polygon) {
        center += point;
    }
    center /= polygon.size();
    const auto exclusive = [&](const QPointF &point) {
        return region.requiredPath.contains(point) && containsWitness(candidate, point)
            && std::none_of(selected.begin(), selected.end(), [&](int index) {
                return index != candidateIndex && containsWitness(candidates[index], point);
            });
    };
    if (exclusive(center)) {
        return true;
    }
    const int stride = std::max(1, static_cast<int>(polygon.size()) / kPruningBoundarySamples);
    for (int edge = 0; edge < polygon.size(); edge += stride) {
        const QPointF midpoint = (polygon[edge] + polygon[(edge + 1) % polygon.size()]) * 0.5;
        for (double fraction : kPruningInsetFractions) {
            if (exclusive(midpoint * (1.0 - fraction) + center * fraction)) {
                return true;
            }
        }
    }

    return false;
}

QVector<int> pruneSelection(QVector<int> selected, const QVector<Candidate> &candidates,
                           const Region &region, const std::function<bool()> &cancelled) {
    for (int position = selected.size() - 1; position >= 0; --position) {
        if (isCancelled(cancelled)) {
            break;
        }
        if (hasExclusiveWitness(selected[position], selected, candidates, region)) {
            continue;
        }
        QVector<int> trial = selected;
        trial.removeAt(position);
        if (subtract(region.required, selectedUnion(candidates, trial)).isEmpty()) {
            selected = std::move(trial);
        }
    }

    return selected;
}

void greedySelection(SearchState *state, const QVector<Candidate> &candidates,
                     const Region &region, int meshCount,
                     const std::function<bool()> &cancelled) {
    QVector<int> selected;
    QBitArray remaining(state->witnesses.size(), true);
    while (witnessCount(remaining) > 0 && !isCancelled(cancelled)) {
        int best = -1;
        int bestGain = 0;
        for (int index = 0; index < candidates.size(); ++index) {
            const int gain = witnessCount(remaining & state->memberships[index]);
            if (gain > bestGain
                || (gain == bestGain && gain > 0 && best >= 0
                    && candidates[index].gain > candidates[best].gain)) {
                best = index;
                bestGain = gain;
            }
        }
        if (best < 0) {
            break;
        }
        selected.push_back(best);
        remaining &= ~state->memberships[best];
    }
    Polygons residual = subtract(region.required, selectedUnion(candidates, selected));
    for (int index = 0; index < meshCount && !residual.isEmpty(); ++index) {
        if (isCancelled(cancelled)) {
            return;
        }
        if (!intersect(residual, candidates[index].polygons).isEmpty()) {
            if (!selected.contains(index)) {
                selected.push_back(index);
            }
            residual = subtract(residual, candidates[index].polygons);
        }
    }
    selected = pruneSelection(std::move(selected), candidates, region, cancelled);
    retainSelection(state, selected, candidates, region);
}

void searchSelection(SearchState *state, const QVector<Candidate> &candidates,
                     const Region &region, const FillOptions &options,
                     const QBitArray &remaining, const std::function<bool()> &cancelled) {
    if (state->nodes >= options.searchNodes || isCancelled(cancelled)) {
        return;
    }
    ++state->nodes;
    if (witnessCount(remaining) == 0) {
        retainSelection(state, state->selected, candidates, region);
        return;
    }
    if (state->selected.size() >= state->best.size()
        || state->selected.size() >= kMaximumSearchDepth) {
        return;
    }
    QVector<int> branch;
    for (int witness = 0; witness < state->choices.size(); ++witness) {
        if (!remaining.testBit(witness)) {
            continue;
        }
        QVector<int> choices;
        for (int index : state->choices[witness]) {
            if (!state->forbidden[index]) {
                choices.push_back(index);
            }
        }
        if (choices.isEmpty()) {
            return;
        }
        if (branch.isEmpty() || choices.size() < branch.size()) {
            branch = std::move(choices);
        }
    }
    std::stable_sort(branch.begin(), branch.end(), [&](int left, int right) {
        const int leftGain = witnessCount(remaining & state->memberships[left]);
        const int rightGain = witnessCount(remaining & state->memberships[right]);
        if (leftGain != rightGain) {
            return leftGain > rightGain;
        }
        return candidates[left].gain > candidates[right].gain;
    });
    QVector<int> excluded;
    for (int index : branch) {
        if (state->nodes >= options.searchNodes || isCancelled(cancelled)) {
            break;
        }
        state->selected.push_back(index);
        searchSelection(state, candidates, region, options,
                        remaining & ~state->memberships[index], cancelled);
        state->selected.removeLast();
        state->forbidden[index] = true;
        excluded.push_back(index);
    }
    for (int index : excluded) {
        state->forbidden[index] = false;
    }
}

QVector<int> selectCover(const Region &region, const QVector<Candidate> &candidates,
                        int meshCount, const FillOptions &options,
                        const std::function<bool()> &cancelled,
                        const std::function<void(int, double, double)> &progress,
                        QJsonObject *diagnostics) {
    SearchState state;
    state.best.resize(meshCount);
    std::iota(state.best.begin(), state.best.end(), 0);
    state.spill = rankValue(area(selectedUnion(candidates, state.best)) - region.area, region);
    state.forbidden = QVector<bool>(candidates.size(), false);
    for (int row = 0; row < kWitnessGridSize; ++row) {
        for (int column = 0; column < kWitnessGridSize; ++column) {
            const QPointF point(region.bounds.left() + region.bounds.width() * (column + 0.5) / kWitnessGridSize,
                                 region.bounds.top() + region.bounds.height() * (row + 0.5) / kWitnessGridSize);
            if (region.requiredPath.contains(point)) {
                state.witnesses.push_back(point);
            }
        }
    }
    const int stride = std::max(1, meshCount / kMaximumMeshWitnesses);
    for (int index = 0; index < meshCount; index += stride) {
        state.witnesses += interiorWitnesses(intersect(candidates[index].polygons, region.required), 1);
    }
    if (progress) {
        progress(state.best.size(), region.area, region.area);
    }
    for (int round = 0; round < kMaximumWitnessRounds && !isCancelled(cancelled); ++round) {
        state.discoveredResidual.clear();
        buildWitnessConstraints(&state, candidates);
        if (progress) {
            progress(state.best.size(), region.area, region.area);
        }
        greedySelection(&state, candidates, region, meshCount, cancelled);
        searchSelection(&state, candidates, region, options,
                        QBitArray(state.witnesses.size(), true), cancelled);
        if (progress) {
            progress(state.best.size(), region.area, region.area);
        }
        if (state.discoveredResidual.isEmpty() || state.nodes >= options.searchNodes) {
            break;
        }
        const QVector<QPointF> more = interiorWitnesses(state.discoveredResidual, kWitnessesPerRound);
        if (more.isEmpty()) {
            break;
        }
        state.witnesses += more;
    }
    diagnostics->insert(QStringLiteral("searchNodes"), state.nodes);
    diagnostics->insert(QStringLiteral("coverageWitnesses"), state.witnesses.size());
    diagnostics->insert(QStringLiteral("searchNodeLimitReached"),
                        options.searchNodes > 0 && state.nodes >= options.searchNodes);
    diagnostics->insert(QStringLiteral("optimality"), QStringLiteral("bounded candidate search; no global optimum claim"));

    return pruneSelection(state.best, candidates, region, cancelled);
}

} // namespace

FillResult fillRegion(const PenFillRequest &request, const QVector<Primitive> &primitives,
                      const FillOptions &options, const std::function<bool()> &cancelled,
                      const std::function<void(int, double, double)> &progress) {
    FillResult result;
    result.fill.shapeLimit = options.shapeBudget;
    result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("parameters"));
    result.diagnostics.insert(QStringLiteral("geometryModel"), QStringLiteral("world polygons on a 1e-6 grid; reconstructed float transforms"));
    result.diagnostics.insert(QStringLiteral("catalogShapes"), primitives.size());
    result.diagnostics.insert(QStringLiteral("boundaryTolerance"), request.boundaryTolerance);
    try {
        if (!std::isfinite(request.boundaryTolerance) || request.boundaryTolerance < kMinimumTolerance
            || options.shapeBudget <= 0 || options.candidateLimit < 0
            || options.searchNodes < 0 || options.refinementSteps < 0 || primitives.isEmpty()) {
            throw std::runtime_error("Catalog cover parameters or primitive catalog are invalid");
        }
        if (isCancelled(cancelled)) {
            result.fill.cancelled = true;
            return result;
        }
        result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("region"));
        const Region region = buildRegion(request, cancelled);
        result.fill.targetArea = region.originalArea;
        result.diagnostics.insert(QStringLiteral("requiredArea"), region.area);
        result.diagnostics.insert(QStringLiteral("enclosureExtraArea"), region.area - region.originalArea);
        result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("completion"));
        QVector<Candidate> candidates = completeCover(region, primitives, cancelled);
        if (isCancelled(cancelled)) {
            result.fill.cancelled = true;
            return result;
        }
        int meshCount = candidates.size();
        result.diagnostics.insert(QStringLiteral("meshPlacements"), meshCount);
        result.diagnostics.insert(QStringLiteral("completionRectangles"), static_cast<int>(
            std::count_if(candidates.begin(), candidates.end(), [](const Candidate &candidate) {
                return candidate.placement.shapeId == 101;
            })));
        if (meshCount == 0) {
            throw std::runtime_error("Catalog cover completion mesh is empty");
        }
        QVector<int> meshIndices(meshCount);
        std::iota(meshIndices.begin(), meshIndices.end(), 0);
        const Polygons meshMissing = subtract(region.required, selectedUnion(candidates, meshIndices));
        if (!meshMissing.isEmpty()) {
            throw std::runtime_error(QStringLiteral("Catalog cover completion mesh leaves %1 uncovered area in %2 components")
                                         .arg(area(meshMissing), 0, 'g', 12).arg(meshMissing.size()).toStdString());
        }
        if (progress) {
            progress(meshCount, region.area, region.area);
        }
        int screened = 0;
        int refined = 0;
        QVector<Seed> seeds;
        result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("candidates"));
        if (options.candidateLimit > 0) {
            seeds = generateSeeds(region, primitives, cancelled, &screened);
        }
        for (const Seed &seed : seeds) {
            if (isCancelled(cancelled) || candidates.size() - meshCount >= options.candidateLimit) {
                break;
            }
            std::optional<Candidate> candidate = polishSeed(seed, primitives[seed.primitive].shape,
                                                           region, options, cancelled, &refined);
            if (candidate && subtract(expanded(candidate->polygons, kVerificationClearance),
                                      region.permitted).isEmpty()) {
                const bool duplicate = std::any_of(candidates.begin(), candidates.end(),
                    [&](const Candidate &existing) {
                        return existing.placement.shapeId == candidate->placement.shapeId
                            && existing.placement.transform == candidate->placement.transform;
                    });
                if (!duplicate) {
                    candidates.push_back(std::move(*candidate));
                }
            }
            if (progress) {
                progress(meshCount, region.area, region.area);
            }
        }
        if (isCancelled(cancelled)) {
            result.fill.cancelled = true;
            return result;
        }
        result.diagnostics.insert(QStringLiteral("screenedTransforms"), screened);
        result.diagnostics.insert(QStringLiteral("refinedTransforms"), refined);
        result.diagnostics.insert(QStringLiteral("catalogCandidates"), candidates.size() - meshCount);
        if (options.candidateLimit > 0) {
            result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("core-remeshing"));
            const QVector<Candidate> proposals = candidates.mid(meshCount);
            try {
                QVector<Candidate> compact = compactCover(request, region, primitives, proposals,
                                                           cancelled, &result.diagnostics);
                if (!compact.isEmpty() && compact.size() < meshCount) {
                    meshCount = compact.size();
                    candidates = std::move(compact);
                    candidates += proposals;
                }
            } catch (const std::exception &failure) {
                result.diagnostics.insert(QStringLiteral("compactCoverError"), QString::fromUtf8(failure.what()));
            }
        }
        if (isCancelled(cancelled)) {
            result.fill.cancelled = true;
            return result;
        }
        result.diagnostics.insert(QStringLiteral("startingCoverPlacements"), meshCount);
        result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("selection"));
        const QVector<int> selected = selectCover(region, candidates, meshCount, options,
                                                  cancelled, progress, &result.diagnostics);
        result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("verification"));
        const Polygons coverage = selectedUnion(candidates, selected);
        const Polygons missing = subtract(region.required, coverage);
        const Polygons outside = subtract(coverage, region.permitted);
        result.fill.cancelled = isCancelled(cancelled);
        result.fill.coveredArea = missing.isEmpty() ? region.originalArea
            : std::max(0.0, region.originalArea - area(missing));
        result.fill.outsideArea = std::max(0.0, area(coverage) - region.originalArea);
        result.fill.unfilled = painterPath(missing);
        result.diagnostics.insert(QStringLiteral("missingArea"), area(missing));
        result.diagnostics.insert(QStringLiteral("outsideEnvelopeArea"), area(outside));
        result.diagnostics.insert(QStringLiteral("coverageVerifiedOnGrid"), missing.isEmpty() && outside.isEmpty());
        if (!missing.isEmpty() || !outside.isEmpty()) {
            throw std::runtime_error("Catalog cover final coverage or spill verification failed");
        }
        if (selected.size() > options.shapeBudget) {
            throw std::runtime_error(QStringLiteral("Complete catalog cover needs %1 shapes; budget is %2")
                                         .arg(selected.size()).arg(options.shapeBudget).toStdString());
        }
        for (int index : selected) {
            PenPlacement placement = candidates[index].placement;
            placement.area = candidates[index].gain;
            result.fill.placements.push_back(placement);
        }
        std::sort(result.fill.placements.begin(), result.fill.placements.end(),
            [](const PenPlacement &left, const PenPlacement &right) {
                return left.shapeId == right.shapeId ? transformLess(left.transform, right.transform)
                                                     : left.shapeId < right.shapeId;
            });
        result.diagnostics.insert(QStringLiteral("placements"), result.fill.placements.size());
        QJsonObject shapeCounts;
        for (const PenPlacement &placement : result.fill.placements) {
            const QString key = QString::number(placement.shapeId);
            shapeCounts.insert(key, shapeCounts.value(key).toInt() + 1);
        }
        result.diagnostics.insert(QStringLiteral("shapeCounts"), shapeCounts);
        result.diagnostics.insert(QStringLiteral("stage"), QStringLiteral("complete"));
    } catch (const std::exception &failure) {
        result.fill.placements.clear();
        result.fill.cancelled = isCancelled(cancelled);
        result.fill.error = QString::fromUtf8(failure.what());
    }

    return result;
}

} // namespace gui::catalog
