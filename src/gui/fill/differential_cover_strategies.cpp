#include "differential_cover_internal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace gui::cover {

QVector<Placement> scaledMeshPlacements(
    const QVector<Placement> &placements,
    const QPointF &center,
    double scale) {
    QVector<Placement> result = placements;
    for (Placement &placement : result) {
        placement.transform.a *= scale;
        placement.transform.b *= scale;
        placement.transform.c *= scale;
        placement.transform.d *= scale;
        placement.transform.e =
            placement.transform.e * scale
            + center.x() * (1.0 - scale);
        placement.transform.f =
            placement.transform.f * scale
            + center.y() * (1.0 - scale);
    }

    return result;
}

MeshCoverPlan meshCoverPlan(
    const QVector<FixedCandidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    const std::function<bool()> &cancelled) {
    MeshCoverPlan result;
    if (candidates.isEmpty()) {
        result.reason =
            QStringLiteral("polygon mesh is unavailable");
        return result;
    }
    if (candidates.size() > options.budget) {
        result.reason =
            QStringLiteral("polygon mesh exceeds the shape budget");
        return result;
    }

    QVector<Placement> placements;
    placements.reserve(candidates.size());
    for (const FixedCandidate &candidate : candidates) {
        placements.push_back({
            candidate.transform,
            candidate.shape->id,
            0.0,
        });
    }
    const QPointF center =
        polygonBounds(mustCover).center();
    const double outsideLimit =
        options.epsSpill
        * static_cast<double>(placements.size());
    for (double scale = 1.0;
         scale + kGeometryEpsilon
             >= kMeshMinimumLegalScale;
         scale -= kMeshLegalScaleStep) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.reason =
                QStringLiteral("cancelled");
            return result;
        }
        QVector<Placement> trial =
            scaledMeshPlacements(
                placements, center, scale);
        ExactCoverState state =
            exactCoverState(
                trial, catalog,
                mustCover, mayCover,
                targetArea);
        if (state.outsideArea
                > outsideLimit
                    + kGeometryEpsilon) {
            continue;
        }
        result.placements =
            std::move(trial);
        result.residual =
            std::move(state.residual);
        result.residualArea =
            state.residualArea;
        result.outsideArea =
            state.outsideArea;
        result.coverageRatio =
            targetArea > kGeometryEpsilon
            ? state.coveredArea / targetArea
            : 0.0;
        result.scale = scale;
        result.accepted =
            result.coverageRatio
            >= kMeshMinimumCompactCoverageRatio;
        result.reason = result.accepted
            ? QStringLiteral("compact polygon mesh")
            : QStringLiteral("polygon mesh coverage is insufficient");
        refreshPlacementGains(
            &result.placements,
            catalog, mustCover);
        return result;
    }
    result.reason =
        QStringLiteral("polygon mesh legalization failed");

    return result;
}

double pointCross(const QPointF &left, const QPointF &right) {
    return left.x() * right.y() - left.y() * right.x();
}

QPointF canonicalDirection(const QPointF &delta) {
    const double length = std::hypot(delta.x(), delta.y());
    if (length <= kGeometryEpsilon) {
        return {};
    }
    QPointF result = delta / length;
    if (result.x() < 0.0
        || (std::abs(result.x()) <= kGeometryEpsilon
            && result.y() < 0.0)) {
        result = -result;
    }

    return result;
}

double directionSinDistance(const QPointF &left,
                            const QPointF &right) {
    return std::abs(pointCross(left, right));
}

