#pragma once

#include "pen_fill.h"
#include "polygon_mesh.h"
#include "region_extract.h"

#include <QtGui>

#include <array>
#include <cstdint>
#include <functional>

namespace gui {

struct RegionFillLayer {
    QColor color;
    double area = 0.0;
    QVector<PenPlacement> placements;
    int drawOrder = -1;
};

struct GeneratedRegionShape {
    int shapeId = 0;
    QTransform transform;
    std::array<std::uint8_t, 4> color = {255, 255, 255, 255};
};

struct GeneratedRegionGroup {
    QVector<GeneratedRegionShape> shapes;
};

struct RegionFillBatchRequest {
    RegionExtractionResult regions;
    QVector<PenPrimitive> primitives;
    PolygonMeshSources meshSources;
    QString overlayGuideId;
    quint64 overlayGeneration = 0;
};

struct RegionFillBatchResult {
    QVector<RegionFillLayer> fills;
    QHash<int, QPainterPath> silhouettes;
    QString overlayGuideId;
    quint64 overlayGeneration = 0;
    QString summary;
    QString error;
    bool cancelled = false;
    int completedRegions = 0;
    int totalRegions = 0;
};

RegionFillBatchResult computeRegionFills(
    const RegionFillBatchRequest &request,
    const std::function<void(int completed, int total)> &progress = {},
    const std::function<bool()> &cancelled = {});

void sortRegionFillLayersByDrawOrder(QVector<RegionFillLayer> *layers);

struct RegionPenConversionOptions {
    double mergeTolerance = 1.1;
    double maximumDssim = 0.001;
    int dssimSupersample = 4;
    QSize comparisonImageSize;
    double closureTolerance = 1e-6;
    int maxOptimizedPointCount = 0;
    int adaptiveSearchSteps = 8;
};

struct RegionPenConversionResult {
    QVector<PenPoint> points;
    QString error;
    int originalPointCount = 0;
    int removedHardPoints = 0;
    bool optimizationSkipped = false;
    double baselineDeviation = 0.0;
    double maximumDeviation = 0.0;
    double dssim = 0.0;

    bool valid() const { return error.isEmpty() && points.size() >= 3; }
};

struct RegionFillContourStats {
    int originalPointCount = 0;
    int optimizedPointCount = 0;
    int flattenedPointCount = 0;
    int removedHardPoints = 0;
    bool optimizationSkipped = false;
    bool baselineRetry = false;
    double dssim = 0.0;
};

RegionPenConversionResult regionOutlineToPenPoints(
    const QPainterPath &outline,
    const RegionPenConversionOptions &options = {});

PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled = {},
                                QPolygonF *optimizedContour = nullptr,
                                RegionFillContourStats *contourStats = nullptr,
                                QVector<PenPoint> *optimizedPenPoints = nullptr,
                                const QSize &comparisonImageSize = {});

QPolygonF simplifyClosedPolygon(const QPolygonF &polygon, double epsilon);

QPolygonF simplifyClosedPolygonCorridor(
    const QPolygonF &polygon, double epsilon,
    const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace);

QPolygonF regionOuterContour(const QPainterPath &outline);

PenFillResult fillPolygonMesh(const QPolygonF &polygon,
                              const PolygonMeshSources &sources,
                              const std::function<bool()> &cancelled = {});

PenFillResult fillRegionOutlineMesh(const QPainterPath &outline,
                                    const PolygonMeshSources &sources,
                                    double simplifyEpsilon,
                                    const std::function<bool()> &cancelled = {});

} // namespace gui
