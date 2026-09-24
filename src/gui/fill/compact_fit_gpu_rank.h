#pragma once

#include <memory>
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

struct RankStats {
    std::string adapter;
    std::string error;
    int columns = 0;
    int rows = 0;
    int calls = 0;
    int refinementPreparations = 0;
    int refinementCalls = 0;
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
};

class RasterRanker {
public:
    virtual ~RasterRanker() = default;
    virtual bool evaluate(const Geometry &pieces, std::vector<double> *scores) = 0;
    virtual bool prepareAdditionCoverage(const Geometry &coverage) = 0;
    virtual bool evaluateAdditions(const Geometry &candidates,
        std::vector<double> *scores, const AdditionWeights &weights = {}) = 0;
    virtual RankStats stats() const = 0;
};

#ifdef FLS_HAS_CUDA
std::unique_ptr<RasterRanker> createRasterRanker(
    const Geometry &preferred, const Geometry &target,
    const Geometry &inner, const Geometry &outer,
    const Geometry &spillFree, double cellSize);
#endif

} // namespace gui::compact::gpu