StructuralAxes structuralAxes(
    const QVector<StructuralEdge> &edges) {
    StructuralAxes result;
    double totalLength = 0.0;
    for (const StructuralEdge &edge : edges) {
        totalLength += edge.length;
    }
    if (edges.size() < 4 || totalLength <= kGeometryEpsilon) {
        return result;
    }

    int bestFirst = -1;
    int bestSecond = -1;
    double bestExplained = -1.0;
    double bestError = std::numeric_limits<double>::max();
    for (int first = 0; first < edges.size(); ++first) {
        for (int second = first + 1; second < edges.size(); ++second) {
            const QPointF firstAxis = edges[first].direction;
            const QPointF secondAxis = edges[second].direction;
            if (directionSinDistance(firstAxis, secondAxis)
                < kStructuralMinimumAxisSeparationSin) {
                continue;
            }
            double explained = 0.0;
            double error = 0.0;
            for (const StructuralEdge &edge : edges) {
                const double distance = std::min(
                    directionSinDistance(edge.direction, firstAxis),
                    directionSinDistance(edge.direction, secondAxis));
                if (distance <= kStructuralDirectionSinTolerance) {
                    explained += edge.length;
                }
                error += edge.length * distance;
            }
            if (explained > bestExplained + kGeometryEpsilon
                || (std::abs(explained - bestExplained)
                        <= kGeometryEpsilon
                    && error < bestError - kGeometryEpsilon)) {
                bestFirst = first;
                bestSecond = second;
                bestExplained = explained;
                bestError = error;
            }
        }
    }
    if (bestFirst < 0 || bestSecond < 0) {
        return result;
    }

    QPointF firstSum;
    QPointF secondSum;
    for (const StructuralEdge &edge : edges) {
        const double firstDistance =
            directionSinDistance(
                edge.direction,
                edges[bestFirst].direction);
        const double secondDistance =
            directionSinDistance(
                edge.direction,
                edges[bestSecond].direction);
        if (std::min(firstDistance, secondDistance)
            > kStructuralDirectionSinTolerance) {
            continue;
        }
        if (firstDistance <= secondDistance) {
            firstSum += edge.direction * edge.length;
        } else {
            secondSum += edge.direction * edge.length;
        }
    }
    result.first = canonicalDirection(firstSum);
    result.second = canonicalDirection(secondSum);
    result.determinant =
        pointCross(result.first, result.second);
    if (std::abs(result.determinant)
        < kStructuralMinimumAxisSeparationSin) {
        return {};
    }
    if (result.determinant < 0.0) {
        std::swap(result.first, result.second);
        result.determinant = -result.determinant;
    }

    double explained = 0.0;
    for (const StructuralEdge &edge : edges) {
        if (std::min(
                directionSinDistance(
                    edge.direction, result.first),
                directionSinDistance(
                    edge.direction, result.second))
            <= kStructuralDirectionSinTolerance) {
            explained += edge.length;
        }
    }
    result.explainedBoundaryFraction =
        explained / totalLength;
    result.valid =
        result.explainedBoundaryFraction
        >= kStructuralMinimumExplainedBoundaryFraction;

    return result;
}

QPointF structuralCoordinates(
    const QPointF &point,
    const StructuralAxes &axes) {
    return {
        pointCross(point, axes.second)
            / axes.determinant,
        pointCross(axes.first, point)
            / axes.determinant,
    };
}

QPointF structuralPoint(
    double first,
    double second,
    const StructuralAxes &axes) {
    return axes.first * first
        + axes.second * second;
}

QVector<double> clusteredCoordinates(
    QVector<double> values,
    double tolerance) {
    std::sort(values.begin(), values.end());
    QVector<double> result;
    QVector<int> counts;
    for (const double value : values) {
        if (result.isEmpty()
            || value - result.back() > tolerance) {
            result.push_back(value);
            counts.push_back(1);
            continue;
        }
        const int count = counts.back();
        result.back() =
            (result.back() * count + value)
            / static_cast<double>(count + 1);
        counts.back() = count + 1;
    }

    return result;
}

double nearestCoordinate(
    double value,
    const QVector<double> &coordinates) {
    const auto found = std::min_element(
        coordinates.cbegin(), coordinates.cend(),
        [value](double left, double right) {
            return std::abs(left - value)
                < std::abs(right - value);
        });

    return found == coordinates.cend()
        ? value : *found;
}

bool sameCoordinate(double left, double right) {
    return std::abs(left - right)
        <= kGeometryEpsilon;
}

