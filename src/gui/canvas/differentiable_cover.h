#pragma once

#include "shape_geometry_store.h"

#include <QtCore>
#include <QtGui>

#include <array>
#include <cstdint>
#include <functional>

namespace gui::cover {

inline constexpr int kDefaultBudget = 100000;
inline constexpr double kDefaultSpillWeight = 8.0;
inline constexpr double kDefaultEpsArea = 0.25;
inline constexpr double kDefaultEpsGain = 1.0;
inline constexpr double kDefaultEpsSpill = 0.25;
inline constexpr int kDefaultAdamIterations = 200;
inline constexpr double kDefaultAdamLearningRate = 0.05;
inline constexpr int kDefaultRestarts = 2;

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

struct ShapeMesh {
    QVector<Vec2> vertices;
    QVector<std::array<int, 3>> triangles;
    QVector<Vec2> boundary;
    QString error;
    int id = 0;
    double area = 0.0;
    bool convex = false;

    bool valid() const {
        return error.isEmpty() && id > 0 && vertices.size() >= 3
            && !triangles.isEmpty() && boundary.size() >= 3 && area > 0.0;
    }
};

struct Affine {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;
};

struct Placement {
    Affine transform;
    int shapeId = 0;
    double coveredArea = 0.0;
};

using Polygons = QVector<QPolygonF>;

struct FillInput {
    Polygons mustCover;
    Polygons mayCover;
    QImage mask;
    QRectF maskBounds;
};

struct FillOptions {
    int budget = kDefaultBudget;
    int adamIterations = kDefaultAdamIterations;
    int restarts = kDefaultRestarts;
    double spillWeight = kDefaultSpillWeight;
    double epsArea = kDefaultEpsArea;
    double epsGain = kDefaultEpsGain;
    double epsSpill = kDefaultEpsSpill;
    double adamLearningRate = kDefaultAdamLearningRate;
    std::uint64_t seed = 0;
    bool useRouter = true;
};

struct FillResult {
    QVector<Placement> placements;
    Polygons residual;
    QString error;
    double residualArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
    bool budgetHit = false;
    bool stalled = false;
    bool cancelled = false;
};

struct AreaGradient {
    std::array<double, 6> coveredGradient{};
    std::array<double, 6> spillGradient{};
    double covered = 0.0;
    double spill = 0.0;
};

QVector<ShapeMesh> buildShapeCatalog(const ShapeGeometryStore &geometry,
                                     QString *error = nullptr);

Polygons polygonsFromPainterPath(const QPainterPath &path);

QPainterPath painterPathFromPolygons(const Polygons &polygons);

AreaGradient evaluateAreaGradient(const ShapeMesh &shape,
                                  const Affine &transform,
                                  const Polygons &coveredSubject,
                                  const Polygons &legalSubject);

FillResult analyticCoverFill(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options = {},
    const std::function<bool()> &cancelled = {});

QTransform toQTransform(const Affine &transform);

} // namespace gui::cover
