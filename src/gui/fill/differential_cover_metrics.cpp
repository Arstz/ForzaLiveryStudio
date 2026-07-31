#include "differential_cover_internal.h"

#include <algorithm>
#include <utility>

namespace gui::cover {

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
                transformedBoundary(*shape, placement.transform));
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
    result.coverage = unionPolygons(result.footprints);
    result.residual =
        differencePolygons(mustCover, result.coverage);
    result.residualArea = polygonSetArea(result.residual);
    result.coveredArea =
        std::max(0.0, targetArea - result.residualArea);
    result.outsideArea = polygonSetArea(
        differencePolygons(
            result.coverage, mayCover));

    return result;
}

void refreshPlacementGains(
    QVector<Placement> *placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover) {
    Polygons residual = mustCover;
    double previousArea = polygonSetArea(residual);
    for (Placement &placement : *placements) {
        const ShapeMesh *shape =
            shapeById(catalog, placement.shapeId);
        if (shape == nullptr) {
            placement.coveredArea = 0.0;
            continue;
        }
        const Polygons footprint{
            transformedBoundary(*shape, placement.transform),
        };
        Polygons nextResidual =
            differencePolygons(residual, footprint);
        const double nextArea = polygonSetArea(nextResidual);
        placement.coveredArea =
            std::max(0.0, previousArea - nextArea);
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