QPolygonF simplifyStructuralPolygon(QPolygonF polygon) {
    bool changed = true;
    while (changed && polygon.size() >= 3) {
        changed = false;
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF previous =
                polygon[
                    (index + polygon.size() - 1)
                    % polygon.size()];
            const QPointF current = polygon[index];
            const QPointF next =
                polygon[(index + 1) % polygon.size()];
            if (QLineF(previous, current).length()
                    <= kGeometryEpsilon
                || (sameCoordinate(
                        previous.x(), current.x())
                    && sameCoordinate(
                        current.x(), next.x()))
                || (sameCoordinate(
                        previous.y(), current.y())
                    && sameCoordinate(
                        current.y(), next.y()))) {
                polygon.removeAt(index);
                changed = true;
                break;
            }
        }
    }

    return polygon;
}

QVector<double> structuralSupportCoordinates(
    const QPolygonF &polygon,
    bool firstCoordinate) {
    QVector<double> result;
    result.reserve(polygon.size());
    for (const QPointF &point : polygon) {
        result.push_back(
            firstCoordinate ? point.x() : point.y());
    }
    std::sort(result.begin(), result.end());
    result.erase(
        std::unique(
            result.begin(), result.end(),
            [](double left, double right) {
                return sameCoordinate(left, right);
            }),
        result.end());

    return result;
}

QVector<StructuralRectangle> structuralRectangles(
    const QPolygonF &polygon,
    const QVector<double> &firstCoordinates,
    const QVector<double> &secondCoordinates,
    quint64 *occupiedMask,
    int *occupiedCount) {
    const int firstCells =
        firstCoordinates.size() - 1;
    const int secondCells =
        secondCoordinates.size() - 1;
    *occupiedMask = 0;
    for (int second = 0;
         second < secondCells; ++second) {
        for (int first = 0;
             first < firstCells; ++first) {
            const QPointF center(
                (firstCoordinates[first]
                 + firstCoordinates[first + 1])
                    * 0.5,
                (secondCoordinates[second]
                 + secondCoordinates[second + 1])
                    * 0.5);
            if (polygon.containsPoint(
                    center, Qt::OddEvenFill)) {
                const int bit =
                    second * firstCells + first;
                *occupiedMask |= quint64{1} << bit;
            }
        }
    }
    *occupiedCount =
        std::popcount(*occupiedMask);

    QVector<StructuralRectangle> result;
    for (int top = 0; top < secondCells; ++top) {
        for (int bottom = top + 1;
             bottom <= secondCells; ++bottom) {
            for (int left = 0;
                 left < firstCells; ++left) {
                quint64 mask = 0;
                for (int right = left + 1;
                     right <= firstCells; ++right) {
                    bool full = true;
                    for (int second = top;
                         second < bottom; ++second) {
                        const int bit =
                            second * firstCells
                            + right - 1;
                        mask |= quint64{1} << bit;
                        if ((*occupiedMask
                             & (quint64{1} << bit))
                            == 0) {
                            full = false;
                        }
                    }
                    if (full) {
                        result.push_back({
                            mask,
                            left,
                            right,
                            top,
                            bottom,
                        });
                    } else {
                        break;
                    }
                }
            }
        }
    }

    QVector<StructuralRectangle> maximal;
    for (int index = 0; index < result.size(); ++index) {
        bool dominated = false;
        for (int other = 0;
             other < result.size(); ++other) {
            if (index == other
                || result[index].mask
                    == result[other].mask) {
                continue;
            }
            if ((result[index].mask
                 & ~result[other].mask)
                == 0) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            maximal.push_back(result[index]);
        }
    }
    std::stable_sort(
        maximal.begin(), maximal.end(),
        [](const StructuralRectangle &left,
           const StructuralRectangle &right) {
            const int leftCount =
                std::popcount(left.mask);
            const int rightCount =
                std::popcount(right.mask);
            if (leftCount != rightCount) {
                return leftCount > rightCount;
            }
            return std::tie(
                       left.top, left.left,
                       left.bottom, left.right)
                < std::tie(
                       right.top, right.left,
                       right.bottom, right.right);
        });

    return maximal;
}

