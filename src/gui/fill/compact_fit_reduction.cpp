#include "compact_fit_reduction.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace gui::compact {
namespace {

constexpr double kComparisonSlack = 1e-7;
constexpr int kExactCandidateTrials = 64;
constexpr int kExactContainmentTrials = 512;
constexpr int kExactNeighbors = 4;
constexpr int kSquareShapeId = 101;

catalog::Polygons exactSupport(const QVector<ReusableCandidate> &pieces, const QVector<int> &excluded = {}) {
    catalog::Polygons polygons;
    for (int index = 0; index < pieces.size(); ++index) {
        if (!excluded.contains(index)) {
            polygons += pieces[index].polygons;
        }
    }

    return catalog::unite(polygons);
}

bool sameCoverage(const catalog::Polygons &first, const catalog::Polygons &second) {
    return catalog::subtract(first, second).isEmpty() && catalog::subtract(second, first).isEmpty();
}

std::optional<ReusableCandidate> exactPiece(const PenPlacement &placement,
    const QVector<catalog::Primitive> &primitives, bool reconstruct) {
    const auto primitive = std::find_if(primitives.cbegin(), primitives.cend(), [&](const auto &entry) {
        return entry.shape.shapeId == placement.shapeId;
    });
    if (primitive == primitives.cend()) {
        return {};
    }
    ReusableCandidate result;
    result.placement = placement;
    if (reconstruct) {
        result.placement.transform = catalog::emittedTransform(placement.transform);
    }
    result.polygons = catalog::mapped(primitive->shape, result.placement.transform);
    result.bounds = catalog::painterPath(result.polygons).boundingRect();
    result.placement.area = catalog::area(result.polygons);

    return result;
}

QVector<QRectF> ownershipBounds(const QVector<ReusableCandidate> &pieces, CoverageOwnership *ownership) {
    QVector<catalog::Polygons> polygons;
    QVector<QRectF> bounds;
    for (const auto &piece : pieces) {
        polygons.push_back(piece.polygons);
    }
    ownership->synchronize(polygons);
    for (int index = 0; index < pieces.size(); ++index) {
        bounds.push_back(catalog::painterPath(ownership->exclusive({index})).boundingRect());
    }

    return bounds;
}

QVector<ReusableCandidate> exactEnvelopeCandidates(const QVector<ReusableCandidate> &pieces,
    const QVector<catalog::Primitive> &primitives, CoverageOwnership *ownership,
    const std::function<bool()> &cancelled) {
    const auto square = std::find_if(primitives.cbegin(), primitives.cend(), [](const auto &entry) {
        return entry.shape.shapeId == kSquareShapeId;
    });
    QVector<ReusableCandidate> result;
    if (square == primitives.cend()) {
        return result;
    }
    for (int first = 0; first < pieces.size() && !(cancelled && cancelled()); ++first) {
        QVector<std::pair<double, int>> neighbors;
        for (int second = first + 1; second < pieces.size(); ++second) {
            if (pieces[first].bounds.intersects(pieces[second].bounds.adjusted(
                    -catalog::kVerificationClearance, -catalog::kVerificationClearance,
                    catalog::kVerificationClearance, catalog::kVerificationClearance))) {
                const auto delta = pieces[first].bounds.center() - pieces[second].bounds.center();
                neighbors.push_back({QPointF::dotProduct(delta, delta), second});
            }
        }
        std::sort(neighbors.begin(), neighbors.end());
        for (int neighbor = 0; neighbor < std::min(kExactNeighbors, static_cast<int>(neighbors.size())); ++neighbor) {
            if (cancelled && cancelled()) {
                return result;
            }
            const auto required = ownership->exclusive({first, neighbors[neighbor].second});
            bool invertible = false;
            const auto inverse = pieces[first].placement.transform.inverted(&invertible);
            if (required.isEmpty() || !invertible) {
                continue;
            }
            for (const auto &frame : {QTransform(), inverse}) {
                const auto bounds = frame.map(catalog::painterPath(required)).boundingRect();
                const auto source = square->shape.bounds;
                QTransform transform;
                transform.translate(bounds.left(), bounds.top());
                transform.scale(bounds.width() / source.width(), bounds.height() / source.height());
                transform.translate(-source.left(), -source.top());
                PenPlacement placement;
                placement.shapeId = kSquareShapeId;
                placement.transform = transform * frame.inverted();
                const auto candidate = exactPiece(placement, primitives, true);
                if (candidate && !candidate->polygons.isEmpty()) {
                    result.push_back(*candidate);
                }
            }
        }
    }

    return result;
}

bool noGreater(double after, double before) {
    return std::isfinite(after) && after <= before + kComparisonSlack;
}

} // namespace

