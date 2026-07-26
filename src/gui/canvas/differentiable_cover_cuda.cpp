#include "differentiable_cover_gpu.h"
#include "differentiable_cover_cuda_kernel.h"

#include <QtCore>

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace gui::cover {
namespace {

constexpr std::uint64_t kMaximumScratchBytes =
    512ULL * 1024ULL * 1024ULL;

QString cudaErrorText(const QString &operation, cudaError_t status) {
    return QStringLiteral("%1 failed: %2")
        .arg(operation, QString::fromLatin1(cudaGetErrorString(status)));
}

template <typename Value>
class DeviceBuffer {
public:
    DeviceBuffer() = default;

    ~DeviceBuffer() {
        cudaFree(data_);
    }

    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    bool upload(const QVector<Value> &values, QString *error) {
        if (!ensure(static_cast<size_t>(values.size()), error)) {
            return false;
        }
        if (values.isEmpty()) {
            return true;
        }
        const cudaError_t status = cudaMemcpy(
            data_, values.constData(),
            static_cast<size_t>(values.size()) * sizeof(Value),
            cudaMemcpyHostToDevice);
        if (status != cudaSuccess) {
            *error = cudaErrorText(
                QStringLiteral("cudaMemcpy upload"), status);
            return false;
        }

        return true;
    }

    bool download(QVector<Value> *values, QString *error) const {
        if (values->isEmpty()) {
            return true;
        }
        const cudaError_t status = cudaMemcpy(
            values->data(), data_,
            static_cast<size_t>(values->size()) * sizeof(Value),
            cudaMemcpyDeviceToHost);
        if (status != cudaSuccess) {
            *error = cudaErrorText(
                QStringLiteral("cudaMemcpy download"), status);
            return false;
        }

        return true;
    }

    bool ensure(size_t requested, QString *error) {
        requested = std::max<size_t>(1, requested);
        if (capacity_ >= requested) {
            return true;
        }
        size_t capacity = 1;
        while (capacity < requested
               && capacity
                   <= std::numeric_limits<size_t>::max() / 2) {
            capacity *= 2;
        }
        capacity = std::max(capacity, requested);
        cudaFree(data_);
        data_ = nullptr;
        capacity_ = 0;
        const cudaError_t status = cudaMalloc(
            reinterpret_cast<void **>(&data_),
            capacity * sizeof(Value));
        if (status != cudaSuccess) {
            *error = cudaErrorText(
                QStringLiteral("cudaMalloc"), status);
            return false;
        }
        capacity_ = capacity;

        return true;
    }

    Value *data() const {
        return data_;
    }

private:
    Value *data_ = nullptr;
    size_t capacity_ = 0;
};

bool boundsIntersect(const cuda::Polygon &polygon,
                     double minimumX,
                     double minimumY,
                     double maximumX,
                     double maximumY) {
    return maximumX >= polygon.minimumX
        && minimumX <= polygon.maximumX
        && maximumY >= polygon.minimumY
        && minimumY <= polygon.maximumY;
}

Vec2 mappedPoint(const Vec2 &point, const Affine &transform) {
    return {
        transform.a * point.x
            + transform.c * point.y + transform.e,
        transform.b * point.x
            + transform.d * point.y + transform.f,
    };
}

class CudaAreaEvaluator final : public GpuAreaEvaluator {
public:
    explicit CudaAreaEvaluator(
        const QVector<ShapeMesh> &catalog) {
        initialize(catalog);
    }

    bool setSubjects(const Polygons &coveredSubject,
                     const Polygons &legalSubject) override {
        if (!available_) {
            return false;
        }
        points_.clear();
        polygons_.clear();
        coveredPolygonCount_ = coveredSubject.size();
        appendPolygons(coveredSubject);
        appendPolygons(legalSubject);
        if (!pointBuffer_.upload(points_, &stats_.error)
            || !polygonBuffer_.upload(
                polygons_, &stats_.error)) {
            return disable(stats_.error);
        }

        return true;
    }

    bool evaluate(const QVector<GpuEvaluationRequest> &requests,
                  QVector<AreaGradient> *results) override {
        if (!available_ || results == nullptr
            || requests.isEmpty()) {
            return false;
        }

        QElapsedTimer timer;
        timer.start();
        QVector<cuda::Transform> transforms;
        transforms.reserve(requests.size());
        for (const GpuEvaluationRequest &request : requests) {
            const auto shape = shapeIndices_.constFind(request.shape);
            if (shape == shapeIndices_.cend()) {
                return disable(
                    QStringLiteral(
                        "CUDA request used an unknown shape"));
            }
            transforms.push_back({
                request.transform.a,
                request.transform.b,
                request.transform.c,
                request.transform.d,
                request.transform.e,
                request.transform.f,
                shape.value(),
            });
        }

        QVector<cuda::Task> tasks;
        std::uint64_t scratchCount = 0;
        buildTasks(requests, &tasks, &scratchCount);
        results->fill({}, requests.size());
        if (tasks.isEmpty()) {
            finishAreas(requests, results);
            finishBatch(
                requests.size(), 0, timer.nsecsElapsed());
            return true;
        }
        if (tasks.size()
                > std::numeric_limits<std::uint32_t>::max()
            || scratchCount
                > std::numeric_limits<std::uint32_t>::max()
            || scratchCount * 2ULL * sizeof(cuda::JetPoint)
                > kMaximumScratchBytes) {
            return disable(
                QStringLiteral(
                    "CUDA evaluation buffers exceed capacity"));
        }

        if (!transformBuffer_.upload(
                transforms, &stats_.error)
            || !taskBuffer_.upload(tasks, &stats_.error)
            || !scratchBuffer_.ensure(
                static_cast<size_t>(scratchCount * 2ULL),
                &stats_.error)
            || !resultBuffer_.ensure(
                static_cast<size_t>(tasks.size()),
                &stats_.error)) {
            return disable(stats_.error);
        }

        const cudaError_t status = cuda::launch(
            pointBuffer_.data(),
            polygonBuffer_.data(),
            shapeVertexBuffer_.data(),
            boundaryBuffer_.data(),
            triangleBuffer_.data(),
            shapeBuffer_.data(),
            transformBuffer_.data(),
            taskBuffer_.data(),
            static_cast<std::uint32_t>(tasks.size()),
            static_cast<std::uint32_t>(scratchCount),
            scratchBuffer_.data(),
            resultBuffer_.data());
        if (status != cudaSuccess) {
            return disable(cudaErrorText(
                QStringLiteral("CUDA kernel launch"), status));
        }

        QVector<cuda::TaskResult> taskResults(tasks.size());
        if (!resultBuffer_.download(
                &taskResults, &stats_.error)) {
            return disable(stats_.error);
        }
        for (const cuda::TaskResult &taskResult : taskResults) {
            if (taskResult.evaluation
                >= static_cast<std::uint32_t>(results->size())) {
                return disable(
                    QStringLiteral(
                        "CUDA returned an invalid evaluation"));
            }
            AreaGradient &evaluation =
                (*results)[static_cast<int>(
                    taskResult.evaluation)];
            double *value = taskResult.legal != 0
                ? &evaluation.spill : &evaluation.covered;
            std::array<double, cuda::kGradientCount> *gradient =
                taskResult.legal != 0
                ? &evaluation.spillGradient
                : &evaluation.coveredGradient;
            *value += taskResult.value;
            for (int parameter = 0;
                 parameter < cuda::kGradientCount; ++parameter) {
                (*gradient)[parameter] +=
                    taskResult.gradient[parameter];
            }
        }
        finishAreas(requests, results);
        finishBatch(
            requests.size(), tasks.size(),
            timer.nsecsElapsed());

        return true;
    }

    bool available() const override {
        return available_;
    }

    bool supportsOptimizerEvaluation() const override {
        return true;
    }

    bool usesDoublePrecision() const override {
        return true;
    }

    GpuEvaluatorStats stats() const override {
        return stats_;
    }

private:
    void appendPolygons(const Polygons &source) {
        for (const QPolygonF &polygon : source) {
            cuda::Polygon description;
            description.start =
                static_cast<std::uint32_t>(points_.size());
            description.count =
                static_cast<std::uint32_t>(polygon.size());
            description.minimumX =
                std::numeric_limits<double>::max();
            description.minimumY =
                std::numeric_limits<double>::max();
            description.maximumX =
                std::numeric_limits<double>::lowest();
            description.maximumY =
                std::numeric_limits<double>::lowest();
            for (const QPointF &point : polygon) {
                const cuda::Point converted{
                    point.x(), point.y(),
                };
                points_.push_back(converted);
                description.minimumX =
                    std::min(
                        description.minimumX, converted.x);
                description.minimumY =
                    std::min(
                        description.minimumY, converted.y);
                description.maximumX =
                    std::max(
                        description.maximumX, converted.x);
                description.maximumY =
                    std::max(
                        description.maximumY, converted.y);
            }
            polygons_.push_back(description);
        }
    }

    void appendTask(std::uint32_t evaluation,
                    std::uint32_t polygon,
                    std::uint32_t primitive,
                    bool legal,
                    std::uint32_t edgeCount,
                    QVector<cuda::Task> *tasks,
                    std::uint64_t *scratchCount) const {
        const std::uint64_t capacity =
            static_cast<std::uint64_t>(
                polygons_[polygon].count)
            + edgeCount + 2;
        if (*scratchCount + capacity
            > std::numeric_limits<std::uint32_t>::max()) {
            *scratchCount =
                std::numeric_limits<std::uint64_t>::max();
            return;
        }
        tasks->push_back({
            evaluation,
            polygon,
            primitive,
            legal ? 1U : 0U,
            static_cast<std::uint32_t>(*scratchCount),
            static_cast<std::uint32_t>(capacity),
        });
        *scratchCount += capacity;
    }

    void buildTasks(
        const QVector<GpuEvaluationRequest> &requests,
        QVector<cuda::Task> *tasks,
        std::uint64_t *scratchCount) const {
        for (int evaluation = 0;
             evaluation < requests.size(); ++evaluation) {
            const GpuEvaluationRequest &request =
                requests[evaluation];
            const ShapeMesh &shape = *request.shape;
            if (shape.convex) {
                double minimumX =
                    std::numeric_limits<double>::max();
                double minimumY =
                    std::numeric_limits<double>::max();
                double maximumX =
                    std::numeric_limits<double>::lowest();
                double maximumY =
                    std::numeric_limits<double>::lowest();
                for (const Vec2 &point : shape.boundary) {
                    const Vec2 mapped =
                        mappedPoint(point, request.transform);
                    minimumX = std::min(minimumX, mapped.x);
                    minimumY = std::min(minimumY, mapped.y);
                    maximumX = std::max(maximumX, mapped.x);
                    maximumY = std::max(maximumY, mapped.y);
                }
                for (int polygon = 0;
                     polygon < polygons_.size(); ++polygon) {
                    if (boundsIntersect(
                            polygons_[polygon],
                            minimumX, minimumY,
                            maximumX, maximumY)) {
                        appendTask(
                            static_cast<std::uint32_t>(
                                evaluation),
                            static_cast<std::uint32_t>(
                                polygon),
                            0,
                            polygon >= coveredPolygonCount_,
                            static_cast<std::uint32_t>(
                                shape.boundary.size()),
                            tasks, scratchCount);
                    }
                }
                continue;
            }

            for (int primitive = 0;
                 primitive < shape.triangles.size();
                 ++primitive) {
                const std::array<int, 3> &triangle =
                    shape.triangles[primitive];
                const std::array<Vec2, 3> mapped = {
                    mappedPoint(
                        shape.vertices[triangle[0]],
                        request.transform),
                    mappedPoint(
                        shape.vertices[triangle[1]],
                        request.transform),
                    mappedPoint(
                        shape.vertices[triangle[2]],
                        request.transform),
                };
                const double minimumX = std::min({
                    mapped[0].x,
                    mapped[1].x,
                    mapped[2].x,
                });
                const double minimumY = std::min({
                    mapped[0].y,
                    mapped[1].y,
                    mapped[2].y,
                });
                const double maximumX = std::max({
                    mapped[0].x,
                    mapped[1].x,
                    mapped[2].x,
                });
                const double maximumY = std::max({
                    mapped[0].y,
                    mapped[1].y,
                    mapped[2].y,
                });
                for (int polygon = 0;
                     polygon < polygons_.size(); ++polygon) {
                    if (boundsIntersect(
                            polygons_[polygon],
                            minimumX, minimumY,
                            maximumX, maximumY)) {
                        appendTask(
                            static_cast<std::uint32_t>(
                                evaluation),
                            static_cast<std::uint32_t>(
                                polygon),
                            static_cast<std::uint32_t>(
                                primitive),
                            polygon >= coveredPolygonCount_,
                            3, tasks, scratchCount);
                    }
                }
            }
        }
    }

    void finishAreas(
        const QVector<GpuEvaluationRequest> &requests,
        QVector<AreaGradient> *results) const {
        for (int evaluation = 0;
             evaluation < requests.size(); ++evaluation) {
            const ShapeMesh &shape =
                *requests[evaluation].shape;
            const Affine &transform =
                requests[evaluation].transform;
            AreaGradient &result = (*results)[evaluation];
            const double legal = result.spill;
            const std::array<double, cuda::kGradientCount>
                legalGradient = result.spillGradient;
            const double determinant =
                transform.a * transform.d
                - transform.b * transform.c;
            const double sign =
                determinant < 0.0 ? -1.0 : 1.0;
            const double transformedArea =
                std::abs(determinant) * shape.area;
            const std::array<double, cuda::kGradientCount>
                areaGradient = {
                    sign * transform.d * shape.area,
                    -sign * transform.c * shape.area,
                    -sign * transform.b * shape.area,
                    sign * transform.a * shape.area,
                    0.0,
                    0.0,
                };
            result.spill =
                std::max(0.0, transformedArea - legal);
            for (int parameter = 0;
                 parameter < cuda::kGradientCount; ++parameter) {
                result.spillGradient[parameter] =
                    areaGradient[parameter]
                    - legalGradient[parameter];
            }
        }
    }

    void finishBatch(qsizetype evaluations,
                     qsizetype tasks,
                     qint64 nanoseconds) {
        ++stats_.batches;
        stats_.evaluations +=
            static_cast<std::uint64_t>(evaluations);
        stats_.intersectionTasks +=
            static_cast<std::uint64_t>(tasks);
        stats_.wallSeconds +=
            static_cast<double>(nanoseconds) * 1e-9;
    }

    bool disable(const QString &error) {
        available_ = false;
        stats_.error = error;
        return false;
    }

    void initialize(const QVector<ShapeMesh> &catalog) {
        stats_.backend = QStringLiteral("CUDA optimizer");
        int deviceCount = 0;
        cudaError_t status = cudaGetDeviceCount(&deviceCount);
        if (status != cudaSuccess || deviceCount <= 0) {
            disable(status == cudaSuccess
                    ? QStringLiteral(
                          "No CUDA device is available")
                    : cudaErrorText(
                          QStringLiteral(
                              "cudaGetDeviceCount"),
                          status));
            return;
        }
        status = cudaSetDevice(0);
        if (status != cudaSuccess) {
            disable(cudaErrorText(
                QStringLiteral("cudaSetDevice"), status));
            return;
        }
        cudaDeviceProp properties{};
        status = cudaGetDeviceProperties(&properties, 0);
        if (status != cudaSuccess) {
            disable(cudaErrorText(
                QStringLiteral("cudaGetDeviceProperties"),
                status));
            return;
        }
        stats_.adapter =
            QString::fromLatin1(properties.name);

        QVector<cuda::Point> shapeVertices;
        QVector<cuda::Point> boundaries;
        QVector<cuda::Triangle> triangles;
        QVector<cuda::Shape> shapes;
        shapes.reserve(catalog.size());
        for (const ShapeMesh &shape : catalog) {
            cuda::Shape converted;
            converted.vertexStart =
                static_cast<std::uint32_t>(
                    shapeVertices.size());
            converted.triangleStart =
                static_cast<std::uint32_t>(
                    triangles.size());
            converted.triangleCount =
                static_cast<std::uint32_t>(
                    shape.triangles.size());
            converted.boundaryStart =
                static_cast<std::uint32_t>(
                    boundaries.size());
            converted.boundaryCount =
                static_cast<std::uint32_t>(
                    shape.boundary.size());
            converted.convex = shape.convex ? 1U : 0U;
            converted.area = shape.area;
            for (const Vec2 &point : shape.vertices) {
                shapeVertices.push_back({
                    point.x, point.y,
                });
            }
            for (const Vec2 &point : shape.boundary) {
                boundaries.push_back({
                    point.x, point.y,
                });
            }
            for (const std::array<int, 3> &triangle
                 : shape.triangles) {
                triangles.push_back({
                    static_cast<std::uint32_t>(
                        triangle[0]),
                    static_cast<std::uint32_t>(
                        triangle[1]),
                    static_cast<std::uint32_t>(
                        triangle[2]),
                });
            }
            shapeIndices_.insert(
                &shape,
                static_cast<std::uint32_t>(
                    shapes.size()));
            shapes.push_back(converted);
        }
        if (!shapeVertexBuffer_.upload(
                shapeVertices, &stats_.error)
            || !boundaryBuffer_.upload(
                boundaries, &stats_.error)
            || !triangleBuffer_.upload(
                triangles, &stats_.error)
            || !shapeBuffer_.upload(
                shapes, &stats_.error)) {
            disable(stats_.error);
            return;
        }
        available_ = true;
    }

    DeviceBuffer<cuda::Point> pointBuffer_;
    DeviceBuffer<cuda::Polygon> polygonBuffer_;
    DeviceBuffer<cuda::Point> shapeVertexBuffer_;
    DeviceBuffer<cuda::Point> boundaryBuffer_;
    DeviceBuffer<cuda::Triangle> triangleBuffer_;
    DeviceBuffer<cuda::Shape> shapeBuffer_;
    DeviceBuffer<cuda::Transform> transformBuffer_;
    DeviceBuffer<cuda::Task> taskBuffer_;
    DeviceBuffer<cuda::JetPoint> scratchBuffer_;
    DeviceBuffer<cuda::TaskResult> resultBuffer_;
    QHash<const ShapeMesh *, std::uint32_t> shapeIndices_;
    QVector<cuda::Point> points_;
    QVector<cuda::Polygon> polygons_;
    GpuEvaluatorStats stats_;
    int coveredPolygonCount_ = 0;
    bool available_ = false;
};

} // namespace

std::unique_ptr<GpuAreaEvaluator> createCudaAreaEvaluator(
    const QVector<ShapeMesh> &catalog) {
    return std::make_unique<CudaAreaEvaluator>(catalog);
}

} // namespace gui::cover