QVector<StructuralRectangle> minimumRectangleCover(
    quint64 occupiedMask,
    const QVector<StructuralRectangle> &rectangles,
    bool *searchLimited) {
    QVector<QVector<int>> rectanglesByCell(
        kStructuralMaximumGridCells);
    for (int index = 0;
         index < rectangles.size(); ++index) {
        for (int bit = 0;
             bit < kStructuralMaximumGridCells; ++bit) {
            if ((rectangles[index].mask
                 & (quint64{1} << bit))
                != 0) {
                rectanglesByCell[bit].push_back(index);
            }
        }
    }

    QVector<int> best;
    quint64 greedyCovered = 0;
    while ((greedyCovered & occupiedMask)
           != occupiedMask) {
        int bestIndex = -1;
        int bestGain = 0;
        for (int index = 0;
             index < rectangles.size(); ++index) {
            const int gain = std::popcount(
                rectangles[index].mask
                & occupiedMask
                & ~greedyCovered);
            if (gain > bestGain) {
                bestIndex = index;
                bestGain = gain;
            }
        }
        if (bestIndex < 0) {
            return {};
        }
        best.push_back(bestIndex);
        greedyCovered |= rectangles[bestIndex].mask;
    }

    QVector<int> current;
    int searchedNodes = 0;
    *searchLimited = false;
    std::function<void(quint64)> search =
        [&](quint64 covered) {
            if (++searchedNodes
                > kStructuralSearchNodeLimit) {
                *searchLimited = true;
                return;
            }
            if ((covered & occupiedMask)
                == occupiedMask) {
                if (current.size() < best.size()) {
                    best = current;
                }
                return;
            }
            if (current.size() + 1 >= best.size()) {
                return;
            }

            const quint64 uncovered =
                occupiedMask & ~covered;
            int selectedBit = -1;
            int selectedOptions =
                std::numeric_limits<int>::max();
            for (int bit = 0;
                 bit < kStructuralMaximumGridCells;
                 ++bit) {
                if ((uncovered
                     & (quint64{1} << bit))
                    == 0) {
                    continue;
                }
                int options = 0;
                for (const int rectangle :
                     rectanglesByCell[bit]) {
                    if ((rectangles[rectangle].mask
                         & uncovered)
                        != 0) {
                        ++options;
                    }
                }
                if (options < selectedOptions) {
                    selectedBit = bit;
                    selectedOptions = options;
                }
            }
            if (selectedBit < 0
                || selectedOptions == 0) {
                return;
            }
            for (const int rectangle :
                 rectanglesByCell[selectedBit]) {
                const quint64 next =
                    covered
                    | rectangles[rectangle].mask;
                if (next == covered) {
                    continue;
                }
                current.push_back(rectangle);
                search(next);
                current.removeLast();
                if (*searchLimited) {
                    return;
                }
            }
        };
    search(0);

    QVector<StructuralRectangle> result;
    result.reserve(best.size());
    for (const int index : best) {
        result.push_back(rectangles[index]);
    }

    return result;
}

Affine structuralRectangleTransform(
    const ShapeMesh &square,
    const StructuralAxes &axes,
    const QVector<double> &firstCoordinates,
    const QVector<double> &secondCoordinates,
    const StructuralRectangle &rectangle,
    double firstScale,
    double secondScale,
    double firstOffset = 0.0,
    double secondOffset = 0.0) {
    const QRectF source = shapeBounds(square);
    const double firstCenter =
        (firstCoordinates[rectangle.left]
         + firstCoordinates[rectangle.right])
        * 0.5 + firstOffset;
    const double secondCenter =
        (secondCoordinates[rectangle.top]
         + secondCoordinates[rectangle.bottom])
        * 0.5 + secondOffset;
    const double firstExtent =
        (firstCoordinates[rectangle.right]
         - firstCoordinates[rectangle.left])
        * firstScale;
    const double secondExtent =
        (secondCoordinates[rectangle.bottom]
         - secondCoordinates[rectangle.top])
        * secondScale;
    const QPointF targetOrigin =
        structuralPoint(
            firstCenter - firstExtent * 0.5,
            secondCenter - secondExtent * 0.5,
            axes);
    const QPointF firstVector =
        axes.first * firstExtent;
    const QPointF secondVector =
        axes.second * secondExtent;
    Affine result;
    result.a =
        firstVector.x() / source.width();
    result.b =
        firstVector.y() / source.width();
    result.c =
        secondVector.x() / source.height();
    result.d =
        secondVector.y() / source.height();
    result.e =
        targetOrigin.x()
        - result.a * source.left()
        - result.c * source.top();
    result.f =
        targetOrigin.y()
        - result.b * source.left()
        - result.d * source.top();

    return result;
}