void CoverageOwnership::synchronize(const QVector<catalog::Polygons> &pieces) {
    if (pieces_ == pieces) {
        return;
    }
    pieces_ = pieces;
    bounds_.clear();
    neighbors_ = QVector<QVector<int>>(pieces.size());
    exclusive_ = QVector<std::optional<catalog::Polygons>>(pieces.size());
    failures_.clear();
    ++rebuilds_;
    for (const auto &piece : pieces) {
        bounds_.push_back(catalog::painterPath(piece).boundingRect());
    }
    for (int first = 0; first < pieces.size(); ++first) {
        for (int second = first + 1; second < pieces.size(); ++second) {
            if (bounds_[first].intersects(bounds_[second])) {
                neighbors_[first].push_back(second);
                neighbors_[second].push_back(first);
            }
        }
    }
}

catalog::Polygons CoverageOwnership::exclusive(const QVector<int> &members) const {
    if (members.isEmpty()) {
        return {};
    }
    if (members.size() == 1 && exclusive_[members.front()]) {
        ++hits_;
        return *exclusive_[members.front()];
    }
    auto ordered = members;
    catalog::Polygons local;
    catalog::Polygons overlapping;
    QVector<bool> included(pieces_.size(), false);
    QVector<bool> adjacent(pieces_.size(), false);
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    for (int member : ordered) {
        included[member] = true;
        local += pieces_[member];
        for (int neighbor : neighbors_[member]) {
            adjacent[neighbor] = true;
        }
    }
    for (int index = 0; index < pieces_.size(); ++index) {
        if (adjacent[index] && !included[index]) {
            overlapping += pieces_[index];
        }
    }
    auto result = catalog::subtract(catalog::unite(local), catalog::unite(overlapping));
    ++computations_;
    if (ordered.size() == 1) {
        exclusive_[ordered.front()] = result;
    }

    return result;
}

void CoverageOwnership::erase(int index) {
    for (int neighbor : neighbors_[index]) {
        if (exclusive_[neighbor]) {
            exclusive_[neighbor].reset();
            ++invalidations_;
        }
    }
    pieces_.removeAt(index);
    bounds_.removeAt(index);
    neighbors_.removeAt(index);
    exclusive_.removeAt(index);
    failures_.clear();
    for (auto &neighbors : neighbors_) {
        neighbors.removeAll(index);
        for (auto &neighbor : neighbors) {
            neighbor -= neighbor > index;
        }
    }
}

bool CoverageOwnership::failed(const QVector<int> &members, bool broad) const {
    if (failures_.contains(groupKey(members, broad))) {
        ++failureHits_;
        return true;
    }

    return false;
}

void CoverageOwnership::rememberFailure(const QVector<int> &members, bool broad) {
    failures_.insert(groupKey(members, broad));
}

QJsonObject CoverageOwnership::diagnostics() const {
    return {{QStringLiteral("computations"), computations_}, {QStringLiteral("cacheHits"), hits_},
        {QStringLiteral("rebuilds"), rebuilds_}, {QStringLiteral("neighborInvalidations"), invalidations_},
        {QStringLiteral("failedGroupHits"), failureHits_}};
}

QString CoverageOwnership::groupKey(const QVector<int> &members, bool broad) {
    auto ordered = members;
    QString result = broad ? QStringLiteral("b") : QStringLiteral("n");
    std::sort(ordered.begin(), ordered.end());
    for (int member : ordered) {
        result += QStringLiteral(":") + QString::number(member);
    }

    return result;
}

