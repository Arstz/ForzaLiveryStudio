#include "compact_fit_gpu_rank.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace gui::compact::gpu {
namespace {

constexpr int kTileExtent = 16;
constexpr int kThreads = 256;
constexpr int kBoundaryFieldPasses = 12;
constexpr long long kMaximumCells = 32LL * 1024 * 1024;
constexpr float kInvalidCoordinate = 3.402823466e+38F;

template <typename Value>
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    ~DeviceBuffer() {
        cudaFree(data_);
    }

    cudaError_t reserve(size_t count) {
        if (count <= capacity_) {
            return cudaSuccess;
        }
        cudaFree(data_);
        data_ = nullptr;
        capacity_ = 0;
        const cudaError_t status = cudaMalloc(reinterpret_cast<void **>(&data_),
            count * sizeof(Value));
        if (status == cudaSuccess) {
            capacity_ = count;
        }

        return status;
    }

    Value *data() const {

        return data_;
    }

private:
    Value *data_ = nullptr;
    size_t capacity_ = 0;
};

struct SetView {
    const Point *points = nullptr;
    const Loop *loops = nullptr;
    int loopCount = 0;
};

struct Tile {
    int piece = 0;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

struct TransformTile {
    int transform = 0;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

__device__ bool contains(float x, float y, const Point *points,
                         const Loop *loops, int loopOffset, int loopCount) {
    int winding = 0;
    for (int loopIndex = 0; loopIndex < loopCount; ++loopIndex) {
        const Loop loop = loops[loopOffset + loopIndex];
        for (int index = 0; index < loop.pointCount; ++index) {
            const Point first = points[loop.pointOffset + index];
            const Point second = points[loop.pointOffset + (index + 1) % loop.pointCount];
            const float cross = (second.x - first.x) * (y - first.y)
                - (second.y - first.y) * (x - first.x);
            if (first.y <= y && second.y > y && cross > 0.0f) {
                ++winding;
            } else if (first.y > y && second.y <= y && cross < 0.0f) {
                --winding;
            }
        }
    }

    return winding != 0;
}

__global__ void maskKernel(SetView preferred, SetView target, SetView inner,
                           SetView outer, SetView spillFree,
                           float originX, float originY,
                           float cellSize, int columns, int cells,
                           unsigned char *masks) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= cells) {
        return;
    }
    const float x = originX + (index % columns + 0.5f) * cellSize;
    const float y = originY + (index / columns + 0.5f) * cellSize;
    unsigned char mask = 0;
    mask |= contains(x, y, preferred.points, preferred.loops, 0,
        preferred.loopCount) ? 1 : 0;
    mask |= contains(x, y, target.points, target.loops, 0,
        target.loopCount) ? 2 : 0;
    mask |= contains(x, y, inner.points, inner.loops, 0,
        inner.loopCount) ? 4 : 0;
    mask |= contains(x, y, outer.points, outer.loops, 0,
        outer.loopCount) ? 8 : 0;
    mask |= contains(x, y, spillFree.points, spillFree.loops, 0,
        spillFree.loopCount) ? 16 : 0;
    masks[index] = mask;
}

__global__ void boundaryFieldKernel(const unsigned char *masks, int columns,
                                    int rows, unsigned short *field) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    const int cells = columns * rows;
    if (index >= cells) {
        return;
    }
    const int x = index % columns;
    const int y = index / columns;
    const bool target = (masks[index] & 2) != 0;
    unsigned short directions = 0;
    if (target && (x == 0 || (masks[index - 1] & 2) == 0)) {
        directions |= 1;
    }
    if (target && (x + 1 == columns || (masks[index + 1] & 2) == 0)) {
        directions |= 2;
    }
    if (target && (y == 0 || (masks[index - columns] & 2) == 0)) {
        directions |= 4;
    }
    if (target && (y + 1 == rows || (masks[index + columns] & 2) == 0)) {
        directions |= 8;
    }
    field[index] = directions == 0 ? static_cast<unsigned short>(0xff00)
        : directions;
}

__global__ void propagateBoundaryFieldKernel(const unsigned short *source,
                                             int columns, int rows,
                                             unsigned short *destination) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    const int cells = columns * rows;
    if (index >= cells) {
        return;
    }
    const int x = index % columns;
    const int y = index / columns;
    unsigned short best = source[index];
    const auto consider = [&](int neighbor) {
        const unsigned short value = source[neighbor];
        const unsigned int distance = value >> 8;
        if (distance < 0xff) {
            const unsigned short propagated = static_cast<unsigned short>(
                ((distance + 1) << 8) | (value & 0xff));
            if ((propagated >> 8) < (best >> 8)) {
                best = propagated;
            }
        }
    };
    if (x > 0) {
        consider(index - 1);
    }
    if (x + 1 < columns) {
        consider(index + 1);
    }
    if (y > 0) {
        consider(index - columns);
    }
    if (y + 1 < rows) {
        consider(index + columns);
    }
    destination[index] = best;
}

__global__ void rasterKernel(const Point *points, const Loop *loops,
                             const Piece *pieces, const Tile *tiles,
                             float originX, float originY, float cellSize,
                             int columns, unsigned int *coverage,
                             unsigned int *ownerXor) {
    const Tile tile = tiles[blockIdx.x];
    const int localX = threadIdx.x;
    const int localY = threadIdx.y;
    if (localX >= tile.width || localY >= tile.height) {
        return;
    }
    const int x = tile.left + localX;
    const int y = tile.top + localY;
    const Piece piece = pieces[tile.piece];
    if (contains(originX + (x + 0.5f) * cellSize,
            originY + (y + 0.5f) * cellSize, points, loops,
            piece.loopOffset, piece.loopCount)) {
        const int index = y * columns + x;
        atomicAdd(coverage + index, 1U);
        atomicXor(ownerXor + index, static_cast<unsigned int>(tile.piece + 1));
    }
}

__global__ void coverageKernel(const Point *points, const Loop *loops,
                               const Piece *pieces, const Tile *tiles,
                               float originX, float originY, float cellSize,
                               int columns, unsigned int *coverage) {
    const Tile tile = tiles[blockIdx.x];
    const int localX = threadIdx.x;
    const int localY = threadIdx.y;
    if (localX >= tile.width || localY >= tile.height) {
        return;
    }
    const int x = tile.left + localX;
    const int y = tile.top + localY;
    const Piece piece = pieces[tile.piece];
    if (contains(originX + (x + 0.5f) * cellSize,
            originY + (y + 0.5f) * cellSize, points, loops,
            piece.loopOffset, piece.loopCount)) {
        atomicExch(coverage + y * columns + x, 1U);
    }
}

