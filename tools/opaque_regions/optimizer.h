#pragma once

#include "fill_contour.h"
#include "polygon_mesh.h"
#include "svg_vector_objects.h"

#include <functional>
#include <map>
#include <set>

namespace opaque {

struct Options {
    std::function<void(const QJsonObject &)> onSupportTrial;
    double flattenTolerance = 0.05;
    double areaTolerance = 1e-6;
    double visibleAreaTolerance = 0;
    double renderRmse = 0.5;
    double renderMaxError = 16.0;
    double silhouetteTolerance = 1e-6;
    double pathWeight = 1.0;
    double segmentWeight = 1.0;
    double controlPointWeight = 0.0;
    double holeWeight = 1.0;
    double componentWeight = 1.0;
    double orderingWeight = 0.0;
    double nativeItemWeight = 0.0;
    double nativeSegmentWeight = 0.0;
    int maximumPasses = 20;
    int maximumChordSpans = 8;
    QString strategy = "legacy";
    double hardPointWeight = 1.0;
    double softPointWeight = 2.0;
    double concavityWeight = 2.0;
    double lookaheadWeight = 1.0;
    int shortlistSize = 12;
    int groupNeighbors = 2;
    int completionCandidates = 16;
    bool compareStrategies = false;
    int componentLimit = 8;
    int checkpointEdits = 32;
    bool progress = false;
    bool colorOwnership = false;
};

struct Item {
    QPainterPath path;
    QColor color;
    std::set<int> owners;
};

struct ColorOptions {
    QString cachePath;
    double tolerance = 5.0;
    double detailScale = 24.0;
    double boundaryTolerance = 0.25;
    double renderRmse = 12.0;
    double renderMaxError = 64.0;
    double localRmse = 12.0;
    int localWindow = 16;
    double contrastRetention = 0.75;
    double coverageOverlap = 1.0;
    bool reconstruct = true;
};

struct ColorResult {
    QVector<Item> items;
    QVector<Item> paletteItems;
    QVector<Item> regionalItems;
    QVector<Item> topologyCandidate;
    QVector<Item> reconstructionCandidate;
    QJsonObject report;
    bool changed = false;
};

struct PenBaseline {
    QVector<Item> items;
    QJsonObject report;
    QString error;
    bool changed = false;
};

struct Encoding {
    QVector<gui::SvgVectorObject> shapes;
    QVector<gui::PolygonMeshPlacement> placements;
    QByteArray records;
    int segments = 0;
    QString error;
};

struct Metrics {
    int paths = 0;
    int fills = 0;
    int singlePathFills = 0;
    int maximumPathsPerFill = 0;
    int subpaths = 0;
    int lineSegments = 0;
    int bezierSegments = 0;
    int maximumSegmentsPerPath = 0;
    int maximumComponentsPerPath = 0;
    int maximumHolesPerPath = 0;
    int orderingEdges = 0;
    int orderingDepth = 0;
    int drawItems = 0;
    int encodedSegments = 0;
    int holes = 0;
    int components = 0;
    int recordBytes = 0;
    double cost = 0.0;
    bool encodingMeasured = false;
    bool orderingMeasured = false;
    bool costMeasured = false;
    QJsonObject json() const;
};

struct Graph {
    std::set<std::pair<int, int>> edges;
    QVector<int> order;
    QString error;
    QJsonObject json() const;
};

using Encoder = std::function<Encoding(const QPainterPath &, QColor)>;
using Renderer = std::function<QImage(const QVector<gui::SvgVectorObject> &)>;

struct Result {
    struct Benchmark {
        QVector<gui::SvgVectorObject> before;
        QVector<gui::SvgVectorObject> after;
        QJsonArray sources;
        double saving = 0.0;
    };
    QVector<Item> items;
    QVector<gui::SvgVectorObject> nativeShapes;
    QVector<Item> candidateItems;
    QVector<gui::SvgVectorObject> ownershipBefore;
    QVector<gui::SvgVectorObject> ownershipAfter;
    QJsonObject candidateReport;
    Metrics before;
    Metrics after;
    Graph graph;
    QJsonArray diagnostics;
    QJsonObject decompositionError;
    QJsonArray strategies;
    QString selectedStrategy = "baseline";
    QString error;
    QVector<Benchmark> benchmarks;
    double silhouetteDifference = 0.0;
    double nativeSilhouetteDifference = 0.0;
    int accepted = 0;
};

QVector<Item> makeItems(const QVector<gui::SvgVectorObject> &objects);
QVector<gui::SvgVectorObject> objects(const QVector<Item> &items);
QImage render(const QVector<gui::SvgVectorObject> &objects, QSize size);
QImage renderCrop(const QVector<gui::SvgVectorObject> &objects, QRect rectangle);
QJsonObject imageError(const QImage &reference, const QImage &actual);
QJsonObject appearanceError(const QImage &reference, const QImage &actual, int window);
QJsonObject absorptionDetailError(const QImage &target, const QImage &baseline, const QImage &actual,
                                 const ColorOptions &limits, double edgeContrast);
QJsonObject absorptionRegionMetrics(const QVector<gui::SvgVectorObject> &regions, double tolerance);
QImage difference(const QImage &reference, const QImage &actual);
QByteArray svg(const QVector<gui::SvgVectorObject> &objects, QSize size);
Metrics pathMetrics(const QVector<gui::SvgVectorObject> &objects, double flattenTolerance);
double booleanCoordinateGrid();
QString penTargetError(const QPainterPath &path, const Options &options);
QJsonObject auditPenTargets(const QVector<gui::SvgVectorObject> &objects, const Options &options);
PenBaseline preparePenTargets(const QVector<Item> &items, const Options &options, const Renderer &renderer);
QVector<gui::SvgVectorObject> clipToViewport(const QVector<gui::SvgVectorObject> &objects,
                                           QSize size, const Options &options);
double silhouetteDifference(const QVector<gui::SvgVectorObject> &before,
                            const QVector<gui::SvgVectorObject> &after, double flattenTolerance);
QJsonObject regionDifficulty(const QPainterPath &path, const Options &options);
Encoder meshEncoder(const gui::PolygonMeshSources &sources, const Options &options);
Graph topologicalOrder(int count, const std::set<std::pair<int, int>> &edges,
                       const QVector<int> &keys = {});
ColorResult abstractColors(const QVector<Item> &baseline, const Options &options,
                          const ColorOptions &colors, const Renderer &renderer);

class Optimizer {
public:
    Optimizer(QVector<Item> baseline, Options options, Encoder encoder, Renderer renderer,
              const QVector<QPainterPath> &fixedOwnership = {}, const QPainterPath &fixedDomain = {});
    Result run();
    const QVector<Item> &baselineItems() const { return baseline_; }
    void setAppearanceGuard(QImage reference, ColorOptions limits);
    void setColorAbsorption(QImage target, ColorOptions limits, double sourceGrowth, double edgeContrast);
    Graph dependencies(const QVector<Item> &items) const;
    QString validate(const QVector<Item> &items) const;
    QJsonObject ownershipDiagnostics() const;

private:
    struct Proposal {
        QVector<Item> items;
        QString kind;
    };
    void proposals(const QVector<Item> &items, const std::function<void(Proposal)> &visit) const;
    Metrics measure(const QVector<Item> &items, QVector<gui::SvgVectorObject> *native,
                    QString *error) const;
    Result runStack();
    Result runIncremental();
    Result runSupports();

    QVector<Item> baseline_;
    QVector<QPainterPath> ownership_;
    QVector<int> ownerSources_;
    QPainterPath domain_;
    Options options_;
    Encoder encoder_;
    Renderer renderer_;
    QImage appearanceReference_;
    ColorOptions appearanceLimits_;
    QImage absorptionReference_;
    ColorOptions absorptionLimits_;
    double absorptionSourceGrowth_ = 0;
    double absorptionEdgeContrast_ = 24;
    bool absorptionEnabled_ = false;
};

int selfTest();

} // namespace opaque