ExactReductionResult reduceExactCoverage(const QVector<PenPlacement> &placements,
    const QVector<catalog::Primitive> &primitives, const QVector<ReusableCandidate> &candidates,
    const std::function<bool()> &cancelled) {
    ExactReductionResult result{placements, {}};
    QVector<ReusableCandidate> pieces;
    CoverageOwnership ownership;
    int trials = 0;
    int containmentTrials = 0;
    int unionChecks = 0;
    int merges = 0;
    int deletions = 0;
    if (placements.size() < 2 || (cancelled && cancelled())) {
        return result;
    }
    for (const auto &placement : placements) {
        const auto piece = exactPiece(placement, primitives, false);
        if (!piece) {
            return result;
        }
        pieces.push_back(*piece);
    }
    const auto original = exactSupport(pieces);
    auto bounds = ownershipBounds(pieces, &ownership);
    auto pool = exactEnvelopeCandidates(pieces, primitives, &ownership, cancelled);
    const int envelopes = pool.size();
    pool += candidates;
    QVector<bool> tried(pool.size(), false);
    QVector<std::tuple<int, double, int>> ranked;
    bool rankNeeded = true;
    for (int index = pieces.size() - 1; index >= 0 && !(cancelled && cancelled()); --index) {
        if (bounds[index].isEmpty()) {
            ++unionChecks;
            if (sameCoverage(original, exactSupport(pieces, {index}))) {
                pieces.removeAt(index);
                ownership.erase(index);
                ++deletions;
            }
        }
    }
    bounds = ownershipBounds(pieces, &ownership);
    while (pieces.size() > 1 && trials < kExactCandidateTrials && containmentTrials < kExactContainmentTrials
            && !(cancelled && cancelled())) {
        if (rankNeeded) {
            ranked.clear();
            for (int candidate = 0; candidate < pool.size() && !(cancelled && cancelled()); ++candidate) {
                if (tried[candidate]) {
                    continue;
                }
                const auto frame = pool[candidate].bounds.adjusted(-catalog::kVerificationClearance,
                    -catalog::kVerificationClearance, catalog::kVerificationClearance, catalog::kVerificationClearance);
                int count = 0;
                for (int index = 0; index < pieces.size(); ++index) {
                    count += !bounds[index].isEmpty() && frame.contains(bounds[index]);
                }
                if (count > 1) {
                    ranked.push_back({-count, frame.width() * frame.height(), candidate});
                }
            }
            std::sort(ranked.rbegin(), ranked.rend());
            rankNeeded = false;
        }
        if (ranked.isEmpty() || (cancelled && cancelled())) {
            break;
        }
        const int best = std::get<2>(ranked.back());
        ranked.removeLast();
        tried[best] = true;
        ++containmentTrials;
        const auto replacement = exactPiece(pool[best].placement, primitives, true);
        if (!replacement || replacement->polygons.isEmpty()
            || !catalog::subtract(replacement->polygons, original).isEmpty()) {
            continue;
        }
        ++trials;
        QVector<int> removed;
        const auto frame = replacement->bounds.adjusted(-catalog::kVerificationClearance,
            -catalog::kVerificationClearance, catalog::kVerificationClearance, catalog::kVerificationClearance);
        for (int index = 0; index < pieces.size(); ++index) {
            if (!bounds[index].isEmpty() && frame.contains(bounds[index])
                && catalog::subtract(ownership.exclusive({index}), replacement->polygons).isEmpty()) {
                removed.push_back(index);
            }
        }
        while (removed.size() > 1 && !(cancelled && cancelled())) {
            const auto required = ownership.exclusive(removed);
            const auto missing = catalog::subtract(required, replacement->polygons);
            if (!missing.isEmpty()) {
                int restore = -1;
                double most = 0.0;
                for (int position = 0; position < removed.size(); ++position) {
                    const double area = catalog::area(catalog::intersect(missing, pieces[removed[position]].polygons));
                    if (area > most) {
                        most = area;
                        restore = position;
                    }
                }
                if (restore < 0) {
                    break;
                }
                removed.removeAt(restore);
                continue;
            }
            auto trial = pieces;
            for (int position = removed.size() - 1; position >= 0; --position) {
                trial.removeAt(removed[position]);
            }
            trial.push_back(*replacement);
            ++unionChecks;
            if (sameCoverage(original, exactSupport(trial))) {
                pieces = std::move(trial);
                bounds = ownershipBounds(pieces, &ownership);
                rankNeeded = true;
                ++merges;
            }
            break;
        }
    }
    result.placements.clear();
    for (const auto &piece : pieces) {
        result.placements.push_back(piece.placement);
    }
    result.diagnostics = {{QStringLiteral("before"), placements.size()}, {QStringLiteral("after"), pieces.size()},
        {QStringLiteral("candidateTrials"), trials}, {QStringLiteral("candidateLimit"), kExactCandidateTrials},
        {QStringLiteral("containmentTrials"), containmentTrials}, {QStringLiteral("containmentLimit"), kExactContainmentTrials},
        {QStringLiteral("envelopeCandidates"), envelopes}, {QStringLiteral("poolSize"), pool.size()},
        {QStringLiteral("unionChecks"), unionChecks}, {QStringLiteral("merges"), merges},
        {QStringLiteral("deletions"), deletions}, {QStringLiteral("ownership"), ownership.diagnostics()}};

    return result;
}