__global__ void coverageDeltaKernel(const Point *points, const Loop *loops,
                                    const Piece *pieces, const Tile *tiles,
                                    float originX, float originY, float cellSize,
                                    int columns, int delta,
                                    unsigned int *coverage) {
    const Tile tile = tiles[blockIdx.x];
    const int localX = threadIdx.x;
    const int localY = threadIdx.y;
    if (localX >= tile.width || localY >= tile.height) {
        return;
    }
    const int x = tile.left + localX;
    const int y = tile.top + localY;
    const Piece piece = pieces[tile.piece];
    if (!contains(originX + (x + 0.5f) * cellSize,
            originY + (y + 0.5f) * cellSize, points, loops,
            piece.loopOffset, piece.loopCount)) {
        return;
    }
    if (delta > 0) {
        atomicAdd(coverage + y * columns + x, static_cast<unsigned int>(delta));
    } else {
        atomicSub(coverage + y * columns + x, static_cast<unsigned int>(-delta));
    }
}

__device__ Point inverseMap(const Affine &transform, float x, float y) {
    const float determinant = transform.m11 * transform.m22
        - transform.m12 * transform.m21;
    if (fabsf(determinant) < 1e-20f) {
        return {kInvalidCoordinate, kInvalidCoordinate};
    }
    const float localX = x - transform.dx;
    const float localY = y - transform.dy;

    return {(transform.m22 * localX - transform.m21 * localY) / determinant,
        (-transform.m12 * localX + transform.m11 * localY) / determinant};
}

__device__ bool transformedContains(float x, float y, const Affine &transform,
                                    const Point *points, const Loop *loops,
                                    Piece primitive) {
    const Point local = inverseMap(transform, x, y);

    return contains(local.x, local.y, points, loops,
        primitive.loopOffset, primitive.loopCount);
}

__device__ unsigned long long boundaryPenalty(unsigned int exposed,
                                              unsigned short field) {
    const unsigned int distance = min(13U,
        static_cast<unsigned int>(field >> 8));
    const unsigned int mismatch = __popc(exposed & ~(field & 0xff));

    return static_cast<unsigned long long>(distance + 2 * mismatch);
}

__global__ void transformAdditionKernel(const Point *points, const Loop *loops,
                                        Piece primitive,
                                        const Affine *transforms,
                                        const TransformTile *tiles,
                                        const unsigned char *masks,
                                        const unsigned short *boundaryField,
                                        const unsigned int *coverage,
                                        float originX, float originY, float cellSize,
                                        int columns, int rows,
                                        unsigned long long *counts) {
    const TransformTile tile = tiles[blockIdx.x];
    const int localX = threadIdx.x;
    const int localY = threadIdx.y;
    if (localX >= tile.width || localY >= tile.height) {
        return;
    }
    const int x = tile.left + localX;
    const int y = tile.top + localY;
    const int index = y * columns + x;
    if (coverage[index] != 0) {
        return;
    }
    const Point local = inverseMap(transforms[tile.transform],
        originX + (x + 0.5f) * cellSize,
        originY + (y + 0.5f) * cellSize);
    if (!contains(local.x, local.y, points, loops,
            primitive.loopOffset, primitive.loopCount)) {
        return;
    }
    const unsigned char mask = masks[index];
    unsigned long long *pieceCounts = counts + tile.transform * 6;
    if ((mask & 1) != 0) {
        atomicAdd(pieceCounts, 1ULL);
    }
    if ((mask & 4) != 0) {
        atomicAdd(pieceCounts + 1, 1ULL);
    }
    if ((mask & 2) == 0) {
        atomicAdd(pieceCounts + 2, 1ULL);
    }
    if ((mask & 8) == 0) {
        atomicAdd(pieceCounts + 3, 1ULL);
    }
    if ((mask & 2) != 0) {
        const bool horizontal = x > 0 && x + 1 < columns
            && coverage[index - 1] != 0 && coverage[index + 1] != 0;
        const bool vertical = y > 0 && y + 1 < rows
            && coverage[index - columns] != 0 && coverage[index + columns] != 0;
        if (horizontal || vertical) {
            atomicAdd(pieceCounts + 4, 1ULL);
        }
        unsigned int exposed = 0;
        const float worldX = originX + (x + 0.5f) * cellSize;
        const float worldY = originY + (y + 0.5f) * cellSize;
        const Affine transform = transforms[tile.transform];
        if (x == 0 || (coverage[index - 1] == 0
                && !transformedContains(worldX - cellSize, worldY,
                    transform, points, loops, primitive))) {
            exposed |= 1;
        }
        if (x + 1 == columns || (coverage[index + 1] == 0
                && !transformedContains(worldX + cellSize, worldY,
                    transform, points, loops, primitive))) {
            exposed |= 2;
        }
        if (y == 0 || (coverage[index - columns] == 0
                && !transformedContains(worldX, worldY - cellSize,
                    transform, points, loops, primitive))) {
            exposed |= 4;
        }
        if (y + 1 == rows || (coverage[index + columns] == 0
                && !transformedContains(worldX, worldY + cellSize,
                    transform, points, loops, primitive))) {
            exposed |= 8;
        }
        if (exposed != 0) {
            atomicAdd(pieceCounts + 5,
                boundaryPenalty(exposed, boundaryField[index]));
        }
    }
}

__global__ void scoreKernel(const unsigned char *masks,
                            const unsigned int *coverage,
                            const unsigned int *ownerXor, int cells,
                            int pieceCount, unsigned long long *counts) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= cells || coverage[index] != 1) {
        return;
    }
    const unsigned int owner = ownerXor[index];
    if (owner == 0 || owner > static_cast<unsigned int>(pieceCount)) {
        return;
    }
    const unsigned char mask = masks[index];
    unsigned long long *pieceCounts = counts + (owner - 1) * 4;
    if ((mask & 1) != 0) {
        atomicAdd(pieceCounts, 1ULL);
    }
    if ((mask & 2) == 0) {
        atomicAdd(pieceCounts + 1, 1ULL);
    }
    if ((mask & 4) != 0) {
        atomicAdd(pieceCounts + 2, 1ULL);
    }
    if ((mask & 8) == 0) {
        atomicAdd(pieceCounts + 3, 1ULL);
    }
}

