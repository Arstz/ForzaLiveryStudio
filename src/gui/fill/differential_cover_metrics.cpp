#include "differential_cover_internal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace gui::cover {

namespace {

struct BoundaryLocation {
    QPointF point;
    QPointF tangent;
    double distance = std::numeric_limits<double>::infinity();
    int primitive = -1;
};

struct FeatureChoice {
    double distance = std::numeric_limits<double>::infinity();
    double tangentError = std::numeric_limits<double>::infinity();
    int featureIndex = -1;
    int primitive = -1;
};

QPointF normalized(const QPointF &vector) {
    const double length =
        std::hypot(vector.x(), vector.y());
    if (length <= kGeometryEpsilon) {
        return {};
    }

    return vector / length;
}

double angleBetween(const QPointF &left,
                    const QPointF &right) {
    if (left.isNull() || right.isNull()) {
        return kPi;
    }

    return std::acos(
        std::clamp(
            QPointF::dotProduct(left, right),
            -1.0, 1.0));
}

BoundaryLocation closestBoundaryLocation(
    const QPointF &point,
    const Polygons &polygons) {
    BoundaryLocation result;
    int primitive = 0;
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0;
             index < polygon.size(); ++index, ++primitive) {
            const QPointF start = polygon[index];
            const QPointF end =
                polygon[(index + 1) % polygon.size()];
            const QPointF edge = end - start;
            const double lengthSquared =
                QPointF::dotProduct(edge, edge);
            const double parameter =
                lengthSquared > kGeometryEpsilon
                ? std::clamp(
                      QPointF::dotProduct(
                          point - start, edge)
                          / lengthSquared,
                      0.0, 1.0)
                : 0.0;
            const QPointF closest =
                start + edge * parameter;
            const double distance =
                QLineF(point, closest).length();
            if (distance
                    < result.distance
                        - kGeometryEpsilon
                || (std::abs(
                        distance - result.distance)
                        <= kGeometryEpsilon
                    && primitive < result.primitive)) {
                result.point = closest;
                result.tangent = normalized(edge);
                result.distance = distance;
                result.primitive = primitive;
            }
        }
    }

    return result;
}

QVector<QPointF> boundarySamples(
    const Polygons &polygons,
    double tolerance) {
    double perimeter = 0.0;
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0;
             index < polygon.size(); ++index) {
            perimeter += QLineF(
                polygon[index],
                polygon[(index + 1)
                        % polygon.size()]).length();
        }
    }
    if (perimeter <= kGeometryEpsilon) {
        return {};
    }

    const double spacing = std::max(
        std::max(
            tolerance * 0.5,
            kGeometryEpsilon),
        perimeter
            / static_cast<double>(
                kMaximumBoundarySamples));
    QVector<QPointF> result;
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0;
             index < polygon.size(); ++index) {
            const QPointF start = polygon[index];
            const QPointF end =
                polygon[(index + 1) % polygon.size()];
            const double length =
                QLineF(start, end).length();
            const int intervals = std::max(
                1,
                static_cast<int>(
                    std::ceil(length / spacing)));
            for (int sample = 0;
                 sample < intervals; ++sample) {
                result.push_back(
                    start
                    + (end - start)
                        * (static_cast<double>(sample)
                           / static_cast<double>(
                               intervals)));
            }
        }
    }

    return result;
}

QVector<double> boundaryDistances(
    const QVector<QPointF> &samples,
    const Polygons &boundary,
    double fallbackDistance) {
    QVector<double> result;
    result.reserve(samples.size());
    for (const QPointF &sample : samples) {
        const BoundaryLocation location =
            closestBoundaryLocation(
                sample, boundary);
        result.push_back(
            std::isfinite(location.distance)
            ? location.distance
            : fallbackDistance);
    }

    return result;
}