std::optional<Affine> legalStructuralRectangle(
    const ShapeMesh &square,
    const StructuralAxes &axes,
    const QVector<double> &firstCoordinates,
    const QVector<double> &secondCoordinates,
    const StructuralRectangle &rectangle,
    const Polygons &mayCover,
    double outsideAllowance,
    double translationTolerance,
    const std::function<bool()> &cancelled) {
    constexpr std::array<double, 9>
        kTranslationFactors = {
            0.0,
            -0.25, 0.25,
            -0.5, 0.5,
            -0.75, 0.75,
            -1.0, 1.0,
        };
    const auto translatedCandidate =
        [&](double firstScale,
            double secondScale) {
            Affine bestTransform;
            double bestOutsideArea =
                std::numeric_limits<double>::max();
            for (const double firstFactor :
                 kTranslationFactors) {
                for (const double secondFactor :
                     kTranslationFactors) {
                    const Affine transform =
                        structuralRectangleTransform(
                            square, axes,
                            firstCoordinates,
                            secondCoordinates,
                            rectangle,
                            firstScale,
                            secondScale,
                            firstFactor
                                * translationTolerance,
                            secondFactor
                                * translationTolerance);
                    const Polygons footprint{
                        transformedBoundary(
                            square, transform),
                    };
                    const double outsideArea =
                        polygonSetArea(
                            differencePolygons(
                                footprint,
                                mayCover));
                    if (outsideArea
                        < bestOutsideArea
                            - kGeometryEpsilon) {
                        bestTransform = transform;
                        bestOutsideArea =
                            outsideArea;
                    }
                }
            }

            return std::make_pair(
                bestTransform,
                bestOutsideArea);
        };
    double firstScale = 1.0;
    double secondScale = 1.0;
    while (firstScale
               >= kStructuralMinimumLegalScale
                   - kGeometryEpsilon
           && secondScale
               >= kStructuralMinimumLegalScale
                   - kGeometryEpsilon) {
        if (cancelled && cancelled()) {
            return std::nullopt;
        }
        const auto current =
            translatedCandidate(
                firstScale,
                secondScale);
        if (current.second
            <= outsideAllowance
                + kGeometryEpsilon) {
            return current.first;
        }
        const bool canShrinkFirst =
            firstScale
                - kStructuralLegalScaleStep
            >= kStructuralMinimumLegalScale
                - kGeometryEpsilon;
        const bool canShrinkSecond =
            secondScale
                - kStructuralLegalScaleStep
            >= kStructuralMinimumLegalScale
                - kGeometryEpsilon;
        if (!canShrinkFirst
            && !canShrinkSecond) {
            break;
        }
        const double firstOutside =
            canShrinkFirst
            ? translatedCandidate(
                  firstScale
                      - kStructuralLegalScaleStep,
                  secondScale).second
            : std::numeric_limits<double>::max();
        const double secondOutside =
            canShrinkSecond
            ? translatedCandidate(
                  firstScale,
                  secondScale
                      - kStructuralLegalScaleStep).second
            : std::numeric_limits<double>::max();
        if (firstOutside
            < secondOutside
                - kGeometryEpsilon) {
            firstScale -=
                kStructuralLegalScaleStep;
        } else {
            secondScale -=
                kStructuralLegalScaleStep;
        }
    }

    return std::nullopt;
}

