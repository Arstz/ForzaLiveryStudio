#include "compact_fit_catalog_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace gui::catalog {
namespace {

constexpr int kMaximumLeewayPairs = 8192;
constexpr int kMaximumLeewayChecks = 512;
constexpr int kMaximumLeewayEdits = 32;
constexpr double kMinimumIntersectionSine = 0.001;
constexpr double kMaximumExtensionLengthRatio = 4.0;
constexpr double kCollinearSine = 1e-12;

double cross(const QPointF &first, const QPointF &second) {
    return first.x() * second.y() - first.y() * second.x();
}

int anchorCount(const QVector<PenLoop> &loops) {
    int count = 0;
    for (const auto &loop : loops)
        count += loop.points.size();

    return count;
}

bool collinear(const QPointF &previous, const QPointF &current, const QPointF &next) {
    const auto incoming = current - previous;
    const auto outgoing = next - current;
    const double lengthProduct = std::hypot(incoming.x(), incoming.y())
        * std::hypot(outgoing.x(), outgoing.y());

    return QPointF::dotProduct(incoming, outgoing) > 0.0
        && std::abs(cross(incoming, outgoing)) <= kCollinearSine * lengthProduct;
}

void removeCollinearAnchors(QVector<PenPoint> *points) {
    int index = 0;
    while (points->size() > 3 && index < points->size()) {
        const int count = points->size();
        const auto &previous = (*points)[(index + count - 1) % count];
        const auto &current = (*points)[index];
        const auto &next = (*points)[(index + 1) % count];
        if (previous.kind == PenPointKind::Hard && current.kind == PenPointKind::Hard
            && next.kind == PenPointKind::Hard
            && collinear(previous.position, current.position, next.position)) {
            points->removeAt(index);
            index = 0;
        } else {
            ++index;
        }
    }
}

std::optional<QPointF> directionIntersection(const QVector<PenPoint> &points,
                                            const QVector<PenBoundarySegment> &segments,
                                            int first, int span, const QRectF &bounds) {
    const int count = points.size();
    const int last = (first + span) % count;
    const auto &incoming = segments[(first + count - 1) % count];
    const auto &outgoing = segments[last];
    const auto start = points[first].position;
    const auto end = points[last].position;
    const auto forward = incoming.end - incoming.start;
    const auto backward = outgoing.start - outgoing.end;
    const double forwardLength = std::hypot(forward.x(), forward.y());
    const double backwardLength = std::hypot(backward.x(), backward.y());
    const double denominator = cross(forward, backward);
    double chainLength = 0.0;

    if (incoming.curved || outgoing.curved
        || std::abs(denominator) <= kMinimumIntersectionSine * forwardLength * backwardLength)
        return std::nullopt;
    const double forwardFraction = cross(end - start, backward) / denominator;
    const double backwardFraction = cross(end - start, forward) / denominator;
    if (forwardFraction <= 0.0 || backwardFraction <= 0.0)
        return std::nullopt;
    for (int offset = 0; offset < span; ++offset)
        chainLength += segments[(first + offset) % count].controlLength();
    if (std::max(forwardFraction * forwardLength, backwardFraction * backwardLength)
        > chainLength * kMaximumExtensionLengthRatio)
        return std::nullopt;
    const auto intersection = start + forward * forwardFraction;
    if (!std::isfinite(intersection.x()) || !std::isfinite(intersection.y())
        || !bounds.contains(intersection))
        return std::nullopt;

    return intersection;
}

QVector<PenPoint> replacementChain(const QVector<PenPoint> &points, int first, int span,
                                  const std::optional<QPointF> &intersection) {
    QVector<PenPoint> result{points[first]};
    const int count = points.size();

    if (intersection)
        result.push_back({*intersection, PenPointKind::Hard});
    for (int offset = span; offset < count; ++offset)
        result.push_back(points[(first + offset) % count]);
    removeCollinearAnchors(&result);

    return result;
}

bool preservesCoverage(const Region &candidate, const Region &coverage) {
    return subtract(coverage.required, candidate.required).isEmpty()
        && subtract(candidate.required, coverage.spillFree).isEmpty();
}

} // namespace