double percentile(QVector<double> values,
                  double fraction) {
    if (values.isEmpty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const int index = std::clamp(
        static_cast<int>(
            std::ceil(
                fraction
                    * static_cast<double>(
                        values.size())))
            - 1,
        0,
        static_cast<int>(
            values.size()) - 1);

    return values[index];
}

QPainterPath polygonPath(
    const Polygons &polygons) {
    return painterPathFromPolygons(polygons);
}

FeatureChoice featureChoice(
    const ContourFeature &feature,
    int featureIndex,
    const Polygons &coverage) {
    FeatureChoice result;
    result.featureIndex = featureIndex;
    if (feature.kind
        == ContourFeatureKind::SmoothJunction) {
        const BoundaryLocation location =
            closestBoundaryLocation(
                feature.position, coverage);
        result.distance = location.distance;
        result.tangentError =
            std::min(
                angleBetween(
                    feature.outgoingTangent,
                    location.tangent),
                angleBetween(
                    feature.outgoingTangent,
                    -location.tangent));
        result.primitive = location.primitive;
        return result;
    }

    int primitive = 0;
    for (const QPolygonF &polygon : coverage) {
        for (int index = 0;
             index < polygon.size(); ++index, ++primitive) {
            const QPointF incoming = normalized(
                polygon[index]
                - polygon[
                    (index + polygon.size() - 1)
                    % polygon.size()]);
            const QPointF outgoing = normalized(
                polygon[
                    (index + 1)
                    % polygon.size()]
                - polygon[index]);
            if (angleBetween(incoming, outgoing)
                < kShapeCornerSalience) {
                continue;
            }
            const double distance =
                QLineF(
                    feature.position,
                    polygon[index]).length();
            const double directError =
                0.5
                * (angleBetween(
                       feature.incomingTangent,
                       incoming)
                   + angleBetween(
                       feature.outgoingTangent,
                       outgoing));
            const double reverseError =
                0.5
                * (angleBetween(
                       feature.incomingTangent,
                       -outgoing)
                   + angleBetween(
                       feature.outgoingTangent,
                       -incoming));
            const double tangentError =
                std::min(
                    directError, reverseError);
            if (distance
                    < result.distance
                        - kGeometryEpsilon
                || (std::abs(
                        distance - result.distance)
                        <= kGeometryEpsilon
                    && (tangentError
                            < result.tangentError
                                - kGeometryEpsilon
                        || (std::abs(
                                tangentError
                                - result.tangentError)
                                <= kGeometryEpsilon
                            && primitive
                                < result.primitive)))) {
                result.distance = distance;
                result.tangentError =
                    tangentError;
                result.primitive = primitive;
            }
        }
    }

    return result;
}

double mean(const QVector<double> &values) {
    if (values.isEmpty()) {
        return 0.0;
    }

    return std::accumulate(
               values.cbegin(),
               values.cend(), 0.0)
        / static_cast<double>(values.size());
}

} // namespace

Polygons placementFootprints(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog) {
    Polygons result;
    result.reserve(placements.size());
    for (const Placement &placement : placements) {
        const ShapeMesh *shape =
            shapeById(catalog, placement.shapeId);
        if (shape != nullptr) {
            result.push_back(
                transformedBoundary(
                    *shape,
                    placement.transform));
        }
    }

    return result;
}

ExactCoverState exactCoverState(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea) {
    ExactCoverState result;
    result.footprints =
        placementFootprints(placements, catalog);
    result.coverage =
        unionPolygons(result.footprints);
    result.residual =
        differencePolygons(
            mustCover, result.coverage);
    result.residualArea =
        polygonSetArea(result.residual);
    result.coveredArea =
        std::max(
            0.0,
            targetArea - result.residualArea);
    result.outsideArea = polygonSetArea(
        differencePolygons(
            result.coverage, mayCover));

    return result;
}

