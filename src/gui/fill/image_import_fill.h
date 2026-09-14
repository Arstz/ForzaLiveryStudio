#pragma once

#include "pen_fill.h"
#include "svg_vector_objects.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

// One vector object to be covered with shapes. The outline is in the source
// image's pixel space (y down) and may contain several islands, each with
// interior cutouts.
struct ImageImportFillUnit {
    QPainterPath outline;
    QColor color;
};

struct ImageImportFillRequest {
    QVector<ImageImportFillUnit> units;
    QVector<PenPrimitive> primitives;
    double boundaryTolerance = 0.1;
    qint64 componentBudgetMs = 3000;
    qint64 componentBudgetMsPerPoint = 40;
    int shapeLimitPerPoint = 6;
};

// Pen loops for one connected component: the outer loop first, then cutouts.
struct ImageImportPenLoops {
    QVector<PenLoop> loops;
    QString via;
    QString error;

    bool valid() const { return error.isEmpty() && !loops.isEmpty(); }
};

// Pen loops for every connected component of one object.
struct ImageImportUnitLoops {
    QVector<QVector<PenLoop>> components;
    QString via;
    QString error;

    bool valid() const { return error.isEmpty() && !components.isEmpty(); }
};

// The outcome for one connected component of one object.
struct ImageImportComponentResult {
    QString error;
    qint64 elapsedMs = 0;
    int loopCount = 0;
    int pointCount = 0;
    int placementCount = 0;
    bool timedOut = false;

    bool filled() const { return error.isEmpty() && placementCount > 0; }
};

struct ImageImportFilledUnit {
    QColor color;
    QVector<PenPlacement> placements;
    QVector<ImageImportComponentResult> components;
    QString via;
    QString error;
    qint64 elapsedMs = 0;
    int componentCount = 0;
    int failedComponentCount = 0;
    int timedOutComponentCount = 0;
    int loopCount = 0;
    bool timedOut = false;

    bool filled() const { return error.isEmpty() && !placements.isEmpty(); }
};

struct ImageImportFillResult {
    QVector<ImageImportFilledUnit> units;
    QHash<QString, int> componentFailureReasons;
    QString summary;
    QString error;
    bool cancelled = false;
    int filledCount = 0;
    int partialCount = 0;
    int failedCount = 0;
    int timedOutCount = 0;
    int placementCount = 0;
    int componentCount = 0;
    int filledComponentCount = 0;
    int failedComponentCount = 0;
    int timedOutComponentCount = 0;

    bool complete() const { return error.isEmpty() && !cancelled && failedComponentCount == 0; }
};

using ImageImportFillProgress = std::function<void(int completed, int total)>;

QVector<ImageImportFillUnit> svgImageImportUnits(const SvgVectorDocument &document);

QVector<QPainterPath> imageImportOutlineComponents(const QPainterPath &outline);

ImageImportPenLoops imageImportPenLoops(const QPainterPath &component,
                                        bool preferSampled = false);

ImageImportUnitLoops imageImportUnitLoops(const QPainterPath &outline);

ImageImportFillResult computeImageImportFills(
    const ImageImportFillRequest &request,
    const ImageImportFillProgress &progress = {},
    const std::function<bool()> &cancelled = {});

} // namespace gui