void CoverageOwnership::synchronize(const QVector<catalog::Polygons> &pieces) {
    if (pieces_ == pieces) {
        return;
    }
    pieces_ = pieces;
    bounds_.clear();
    neighbors_ = QVector<QVector<int>>(pieces.size());
    exclusive_ = QVector<std::optional<catalog::Polygons>>(pieces.size());
    failures_.clear();
    ++rebuilds_;
    for (const auto &piece : pieces) {
        bounds_.push_back(catalog::painterPath(piece).boundingRect());
    }
    for (int first = 0; first < pieces.size(); ++first) {
        for (int second = first + 1; second < pieces.size(); ++second) {
            if (bounds_[first].intersects(bounds_[second])) {
                neighbors_[first].push_back(second);
                neighbors_[second].push_back(first);
            }
        }
    }
}

catalog::Polygons CoverageOwnership::exclusive(const QVector<int> &members) const {
    if (members.isEmpty()) {
        return {};
    }
    if (members.size() == 1 && exclusive_[members.front()]) {
        ++hits_;
        return *exclusive_[members.front()];
    }
    auto ordered = members;
    catalog::Polygons local;
    catalog::Polygons overlapping;
    QVector<bool> included(pieces_.size(), false);
    QVector<bool> adjacent(pieces_.size(), false);
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    for (int member : ordered) {
        included[member] = true;
        local += pieces_[member];
        for (int neighbor : neighbors_[member]) {
            adjacent[neighbor] = true;
        }
    }
    for (int index = 0; index < pieces_.size(); ++index) {
        if (adjacent[index] && !included[index]) {
            overlapping += pieces_[index];
        }
    }
    auto result = catalog::subtract(catalog::unite(local), catalog::unite(overlapping));
    ++computations_;
    if (ordered.size() == 1) {
        exclusive_[ordered.front()] = result;
    }

    return result;
}

void CoverageOwnership::erase(int index) {
    for (int neighbor : neighbors_[index]) {
        if (exclusive_[neighbor]) {
            exclusive_[neighbor].reset();
            ++invalidations_;
        }
    }
    pieces_.removeAt(index);
    bounds_.removeAt(index);
    neighbors_.removeAt(index);
    exclusive_.removeAt(index);
    failures_.clear();
    for (auto &neighbors : neighbors_) {
        neighbors.removeAll(index);
        for (auto &neighbor : neighbors) {
            neighbor -= neighbor > index;
        }
    }
}

bool CoverageOwnership::failed(const QVector<int> &members, bool broad) const {
    if (failures_.contains(groupKey(members, broad))) {
        ++failureHits_;
        return true;
    }

    return false;
}

QJsonObject CoverageOwnership::diagnostics() const {
    return {{QStringLiteral("computations"), computations_}, {QStringLiteral("cacheHits"), hits_},
        {QStringLiteral("rebuilds"), rebuilds_}, {QStringLiteral("neighborInvalidations"), invalidations_},
        {QStringLiteral("failedGroupHits"), failureHits_}};
}

QString CoverageOwnership::groupKey(const QVector<int> &members, bool broad) {
    auto ordered = members;
    QString result = broad ? QStringLiteral("b") : QStringLiteral("n");
    std::sort(ordered.begin(), ordered.end());
    for (int member : ordered) {
        result += QStringLiteral(":") + QString::number(member);
    }

    return result;
}