FeatureMatchSummary matchContourFeatures(
    const Polygons &coverage,
    const QVector<ContourFeature> &features) {
    FeatureMatchSummary result;
    result.distances.fill(
        std::numeric_limits<double>::infinity(),
        features.size());
    result.tangentErrors.fill(
        kPi, features.size());
    for (const ContourFeature &feature : features) {
        result.totalWeight += feature.weight;
    }
    if (coverage.isEmpty()) {
        return result;
    }

    QVector<FeatureChoice> choices;
    choices.reserve(features.size());
    for (int index = 0;
         index < features.size(); ++index) {
        FeatureChoice choice =
            featureChoice(
                features[index],
                index, coverage);
        result.distances[index] =
            choice.distance;
        result.tangentErrors[index] =
            choice.tangentError;
        if (choice.distance
                <= features[index].captureRadius
                    + kGeometryEpsilon
            && choice.tangentError
                <= kFeatureTangentSigma * 2.0) {
            choices.push_back(choice);
        }
    }
    std::stable_sort(
        choices.begin(), choices.end(),
        [](const FeatureChoice &left,
           const FeatureChoice &right) {
            if (std::abs(
                    left.distance - right.distance)
                > kGeometryEpsilon) {
                return left.distance
                    < right.distance;
            }
            if (std::abs(
                    left.tangentError
                    - right.tangentError)
                > kGeometryEpsilon) {
                return left.tangentError
                    < right.tangentError;
            }
            return left.featureIndex
                < right.featureIndex;
        });
    QSet<int> claimedPrimitives;
    for (const FeatureChoice &choice : choices) {
        if (claimedPrimitives.contains(
                choice.primitive)) {
            continue;
        }
        const ContourFeature &feature =
            features[choice.featureIndex];
        if (exposedFeatureArc(
                coverage, feature)
            < feature.captureRadius
                * kMinimumFeatureArcFraction) {
            continue;
        }
        claimedPrimitives.insert(
            choice.primitive);
        result.representedIds.push_back(
            feature.id);
        result.representedWeight +=
            feature.weight;
    }
    std::sort(
        result.representedIds.begin(),
        result.representedIds.end());

    return result;
}

double permittedOutsideEnvelopeArea(
    const FillOptions &options) {
    return options.useWeightedContour
        ? kEnvelopeAreaEpsilon
        : options.epsSpill;
}

bool legalEnvelopeArea(
    double outsideEnvelopeArea,
    const FillOptions &options) {
    return std::isfinite(outsideEnvelopeArea)
        && outsideEnvelopeArea
            <= permittedOutsideEnvelopeArea(
                   options)
                + kGeometryEpsilon;
}

bool legalOutwardDistance(
    double maximumOutwardDistance,
    const FillOptions &options) {
    return options.useContourLeeway
        || !options.useWeightedContour
        || (std::isfinite(
                maximumOutwardDistance)
            && maximumOutwardDistance
                <= options.boundaryTolerance
                    + kEnvelopeDistanceEpsilon);
}

double exposedFeatureArc(
    const Polygons &coverage,
    const ContourFeature &feature) {
    double result = 0.0;
    for (const QPolygonF &polygon : coverage) {
        for (int index = 0;
             index < polygon.size(); ++index) {
            const QPointF start = polygon[index];
            const QPointF end =
                polygon[(index + 1)
                        % polygon.size()];
            const QPointF middle =
                (start + end) * 0.5;
            const double distance =
                std::min({
                    QLineF(
                        feature.position,
                        start).length(),
                    QLineF(
                        feature.position,
                        middle).length(),
                    QLineF(
                        feature.position,
                        end).length(),
                });
            if (distance
                <= feature.captureRadius) {
                result +=
                    QLineF(start, end).length();
            }
        }
    }

    return result;
}