__global__ void additionKernel(const Point *points, const Loop *loops,
                               const Piece *pieces, const Tile *tiles,
                               const unsigned char *masks,
                               const unsigned short *boundaryField,
                               const unsigned int *coverage,
                               float originX, float originY, float cellSize,
                               int columns, int rows,
                               unsigned long long *counts) {
    const Tile tile = tiles[blockIdx.x];
    const int localX = threadIdx.x;
    const int localY = threadIdx.y;
    if (localX >= tile.width || localY >= tile.height) {
        return;
    }
    const int x = tile.left + localX;
    const int y = tile.top + localY;
    const int index = y * columns + x;
    if (coverage[index] != 0) {
        return;
    }
    const Piece piece = pieces[tile.piece];
    if (!contains(originX + (x + 0.5f) * cellSize,
            originY + (y + 0.5f) * cellSize, points, loops,
            piece.loopOffset, piece.loopCount)) {
        return;
    }
    const unsigned char mask = masks[index];
    unsigned long long *pieceCounts = counts + tile.piece * 6;
    if ((mask & 1) != 0) {
        atomicAdd(pieceCounts, 1ULL);
    }
    if ((mask & 4) != 0) {
        atomicAdd(pieceCounts + 1, 1ULL);
    }
    if ((mask & 2) == 0) {
        atomicAdd(pieceCounts + 2, 1ULL);
    }
    if ((mask & 8) == 0) {
        atomicAdd(pieceCounts + 3, 1ULL);
    }
    if ((mask & 2) != 0) {
        const bool horizontal = x > 0 && x + 1 < columns
            && coverage[index - 1] != 0 && coverage[index + 1] != 0;
        const bool vertical = y > 0 && y + 1 < rows
            && coverage[index - columns] != 0 && coverage[index + columns] != 0;
        if (horizontal || vertical) {
            atomicAdd(pieceCounts + 4, 1ULL);
        }
        unsigned int exposed = 0;
        const float worldX = originX + (x + 0.5f) * cellSize;
        const float worldY = originY + (y + 0.5f) * cellSize;
        if (x == 0 || (coverage[index - 1] == 0
                && !contains(worldX - cellSize, worldY, points, loops,
                    piece.loopOffset, piece.loopCount))) {
            exposed |= 1;
        }
        if (x + 1 == columns || (coverage[index + 1] == 0
                && !contains(worldX + cellSize, worldY, points, loops,
                    piece.loopOffset, piece.loopCount))) {
            exposed |= 2;
        }
        if (y == 0 || (coverage[index - columns] == 0
                && !contains(worldX, worldY - cellSize, points, loops,
                    piece.loopOffset, piece.loopCount))) {
            exposed |= 4;
        }
        if (y + 1 == rows || (coverage[index + columns] == 0
                && !contains(worldX, worldY + cellSize, points, loops,
                    piece.loopOffset, piece.loopCount))) {
            exposed |= 8;
        }
        if (exposed != 0) {
            atomicAdd(pieceCounts + 5,
                boundaryPenalty(exposed, boundaryField[index]));
        }
    }
}

