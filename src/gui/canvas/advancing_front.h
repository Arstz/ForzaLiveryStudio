#pragma once

#include "pen_fill.h"
#include "polygon_mesh.h"

#include <QtCore>
#include <QtGui>

#include <functional>

namespace gui {

inline constexpr double kAdvancingFrontContourEpsilon = 1.9;
inline constexpr double kAdvancingFrontMinimumTargetCoverage = 0.99;
inline constexpr double kAdvancingFrontMaximumLeakageFraction = 0.02;

struct AdvancingFrontOptions {
    QVector<PenPlacement> baselinePlacements;
    QVector<double> exposedSimplificationEpsilons = {0.35, 0.5, 0.75, 1.0};
    double originalContourEpsilon = kAdvancingFrontContourEpsilon;
    double fallbackContourEpsilon = kAdvancingFrontContourEpsilon;
    double candidateGuideEpsilon = 0.35;
    double contourMatchDistance = 1.9;
    double sampleSpacing = 0.5;
    double contourWeight = 0.75;
    double areaWeight = 0.25;
    double regressionScoreDrop = 0.02;
    double maximumLeakageDssim = 0.01;
    double maximumOmissionDssim = 0.01;
    double maximumFinalDssim = 0.01;
    double completeFitDssim = 0.01;
    double completeFitCoverage = 0.98;
    double maximumScaleCondition = 20.0;
    double redundancyPenaltyWeight = 0.02;
    double fragmentationPenaltyWeight = 0.03;
    double minimumTargetCoverage = kAdvancingFrontMinimumTargetCoverage;
    double maximumLeakageFraction = kAdvancingFrontMaximumLeakageFraction;
    double baselineReplacementCoverage = 0.8;
    int dssimSupersample = 4;
    int dssimPadding = 8;
    int seedEdgeCount = 16;
    int finalistCount = 12;
    int candidateGuidePointCap = 48;
    int preflightCandidateBudget = 256;
    int minimumBaselineShapesReplaced = 2;
    int primitiveBoundaryOrientations = 8;
    int minimumSpanTests = 6;
    int regressionSpanCount = 3;
    int maximumSpanPoints = 64;
    int minimumBoundarySamples = 32;
    int maximumBoundarySamples = 256;
    int rollbackDepth = 2;
    int alternativesPerStep = 4;
    int cheapBudgetBase = 4096;
    int cheapBudgetPerPoint = 512;
    int cheapBudgetCap = 250000;
    int dssimBudgetBase = 128;
    int dssimBudgetPerPoint = 16;
    int dssimBudgetCap = 8192;
    int coverageRebuildInterval = 16;
    int workerCount = 0;
    int maximumOutputShapes = -1;
    bool verboseRejectedCandidates = false;
};

struct AdvancingFrontStats {
    QHash<int, int> acceptedPrimitiveCounts;
    QHash<QString, int> rejectedCandidateCounts;
    QStringList diagnostics;
    qint64 candidateElapsedMs = 0;
    qint64 dssimElapsedMs = 0;
    qint64 fallbackElapsedMs = 0;
    qint64 totalElapsedMs = 0;
    double initialArea = 0.0;
    double residualArea = 0.0;
    double discardedArea = 0.0;
    double leakageDssim = 0.0;
    double omissionDssim = 0.0;
    double finalDssim = 0.0;
    double targetCoverage = 0.0;
    double leakageFraction = 0.0;
    int originalPointCount = 0;
    int guidePointCount = 0;
    int cheapCandidates = 0;
    int prunedCandidates = 0;
    int refinedCandidates = 0;
    int acceptedCandidates = 0;
    int componentSplits = 0;
    int backtracks = 0;
    int fallbackShapes = 0;
    int preflightCandidates = 0;
    int baselineShapesReplaced = 0;
    bool cheapBudgetExhausted = false;
    bool dssimBudgetExhausted = false;
    bool finalGuardFailed = false;
    bool countLimitReached = false;
    bool preflightRejected = false;
};

struct AdvancingFrontResult {
    QVector<PenPlacement> placements;
    AdvancingFrontStats stats;
    QPainterPath coverage;
    QPainterPath residual;
    QString error;
    bool cancelled = false;
    bool structurallyComplete = false;

    bool valid() const {
        return error.isEmpty() && !placements.isEmpty() && !cancelled
            && structurallyComplete;
    }
};

AdvancingFrontOptions defaultAdvancingFrontOptions();

AdvancingFrontResult fillRegionAdvancingFront(
    const QPainterPath &target,
    const QVector<PenPrimitive> &primitives,
    const PolygonMeshSources &meshSources,
    const QSize &imageSize,
    const AdvancingFrontOptions &options = {},
    const std::function<void(double residualFraction, int candidatesTested)> &progress = {},
    const std::function<bool()> &cancelled = {});

QString advancingFrontOptionsLog(const AdvancingFrontOptions &options);

} // namespace gui
