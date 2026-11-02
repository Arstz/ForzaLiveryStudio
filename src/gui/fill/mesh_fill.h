#pragma once

#include "compact_fit_catalog.h"
#include "mesh_data.h"

namespace gui {

struct MeshFillRequest {
    fls::scene::MeshData mesh;
    QVector<catalog::Primitive> primitives;
    int shapeBudget = catalog::kDefaultShapeBudget;
    int profileTrialBudget = 0;
};

struct MeshFillLayer {
    QString name;
    QColor color;
    QVector<PenPlacement> placements;
    int face = -1;
    bool complete = false;
};

struct MeshFillProgress {
    int completedFaces = 0;
    int totalFaces = 0;
    int shapeCount = 0;
};

struct MeshFillResult {
    QVector<MeshFillLayer> layers;
    QJsonObject diagnostics;
    QString error;
    int totalFaces = 0;
    int completedFaces = 0;
    int shapeCount = 0;
    bool stopped = false;
};

MeshFillResult fillMesh(const MeshFillRequest &request,
                        const std::function<bool()> &stopRequested = {},
                        const std::function<void(const MeshFillProgress &)> &progress = {});

} // namespace gui