CoverErrorMetrics evaluateCoverMetrics(
    const Polygons &target,
    const Polygons &legalEnvelope,
    const Polygons &coverage,
    const QVector<ContourFeature> &features,
    const FillOptions &options) {
    CoverErrorMetrics result;
    const double targetArea =
        polygonSetArea(target);
    const double truePositiveArea =
        polygonSetArea(
            intersectionPolygons(
                target, coverage));
    result.missingArea = polygonSetArea(
        differencePolygons(
            target, coverage));
    result.outsideTargetArea =
        polygonSetArea(
            differencePolygons(
                coverage, target));
    result.outsideEnvelopeArea =
        polygonSetArea(
            differencePolygons(
                coverage, legalEnvelope));
    const double tverskyDenominator =
        truePositiveArea
        + options.tverskyAlpha
            * result.outsideTargetArea
        + options.tverskyBeta
            * result.missingArea;
    result.tversky =
        tverskyDenominator > kGeometryEpsilon
        ? truePositiveArea
            / tverskyDenominator
        : (targetArea <= kGeometryEpsilon
               ? 1.0
               : 0.0);

    const QRectF targetBounds =
        polygonBounds(target);
    const double fallbackDistance =
        std::max(
            options.boundaryTolerance,
            std::hypot(
                targetBounds.width(),
                targetBounds.height()));
    const QVector<QPointF> targetSamples =
        boundarySamples(
            target,
            options.boundaryTolerance);
    const QVector<QPointF> coverageSamples =
        boundarySamples(
            coverage,
            options.boundaryTolerance);
    const QVector<double> targetDistances =
        boundaryDistances(
            targetSamples, coverage,
            fallbackDistance);
    const QVector<double> coverageDistances =
        boundaryDistances(
            coverageSamples, target,
            fallbackDistance);
    QVector<double> symmetricDistances =
        targetDistances;
    symmetricDistances += coverageDistances;
    result.meanBoundaryDistance =
        mean(symmetricDistances);
    if (!symmetricDistances.isEmpty()) {
        double squaredDistance = 0.0;
        for (const double distance :
             symmetricDistances) {
            squaredDistance +=
                distance * distance;
        }
        result.boundaryDistanceRms =
            std::sqrt(
                squaredDistance
                / static_cast<double>(
                    symmetricDistances.size()));
    }
    result.boundaryDistance95 =
        percentile(
            symmetricDistances, 0.95);
    const QPainterPath targetPath =
        polygonPath(target);
    for (int index = 0;
         index < coverageSamples.size(); ++index) {
        if (!targetPath.contains(
                coverageSamples[index])) {
            result.maximumOutwardDistance =
                std::max(
                    result.maximumOutwardDistance,
                    coverageDistances[index]);
        }
    }
    const auto withinTolerance =
        [&](const QVector<double> &distances) {
            return std::count_if(
                distances.cbegin(),
                distances.cend(),
                [&](double distance) {
                    return distance
                        <= options.boundaryTolerance;
                });
        };
    const double recall =
        targetDistances.isEmpty()
        ? 1.0
        : static_cast<double>(
              withinTolerance(
                  targetDistances))
            / static_cast<double>(
                targetDistances.size());
    const double precision =
        coverageDistances.isEmpty()
        ? 0.0
        : static_cast<double>(
              withinTolerance(
                  coverageDistances))
            / static_cast<double>(
                coverageDistances.size());
    result.boundaryFScore =
        precision + recall
                > kGeometryEpsilon
        ? 2.0 * precision * recall
            / (precision + recall)
        : 0.0;

    const FeatureMatchSummary featureMatches =
        matchContourFeatures(
            coverage, features);
    result.representedFeatureWeight =
        featureMatches.representedWeight;
    result.totalFeatureWeight =
        featureMatches.totalWeight;
    result.representedFeatures =
        featureMatches.representedIds.size();
    result.totalFeatures = features.size();
    QVector<double> finiteFeatureDistances;
    QVector<double> finiteTangentErrors;
    for (int index = 0;
         index < features.size(); ++index) {
        finiteFeatureDistances.push_back(
            std::isfinite(
                featureMatches.distances[index])
            ? featureMatches.distances[index]
            : fallbackDistance);
        finiteTangentErrors.push_back(
            std::isfinite(
                featureMatches.tangentErrors[index])
            ? featureMatches.tangentErrors[index]
            : kPi);
    }
    result.meanFeatureDistance =
        mean(finiteFeatureDistances);
    result.featureDistance95 =
        percentile(
            finiteFeatureDistances, 0.95);
    if (!finiteFeatureDistances.isEmpty()) {
        result.maximumFeatureDistance =
            *std::max_element(
                finiteFeatureDistances.cbegin(),
                finiteFeatureDistances.cend());
    }
    result.meanTangentError =
        mean(finiteTangentErrors);
    if (!finiteTangentErrors.isEmpty()) {
        result.maximumTangentError =
            *std::max_element(
                finiteTangentErrors.cbegin(),
                finiteTangentErrors.cend());
    }

    return result;
}

