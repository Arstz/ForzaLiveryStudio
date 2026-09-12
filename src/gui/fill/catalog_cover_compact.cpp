#include "catalog_cover_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace gui::catalog {
namespace {

constexpr int kMaximumBoundaryPlacements = 96;
constexpr int kMinimumBoundaryGain = 3;
constexpr int kMaximumSimplificationPasses = 16;
constexpr int kProfileDirections = 8;
constexpr int kMaximumBoundaryJobs = 96;
constexpr int kCandidatesPerSpan = 3;
constexpr int kPlacementProbeCount = 12;
constexpr int kMaximumCoreMergeTrials = 12000;
constexpr double kMinimumMergeBasisDeterminant = 0.1;
constexpr double kAnalyticToleranceFraction = 0.25;
constexpr std::array<double, 5> kSeedClearances = {0.0, 0.0625, 0.125, 0.25, 0.5};
constexpr std::array<double, 3> kProfileAngles = {0.125, 0.25, 0.5};

QVector<QPointF> boundaryWitnesses(const Region &region) {
    QVector<QPointF> result;
    for (const QPolygonF &polygon : region.required) {
        for (int index = 0; index < polygon.size(); ++index) {
            result.push_back(polygon[index]);
            result.push_back((polygon[index] + polygon[(index + 1) % polygon.size()]) * 0.5);
        }
    }

    return result;
}

int boundaryGain(const Candidate &candidate, const QVector<QPointF> &witnesses,
                  const QVector<bool> &covered) {
    int result = 0;
    for (int index = 0; index < witnesses.size(); ++index) {
        if (!covered[index] && candidate.bounds.contains(witnesses[index])
            && candidate.path.contains(witnesses[index])) {
            ++result;
        }
    }

    return result;
}

std::optional<Candidate> legalCandidate(const PenPrimitive &shape, const QTransform &transform,
                                        const Region &region) {
    Candidate result;
    result.placement.shapeId = shape.shapeId;
    result.placement.transform = emittedTransform(transform);
    const QRectF bounds = result.placement.transform.mapRect(shape.bounds);
    if (!std::isfinite(bounds.left()) || !std::isfinite(bounds.top())
        || !std::isfinite(bounds.right()) || !std::isfinite(bounds.bottom())
        || !QRectF(-kMaximumCoordinate, -kMaximumCoordinate,
                    2.0 * kMaximumCoordinate, 2.0 * kMaximumCoordinate).contains(bounds)) {
        return {};
    }
    for (const QPolygonF &polygon : shape.contours) {
        const int stride = std::max(1, static_cast<int>(polygon.size()) / kPlacementProbeCount);
        for (int index = 0; index < polygon.size(); index += stride) {
            if (!region.permittedPath.contains(result.placement.transform.map(polygon[index]))) {
                return {};
            }
        }
    }
    result.polygons = mapped(shape, result.placement.transform);
    if (result.polygons.isEmpty()
        || !subtract(expanded(result.polygons, kVerificationClearance), region.permitted).isEmpty()) {
        return {};
    }
    result.path = painterPath(result.polygons);
    result.bounds = result.path.boundingRect();
    result.gain = area(intersect(result.polygons, region.required));
    result.spill = area(subtract(result.polygons, region.required));

    return result;
}

QPointF supportPoint(const QPolygonF &polygon, const QPointF &direction) {
    return *std::max_element(polygon.begin(), polygon.end(), [&](const QPointF &left, const QPointF &right) {
        return QPointF::dotProduct(left, direction) < QPointF::dotProduct(right, direction);
    });
}

QPointF curvePoint(const PenBoundarySegment &segment, double parameter) {
    return segment.start * ((1.0 - parameter) * (1.0 - parameter))
        + segment.control * (2.0 * parameter * (1.0 - parameter))
        + segment.end * (parameter * parameter);
}

QVector<Candidate> contourProposals(const PenFillRequest &request, const Region &region,
                                   const QVector<Primitive> &primitives,
                                   const std::function<bool()> &cancelled,
                                   QJsonObject *diagnostics) {
    const auto loops = request.loops.isEmpty()
        ? QVector<PenLoop>{{request.points, PenLoopKind::Outer}} : request.loops;
    const PenContour contour = buildPenContour(loops, request.boundaryTolerance * kGeometryFraction);
    QVector<QVector<std::array<QPointF, 3>>> profiles;
    QVector<Candidate> result;
    int jobs = 0;
    int evaluated = 0;
    for (const Primitive &primitive : primitives) {
        QVector<std::array<QPointF, 3>> source;
        for (int direction = 0; direction < kProfileDirections; ++direction) {
            const double angle = direction * 2.0 * std::acos(-1.0) / kProfileDirections;
            const QPointF normal(std::cos(angle), std::sin(angle));
            const QPointF tangent(-normal.y(), normal.x());
            for (double fraction : kProfileAngles) {
                const double cosine = std::cos(fraction * std::acos(-1.0));
                const double sine = std::sin(fraction * std::acos(-1.0));
                for (const QPolygonF &polygon : primitive.shape.contours) {
                    std::array<QPointF, 3> anchors = {
                        supportPoint(polygon, normal * cosine - tangent * sine),
                        supportPoint(polygon, normal),
                        supportPoint(polygon, normal * cosine + tangent * sine),
                    };
                    if (std::abs(signedArea({anchors[0], anchors[1], anchors[2]})) > kMinimumDeterminant
                        && !source.contains(anchors)) {
                        source.push_back(anchors);
                        std::swap(anchors[0], anchors[2]);
                        source.push_back(anchors);
                    }
                }
            }
        }
        profiles.push_back(source);
    }
    for (const PenContourLoop &loop : contour.loops) {
        for (const PenBoundarySegment &segment : loop.segments) {
            if (!segment.curved) {
                continue;
            }
            for (const auto &interval : {std::pair{0.0, 1.0}, std::pair{0.0, 0.5}, std::pair{0.5, 1.0}}) {
                if (++jobs > kMaximumBoundaryJobs || (cancelled && cancelled())) {
                    diagnostics->insert(QStringLiteral("contourCandidates"), result.size());
                    return result;
                }
                QVector<QPointF> samples;
                const QPointF start = curvePoint(segment, interval.first);
                const QPointF end = curvePoint(segment, interval.second);
                const QPointF middle = curvePoint(segment, (interval.first + interval.second) * 0.5);
                const QPointF chord = end - start;
                const double length = std::hypot(chord.x(), chord.y());
                if (length <= region.tolerance) {
                    continue;
                }
                QPointF outward(chord.y() / length, -chord.x() / length);
                if (region.requiredPath.contains(middle + outward * region.tolerance)) {
                    outward = -outward;
                }
                for (int index = 1; index < 8; ++index) {
                    const double parameter = interval.first
                        + (interval.second - interval.first) * index / 8.0;
                    const QPointF tangent = (segment.control - segment.start) * (1.0 - parameter)
                        + (segment.end - segment.control) * parameter;
                    QPointF normal(tangent.y(), -tangent.x());
                    const double magnitude = std::hypot(normal.x(), normal.y());
                    if (magnitude > 0.0) {
                        normal /= magnitude;
                        if (QPointF::dotProduct(normal, outward) < 0.0) {
                            normal = -normal;
                        }
                    }
                    samples.push_back(curvePoint(segment, parameter)
                        + normal * (region.tolerance * 0.1));
                }
                QVector<Candidate> retained;
                QVector<int> scores;
                for (int primitiveIndex = 0; primitiveIndex < primitives.size(); ++primitiveIndex) {
                    const PenPrimitive &shape = primitives[primitiveIndex].shape;
                    std::optional<Candidate> best;
                    int bestScore = 0;
                    for (const auto &source : profiles[primitiveIndex]) {
                        for (double fraction : kSeedClearances) {
                            if (cancelled && cancelled()) {
                                return {};
                            }
                            const QPointF offset = outward * (fraction * region.tolerance);
                            const QTransform transform = affineFromAnchors(source,
                                {start + offset, middle + offset, end + offset});
                            const double size = shape.area * std::abs(transform.determinant());
                            if (!std::isfinite(size) || size <= 0.0 || size > region.area) {
                                continue;
                            }
                            bool invertible = false;
                            const QTransform inverse = transform.inverted(&invertible);
                            if (!invertible) {
                                continue;
                            }
                            int score = 0;
                            for (const QPointF &sample : samples) {
                                if (shape.silhouette.contains(inverse.map(sample))) {
                                    ++score;
                                }
                            }
                            if (score < kMinimumBoundaryGain || score < bestScore) {
                                continue;
                            }
                            ++evaluated;
                            auto candidate = legalCandidate(shape, transform, region);
                            if (candidate && (score > bestScore || !best || candidate->spill < best->spill)) {
                                best = std::move(candidate);
                                bestScore = score;
                            }
                        }
                    }
                    if (best) {
                        int position = 0;
                        while (position < retained.size() && (scores[position] > bestScore
                            || (scores[position] == bestScore && retained[position].spill <= best->spill))) {
                            ++position;
                        }
                        retained.insert(position, std::move(*best));
                        scores.insert(position, bestScore);
                        if (retained.size() > kCandidatesPerSpan) {
                            retained.removeLast();
                            scores.removeLast();
                        }
                    }
                }
                result += retained;
            }
        }
    }
    diagnostics->insert(QStringLiteral("contourCandidates"), result.size());
    diagnostics->insert(QStringLiteral("contourTransformsVerified"), evaluated);

    return result;
}

QVector<Candidate> analyticProposals(const PenFillRequest &request, const Region &region,
                                    const QVector<Primitive> &primitives,
                                    const std::function<bool()> &cancelled,
                                    QJsonObject *diagnostics) {
    PenFillRequest seedRequest = request;
    QVector<Candidate> result;
    const QVector<QPointF> witnesses = boundaryWitnesses(region);
    const QVector<bool> uncovered(witnesses.size(), false);
    seedRequest.primitives.clear();
    seedRequest.discardNegligiblePlacements = false;
    seedRequest.boundaryTolerance *= kAnalyticToleranceFraction;
    for (const Primitive &primitive : primitives) {
        seedRequest.primitives.push_back(primitive.shape);
    }
    const PenFillResult analytic = fillPenPath(seedRequest, cancelled);
    diagnostics->insert(QStringLiteral("analyticSeedPlacements"), analytic.placements.size());
    diagnostics->insert(QStringLiteral("analyticSeedError"), analytic.error);
    for (const PenPlacement &placement : analytic.placements) {
        if (cancelled && cancelled()) {
            return {};
        }
        if (placement.shapeId == 101 || placement.shapeId == 103) {
            continue;
        }
        const auto shape = std::find_if(primitives.begin(), primitives.end(), [&](const Primitive &primitive) {
            return primitive.shape.shapeId == placement.shapeId;
        });
        if (shape == primitives.end()) {
            continue;
        }
        const QRectF bounds = placement.transform.mapRect(shape->shape.bounds);
        std::optional<Candidate> best;
        int bestGain = 0;
        for (double fraction : kSeedClearances) {
            const double clearance = region.tolerance * fraction;
            for (int direction = -1; direction < 8; ++direction) {
                QTransform adjust;
                if (direction < 0) {
                    adjust.translate(bounds.center().x(), bounds.center().y());
                    adjust.scale(1.0 + 2.0 * clearance / bounds.width(),
                                 1.0 + 2.0 * clearance / bounds.height());
                    adjust.translate(-bounds.center().x(), -bounds.center().y());
                } else {
                    const double angle = direction * std::acos(-1.0) / 4.0;
                    adjust.translate(clearance * std::cos(angle), clearance * std::sin(angle));
                }
                auto candidate = legalCandidate(shape->shape, placement.transform * adjust, region);
                if (!candidate) {
                    continue;
                }
                const int gain = boundaryGain(*candidate, witnesses, uncovered);
                if (gain > bestGain || (gain == bestGain && best && candidate->spill < best->spill)) {
                    best = std::move(candidate);
                    bestGain = gain;
                }
            }
        }
        if (best) {
            result.push_back(std::move(*best));
        }
    }
    diagnostics->insert(QStringLiteral("legalAnalyticBoundarySeeds"), result.size());

    return result;
}

QVector<Candidate> selectBoundary(const QVector<Candidate> &proposals, const Region &region,
                                  const std::function<bool()> &cancelled) {
    const QVector<QPointF> witnesses = boundaryWitnesses(region);
    QVector<bool> covered(witnesses.size(), false);
    QVector<Candidate> result;
    for (int iteration = 0; iteration < kMaximumBoundaryPlacements; ++iteration) {
        if (cancelled && cancelled()) {
            return {};
        }
        int best = -1;
        int bestGain = kMinimumBoundaryGain - 1;
        for (int index = 0; index < proposals.size(); ++index) {
            const int gain = boundaryGain(proposals[index], witnesses, covered);
            if (gain > bestGain || (gain == bestGain && best >= 0
                && proposals[index].spill < proposals[best].spill)) {
                best = index;
                bestGain = gain;
            }
        }
        if (best < 0) {
            break;
        }
        const Candidate &candidate = proposals[best];
        result.push_back(candidate);
        for (int index = 0; index < witnesses.size(); ++index) {
            covered[index] = covered[index] || (candidate.bounds.contains(witnesses[index])
                && candidate.path.contains(witnesses[index]));
        }
    }

    return result;
}

Polygons simplifyCore(const Region &region, const Polygons &covered,
                       const std::function<bool()> &cancelled, int *removed,
                       QJsonObject *diagnostics) {
    Polygons result = region.required;
    for (int pass = 0; pass < kMaximumSimplificationPasses; ++pass) {
        bool changed = false;
        for (QPolygonF &polygon : result) {
            for (int index = polygon.size() - 1; index >= 0 && polygon.size() > 3; --index) {
                if (cancelled && cancelled()) {
                    return {};
                }
                QPolygonF ear({polygon[(index + polygon.size() - 1) % polygon.size()],
                               polygon[index], polygon[(index + 1) % polygon.size()]});
                const double orientation = signedArea(ear);
                if (orientation < 0.0) {
                    std::reverse(ear.begin(), ear.end());
                }
                const Polygons guard = expanded({ear}, orientation >= 0.0
                    ? kVerificationClearance : std::max(kVerificationClearance, region.tolerance * 0.2));
                const bool allowed = orientation >= 0.0
                    ? subtract(guard, covered).isEmpty()
                    : subtract(guard, region.permitted).isEmpty();
                if (allowed) {
                    polygon.removeAt(index);
                    changed = true;
                    ++*removed;
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    result = unite(result);
    const Polygons missing = subtract(region.required,
        unite(expanded(result, kVerificationClearance) + covered));
    const Polygons outside = subtract(result, region.permitted);
    diagnostics->insert(QStringLiteral("simplifiedCoreMissingArea"), area(missing));
    diagnostics->insert(QStringLiteral("simplifiedCoreOutsideArea"), area(outside));
    if (!missing.isEmpty() || !outside.isEmpty()) {
        *removed = 0;
        return region.required;
    }

    return result;
}

QVector<Candidate> mergeCore(QVector<Candidate> mesh, const Polygons &covered,
                             const Region &region, const QVector<Primitive> &primitives,
                             const std::function<bool()> &cancelled, QJsonObject *diagnostics) {
    const auto square = std::find_if(primitives.begin(), primitives.end(), [](const Primitive &primitive) {
        return primitive.shape.shapeId == 101;
    });
    if (square == primitives.end()) {
        return mesh;
    }
    const std::array<QPointF, 3> source = {square->shape.bounds.topLeft(),
        square->shape.bounds.topRight(), square->shape.bounds.bottomLeft()};
    int trials = 0;
    int merges = 0;
    bool changed = true;
    while (changed && trials < kMaximumCoreMergeTrials) {
        changed = false;
        for (int first = 0; first < mesh.size(); ++first) {
            for (int second = first + 1; second < mesh.size(); ++second) {
                if ((cancelled && cancelled()) || trials >= kMaximumCoreMergeTrials) {
                    diagnostics->insert(QStringLiteral("coreMergeTrials"), trials);
                    diagnostics->insert(QStringLiteral("coreMerges"), merges);
                    return mesh;
                }
                if (!mesh[first].bounds.intersects(mesh[second].bounds)) {
                    continue;
                }
                const Polygons required = subtract(intersect(unite(mesh[first].polygons
                    + mesh[second].polygons), region.required), covered);
                QPolygonF points;
                for (const QPolygonF &polygon : required) {
                    points += polygon;
                }
                const QPolygonF hull = convexHull(points);
                if (hull.size() < 3 || !subtract({hull}, region.permitted).isEmpty()) {
                    continue;
                }
                const Polygons guarded = expanded(required, kVerificationClearance);
                std::optional<Candidate> replacement;
                for (int edge = 0; edge < hull.size() && !replacement
                     && trials < kMaximumCoreMergeTrials; ++edge) {
                    const QPointF along = hull[(edge + 1) % hull.size()] - hull[edge];
                    const QPointF horizontal = along / std::hypot(along.x(), along.y());
                    for (int other = -1; other < hull.size() && !replacement
                         && trials < kMaximumCoreMergeTrials; ++other) {
                        if (++trials > kMaximumCoreMergeTrials) {
                            break;
                        }
                        QPointF vertical(-horizontal.y(), horizontal.x());
                        if (other >= 0) {
                            vertical = hull[(other + 1) % hull.size()] - hull[other];
                            vertical /= std::hypot(vertical.x(), vertical.y());
                        }
                        const QTransform frame(horizontal.x(), horizontal.y(), vertical.x(), vertical.y(),
                                                hull.front().x(), hull.front().y());
                        if (std::abs(frame.determinant()) < kMinimumMergeBasisDeterminant) {
                            continue;
                        }
                        const double coordinate = std::max({1.0, std::abs(hull.boundingRect().left()),
                            std::abs(hull.boundingRect().right()), std::abs(hull.boundingRect().top()),
                            std::abs(hull.boundingRect().bottom())});
                        const double clearance = std::max(kVerificationClearance * 4.0,
                            coordinate * std::numeric_limits<float>::epsilon() * 4.0)
                            / std::abs(frame.determinant());
                        const QRectF bounds = frame.inverted().map(hull).boundingRect()
                            .adjusted(-clearance, -clearance, clearance, clearance);
                        auto candidate = legalCandidate(square->shape, affineFromAnchors(source,
                            {frame.map(bounds.topLeft()), frame.map(bounds.topRight()), frame.map(bounds.bottomLeft())}), region);
                        if (candidate && subtract(guarded, candidate->polygons).isEmpty()) {
                            replacement = std::move(candidate);
                        }
                    }
                }
                if (replacement) {
                    mesh[first] = std::move(*replacement);
                    mesh.removeAt(second);
                    --second;
                    changed = true;
                    ++merges;
                }
            }
        }
    }
    diagnostics->insert(QStringLiteral("coreMergeTrials"), trials);
    diagnostics->insert(QStringLiteral("coreMerges"), merges);

    return mesh;
}

} // namespace

QVector<Candidate> compactCover(const PenFillRequest &request, const Region &region,
                               const QVector<Primitive> &primitives,
                               const QVector<Candidate> &proposals,
                               const std::function<bool()> &cancelled,
                               QJsonObject *diagnostics) {
    QVector<Candidate> pool = analyticProposals(request, region, primitives, cancelled, diagnostics);
    pool += contourProposals(request, region, primitives, cancelled, diagnostics);
    pool += proposals;
    QVector<Candidate> result = selectBoundary(pool, region, cancelled);
    Polygons covered;
    for (const Candidate &candidate : result) {
        covered += candidate.polygons;
    }
    covered = unite(covered);
    int removed = 0;
    Region core = region;
    core.required = simplifyCore(region, covered, cancelled, &removed, diagnostics);
    core.requiredPath = painterPath(core.required);
    core.bounds = core.requiredPath.boundingRect();
    core.area = area(core.required);
    diagnostics->insert(QStringLiteral("boundaryPlacements"), result.size());
    diagnostics->insert(QStringLiteral("removedCoreVertices"), removed);
    if (cancelled && cancelled()) {
        return {};
    }
    if (removed == 0) {
        return {};
    }
    try {
        const QVector<Candidate> mesh = completeCover(core, primitives, cancelled);
        result += mergeCore(mesh, covered, region, primitives, cancelled, diagnostics);
        Polygons support;
        for (const Candidate &candidate : result) {
            support += candidate.polygons;
        }
        support = unite(support);
        if (!subtract(region.required, support).isEmpty()
            || !subtract(support, region.permitted).isEmpty()) {
            diagnostics->insert(QStringLiteral("compactCoverError"), QStringLiteral("Coverage or spill verification failed"));
            return {};
        }
    } catch (const std::exception &failure) {
        diagnostics->insert(QStringLiteral("compactCoverError"), QString::fromUtf8(failure.what()));
        return {};
    }
    diagnostics->insert(QStringLiteral("compactPlacements"), result.size());

    return result;
}

} // namespace gui::catalog
