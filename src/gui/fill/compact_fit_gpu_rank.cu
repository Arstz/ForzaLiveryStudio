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
constexpr long long kMaximumCells = 32LL * 1024 * 1024;

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
    unsigned long long *pieceCounts = counts + tile.piece * 5;
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
            || !check(coverage_.reserve(cells_))
            || !check(ownerXor_.reserve(cells_))) {
            return;
        }
        maskKernel<<<(cells_ + kThreads - 1) / kThreads, kThreads, 0, stream_>>>(
            preferred_, target_, inner_, outer_, spillFree_,
            originX_, originY_, cellSize_,
            columns_, cells_, masks_.data());
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

    bool evaluateAdditions(const Geometry &candidates, std::vector<double> *scores,
                           const AdditionWeights &weights) override {
        const std::lock_guard lock(mutex_);
        const auto start = std::chrono::steady_clock::now();
        if (scores == nullptr || !stats_.error.empty() || candidates.pieces.empty()) {
            return false;
        }
        const std::vector<Tile> candidateTiles = makeTiles(candidates);
        const size_t countSize = candidates.pieces.size() * 5;
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
                coverage_.data(), originX_, originY_, cellSize_, columns_, rows_,
                counts_.data());
        }
        std::vector<unsigned long long> counts(countSize);
        if (!check(cudaGetLastError())
            || !check(cudaMemcpyAsync(counts.data(), counts_.data(),
                countSize * sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream_))
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        const double cellArea = static_cast<double>(cellSize_) * cellSize_;
        scores->resize(candidates.pieces.size());
        for (size_t index = 0; index < candidates.pieces.size(); ++index) {
            (*scores)[index] = counts[index * 5 + 3] == 0
                ? cellArea * (weights.preferred * counts[index * 5]
                    + weights.inner * counts[index * 5 + 1]
                    - weights.spill * counts[index * 5 + 2]
                    + weights.join * counts[index * 5 + 4])
                : -std::numeric_limits<double>::infinity();
        }
        ++stats_.refinementCalls;
        stats_.refinementMilliseconds += elapsed(start);

        return true;
    }

    RankStats stats() const override {

        return stats_;
    }

private:
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
    DeviceBuffer<unsigned char> masks_;
    DeviceBuffer<unsigned int> coverage_;
    DeviceBuffer<unsigned int> ownerXor_;
    DeviceBuffer<unsigned long long> counts_;
    SetView preferred_;
    SetView target_;
    SetView inner_;
    SetView outer_;
    SetView spillFree_;
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

} // namespace

std::unique_ptr<RasterRanker> createRasterRanker(
    const Geometry &preferred, const Geometry &target,
    const Geometry &inner, const Geometry &outer,
    const Geometry &spillFree, double cellSize) {
    return std::make_unique<CudaRasterRanker>(
        preferred, target, inner, outer, spillFree, cellSize);
}

} // namespace gui::compact::gpu