StructuralCoverPlan structuralCoverPlan(
    const QVector<ContourSpan> &boundarySpans,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    const std::function<bool()> &cancelled) {
    StructuralCoverPlan result;
    const ShapeMesh *square =
        shapeById(catalog, 101);
    if (square == nullptr) {
        result.reason =
            QStringLiteral("Square is unavailable");
        return result;
    }
    if (boundarySpans.size() < 4) {
        result.reason =
            QStringLiteral("too few boundary spans");
        return result;
    }

    QVector<QPointF> vertices;
    QVector<StructuralEdge> edges;
    vertices.reserve(boundarySpans.size());
    edges.reserve(boundarySpans.size());
    for (const ContourSpan &span : boundarySpans) {
        vertices.push_back(span.start);
    }
    const QRectF bounds =
        polygonBounds(mustCover);
    const double diagonal =
        std::hypot(bounds.width(), bounds.height());
    const double coordinateTolerance =
        std::max(
            kStructuralMinimumCoordinateTolerance,
            diagonal
                * kStructuralCoordinateToleranceFraction);
    for (const ContourSpan &span : boundarySpans) {
        const QPointF chord =
            span.end - span.start;
        const double length =
            std::hypot(chord.x(), chord.y());
        if (length <= kGeometryEpsilon) {
            result.reason =
                QStringLiteral("degenerate boundary span");
            return result;
        }
        double maximumBow = 0.0;
        if (span.curved) {
            maximumBow =
                std::abs(
                    pointCross(
                        chord,
                        span.control - span.start))
                / length * 0.5;
            if (maximumBow
                > coordinateTolerance) {
                result.reason =
                    QStringLiteral("boundary curvature exceeds structural tolerance");
                return result;
            }
        }
        edges.push_back({
            canonicalDirection(chord),
            length,
            maximumBow,
        });
    }

    const StructuralAxes axes =
        structuralAxes(edges);
    result.explainedBoundaryFraction =
        axes.explainedBoundaryFraction;
    if (!axes.valid) {
        result.reason =
            QStringLiteral("boundary does not resolve to two axes");
        return result;
    }
    const double basisTolerance =
        coordinateTolerance
        / axes.determinant;
    QVector<QPointF> coordinateVertices;
    QVector<double> rawFirstCoordinates;
    QVector<double> rawSecondCoordinates;
    coordinateVertices.reserve(vertices.size());
    rawFirstCoordinates.reserve(vertices.size());
    rawSecondCoordinates.reserve(vertices.size());
    for (const QPointF &vertex : vertices) {
        const QPointF coordinate =
            structuralCoordinates(vertex, axes);
        coordinateVertices.push_back(coordinate);
        rawFirstCoordinates.push_back(
            coordinate.x());
        rawSecondCoordinates.push_back(
            coordinate.y());
    }
    const QVector<double> firstClusters =
        clusteredCoordinates(
            rawFirstCoordinates,
            basisTolerance);
    const QVector<double> secondClusters =
        clusteredCoordinates(
            rawSecondCoordinates,
            basisTolerance);
    QPolygonF snapped;
    snapped.reserve(coordinateVertices.size());
    for (const QPointF &coordinate :
         coordinateVertices) {
        snapped.push_back({
            nearestCoordinate(
                coordinate.x(), firstClusters),
            nearestCoordinate(
                coordinate.y(), secondClusters),
        });
    }
    snapped =
        simplifyStructuralPolygon(
            std::move(snapped));
    if (snapped.size() < 4) {
        result.reason =
            QStringLiteral("snapped boundary collapsed");
        return result;
    }
    for (int index = 0;
         index < snapped.size(); ++index) {
        const QPointF &left = snapped[index];
        const QPointF &right =
            snapped[(index + 1)
                    % snapped.size()];
        if (sameCoordinate(
                left.x(), right.x())
            == sameCoordinate(
                left.y(), right.y())) {
            result.reason =
                QStringLiteral("snapped boundary is not axis aligned");
            return result;
        }
    }

    const QVector<double> firstCoordinates =
        structuralSupportCoordinates(
            snapped, true);
    const QVector<double> secondCoordinates =
        structuralSupportCoordinates(
            snapped, false);
    if (firstCoordinates.size() < 2
        || secondCoordinates.size() < 2
        || firstCoordinates.size()
            > kStructuralMaximumSupportLines
        || secondCoordinates.size()
            > kStructuralMaximumSupportLines) {
        result.reason =
            QStringLiteral("structural grid is too large");
        return result;
    }
    const int totalCells =
        (firstCoordinates.size() - 1)
        * (secondCoordinates.size() - 1);
    if (totalCells <= 0
        || totalCells
            > kStructuralMaximumGridCells) {
        result.reason =
            QStringLiteral("structural grid cell limit exceeded");
        return result;
    }

    quint64 occupiedMask = 0;
    int occupiedCount = 0;
    const QVector<StructuralRectangle>
        rectangles =
            structuralRectangles(
                snapped,
                firstCoordinates,
                secondCoordinates,
                &occupiedMask,
                &occupiedCount);
    result.gridCells = occupiedCount;
    result.rectangleCandidates =
        rectangles.size();
    if (occupiedCount == 0
        || rectangles.isEmpty()) {
        result.reason =
            QStringLiteral("structural grid is empty");
        return result;
    }

    bool searchLimited = false;
    const QVector<StructuralRectangle>
        selected =
            minimumRectangleCover(
                occupiedMask,
                rectangles,
                &searchLimited);
    if (selected.isEmpty()) {
        result.reason = searchLimited
            ? QStringLiteral("rectangle search limit reached")
            : QStringLiteral("rectangle cover is unavailable");
        return result;
    }
    if (selected.size() > options.budget) {
        result.reason =
            QStringLiteral("rectangle cover exceeds the shape budget");
        return result;
    }

    const double outsideAllowance =
        options.epsSpill
        / static_cast<double>(selected.size());
    result.placements.reserve(selected.size());
    for (const StructuralRectangle &rectangle :
         selected) {
        const std::optional<Affine> transform =
            legalStructuralRectangle(
                *square, axes,
                firstCoordinates,
                secondCoordinates,
                rectangle, mayCover,
                outsideAllowance,
                basisTolerance,
                cancelled);
        if (!transform.has_value()) {
            result.cancelled =
                cancelled && cancelled();
            result.reason = result.cancelled
                ? QStringLiteral("cancelled")
                : QStringLiteral("rectangle legalization failed");
            return result;
        }
        result.placements.push_back({
            *transform,
            square->id,
            0.0,
        });
    }

    const ExactCoverState state =
        exactCoverState(
            result.placements, catalog,
            mustCover, mayCover,
            targetArea);
    result.residual = state.residual;
    result.residualArea =
        state.residualArea;
    result.outsideArea =
        state.outsideArea;
    result.coverageRatio =
        targetArea > kGeometryEpsilon
        ? state.coveredArea / targetArea
        : 0.0;
    result.eligible = true;
    result.residualThickness =
        result.residual.isEmpty()
        ? 0.0
        : distanceSeed(result.residual).radius;
    result.accepted =
        (result.coverageRatio
             >= kStructuralMinimumCompactCoverageRatio
         || (result.coverageRatio
             >= kStructuralMinimumSeedCoverageRatio
             && result.residualThickness
                 <= coordinateTolerance))
        && result.outsideArea
            <= options.epsSpill
                + kGeometryEpsilon;
    result.seeded =
        !result.accepted
        && result.coverageRatio
            >= kStructuralMinimumSeedCoverageRatio
        && result.outsideArea
            <= options.epsSpill
                + kGeometryEpsilon;
    result.reason = result.accepted
        ? (result.coverageRatio
                   >= kStructuralMinimumCompactCoverageRatio
               ? QStringLiteral("compact structural cover")
               : QStringLiteral("compact structural boundary residual"))
        : (result.seeded
               ? QStringLiteral("structural residual seed")
               : QStringLiteral("structural coverage is insufficient"));
    refreshPlacementGains(
        &result.placements,
        catalog, mustCover);

    return result;
}

} // namespace gui::cover
