#pragma once

#include "pen_fill.h"

namespace gui {

struct CurveFillCatalog {
    QVector<PenPrimitive> primitives;
    QVector<cover::ShapeMesh> meshes;
    QString error;

    bool valid() const {
        return error.isEmpty() && !primitives.isEmpty() && !meshes.isEmpty();
    }
};

using CurveCatalogProgress =
    std::function<void(const QString &, int, int)>;

struct CurveTemplateGenerationResult {
    int generated = 0;
    int skipped = 0;
    bool cancelled = false;
    QString error;
};

CurveFillCatalog buildCurveFillCatalog(
    const ShapeGeometryStore &geometry,
    const CurveCatalogProgress &progress = {},
    const std::function<bool()> &cancelled = {});

CurveFillCatalog cachedCurveFillCatalog(
    const ShapeGeometryStore &geometry,
    const CurveCatalogProgress &progress = {},
    const std::function<bool()> &cancelled = {});

void invalidateCurveFillCatalog();

CurveTemplateGenerationResult generateCurveFillTemplates(
    const ShapeGeometryStore &geometry,
    const QString &assetDirectory,
    bool force,
    const CurveCatalogProgress &progress = {},
    const std::function<bool()> &cancelled = {});

QVector<PenPrimitive> buildCurvePrimitiveCatalog(
    const ShapeGeometryStore &geometry,
    QString *error = nullptr);

} // namespace gui