std::pair<Point, Point> bounds(const Geometry &geometry) {
    Point minimum{std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    Point maximum{-std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    for (const Point &point : geometry.points) {
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
    }

    return {minimum, maximum};
}

class CudaRasterRanker final : public RasterRanker {
public:
    CudaRasterRanker(const Geometry &preferred, const Geometry &target,
                     const Geometry &inner, const Geometry &outer,
                     const Geometry &spillFree, double requestedCellSize) {
        const auto start = std::chrono::steady_clock::now();
        const auto [minimum, maximum] = bounds(outer);
        if (outer.points.empty() || !std::isfinite(requestedCellSize)
            || requestedCellSize <= 0.0) {
            stats_.error = "Invalid ownership raster geometry";
            return;
        }
        cellSize_ = static_cast<float>(requestedCellSize);
        const double width = maximum.x - minimum.x + 2.0 * cellSize_;
        const double height = maximum.y - minimum.y + 2.0 * cellSize_;
        const double requiredCellSize = std::sqrt(width * height / kMaximumCells);
        cellSize_ = static_cast<float>(std::max<double>(cellSize_, requiredCellSize));
        originX_ = std::floor(minimum.x / cellSize_) * cellSize_ - cellSize_;
        originY_ = std::floor(minimum.y / cellSize_) * cellSize_ - cellSize_;
        columns_ = std::max(1, static_cast<int>(std::ceil(
            (maximum.x - originX_) / cellSize_)) + 1);
        rows_ = std::max(1, static_cast<int>(std::ceil(
            (maximum.y - originY_) / cellSize_)) + 1);
        cells_ = columns_ * rows_;
        if (!check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking))) {
            return;
        }
        int device = 0;
        cudaDeviceProp properties{};
        if (check(cudaGetDevice(&device)) && check(cudaGetDeviceProperties(&properties, device))) {
            stats_.adapter = properties.name;
        }
        if (!uploadSet(preferred, &preferredPoints_, &preferredLoops_, &preferred_)
            || !uploadSet(target, &targetPoints_, &targetLoops_, &target_)
            || !uploadSet(inner, &innerPoints_, &innerLoops_, &inner_)
            || !uploadSet(outer, &outerPoints_, &outerLoops_, &outer_)
            || !uploadSet(spillFree, &spillFreePoints_, &spillFreeLoops_, &spillFree_)
            || !check(masks_.reserve(cells_))
            || !check(boundaryFieldA_.reserve(cells_))
            || !check(boundaryFieldB_.reserve(cells_))
            || !check(coverage_.reserve(cells_))
            || !check(persistentCoverage_.reserve(cells_))
            || !check(ownerXor_.reserve(cells_))) {
            return;
        }
        maskKernel<<<(cells_ + kThreads - 1) / kThreads, kThreads, 0, stream_>>>(
            preferred_, target_, inner_, outer_, spillFree_,
            originX_, originY_, cellSize_,
            columns_, cells_, masks_.data());
        boundaryFieldKernel<<<(cells_ + kThreads - 1) / kThreads,
            kThreads, 0, stream_>>>(masks_.data(), columns_, rows_,
            boundaryFieldA_.data());
        unsigned short *source = boundaryFieldA_.data();
        unsigned short *destination = boundaryFieldB_.data();
        for (int pass = 0; pass < kBoundaryFieldPasses; ++pass) {
            propagateBoundaryFieldKernel<<<(cells_ + kThreads - 1) / kThreads,
                kThreads, 0, stream_>>>(source, columns_, rows_, destination);
            std::swap(source, destination);
        }
        boundaryField_ = source;
        if (!check(cudaGetLastError()) || !check(cudaStreamSynchronize(stream_))) {
            return;
        }
        stats_.columns = columns_;
        stats_.rows = rows_;
        stats_.cellSize = cellSize_;
        stats_.setupMilliseconds = elapsed(start);
    }

    ~CudaRasterRanker() override {
        if (stream_ != nullptr) {
            cudaStreamDestroy(stream_);
        }
    }

    bool evaluate(const Geometry &geometry, std::vector<double> *scores) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (scores == nullptr || !stats_.error.empty() || geometry.pieces.empty()) {
            return false;
        }
        std::vector<Tile> tiles = makeTiles(geometry);
        const size_t countSize = geometry.pieces.size() * 4;
        if (!upload(geometry.points, &piecePoints_)
            || !upload(geometry.loops, &pieceLoops_)
            || !upload(geometry.pieces, &pieces_)
            || !upload(tiles, &tiles_)
            || !check(counts_.reserve(countSize))
            || !check(cudaMemsetAsync(coverage_.data(), 0,
                cells_ * sizeof(unsigned int), stream_))
            || !check(cudaMemsetAsync(ownerXor_.data(), 0,
                cells_ * sizeof(unsigned int), stream_))
            || !check(cudaMemsetAsync(counts_.data(), 0,
                countSize * sizeof(unsigned long long), stream_))) {
            return false;
        }
        if (!tiles.empty()) {
            const dim3 threads(kTileExtent, kTileExtent);
            rasterKernel<<<static_cast<unsigned int>(tiles.size()), threads, 0, stream_>>>(
                piecePoints_.data(), pieceLoops_.data(), pieces_.data(), tiles_.data(),
                originX_, originY_, cellSize_, columns_, coverage_.data(), ownerXor_.data());
        }
        scoreKernel<<<(cells_ + kThreads - 1) / kThreads, kThreads, 0, stream_>>>(
            masks_.data(), coverage_.data(), ownerXor_.data(), cells_,
            static_cast<int>(geometry.pieces.size()), counts_.data());
        std::vector<unsigned long long> counts(countSize);
        if (!check(cudaGetLastError())
            || !check(cudaMemcpyAsync(counts.data(), counts_.data(),
                countSize * sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream_))
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        const double cellArea = static_cast<double>(cellSize_) * cellSize_;
        scores->resize(geometry.pieces.size());
        for (size_t index = 0; index < geometry.pieces.size(); ++index) {
            (*scores)[index] = cellArea * (4.0 * counts[index * 4]
                - 4.0 * counts[index * 4 + 1] + 16.0 * counts[index * 4 + 2]
                - 16.0 * counts[index * 4 + 3]);
        }
        ++stats_.calls;
        stats_.evaluationMilliseconds += elapsed(start);

        return true;
    }

    bool prepareAdditionCoverage(const Geometry &coverage) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (!stats_.error.empty()) {
            return false;
        }
        const std::vector<Tile> coverageTiles = makeTiles(coverage);
        if (!upload(coverage.points, &piecePoints_)
            || !upload(coverage.loops, &pieceLoops_)
            || !upload(coverage.pieces, &pieces_)
            || !upload(coverageTiles, &tiles_)
            || !check(cudaMemsetAsync(coverage_.data(), 0,
                cells_ * sizeof(unsigned int), stream_))) {
            return false;
        }
        const dim3 threads(kTileExtent, kTileExtent);
        if (!coverageTiles.empty()) {
            coverageKernel<<<static_cast<unsigned int>(coverageTiles.size()),
                threads, 0, stream_>>>(piecePoints_.data(), pieceLoops_.data(),
                pieces_.data(), tiles_.data(), originX_, originY_, cellSize_,
                columns_, coverage_.data());
        }
        if (!check(cudaGetLastError()) || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        ++stats_.refinementPreparations;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    bool preparePlacementCoverage(const Geometry &coverage) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (!stats_.error.empty()
            || !rasterCoverage(coverage, persistentCoverage_.data())) {
            return false;
        }
        ++stats_.persistentPreparations;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    bool prepareReplacementCoverage(const Geometry &current) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (!stats_.error.empty()
            || !check(cudaMemcpyAsync(coverage_.data(), persistentCoverage_.data(),
                cells_ * sizeof(unsigned int), cudaMemcpyDeviceToDevice, stream_))
            || !applyCoverageDelta(current, -1, coverage_.data())
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        ++stats_.refinementPreparations;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    bool commitReplacement(const Geometry &previous,
                           const Geometry &replacement) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (!stats_.error.empty()
            || !applyCoverageDelta(previous, -1, persistentCoverage_.data())
            || !applyCoverageDelta(replacement, 1, persistentCoverage_.data())
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        ++stats_.persistentCommits;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    bool evaluateAdditions(const Geometry &candidates, std::vector<double> *scores,
                           const AdditionWeights &weights) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (scores == nullptr || !stats_.error.empty() || candidates.pieces.empty()) {
            return false;
        }
        const std::vector<Tile> candidateTiles = makeTiles(candidates);
        const size_t countSize = candidates.pieces.size() * 6;
        if (!upload(candidates.points, &candidatePoints_)
            || !upload(candidates.loops, &candidateLoops_)
            || !upload(candidates.pieces, &candidates_)
            || !upload(candidateTiles, &candidateTiles_)
            || !check(counts_.reserve(countSize))
            || !check(cudaMemsetAsync(counts_.data(), 0,
                countSize * sizeof(unsigned long long), stream_))) {
            return false;
        }
        const dim3 threads(kTileExtent, kTileExtent);
        if (!candidateTiles.empty()) {
            additionKernel<<<static_cast<unsigned int>(candidateTiles.size()),
                threads, 0, stream_>>>(candidatePoints_.data(), candidateLoops_.data(),
                candidates_.data(), candidateTiles_.data(), masks_.data(),
                boundaryField_, coverage_.data(), originX_, originY_, cellSize_,
                columns_, rows_, counts_.data());
        }
        std::vector<unsigned long long> counts(countSize);
        if (!check(cudaGetLastError())
            || !check(cudaMemcpyAsync(counts.data(), counts_.data(),
                countSize * sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream_))
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        assignAdditionScores(counts, candidates.pieces.size(), weights, scores,
            static_cast<double>(cellSize_) * cellSize_);
        ++stats_.refinementCalls;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    bool evaluateTransforms(const Geometry &primitive,
                            const std::vector<Affine> &transforms,
                            std::vector<double> *scores,
                            const AdditionWeights &weights) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (scores == nullptr || !stats_.error.empty()
            || primitive.pieces.size() != 1 || transforms.empty()) {
            return false;
        }
        const std::vector<TransformTile> candidateTiles = makeTransformTiles(
            primitive, transforms);
        const size_t countSize = transforms.size() * 6;
        if (!upload(primitive.points, &candidatePoints_)
            || !upload(primitive.loops, &candidateLoops_)
            || !upload(transforms, &transforms_)
            || !upload(candidateTiles, &transformTiles_)
            || !check(counts_.reserve(countSize))
            || !check(cudaMemsetAsync(counts_.data(), 0,
                countSize * sizeof(unsigned long long), stream_))) {
            return false;
        }
        const dim3 threads(kTileExtent, kTileExtent);
        if (!candidateTiles.empty()) {
            transformAdditionKernel<<<static_cast<unsigned int>(candidateTiles.size()),
                threads, 0, stream_>>>(candidatePoints_.data(), candidateLoops_.data(),
                primitive.pieces.front(), transforms_.data(), transformTiles_.data(),
                masks_.data(), boundaryField_, coverage_.data(), originX_, originY_,
                cellSize_, columns_, rows_, counts_.data());
        }
        std::vector<unsigned long long> counts(countSize);
        if (!check(cudaGetLastError())
            || !check(cudaMemcpyAsync(counts.data(), counts_.data(),
                countSize * sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream_))
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        assignAdditionScores(counts, transforms.size(), weights, scores,
            static_cast<double>(cellSize_) * cellSize_);
        ++stats_.refinementCalls;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    RankStats stats() const override {

        return stats_;
    }

private:
    bool rasterCoverage(const Geometry &geometry, unsigned int *destination) {
        const std::vector<Tile> coverageTiles = makeTiles(geometry);
        if (!upload(geometry.points, &piecePoints_)
            || !upload(geometry.loops, &pieceLoops_)
            || !upload(geometry.pieces, &pieces_)
            || !upload(coverageTiles, &tiles_)
            || !check(cudaMemsetAsync(destination, 0,
                cells_ * sizeof(unsigned int), stream_))) {
            return false;
        }
        if (!coverageTiles.empty()) {
            const dim3 threads(kTileExtent, kTileExtent);
            coverageDeltaKernel<<<static_cast<unsigned int>(coverageTiles.size()),
                threads, 0, stream_>>>(piecePoints_.data(), pieceLoops_.data(),
                pieces_.data(), tiles_.data(), originX_, originY_, cellSize_,
                columns_, 1, destination);
        }

        return check(cudaGetLastError()) && check(cudaStreamSynchronize(stream_));
    }

    bool applyCoverageDelta(const Geometry &geometry, int delta,
                            unsigned int *destination) {
        const std::vector<Tile> coverageTiles = makeTiles(geometry);
        if (!upload(geometry.points, &piecePoints_)
            || !upload(geometry.loops, &pieceLoops_)
            || !upload(geometry.pieces, &pieces_)
            || !upload(coverageTiles, &tiles_)) {
            return false;
        }
        if (!coverageTiles.empty()) {
            const dim3 threads(kTileExtent, kTileExtent);
            coverageDeltaKernel<<<static_cast<unsigned int>(coverageTiles.size()),
                threads, 0, stream_>>>(piecePoints_.data(), pieceLoops_.data(),
                pieces_.data(), tiles_.data(), originX_, originY_, cellSize_,
                columns_, delta, destination);
        }

        return check(cudaGetLastError());
    }

    static void assignAdditionScores(const std::vector<unsigned long long> &counts,
                                     size_t candidates,
                                     const AdditionWeights &weights,
                                     std::vector<double> *scores,
                                     double cellArea = 1.0) {
        scores->resize(candidates);
        for (size_t index = 0; index < candidates; ++index) {
            (*scores)[index] = counts[index * 6 + 3] == 0
                ? cellArea * (weights.preferred * counts[index * 6]
                    + weights.inner * counts[index * 6 + 1]
                    - weights.spill * counts[index * 6 + 2]
                    + weights.join * counts[index * 6 + 4]
                    - weights.boundary * counts[index * 6 + 5])
                : -std::numeric_limits<double>::infinity();
        }
    }

    template <typename Value>
    bool upload(const std::vector<Value> &source, DeviceBuffer<Value> *destination) {
        return check(destination->reserve(source.size()))
            && (source.empty() || check(cudaMemcpyAsync(destination->data(), source.data(),
                source.size() * sizeof(Value), cudaMemcpyHostToDevice, stream_)));
    }

    bool uploadSet(const Geometry &geometry, DeviceBuffer<Point> *points,
                   DeviceBuffer<Loop> *loops, SetView *view) {
        if (!upload(geometry.points, points) || !upload(geometry.loops, loops)) {
            return false;
        }
        *view = {points->data(), loops->data(), static_cast<int>(geometry.loops.size())};

        return true;
    }

    std::vector<Tile> makeTiles(const Geometry &geometry) const {
        std::vector<Tile> result;
        for (int pieceIndex = 0; pieceIndex < static_cast<int>(geometry.pieces.size()); ++pieceIndex) {
            const Piece piece = geometry.pieces[pieceIndex];
            if (piece.loopCount == 0) {
                continue;
            }
            float minimumX = std::numeric_limits<float>::infinity();
            float minimumY = std::numeric_limits<float>::infinity();
            float maximumX = -std::numeric_limits<float>::infinity();
            float maximumY = -std::numeric_limits<float>::infinity();
            for (int loopIndex = 0; loopIndex < piece.loopCount; ++loopIndex) {
                const Loop loop = geometry.loops[piece.loopOffset + loopIndex];
                for (int pointIndex = 0; pointIndex < loop.pointCount; ++pointIndex) {
                    const Point point = geometry.points[loop.pointOffset + pointIndex];
                    minimumX = std::min(minimumX, point.x);
                    minimumY = std::min(minimumY, point.y);
                    maximumX = std::max(maximumX, point.x);
                    maximumY = std::max(maximumY, point.y);
                }
            }
            const int left = std::clamp(static_cast<int>(std::floor(
                (minimumX - originX_) / cellSize_)), 0, columns_);
            const int top = std::clamp(static_cast<int>(std::floor(
                (minimumY - originY_) / cellSize_)), 0, rows_);
            const int right = std::clamp(static_cast<int>(std::ceil(
                (maximumX - originX_) / cellSize_)), 0, columns_);
            const int bottom = std::clamp(static_cast<int>(std::ceil(
                (maximumY - originY_) / cellSize_)), 0, rows_);
            for (int y = top; y < bottom; y += kTileExtent) {
                for (int x = left; x < right; x += kTileExtent) {
                    result.push_back({pieceIndex, x, y,
                        std::min(kTileExtent, right - x),
                        std::min(kTileExtent, bottom - y)});
                }
            }
        }

        return result;
    }

    std::vector<TransformTile> makeTransformTiles(
            const Geometry &primitive, const std::vector<Affine> &transforms) const {
        std::vector<TransformTile> result;
        for (int transformIndex = 0;
             transformIndex < static_cast<int>(transforms.size()); ++transformIndex) {
            const Affine &transform = transforms[transformIndex];
            float minimumX = std::numeric_limits<float>::infinity();
            float minimumY = std::numeric_limits<float>::infinity();
            float maximumX = -std::numeric_limits<float>::infinity();
            float maximumY = -std::numeric_limits<float>::infinity();
            for (const Point &point : primitive.points) {
                const float x = transform.m11 * point.x + transform.m21 * point.y
                    + transform.dx;
                const float y = transform.m12 * point.x + transform.m22 * point.y
                    + transform.dy;
                minimumX = std::min(minimumX, x);
                minimumY = std::min(minimumY, y);
                maximumX = std::max(maximumX, x);
                maximumY = std::max(maximumY, y);
            }
            const int left = std::clamp(static_cast<int>(std::floor(
                (minimumX - originX_) / cellSize_)), 0, columns_);
            const int top = std::clamp(static_cast<int>(std::floor(
                (minimumY - originY_) / cellSize_)), 0, rows_);
            const int right = std::clamp(static_cast<int>(std::ceil(
                (maximumX - originX_) / cellSize_)), 0, columns_);
            const int bottom = std::clamp(static_cast<int>(std::ceil(
                (maximumY - originY_) / cellSize_)), 0, rows_);
            for (int y = top; y < bottom; y += kTileExtent) {
                for (int x = left; x < right; x += kTileExtent) {
                    result.push_back({transformIndex, x, y,
                        std::min(kTileExtent, right - x),
                        std::min(kTileExtent, bottom - y)});
                }
            }
        }

        return result;
    }

    bool check(cudaError_t status) {
        if (status != cudaSuccess) {
            stats_.error = cudaGetErrorString(status);
            return false;
        }

        return true;
    }

    static double elapsed(const std::chrono::steady_clock::time_point &start) {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }

    DeviceBuffer<Point> preferredPoints_;
    DeviceBuffer<Point> targetPoints_;
    DeviceBuffer<Point> innerPoints_;
    DeviceBuffer<Point> outerPoints_;
    DeviceBuffer<Point> spillFreePoints_;
    DeviceBuffer<Point> piecePoints_;
    DeviceBuffer<Point> candidatePoints_;
    DeviceBuffer<Loop> preferredLoops_;
    DeviceBuffer<Loop> targetLoops_;
    DeviceBuffer<Loop> innerLoops_;
    DeviceBuffer<Loop> outerLoops_;
    DeviceBuffer<Loop> spillFreeLoops_;
    DeviceBuffer<Loop> pieceLoops_;
    DeviceBuffer<Loop> candidateLoops_;
    DeviceBuffer<Piece> pieces_;
    DeviceBuffer<Piece> candidates_;
    DeviceBuffer<Tile> tiles_;
    DeviceBuffer<Tile> candidateTiles_;
    DeviceBuffer<TransformTile> transformTiles_;
    DeviceBuffer<Affine> transforms_;
    DeviceBuffer<unsigned char> masks_;
    DeviceBuffer<unsigned short> boundaryFieldA_;
    DeviceBuffer<unsigned short> boundaryFieldB_;
    DeviceBuffer<unsigned int> coverage_;
    DeviceBuffer<unsigned int> persistentCoverage_;
    DeviceBuffer<unsigned int> ownerXor_;
    DeviceBuffer<unsigned long long> counts_;
    SetView preferred_;
    SetView target_;
    SetView inner_;
    SetView outer_;
    SetView spillFree_;
    unsigned short *boundaryField_ = nullptr;
    RankStats stats_;
    std::mutex mutex_;
    cudaStream_t stream_ = nullptr;
    float originX_ = 0.0f;
    float originY_ = 0.0f;
    float cellSize_ = 1.0f;
    int columns_ = 0;
    int rows_ = 0;
    int cells_ = 0;
};

__global__ void bitmaskScoreKernel(const MaskWord *words, const int *offsets,
                                   const std::uint64_t *missing, int cellWords,
                                   int candidates, MaskCounts *counts) {
    const int candidate = blockIdx.x * blockDim.x + threadIdx.x;
    if (candidate >= candidates)
        return;
    MaskCounts count{};
    for (int entry = offsets[candidate]; entry < offsets[candidate + 1]; ++entry) {
        const MaskWord word = words[entry];
        const unsigned int matched = __popcll(word.bits & missing[word.index]);
        if (word.index < cellWords)
            count.cells += matched;
        else
            count.boundary += matched;
    }
    counts[candidate] = count;
}

__global__ void bitmaskRemoveKernel(const MaskWord *words, const int *offsets,
                                    std::uint64_t *missing, int candidate) {
    for (int entry = offsets[candidate] + threadIdx.x;
         entry < offsets[candidate + 1]; entry += blockDim.x) {
        const MaskWord word = words[entry];
        missing[word.index] &= ~word.bits;
    }
}

class CudaBitmaskCover final : public BitmaskCover {
public:
    CudaBitmaskCover(const std::vector<MaskWord> &words,
                     const std::vector<int> &offsets,
                     const std::vector<std::uint64_t> &missing, int cellWords)
        : candidateCount_(static_cast<int>(offsets.size()) - 1), cellWords_(cellWords) {
        if (offsets.empty() || missing.empty() || cellWords < 0
            || cellWords > static_cast<int>(missing.size())
            || offsets.back() != static_cast<int>(words.size())) {
            error_ = "Invalid bitmask cover";
            return;
        }
        if (!check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking))
            || !check(deviceWords_.reserve(words.size()))
            || !check(deviceOffsets_.reserve(offsets.size()))
            || !check(deviceMissing_.reserve(missing.size()))
            || !check(deviceCounts_.reserve(candidateCount_))
            || !check(cudaMemcpyAsync(deviceWords_.data(), words.data(),
                words.size() * sizeof(MaskWord), cudaMemcpyHostToDevice, stream_))
            || !check(cudaMemcpyAsync(deviceOffsets_.data(), offsets.data(),
                offsets.size() * sizeof(int), cudaMemcpyHostToDevice, stream_))
            || !check(cudaMemcpyAsync(deviceMissing_.data(), missing.data(),
                missing.size() * sizeof(std::uint64_t), cudaMemcpyHostToDevice, stream_))
            || !check(cudaStreamSynchronize(stream_)))
            return;
    }

    ~CudaBitmaskCover() override {
        if (stream_ != nullptr)
            cudaStreamDestroy(stream_);
    }

    bool score(std::vector<MaskCounts> *counts) override {
        if (!counts || !error_.empty())
            return false;
        counts->resize(candidateCount_);
        bitmaskScoreKernel<<<(candidateCount_ + kThreads - 1) / kThreads,
            kThreads, 0, stream_>>>(deviceWords_.data(), deviceOffsets_.data(),
            deviceMissing_.data(), cellWords_, candidateCount_, deviceCounts_.data());

        return check(cudaGetLastError())
            && check(cudaMemcpyAsync(counts->data(), deviceCounts_.data(),
                candidateCount_ * sizeof(MaskCounts), cudaMemcpyDeviceToHost, stream_))
            && check(cudaStreamSynchronize(stream_));
    }

    bool remove(int candidate) override {
        if (!error_.empty() || candidate < 0 || candidate >= candidateCount_)
            return false;
        bitmaskRemoveKernel<<<1, kThreads, 0, stream_>>>(deviceWords_.data(),
            deviceOffsets_.data(), deviceMissing_.data(), candidate);

        return check(cudaGetLastError());
    }

    std::string error() const override {
        return error_;
    }

private:
    bool check(cudaError_t status) {
        if (status != cudaSuccess) {
            error_ = cudaGetErrorString(status);
            return false;
        }

        return true;
    }

    DeviceBuffer<MaskWord> deviceWords_;
    DeviceBuffer<int> deviceOffsets_;
    DeviceBuffer<std::uint64_t> deviceMissing_;
    DeviceBuffer<MaskCounts> deviceCounts_;
    cudaStream_t stream_ = nullptr;
    std::string error_;
    int candidateCount_ = 0;
    int cellWords_ = 0;
};

struct MaskTile {
    int candidate = 0;
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

struct MaskWitnessJob {
    int candidate = 0;
    int witness = 0;
};

__device__ bool maskContains(double x, double y, const MaskPoint *points,
                             const Loop *loops, Piece piece) {
    int winding = 0;
    for (int loopIndex = piece.loopOffset;
         loopIndex < piece.loopOffset + piece.loopCount; ++loopIndex) {
        const Loop loop = loops[loopIndex];
        for (int index = 0; index < loop.pointCount; ++index) {
            const MaskPoint first = points[loop.pointOffset + index];
            const MaskPoint second = points[loop.pointOffset + (index + 1) % loop.pointCount];
            if (y < fmin(first.y, second.y) || y >= fmax(first.y, second.y))
                continue;
            const double crossing = first.x + (second.x - first.x)
                * (y - first.y) / (second.y - first.y);
            if (crossing <= x)
                winding += second.y > first.y ? 1 : -1;
        }
    }

    return winding != 0;
}

__global__ void bitmaskRasterKernel(const MaskPoint *points, const Loop *loops,
                                    const Piece *pieces, const MaskTile *tiles,
                                    MaskGrid grid, int wordsPerCandidate,
                                    std::uint64_t *masks) {
    const MaskTile tile = tiles[blockIdx.x];
    const int column = tile.left + threadIdx.x;
    const int row = tile.top + threadIdx.y;
    if (threadIdx.x >= tile.width || threadIdx.y >= tile.height)
        return;
    const double x = grid.originX + (column + 0.5) * grid.step;
    const double y = grid.originY + (row + 0.5) * grid.step;
    const Piece piece = pieces[tile.candidate];
    if (maskContains(x, y, points, loops, piece)) {
        const int bit = row * grid.width + column;
        auto *words = reinterpret_cast<unsigned long long *>(masks);
        atomicOr(&words[tile.candidate * wordsPerCandidate + bit / 64],
            1ULL << (bit % 64));
    }
}

__global__ void bitmaskWitnessKernel(const MaskPoint *points, const Loop *loops,
                                     const Piece *pieces, const MaskPoint *witnesses,
                                     const MaskWitnessJob *jobs, int jobCount,
                                     int wordsPerCandidate, std::uint64_t *masks) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index >= jobCount)
        return;
    const MaskWitnessJob job = jobs[index];
    const MaskPoint witness = witnesses[job.witness];
    if (maskContains(witness.x, witness.y, points, loops, pieces[job.candidate])) {
        auto *words = reinterpret_cast<unsigned long long *>(masks);
        atomicOr(&words[job.candidate * wordsPerCandidate + job.witness / 64],
            1ULL << (job.witness % 64));
    }
}

} // namespace

bool rasterizeBitmasks(const MaskGeometry &geometry, const MaskGrid &grid,
                       const std::vector<MaskPoint> &witnesses,
                       std::vector<std::uint64_t> *masks,
                       std::vector<std::uint64_t> *boundaryMasks,
                       std::string *error) {
    if (!masks || !boundaryMasks || !error || grid.width <= 0 || grid.height <= 0
        || grid.step <= 0 || geometry.pieces.size() != geometry.bounds.size())
        return false;
    error->clear();
    const int wordsPerCandidate = (grid.width * grid.height + 63) / 64;
    const int boundaryWordsPerCandidate = (static_cast<int>(witnesses.size()) + 63) / 64;
    std::vector<MaskTile> tiles;
    std::vector<MaskWitnessJob> jobs;
    for (int candidate = 0; candidate < static_cast<int>(geometry.pieces.size()); ++candidate) {
        const MaskBounds bounds = geometry.bounds[candidate];
        const int left = std::clamp(static_cast<int>(std::floor(
            (bounds.left - grid.originX) / grid.step - 0.5)) - 1, 0, grid.width);
        const int top = std::clamp(static_cast<int>(std::floor(
            (bounds.top - grid.originY) / grid.step - 0.5)) - 1, 0, grid.height);
        const int right = std::clamp(static_cast<int>(std::ceil(
            (bounds.right - grid.originX) / grid.step - 0.5)) + 1, 0, grid.width);
        const int bottom = std::clamp(static_cast<int>(std::ceil(
            (bounds.bottom - grid.originY) / grid.step - 0.5)) + 1, 0, grid.height);
        for (int row = top; row < bottom; row += kTileExtent)
            for (int column = left; column < right; column += kTileExtent)
                tiles.push_back({candidate, column, row,
                    std::min(kTileExtent, right - column),
                    std::min(kTileExtent, bottom - row)});
        for (int witness = 0; witness < static_cast<int>(witnesses.size()); ++witness) {
            const MaskPoint point = witnesses[witness];
            if (point.x >= bounds.left && point.x <= bounds.right
                && point.y >= bounds.top && point.y <= bounds.bottom)
                jobs.push_back({candidate, witness});
        }
    }
    const size_t wordCount = geometry.pieces.size() * wordsPerCandidate;
    const size_t boundaryWordCount = geometry.pieces.size() * boundaryWordsPerCandidate;
    DeviceBuffer<MaskPoint> devicePoints;
    DeviceBuffer<MaskPoint> deviceWitnesses;
    DeviceBuffer<Loop> deviceLoops;
    DeviceBuffer<Piece> devicePieces;
    DeviceBuffer<MaskTile> deviceTiles;
    DeviceBuffer<MaskWitnessJob> deviceJobs;
    DeviceBuffer<std::uint64_t> deviceMasks;
    DeviceBuffer<std::uint64_t> deviceBoundaryMasks;
    const auto check = [&](cudaError_t status) {
        if (status != cudaSuccess) {
            *error = cudaGetErrorString(status);
            return false;
        }

        return true;
    };
    if (!check(devicePoints.reserve(geometry.points.size()))
        || !check(deviceWitnesses.reserve(witnesses.size()))
        || !check(deviceLoops.reserve(geometry.loops.size()))
        || !check(devicePieces.reserve(geometry.pieces.size()))
        || !check(deviceTiles.reserve(tiles.size()))
        || !check(deviceJobs.reserve(jobs.size()))
        || !check(deviceMasks.reserve(wordCount))
        || !check(deviceBoundaryMasks.reserve(boundaryWordCount))
        || !check(cudaMemcpy(devicePoints.data(), geometry.points.data(),
            geometry.points.size() * sizeof(MaskPoint), cudaMemcpyHostToDevice))
        || !check(cudaMemcpy(deviceWitnesses.data(), witnesses.data(),
            witnesses.size() * sizeof(MaskPoint), cudaMemcpyHostToDevice))
        || !check(cudaMemcpy(deviceLoops.data(), geometry.loops.data(),
            geometry.loops.size() * sizeof(Loop), cudaMemcpyHostToDevice))
        || !check(cudaMemcpy(devicePieces.data(), geometry.pieces.data(),
            geometry.pieces.size() * sizeof(Piece), cudaMemcpyHostToDevice))
        || !check(cudaMemcpy(deviceTiles.data(), tiles.data(),
            tiles.size() * sizeof(MaskTile), cudaMemcpyHostToDevice))
        || !check(cudaMemcpy(deviceJobs.data(), jobs.data(),
            jobs.size() * sizeof(MaskWitnessJob), cudaMemcpyHostToDevice))
        || !check(cudaMemset(deviceMasks.data(), 0,
            wordCount * sizeof(std::uint64_t)))
        || !check(cudaMemset(deviceBoundaryMasks.data(), 0,
            boundaryWordCount * sizeof(std::uint64_t))))
        return false;
    if (!tiles.empty()) {
        bitmaskRasterKernel<<<static_cast<unsigned int>(tiles.size()),
            dim3(kTileExtent, kTileExtent)>>>(devicePoints.data(), deviceLoops.data(),
            devicePieces.data(), deviceTiles.data(), grid, wordsPerCandidate,
            deviceMasks.data());
    }
    if (!jobs.empty()) {
        bitmaskWitnessKernel<<<(jobs.size() + kThreads - 1) / kThreads,
            kThreads>>>(devicePoints.data(), deviceLoops.data(), devicePieces.data(),
            deviceWitnesses.data(), deviceJobs.data(), static_cast<int>(jobs.size()),
            boundaryWordsPerCandidate, deviceBoundaryMasks.data());
    }
    masks->resize(wordCount);
    boundaryMasks->resize(boundaryWordCount);

    return check(cudaGetLastError())
        && check(cudaMemcpy(masks->data(), deviceMasks.data(),
            wordCount * sizeof(std::uint64_t), cudaMemcpyDeviceToHost))
        && check(cudaMemcpy(boundaryMasks->data(), deviceBoundaryMasks.data(),
            boundaryWordCount * sizeof(std::uint64_t), cudaMemcpyDeviceToHost));
}

std::unique_ptr<BitmaskCover> createBitmaskCover(
    const std::vector<MaskWord> &words, const std::vector<int> &offsets,
    const std::vector<std::uint64_t> &missing, int cellWords) {
    return std::make_unique<CudaBitmaskCover>(words, offsets, missing, cellWords);
}

std::unique_ptr<RasterRanker> createRasterRanker(
    const Geometry &preferred, const Geometry &target,
    const Geometry &inner, const Geometry &outer,
    const Geometry &spillFree, double cellSize) {
    return std::make_unique<CudaRasterRanker>(
        preferred, target, inner, outer, spillFree, cellSize);
}

} // namespace gui::compact::gpu