ExactReductionResult reduceExactCoverage(const QVector<PenPlacement> &placements,
    const QVector<catalog::Primitive> &primitives, const QVector<ReusableCandidate> &candidates,
    const std::function<bool()> &cancelled) {
    ExactReductionResult result{placements, {}};
    QVector<ReusableCandidate> pieces;
    CoverageOwnership ownership;
    int trials = 0;
    int containmentTrials = 0;
    int unionChecks = 0;
    int merges = 0;
    int deletions = 0;
    if (placements.size() < 2 || (cancelled && cancelled())) {
        return result;
    }
    for (const auto &placement : placements) {
        const auto piece = exactPiece(placement, primitives, false);
        if (!piece) {
            return result;
        }
        pieces.push_back(*piece);
    }
    const auto original = exactSupport(pieces);
    auto bounds = ownershipBounds(pieces, &ownership);
    auto pool = exactEnvelopeCandidates(pieces, primitives, &ownership, cancelled);
    const int envelopes = pool.size();
    pool += candidates;
    QVector<bool> tried(pool.size(), false);
    QVector<std::tuple<int, double, int>> ranked;
    bool rankNeeded = true;
    for (int index = pieces.size() - 1; index >= 0 && !(cancelled && cancelled()); --index) {
        if (bounds[index].isEmpty()) {
            ++unionChecks;
            if (sameCoverage(original, exactSupport(pieces, {index}))) {
                pieces.removeAt(index);
                ownership.erase(index);
                ++deletions;
            }
        }
    }
    bounds = ownershipBounds(pieces, &ownership);
    while (pieces.size() > 1 && trials < kExactCandidateTrials && containmentTrials < kExactContainmentTrials
            && !(cancelled && cancelled())) {
        if (rankNeeded) {
            ranked.clear();
            for (int candidate = 0; candidate < pool.size() && !(cancelled && cancelled()); ++candidate) {
                if (tried[candidate]) {
                    continue;
                }
                const auto frame = pool[candidate].bounds.adjusted(-catalog::kVerificationClearance,
                    -catalog::kVerificationClearance, catalog::kVerificationClearance, catalog::kVerificationClearance);
                int count = 0;
                for (int index = 0; index < pieces.size(); ++index) {
                    count += !bounds[index].isEmpty() && frame.contains(bounds[index]);
                }
                if (count > 1) {
                    ranked.push_back({-count, frame.width() * frame.height(), candidate});
                }
            }
            std::sort(ranked.rbegin(), ranked.rend());
            rankNeeded = false;
        }
        if (ranked.isEmpty() || (cancelled && cancelled())) {
            break;
        }
        const int best = std::get<2>(ranked.back());
        ranked.removeLast();
        tried[best] = true;
        ++containmentTrials;
        const auto replacement = exactPiece(pool[best].placement, primitives, true);
        if (!replacement || replacement->polygons.isEmpty()
            || !catalog::subtract(replacement->polygons, original).isEmpty()) {
            continue;
        }
        ++trials;
        QVector<int> removed;
        const auto frame = replacement->bounds.adjusted(-catalog::kVerificationClearance,
            -catalog::kVerificationClearance, catalog::kVerificationClearance, catalog::kVerificationClearance);
        for (int index = 0; index < pieces.size(); ++index) {
            if (!bounds[index].isEmpty() && frame.contains(bounds[index])
                && catalog::subtract(ownership.exclusive({index}), replacement->polygons).isEmpty()) {
                removed.push_back(index);
            }
        }
        while (removed.size() > 1 && !(cancelled && cancelled())) {
            const auto required = ownership.exclusive(removed);
            const auto missing = catalog::subtract(required, replacement->polygons);
            if (!missing.isEmpty()) {
                int restore = -1;
                double most = 0.0;
                for (int position = 0; position < removed.size(); ++position) {
                    const double area = catalog::area(catalog::intersect(missing, pieces[removed[position]].polygons));
                    if (area > most) {
                        most = area;
                        restore = position;
                    }
                }
                if (restore < 0) {
                    break;
                }
                removed.removeAt(restore);
                continue;
            }
            auto trial = pieces;
            for (int position = removed.size() - 1; position >= 0; --position) {
                trial.removeAt(removed[position]);
            }
            trial.push_back(*replacement);
            ++unionChecks;
            if (sameCoverage(original, exactSupport(trial))) {
                pieces = std::move(trial);
                bounds = ownershipBounds(pieces, &ownership);
                rankNeeded = true;
                ++merges;
            }
            break;
        }
    }
    result.placements.clear();
    for (const auto &piece : pieces) {
        result.placements.push_back(piece.placement);
    }
    result.diagnostics = {{QStringLiteral("before"), placements.size()}, {QStringLiteral("after"), pieces.size()},
        {QStringLiteral("candidateTrials"), trials}, {QStringLiteral("candidateLimit"), kExactCandidateTrials},
        {QStringLiteral("containmentTrials"), containmentTrials}, {QStringLiteral("containmentLimit"), kExactContainmentTrials},
        {QStringLiteral("envelopeCandidates"), envelopes}, {QStringLiteral("poolSize"), pool.size()},
        {QStringLiteral("unionChecks"), unionChecks}, {QStringLiteral("merges"), merges},
        {QStringLiteral("deletions"), deletions}, {QStringLiteral("ownership"), ownership.diagnostics()}};

    return result;
}