void assignFeatureOwnership(
    QVector<Placement> *placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &coverage,
    const QVector<ContourFeature> &features) {
    for (Placement &placement : *placements) {
        placement.ownedFeatureIds.clear();
        placement.exposedContourArc = 0.0;
    }
    const FeatureMatchSummary matches =
        matchContourFeatures(
            coverage, features);
    for (const int featureId :
         matches.representedIds) {
        const auto foundFeature =
            std::find_if(
                features.cbegin(),
                features.cend(),
                [featureId](
                    const ContourFeature &feature) {
                    return feature.id
                        == featureId;
                });
        if (foundFeature == features.cend()) {
            continue;
        }
        int owner = -1;
        double ownerDistance =
            std::numeric_limits<double>::max();
        for (int index = 0;
             index < placements->size(); ++index) {
            const Placement &placement =
                placements->at(index);
            const ShapeMesh *shape =
                shapeById(
                    catalog,
                    placement.shapeId);
            if (shape == nullptr) {
                continue;
            }
            const Polygons footprint{
                transformedBoundary(
                    *shape,
                    placement.transform),
            };
            const BoundaryLocation candidate =
                closestBoundaryLocation(
                    foundFeature->position,
                    footprint);
            const BoundaryLocation exposed =
                closestBoundaryLocation(
                    candidate.point,
                    coverage);
            if (candidate.distance
                    <= foundFeature->captureRadius
                && exposed.distance
                    <= std::max(
                        kGeometryEpsilon,
                        foundFeature->captureRadius
                            * 0.05)
                && (candidate.distance
                        < ownerDistance
                            - kGeometryEpsilon
                    || (std::abs(
                            candidate.distance
                            - ownerDistance)
                            <= kGeometryEpsilon
                        && index < owner))) {
                owner = index;
                ownerDistance =
                    candidate.distance;
            }
        }
        if (owner >= 0) {
            Placement &placement =
                (*placements)[owner];
            placement.ownedFeatureIds.push_back(
                featureId);
            placement.exposedContourArc +=
                exposedFeatureArc(
                    coverage, *foundFeature);
        }
    }
}

void refreshPlacementGains(
    QVector<Placement> *placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover) {
    Polygons residual = mustCover;
    double previousArea =
        polygonSetArea(residual);
    for (Placement &placement : *placements) {
        const ShapeMesh *shape =
            shapeById(catalog, placement.shapeId);
        if (shape == nullptr) {
            placement.coveredArea = 0.0;
            continue;
        }
        const Polygons footprint{
            transformedBoundary(
                *shape, placement.transform),
        };
        Polygons nextResidual =
            differencePolygons(
                residual, footprint);
        const double nextArea =
            polygonSetArea(nextResidual);
        placement.coveredArea =
            std::max(
                0.0,
                previousArea - nextArea);
        residual = std::move(nextResidual);
        previousArea = nextArea;
    }
}

#ifdef FLS_DIFFERENTIAL_COVER_TESTS
double placementUnionAreaForTesting(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog) {
    return polygonSetArea(
        unionPolygons(
            placementFootprints(
                placements, catalog)));
}
#endif

} // namespace gui::cover