Region simplifyLeewayRegion(const PenFillRequest &request, const Region &authored,
                            const Region &coverage, const std::function<bool()> &cancelled,
                            QJsonObject *diagnostics) {
    auto working = request;
    auto result = authored;
    const auto leewayPath = painterPath(coverage.leeway);
    const auto leewayBounds = leewayPath.boundingRect();
    int pairs = 0;
    int checks = 0;
    int shortcuts = 0;
    int intersections = 0;

    if (working.loops.isEmpty() && !working.points.isEmpty())
        working.loops = {{working.points, PenLoopKind::Outer}};
    for (auto &loop : working.loops)
        materializePenHandles(loop.points);
    const int originalAnchors = anchorCount(working.loops);
    const auto stopped = [&] {
        return coverage.leeway.isEmpty() || pairs >= kMaximumLeewayPairs
            || checks >= kMaximumLeewayChecks || (cancelled && cancelled());
    };
    for (int edit = 0; edit < kMaximumLeewayEdits && !stopped(); ++edit) {
        auto best = working;
        auto bestRegion = result;
        int bestAnchors = anchorCount(working.loops);
        bool bestIntersection = false;

        for (int loopIndex = 0; loopIndex < working.loops.size() && !stopped(); ++loopIndex) {
            const auto &points = working.loops[loopIndex].points;
            const auto segments = penSegments(points);
            const int count = points.size();

            for (int first = 0; first < count && !stopped(); ++first) {
                ++pairs;
                if (points[first].kind != PenPointKind::Hard)
                    continue;
                QPolygonF chain{points[first].position, points[(first + 1) % count].position};
                for (int span = 2; span <= count - 2 && !stopped(); ++span) {
                    const int middle = (first + span - 1) % count;
                    const int last = (first + span) % count;
                    const int previous = (middle + count - 1) % count;

                    ++pairs;
                    if (!leewayPath.contains(points[middle].position)
                        && (segments[previous].curved || segments[middle].curved
                            || !collinear(points[previous].position, points[middle].position, points[last].position)))
                        break;
                    chain.push_back(points[last].position);
                    if (points[last].kind != PenPointKind::Hard
                        || !chain.boundingRect().intersects(leewayBounds))
                        continue;
                    const auto intersection = directionIntersection(points, segments, first, span, leewayBounds);
                    for (bool extend : {false, true}) {
                        if (stopped() || (extend && !intersection))
                            continue;
                        auto trial = working;
                        trial.loops[loopIndex].points = replacementChain(points, first, span,
                            extend ? intersection : std::nullopt);
                        if (anchorCount(trial.loops) >= bestAnchors)
                            continue;
                        ++checks;
                        try {
                            auto candidate = buildRegion(trial, cancelled);
                            if (!preservesCoverage(candidate, coverage))
                                continue;
                            bestAnchors = anchorCount(trial.loops);
                            best = std::move(trial);
                            bestRegion = std::move(candidate);
                            bestIntersection = extend;
                        } catch (const std::runtime_error &) {
                        }
                    }
                }
            }
        }
        if (bestAnchors == anchorCount(working.loops))
            break;
        working = std::move(best);
        result = std::move(bestRegion);
        if (bestIntersection)
            ++intersections;
        else
            ++shortcuts;
    }
    if (diagnostics)
        *diagnostics = {{QStringLiteral("anchorsBefore"), originalAnchors},
            {QStringLiteral("anchorsAfter"), anchorCount(working.loops)},
            {QStringLiteral("shortcuts"), shortcuts}, {QStringLiteral("intersections"), intersections},
            {QStringLiteral("pairs"), pairs}, {QStringLiteral("geometryChecks"), checks}};

    return result;
}

} // namespace gui::catalog