ReductionState reductionState(const catalog::Polygons &coverage,
                              const catalog::Polygons &required,
                              const catalog::Polygons &visibleTarget,
                              const catalog::Polygons &leeway,
                              const BoundaryModel &boundary,
                              double inwardAllowance) {
    const auto visibleCoverage = leeway.isEmpty()
        ? coverage : catalog::subtract(coverage, leeway);
    const auto observed = boundary.observationSupport(visibleCoverage);
    ReductionState result;
    result.coverage = coverage;
    result.observed = observed;
    result.deepMissing = catalog::subtract(required, catalog::expanded(coverage, inwardAllowance));
    result.metrics = boundary.measure(visibleCoverage, observed);
    for (auto polygon : observed) {
        if (catalog::signedArea(polygon) < 0) {
            std::reverse(polygon.begin(), polygon.end());
            result.observedHoles.push_back(std::move(polygon));
        }
    }
    result.observedHoles = catalog::intersect(catalog::unite(result.observedHoles), visibleTarget);
    catalog::Polygons intendedHoles;
    for (auto polygon : visibleTarget) {
        if (catalog::signedArea(polygon) < 0) {
            std::reverse(polygon.begin(), polygon.end());
            intendedHoles.push_back(std::move(polygon));
        }
    }
    if (!intendedHoles.isEmpty()) {
        result.observedHoles = catalog::subtract(result.observedHoles,
            catalog::expanded(catalog::unite(intendedHoles), inwardAllowance));
    }
    result.cornerDistances = result.metrics.cornerDistances;
    result.missingArea = catalog::area(catalog::subtract(visibleTarget, visibleCoverage));
    result.spillArea = catalog::area(catalog::subtract(visibleCoverage, visibleTarget));

    return result;
}

catalog::Polygons repairResidual(const ReductionState &state, const catalog::Polygons &target,
                                 const catalog::Polygons &interior, const BoundaryMetrics &targetMetrics) {
    const auto visibleMissing = catalog::subtract(target, catalog::unite(state.observed + state.coverage));
    const auto cracks = state.metrics.components > targetMetrics.components
        ? visibleMissing : catalog::intersect(visibleMissing, interior);

    return catalog::unite(state.deepMissing + state.observedHoles + cracks);
}

bool nonWorseningReduction(const ReductionState &after, const ReductionState &before,
                            const BoundaryMetrics &target) {
    const auto &next = after.metrics;
    const auto &previous = before.metrics;
    if (after.coverage.isEmpty() || !noGreater(after.missingArea, before.missingArea)
        || !noGreater(after.spillArea, before.spillArea)
        || !noGreater(next.tangentEnergy, previous.tangentEnergy)
        || !noGreater(next.turnEnergy, previous.turnEnergy)
        || !noGreater(next.cornerEnergy, previous.cornerEnergy)
        || !noGreater(next.maximumDistance, previous.maximumDistance)
        || !noGreater(next.maximumExcessTurn, previous.maximumExcessTurn)
        || !noGreater(next.maximumCornerDistance, previous.maximumCornerDistance)
        || next.cornerDefects > previous.cornerDefects
        || std::abs(next.components - target.components) > std::abs(previous.components - target.components)
        || std::abs(next.holes - target.holes) > std::abs(previous.holes - target.holes)) {
        return false;
    }

    if (after.cornerDistances.size() != before.cornerDistances.size()) {
        return false;
    }
    for (int index = 0; index < after.cornerDistances.size(); ++index) {
        if (!noGreater(after.cornerDistances[index], before.cornerDistances[index])) {
            return false;
        }
    }

    return catalog::area(catalog::subtract(after.deepMissing, before.deepMissing)) <= kComparisonSlack
        && catalog::area(catalog::subtract(after.observedHoles, before.observedHoles)) <= kComparisonSlack;
}

bool preservesCoverage(const ReductionState &after, const ReductionState &before,
                        const BoundaryMetrics &target, double cornerAllowance, bool growing) {
    if (after.coverage.isEmpty() || after.cornerDistances.size() != before.cornerDistances.size()
        || (!growing && std::abs(after.metrics.components - target.components) > std::abs(before.metrics.components - target.components))
        || catalog::area(catalog::subtract(after.deepMissing, before.deepMissing)) > kComparisonSlack
        || catalog::area(catalog::subtract(after.observedHoles, before.observedHoles)) > kComparisonSlack) {
        return false;
    }
    for (int index = 0; index < after.cornerDistances.size(); ++index) {
        if (!noGreater(after.cornerDistances[index], std::max(cornerAllowance, before.cornerDistances[index]))) {
            return false;
        }
    }

    return true;
}

} // namespace gui::compact
