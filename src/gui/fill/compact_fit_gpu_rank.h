#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace gui::compact::gpu {

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

struct Loop {
    int pointOffset = 0;
    int pointCount = 0;
};

struct Piece {
    int loopOffset = 0;
    int loopCount = 0;
};

struct Geometry {
    std::vector<Point> points;
    std::vector<Loop> loops;
    std::vector<Piece> pieces;
};

struct Affine {
    float m11 = 1.0f;
    float m12 = 0.0f;
    float m21 = 0.0f;
    float m22 = 1.0f;
    float dx = 0.0f;
    float dy = 0.0f;
};

struct RankStats {
    std::string adapter;
    std::string error;
    int columns = 0;
    int rows = 0;
    int calls = 0;
    int refinementPreparations = 0;
    int refinementCalls = 0;
    int persistentPreparations = 0;
    int persistentCommits = 0;
    double cellSize = 0.0;
    double setupMilliseconds = 0.0;
    double evaluationMilliseconds = 0.0;
    double refinementMilliseconds = 0.0;
};

struct AdditionWeights {
    double preferred = 4.0;
    double inner = 16.0;
    double spill = 4.0;
    double join = 32.0;
    double boundary = 0.25;
};

class RasterRanker {
public:
    virtual ~RasterRanker() = default;
    virtual bool evaluate(const Geometry &pieces, std::vector<double> *scores) = 0;
    virtual bool prepareAdditionCoverage(const Geometry &coverage) = 0;
    virtual bool preparePlacementCoverage(const Geometry &coverage) = 0;
    virtual bool prepareReplacementCoverage(const Geometry &current) = 0;
    virtual bool commitReplacement(const Geometry &previous,
        const Geometry &replacement) = 0;
    virtual bool evaluateAdditions(const Geometry &candidates,
        std::vector<double> *scores, const AdditionWeights &weights = {}) = 0;
    virtual bool evaluateTransforms(const Geometry &primitive,
        const std::vector<Affine> &transforms, std::vector<double> *scores,
        const AdditionWeights &weights = {}) = 0;
    virtual RankStats stats() const = 0;
};

struct MaskWord {
    int index = 0;
    std::uint64_t bits = 0;
};

struct MaskCounts {
    unsigned int cells = 0;
    unsigned int boundary = 0;
};

struct MaskPoint {
    double x = 0.0;
    double y = 0.0;
};

struct MaskAffine {
    double m11 = 1.0;
    double m12 = 0.0;
    double m21 = 0.0;
    double m22 = 1.0;
    double dx = 0.0;
    double dy = 0.0;
};

struct MaskBounds {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

struct MaskGeometry {
    std::vector<MaskPoint> points;
    std::vector<Loop> loops;
    std::vector<Piece> pieces;
    std::vector<MaskBounds> bounds;
};

struct MaskGrid {
    double originX = 0.0;
    double originY = 0.0;
    double step = 1.0;
    int width = 0;
    int height = 0;
};

class BitmaskCover {
public:
    virtual ~BitmaskCover() = default;
    virtual bool score(std::vector<MaskCounts> *counts) = 0;
    virtual bool remove(int candidate) = 0;
    virtual std::string error() const = 0;
};

#ifdef FLS_HAS_CUDA
bool verifyRasterMaskIndex(const Geometry &geometry, double cellSize, std::string *error);

bool rasterizeBitmasks(const MaskGeometry &geometry, const MaskGrid &grid,
                       const std::vector<MaskPoint> &witnesses,
                       std::vector<std::uint64_t> *masks,
                       std::vector<std::uint64_t> *boundaryMasks,
                       std::string *error);
bool screenTransformContainment(const MaskGeometry &envelope,
                                const std::vector<MaskPoint> &probes,
                                const std::vector<MaskAffine> &transforms,
                                double clearance,
                                std::vector<std::uint8_t> *possible,
                                std::string *error);
bool fitContainmentRadii(const MaskGeometry &envelope,
                         const std::vector<MaskPoint> &probes,
                         const std::vector<MaskPoint> &centers,
                         const std::vector<MaskAffine> &unitTransforms,
                         double maximumRadius, int iterations, double clearance,
                         std::vector<double> *radii, std::string *error);
bool proveTransformContainment(const MaskGeometry &envelope,
                               const MaskGeometry &shape,
                               const std::vector<MaskAffine> &transforms,
                               double clearance,
                               std::vector<std::uint8_t> *contained,
                               std::string *error);
std::unique_ptr<BitmaskCover> createBitmaskCover(
    const std::vector<MaskWord> &words, const std::vector<int> &offsets,
    const std::vector<std::uint64_t> &missing, int cellWords);
std::unique_ptr<RasterRanker> createRasterRanker(
    const Geometry &preferred, const Geometry &target,
    const Geometry &inner, const Geometry &outer,
    const Geometry &spillFree, double cellSize);
#endif

} // namespace gui::compact::gpu
