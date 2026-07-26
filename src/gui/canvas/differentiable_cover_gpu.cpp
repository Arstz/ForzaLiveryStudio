#include "differentiable_cover_gpu.h"

#include <QtCore>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace gui::cover {
namespace {

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

constexpr UINT kShaderThreads = 64;
constexpr std::uint64_t kMaximumScratchBytes = 512ULL * 1024ULL * 1024ULL;

struct GpuPoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct GpuPolygon {
    std::uint32_t start = 0;
    std::uint32_t count = 0;
    float minimumX = 0.0f;
    float minimumY = 0.0f;
    float maximumX = 0.0f;
    float maximumY = 0.0f;
};

struct GpuTriangle {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
    std::uint32_t padding = 0;
};

struct GpuShape {
    std::uint32_t vertexStart = 0;
    std::uint32_t triangleStart = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t boundaryStart = 0;
    std::uint32_t boundaryCount = 0;
    std::uint32_t convex = 0;
    float area = 0.0f;
    std::uint32_t padding = 0;
};

struct GpuTransform {
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 0.0f;
    float e = 0.0f;
    float f = 0.0f;
    std::uint32_t shape = 0;
    std::uint32_t padding = 0;
};

struct GpuTask {
    std::uint32_t evaluation = 0;
    std::uint32_t polygon = 0;
    std::uint32_t primitive = 0;
    std::uint32_t legal = 0;
    std::uint32_t scratchOffset = 0;
    std::uint32_t scratchCapacity = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
};

struct GpuTaskResult {
    float value = 0.0f;
    std::array<float, 6> gradient{};
    std::uint32_t evaluation = 0;
    std::uint32_t legal = 0;
};

struct GpuConstants {
    std::uint32_t taskCount = 0;
    std::uint32_t scratchHalf = 0;
    std::uint32_t padding0 = 0;
    std::uint32_t padding1 = 0;
};

static_assert(sizeof(GpuPoint) == 8);
static_assert(sizeof(GpuPolygon) == 24);
static_assert(sizeof(GpuTriangle) == 16);
static_assert(sizeof(GpuShape) == 32);
static_assert(sizeof(GpuTransform) == 32);
static_assert(sizeof(GpuTask) == 32);
static_assert(sizeof(GpuTaskResult) == 36);
static_assert(sizeof(GpuConstants) == 16);

struct InputBuffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    UINT capacity = 0;
    UINT stride = 0;
};

struct OutputBuffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> view;
    ComPtr<ID3D11Buffer> staging;
    UINT capacity = 0;
    UINT stride = 0;
};

constexpr char kComputeShader[] = R"(
struct Point {
    float x;
    float y;
};

struct Polygon {
    uint start;
    uint count;
    float minimumX;
    float minimumY;
    float maximumX;
    float maximumY;
};

struct Triangle {
    uint first;
    uint second;
    uint third;
    uint padding;
};

struct Shape {
    uint vertexStart;
    uint triangleStart;
    uint triangleCount;
    uint boundaryStart;
    uint boundaryCount;
    uint convex;
    float area;
    uint padding;
};

struct Transform {
    float a;
    float b;
    float c;
    float d;
    float e;
    float f;
    uint shape;
    uint padding;
};

struct Task {
    uint evaluation;
    uint polygon;
    uint primitive;
    uint legal;
    uint scratchOffset;
    uint scratchCapacity;
    uint padding0;
    uint padding1;
};

struct Jet {
    float value;
    float gradient[6];
};

struct JetPoint {
    Jet x;
    Jet y;
};

struct TaskResult {
    float value;
    float gradient[6];
    uint evaluation;
    uint legal;
};

StructuredBuffer<Point> points : register(t0);
StructuredBuffer<Polygon> polygons : register(t1);
StructuredBuffer<Point> shapeVertices : register(t2);
StructuredBuffer<Point> boundaries : register(t3);
StructuredBuffer<Triangle> triangles : register(t4);
StructuredBuffer<Shape> shapes : register(t5);
StructuredBuffer<Transform> transforms : register(t6);
StructuredBuffer<Task> tasks : register(t7);
RWStructuredBuffer<JetPoint> scratch : register(u0);
RWStructuredBuffer<TaskResult> taskResults : register(u1);

cbuffer Constants : register(b0) {
    uint taskCount;
    uint scratchHalf;
    uint padding0;
    uint padding1;
};

Jet zeroJet() {
    Jet result = (Jet)0;
    return result;
}

Jet constantJet(float value) {
    Jet result = zeroJet();
    result.value = value;
    return result;
}

Jet addJet(Jet left, Jet right) {
    Jet result;
    result.value = left.value + right.value;
    [unroll]
    for (uint parameter = 0; parameter < 6; ++parameter) {
        result.gradient[parameter] =
            left.gradient[parameter] + right.gradient[parameter];
    }
    return result;
}

Jet subtractJet(Jet left, Jet right) {
    Jet result;
    result.value = left.value - right.value;
    [unroll]
    for (uint parameter = 0; parameter < 6; ++parameter) {
        result.gradient[parameter] =
            left.gradient[parameter] - right.gradient[parameter];
    }
    return result;
}

Jet multiplyJet(Jet left, Jet right) {
    Jet result;
    result.value = left.value * right.value;
    [unroll]
    for (uint parameter = 0; parameter < 6; ++parameter) {
        result.gradient[parameter] =
            left.gradient[parameter] * right.value
            + left.value * right.gradient[parameter];
    }
    return result;
}

Jet divideJet(Jet left, Jet right) {
    Jet result;
    float denominator = right.value * right.value;
    result.value = left.value / right.value;
    [unroll]
    for (uint parameter = 0; parameter < 6; ++parameter) {
        result.gradient[parameter] =
            (left.gradient[parameter] * right.value
             - left.value * right.gradient[parameter])
            / denominator;
    }
    return result;
}

Jet scaleJet(Jet value, float scale) {
    Jet result;
    result.value = value.value * scale;
    [unroll]
    for (uint parameter = 0; parameter < 6; ++parameter) {
        result.gradient[parameter] = value.gradient[parameter] * scale;
    }
    return result;
}

JetPoint constantPoint(Point sourcePoint) {
    JetPoint result;
    result.x = constantJet(sourcePoint.x);
    result.y = constantJet(sourcePoint.y);
    return result;
}

JetPoint transformedPoint(Point sourcePoint, Transform transform) {
    JetPoint result;
    result.x = zeroJet();
    result.y = zeroJet();
    result.x.value =
        transform.a * sourcePoint.x
        + transform.c * sourcePoint.y + transform.e;
    result.y.value =
        transform.b * sourcePoint.x
        + transform.d * sourcePoint.y + transform.f;
    result.x.gradient[0] = sourcePoint.x;
    result.y.gradient[1] = sourcePoint.x;
    result.x.gradient[2] = sourcePoint.y;
    result.y.gradient[3] = sourcePoint.y;
    result.x.gradient[4] = 1.0;
    result.y.gradient[5] = 1.0;
    return result;
}

JetPoint addPoint(JetPoint left, JetPoint right) {
    JetPoint result;
    result.x = addJet(left.x, right.x);
    result.y = addJet(left.y, right.y);
    return result;
}

JetPoint subtractPoint(JetPoint left, JetPoint right) {
    JetPoint result;
    result.x = subtractJet(left.x, right.x);
    result.y = subtractJet(left.y, right.y);
    return result;
}

JetPoint scalePoint(JetPoint sourcePoint, Jet scale) {
    JetPoint result;
    result.x = multiplyJet(sourcePoint.x, scale);
    result.y = multiplyJet(sourcePoint.y, scale);
    return result;
}

Jet crossPoint(JetPoint left, JetPoint right) {
    return subtractJet(
        multiplyJet(left.x, right.y),
        multiplyJet(left.y, right.x));
}

Jet sideValue(
    JetPoint sourcePoint,
    JetPoint windowStart,
    JetPoint windowEnd) {
    return crossPoint(
        subtractPoint(windowEnd, windowStart),
        subtractPoint(sourcePoint, windowStart));
}

JetPoint intersectionPoint(
    JetPoint start,
    JetPoint end,
    Jet startSide,
    Jet endSide) {
    Jet denominator = subtractJet(startSide, endSide);
    if (abs(denominator.value) <= 1e-10) {
        return end;
    }
    Jet fraction = divideJet(startSide, denominator);
    return addPoint(start, scalePoint(subtractPoint(end, start), fraction));
}

uint triangleVertex(Triangle shapeTriangle, uint index) {
    if (index == 0) {
        return shapeTriangle.first;
    }
    if (index == 1) {
        return shapeTriangle.second;
    }
    return shapeTriangle.third;
}

JetPoint windowPoint(
    Shape shape,
    Transform transform,
    uint primitive,
    uint index) {
    if (shape.convex != 0) {
        return transformedPoint(
            boundaries[shape.boundaryStart + index], transform);
    }
    Triangle shapeTriangle = triangles[shape.triangleStart + primitive];
    return transformedPoint(
        shapeVertices[
            shape.vertexStart + triangleVertex(shapeTriangle, index)],
        transform);
}

uint clipEdge(
    uint source,
    uint destination,
    uint count,
    uint capacity,
    JetPoint windowStart,
    JetPoint windowEnd) {
    if (count == 0) {
        return 0;
    }
    uint resultCount = 0;
    JetPoint start = scratch[source + count - 1];
    Jet startSide = sideValue(start, windowStart, windowEnd);
    bool startInside = startSide.value >= -1e-10;
    for (uint index = 0; index < count; ++index) {
        JetPoint end = scratch[source + index];
        Jet endSide = sideValue(end, windowStart, windowEnd);
        bool endInside = endSide.value >= -1e-10;
        if (endInside != startInside && resultCount < capacity) {
            scratch[destination + resultCount] =
                intersectionPoint(start, end, startSide, endSide);
            ++resultCount;
        }
        if (endInside && resultCount < capacity) {
            scratch[destination + resultCount] = end;
            ++resultCount;
        }
        start = end;
        startSide = endSide;
        startInside = endInside;
    }
    return resultCount;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID) {
    uint taskIndex = dispatchId.x;
    if (taskIndex >= taskCount) {
        return;
    }

    Task task = tasks[taskIndex];
    Polygon polygon = polygons[task.polygon];
    Transform transform = transforms[task.evaluation];
    Shape shape = shapes[transform.shape];
    uint edgeCount = shape.convex != 0 ? shape.boundaryCount : 3;
    uint firstBuffer = task.scratchOffset;
    uint secondBuffer = scratchHalf + task.scratchOffset;
    for (uint index = 0; index < polygon.count; ++index) {
        scratch[firstBuffer + index] =
            constantPoint(points[polygon.start + index]);
    }

    bool reverse = false;
    if (shape.convex != 0) {
        float doubledArea = 0.0;
        for (uint edge = 0; edge < edgeCount; ++edge) {
            JetPoint left = windowPoint(
                shape, transform, task.primitive, edge);
            JetPoint right = windowPoint(
                shape, transform, task.primitive,
                (edge + 1) % edgeCount);
            doubledArea +=
                left.x.value * right.y.value
                - left.y.value * right.x.value;
        }
        reverse = doubledArea < 0.0;
    } else {
        JetPoint first = windowPoint(shape, transform, task.primitive, 0);
        JetPoint second = windowPoint(shape, transform, task.primitive, 1);
        JetPoint third = windowPoint(shape, transform, task.primitive, 2);
        reverse = crossPoint(
            subtractPoint(second, first),
            subtractPoint(third, first)).value < 0.0;
    }

    uint count = polygon.count;
    bool sourceIsFirst = true;
    for (uint edge = 0; edge < edgeCount && count > 0; ++edge) {
        uint startIndex = reverse
            ? (edgeCount - edge) % edgeCount
            : edge;
        uint endIndex = reverse
            ? (edgeCount - edge - 1 + edgeCount) % edgeCount
            : (edge + 1) % edgeCount;
        JetPoint windowStart = windowPoint(
            shape, transform, task.primitive, startIndex);
        JetPoint windowEnd = windowPoint(
            shape, transform, task.primitive, endIndex);
        uint source = sourceIsFirst ? firstBuffer : secondBuffer;
        uint destination = sourceIsFirst ? secondBuffer : firstBuffer;
        count = clipEdge(
            source, destination, count, task.scratchCapacity,
            windowStart, windowEnd);
        sourceIsFirst = !sourceIsFirst;
    }

    Jet area = zeroJet();
    if (count >= 3) {
        uint source = sourceIsFirst ? firstBuffer : secondBuffer;
        for (uint index = 0; index < count; ++index) {
            area = addJet(
                area,
                crossPoint(
                    scratch[source + index],
                    scratch[source + ((index + 1) % count)]));
        }
        area = scaleJet(area, 0.5);
    }

    TaskResult result;
    result.value = area.value;
    [unroll]
    for (uint parameter = 0; parameter < 6; ++parameter) {
        result.gradient[parameter] = area.gradient[parameter];
    }
    result.evaluation = task.evaluation;
    result.legal = task.legal;
    taskResults[taskIndex] = result;
}
)";

UINT grownCapacity(UINT requested) {
    UINT result = 1;
    while (result < requested && result <= std::numeric_limits<UINT>::max() / 2) {
        result *= 2;
    }
    return std::max(result, requested);
}

QString hresultText(const char *operation, HRESULT result) {
    return QStringLiteral("%1 failed (HRESULT 0x%2)")
        .arg(QString::fromLatin1(operation))
        .arg(static_cast<quint32>(result), 8, 16, QLatin1Char('0'));
}

bool boundsIntersect(const GpuPolygon &polygon,
                     double minimumX,
                     double minimumY,
                     double maximumX,
                     double maximumY) {
    const double scale = std::max({
        1.0,
        std::abs(minimumX),
        std::abs(minimumY),
        std::abs(maximumX),
        std::abs(maximumY),
        std::abs(static_cast<double>(polygon.minimumX)),
        std::abs(static_cast<double>(polygon.minimumY)),
        std::abs(static_cast<double>(polygon.maximumX)),
        std::abs(static_cast<double>(polygon.maximumY)),
    });
    const double padding =
        scale * std::numeric_limits<float>::epsilon() * 8.0;
    return maximumX + padding >= polygon.minimumX
        && minimumX - padding <= polygon.maximumX
        && maximumY + padding >= polygon.minimumY
        && minimumY - padding <= polygon.maximumY;
}

Vec2 mappedPoint(const Vec2 &point, const Affine &transform) {
    return {
        transform.a * point.x + transform.c * point.y + transform.e,
        transform.b * point.x + transform.d * point.y + transform.f,
    };
}

class D3dAreaEvaluator final : public GpuAreaEvaluator {
public:
    explicit D3dAreaEvaluator(const QVector<ShapeMesh> &catalog) {
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
        if (!uploadInput(&pointBuffer_, points_)) {
            return disable(QStringLiteral("Could not upload GPU subject points"));
        }
        if (!uploadInput(&polygonBuffer_, polygons_)) {
            return disable(QStringLiteral("Could not upload GPU subject polygons"));
        }

        return true;
    }

    bool evaluate(const QVector<GpuEvaluationRequest> &requests,
                  QVector<AreaGradient> *results) override {
        if (!available_ || results == nullptr || requests.isEmpty()) {
            return false;
        }

        QElapsedTimer timer;
        timer.start();
        QVector<GpuTransform> transforms;
        transforms.reserve(requests.size());
        for (const GpuEvaluationRequest &request : requests) {
            const auto shape = shapeIndices_.constFind(request.shape);
            if (shape == shapeIndices_.cend()) {
                return disable(QStringLiteral("GPU request used an unknown shape"));
            }
            transforms.push_back({
                static_cast<float>(request.transform.a),
                static_cast<float>(request.transform.b),
                static_cast<float>(request.transform.c),
                static_cast<float>(request.transform.d),
                static_cast<float>(request.transform.e),
                static_cast<float>(request.transform.f),
                shape.value(),
                0,
            });
        }

        QVector<GpuTask> tasks;
        std::uint64_t scratchCount = 0;
        buildTasks(requests, &tasks, &scratchCount);
        results->fill({}, requests.size());
        if (tasks.isEmpty()) {
            finishAreas(requests, results);
            ++stats_.batches;
            stats_.evaluations += requests.size();
            stats_.wallSeconds +=
                static_cast<double>(timer.nsecsElapsed()) * 1e-9;
            return true;
        }
        if (tasks.size() > static_cast<qsizetype>(
                static_cast<std::uint64_t>(kShaderThreads) * 65535ULL)
            || scratchCount > std::numeric_limits<std::uint32_t>::max()
            || scratchCount * 2ULL * sizeof(float) * 14ULL
                > kMaximumScratchBytes) {
            if (requests.size() > 1) {
                const qsizetype middle = requests.size() / 2;
                QVector<AreaGradient> firstResults;
                QVector<AreaGradient> secondResults;
                if (!evaluate(
                        requests.mid(0, middle),
                        &firstResults)
                    || !evaluate(
                        requests.mid(middle),
                        &secondResults)) {
                    return false;
                }
                *results = std::move(firstResults);
                *results += secondResults;

                return true;
            }
            return disable(QStringLiteral("GPU evaluation buffers exceed capacity"));
        }

        if (!uploadInput(&transformBuffer_, transforms)
            || !uploadInput(&taskBuffer_, tasks)
            || !ensureOutput(
                &scratchBuffer_,
                static_cast<UINT>(scratchCount * 2ULL),
                sizeof(float) * 14,
                false)
            || !ensureOutput(
                &resultBuffer_,
                static_cast<UINT>(tasks.size()),
                sizeof(GpuTaskResult),
                true)) {
            return disable(QStringLiteral("Could not allocate GPU evaluation buffers"));
        }

        GpuConstants constants;
        constants.taskCount = static_cast<std::uint32_t>(tasks.size());
        constants.scratchHalf = static_cast<std::uint32_t>(scratchCount);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT status = context_->Map(
            constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(status)) {
            return disable(hresultText("Map(constants)", status));
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constantBuffer_.Get(), 0);

        std::array<ID3D11ShaderResourceView *, 8> views = {
            pointBuffer_.view.Get(),
            polygonBuffer_.view.Get(),
            shapeVertexBuffer_.view.Get(),
            boundaryBuffer_.view.Get(),
            triangleBuffer_.view.Get(),
            shapeBuffer_.view.Get(),
            transformBuffer_.view.Get(),
            taskBuffer_.view.Get(),
        };
        std::array<ID3D11UnorderedAccessView *, 2> outputs = {
            scratchBuffer_.view.Get(),
            resultBuffer_.view.Get(),
        };
        ID3D11Buffer *constantsBuffer = constantBuffer_.Get();
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetShaderResources(
            0, static_cast<UINT>(views.size()), views.data());
        context_->CSSetUnorderedAccessViews(
            0, static_cast<UINT>(outputs.size()), outputs.data(), nullptr);
        context_->CSSetConstantBuffers(0, 1, &constantsBuffer);
        context_->Dispatch(
            (static_cast<UINT>(tasks.size()) + kShaderThreads - 1)
                / kShaderThreads,
            1, 1);

        std::array<ID3D11ShaderResourceView *, 8> emptyViews{};
        std::array<ID3D11UnorderedAccessView *, 2> emptyOutputs{};
        context_->CSSetShaderResources(
            0, static_cast<UINT>(emptyViews.size()), emptyViews.data());
        context_->CSSetUnorderedAccessViews(
            0, static_cast<UINT>(emptyOutputs.size()),
            emptyOutputs.data(), nullptr);
        context_->CopyResource(
            resultBuffer_.staging.Get(), resultBuffer_.buffer.Get());
        status = context_->Map(
            resultBuffer_.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(status)) {
            return disable(hresultText("Map(results)", status));
        }

        const auto *taskResults =
            static_cast<const GpuTaskResult *>(mapped.pData);
        for (int taskIndex = 0; taskIndex < tasks.size(); ++taskIndex) {
            const GpuTaskResult &taskResult = taskResults[taskIndex];
            if (taskResult.evaluation >=
                static_cast<std::uint32_t>(results->size())) {
                context_->Unmap(resultBuffer_.staging.Get(), 0);
                return disable(QStringLiteral("GPU returned an invalid evaluation"));
            }
            AreaGradient &evaluation =
                (*results)[static_cast<int>(taskResult.evaluation)];
            double *value = taskResult.legal != 0
                ? &evaluation.spill : &evaluation.covered;
            std::array<double, 6> *gradient = taskResult.legal != 0
                ? &evaluation.spillGradient
                : &evaluation.coveredGradient;
            *value += taskResult.value;
            for (int parameter = 0; parameter < 6; ++parameter) {
                (*gradient)[parameter] += taskResult.gradient[parameter];
            }
        }
        context_->Unmap(resultBuffer_.staging.Get(), 0);
        finishAreas(requests, results);

        ++stats_.batches;
        stats_.evaluations += requests.size();
        stats_.intersectionTasks += tasks.size();
        stats_.wallSeconds +=
            static_cast<double>(timer.nsecsElapsed()) * 1e-9;
        return true;
    }

    bool available() const override {
        return available_;
    }

    bool supportsOptimizerEvaluation() const override {
        return false;
    }

    bool usesDoublePrecision() const override {
        return false;
    }

    GpuEvaluatorStats stats() const override {
        return stats_;
    }

private:
    template <typename Value>
    bool uploadInput(InputBuffer *target, const QVector<Value> &values) {
        const UINT count = std::max<UINT>(1, static_cast<UINT>(values.size()));
        if (target->capacity < count || target->stride != sizeof(Value)) {
            target->buffer.Reset();
            target->view.Reset();
            target->capacity = grownCapacity(count);
            target->stride = sizeof(Value);
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = target->capacity * target->stride;
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            description.StructureByteStride = target->stride;
            HRESULT status = device_->CreateBuffer(
                &description, nullptr, &target->buffer);
            if (FAILED(status)) {
                stats_.error = hresultText("CreateBuffer(input)", status);
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
            viewDescription.Format = DXGI_FORMAT_UNKNOWN;
            viewDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            viewDescription.Buffer.NumElements = target->capacity;
            status = device_->CreateShaderResourceView(
                target->buffer.Get(), &viewDescription, &target->view);
            if (FAILED(status)) {
                stats_.error =
                    hresultText("CreateShaderResourceView", status);
                return false;
            }
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT status = context_->Map(
            target->buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(status)) {
            stats_.error = hresultText("Map(input)", status);
            return false;
        }
        if (!values.isEmpty()) {
            std::memcpy(
                mapped.pData, values.constData(),
                static_cast<size_t>(values.size()) * sizeof(Value));
        } else {
            std::memset(mapped.pData, 0, sizeof(Value));
        }
        context_->Unmap(target->buffer.Get(), 0);
        return true;
    }

    bool ensureOutput(OutputBuffer *target,
                      UINT count,
                      UINT stride,
                      bool staging) {
        count = std::max<UINT>(1, count);
        if (target->capacity >= count && target->stride == stride
            && (!staging || target->staging != nullptr)) {
            return true;
        }
        target->buffer.Reset();
        target->view.Reset();
        target->staging.Reset();
        target->capacity = grownCapacity(count);
        target->stride = stride;

        D3D11_BUFFER_DESC description{};
        description.ByteWidth = target->capacity * stride;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        description.StructureByteStride = stride;
        HRESULT status = device_->CreateBuffer(
            &description, nullptr, &target->buffer);
        if (FAILED(status)) {
            stats_.error = hresultText("CreateBuffer(output)", status);
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC viewDescription{};
        viewDescription.Format = DXGI_FORMAT_UNKNOWN;
        viewDescription.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        viewDescription.Buffer.NumElements = target->capacity;
        status = device_->CreateUnorderedAccessView(
            target->buffer.Get(), &viewDescription, &target->view);
        if (FAILED(status)) {
            stats_.error = hresultText("CreateUnorderedAccessView", status);
            return false;
        }
        if (!staging) {
            return true;
        }

        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        status = device_->CreateBuffer(
            &description, nullptr, &target->staging);
        if (FAILED(status)) {
            stats_.error = hresultText("CreateBuffer(staging)", status);
            return false;
        }
        return true;
    }

    void appendPolygons(const Polygons &source) {
        for (const QPolygonF &polygon : source) {
            GpuPolygon description;
            description.start = static_cast<std::uint32_t>(points_.size());
            description.count = static_cast<std::uint32_t>(polygon.size());
            description.minimumX = std::numeric_limits<float>::max();
            description.minimumY = std::numeric_limits<float>::max();
            description.maximumX = std::numeric_limits<float>::lowest();
            description.maximumY = std::numeric_limits<float>::lowest();
            for (const QPointF &point : polygon) {
                const GpuPoint converted{
                    static_cast<float>(point.x()),
                    static_cast<float>(point.y()),
                };
                points_.push_back(converted);
                description.minimumX =
                    std::min(description.minimumX, converted.x);
                description.minimumY =
                    std::min(description.minimumY, converted.y);
                description.maximumX =
                    std::max(description.maximumX, converted.x);
                description.maximumY =
                    std::max(description.maximumY, converted.y);
            }
            polygons_.push_back(description);
        }
    }

    void appendTask(std::uint32_t evaluation,
                    std::uint32_t polygon,
                    std::uint32_t primitive,
                    bool legal,
                    std::uint32_t edgeCount,
                    QVector<GpuTask> *tasks,
                    std::uint64_t *scratchCount) const {
        const std::uint64_t capacity =
            static_cast<std::uint64_t>(polygons_[polygon].count)
            + edgeCount + 2;
        if (*scratchCount + capacity
            > std::numeric_limits<std::uint32_t>::max()) {
            *scratchCount = std::numeric_limits<std::uint64_t>::max();
            return;
        }
        tasks->push_back({
            evaluation,
            polygon,
            primitive,
            legal ? 1U : 0U,
            static_cast<std::uint32_t>(*scratchCount),
            static_cast<std::uint32_t>(capacity),
            0,
            0,
        });
        *scratchCount += capacity;
    }

    void buildTasks(const QVector<GpuEvaluationRequest> &requests,
                    QVector<GpuTask> *tasks,
                    std::uint64_t *scratchCount) const {
        for (int evaluation = 0; evaluation < requests.size(); ++evaluation) {
            const GpuEvaluationRequest &request = requests[evaluation];
            const ShapeMesh &shape = *request.shape;
            if (shape.convex) {
                double minimumX = std::numeric_limits<double>::max();
                double minimumY = std::numeric_limits<double>::max();
                double maximumX = std::numeric_limits<double>::lowest();
                double maximumY = std::numeric_limits<double>::lowest();
                for (const Vec2 &point : shape.boundary) {
                    const Vec2 mapped = mappedPoint(point, request.transform);
                    minimumX = std::min(minimumX, mapped.x);
                    minimumY = std::min(minimumY, mapped.y);
                    maximumX = std::max(maximumX, mapped.x);
                    maximumY = std::max(maximumY, mapped.y);
                }
                for (int polygon = 0; polygon < polygons_.size(); ++polygon) {
                    if (boundsIntersect(
                            polygons_[polygon], minimumX, minimumY,
                            maximumX, maximumY)) {
                        appendTask(
                            static_cast<std::uint32_t>(evaluation),
                            static_cast<std::uint32_t>(polygon), 0,
                            polygon >= coveredPolygonCount_,
                            static_cast<std::uint32_t>(shape.boundary.size()),
                            tasks, scratchCount);
                    }
                }
                continue;
            }

            for (int primitive = 0; primitive < shape.triangles.size();
                 ++primitive) {
                const std::array<int, 3> &triangle =
                    shape.triangles[primitive];
                std::array<Vec2, 3> mapped = {
                    mappedPoint(shape.vertices[triangle[0]], request.transform),
                    mappedPoint(shape.vertices[triangle[1]], request.transform),
                    mappedPoint(shape.vertices[triangle[2]], request.transform),
                };
                const double minimumX = std::min({
                    mapped[0].x, mapped[1].x, mapped[2].x,
                });
                const double minimumY = std::min({
                    mapped[0].y, mapped[1].y, mapped[2].y,
                });
                const double maximumX = std::max({
                    mapped[0].x, mapped[1].x, mapped[2].x,
                });
                const double maximumY = std::max({
                    mapped[0].y, mapped[1].y, mapped[2].y,
                });
                for (int polygon = 0; polygon < polygons_.size(); ++polygon) {
                    if (boundsIntersect(
                            polygons_[polygon], minimumX, minimumY,
                            maximumX, maximumY)) {
                        appendTask(
                            static_cast<std::uint32_t>(evaluation),
                            static_cast<std::uint32_t>(polygon),
                            static_cast<std::uint32_t>(primitive),
                            polygon >= coveredPolygonCount_, 3,
                            tasks, scratchCount);
                    }
                }
            }
        }
    }

    void finishAreas(const QVector<GpuEvaluationRequest> &requests,
                     QVector<AreaGradient> *results) const {
        for (int evaluation = 0; evaluation < requests.size(); ++evaluation) {
            const ShapeMesh &shape = *requests[evaluation].shape;
            const Affine &transform = requests[evaluation].transform;
            AreaGradient &result = (*results)[evaluation];
            const double legal = result.spill;
            const std::array<double, 6> legalGradient =
                result.spillGradient;
            const double determinant =
                transform.a * transform.d - transform.b * transform.c;
            const double sign = determinant < 0.0 ? -1.0 : 1.0;
            const double transformedArea =
                std::abs(determinant) * shape.area;
            const std::array<double, 6> areaGradient = {
                sign * transform.d * shape.area,
                -sign * transform.c * shape.area,
                -sign * transform.b * shape.area,
                sign * transform.a * shape.area,
                0.0,
                0.0,
            };
            result.spill = std::max(0.0, transformedArea - legal);
            for (int parameter = 0; parameter < 6; ++parameter) {
                result.spillGradient[parameter] =
                    areaGradient[parameter] - legalGradient[parameter];
            }
        }
    }

    bool disable(const QString &error) {
        available_ = false;
        stats_.error = error;
        return false;
    }

    void initialize(const QVector<ShapeMesh> &catalog) {
        stats_.backend = QStringLiteral("Direct3D 11 compute");
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT status = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_SINGLETHREADED,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &device_, &selectedLevel, &context_);
        if (FAILED(status)) {
            disable(hresultText("D3D11CreateDevice", status));
            return;
        }

        ComPtr<ID3DBlob> shaderBytes;
        ComPtr<ID3DBlob> compilerError;
        status = D3DCompile(
            kComputeShader, std::strlen(kComputeShader), nullptr,
            nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &shaderBytes, &compilerError);
        if (FAILED(status)) {
            const QString detail = compilerError != nullptr
                ? QString::fromUtf8(
                      static_cast<const char *>(
                          compilerError->GetBufferPointer()),
                      static_cast<qsizetype>(
                          compilerError->GetBufferSize()))
                : hresultText("D3DCompile", status);
            disable(detail.trimmed());
            return;
        }
        status = device_->CreateComputeShader(
            shaderBytes->GetBufferPointer(),
            shaderBytes->GetBufferSize(), nullptr, &shader_);
        if (FAILED(status)) {
            disable(hresultText("CreateComputeShader", status));
            return;
        }

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(GpuConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        status = device_->CreateBuffer(
            &constantDescription, nullptr, &constantBuffer_);
        if (FAILED(status)) {
            disable(hresultText("CreateBuffer(constants)", status));
            return;
        }

        QVector<GpuPoint> shapeVertices;
        QVector<GpuPoint> boundaries;
        QVector<GpuTriangle> triangles;
        QVector<GpuShape> shapes;
        shapes.reserve(catalog.size());
        for (const ShapeMesh &shape : catalog) {
            GpuShape converted;
            converted.vertexStart =
                static_cast<std::uint32_t>(shapeVertices.size());
            converted.triangleStart =
                static_cast<std::uint32_t>(triangles.size());
            converted.triangleCount =
                static_cast<std::uint32_t>(shape.triangles.size());
            converted.boundaryStart =
                static_cast<std::uint32_t>(boundaries.size());
            converted.boundaryCount =
                static_cast<std::uint32_t>(shape.boundary.size());
            converted.convex = shape.convex ? 1U : 0U;
            converted.area = static_cast<float>(shape.area);
            for (const Vec2 &point : shape.vertices) {
                shapeVertices.push_back({
                    static_cast<float>(point.x),
                    static_cast<float>(point.y),
                });
            }
            for (const Vec2 &point : shape.boundary) {
                boundaries.push_back({
                    static_cast<float>(point.x),
                    static_cast<float>(point.y),
                });
            }
            for (const std::array<int, 3> &triangle : shape.triangles) {
                triangles.push_back({
                    static_cast<std::uint32_t>(triangle[0]),
                    static_cast<std::uint32_t>(triangle[1]),
                    static_cast<std::uint32_t>(triangle[2]),
                    0,
                });
            }
            shapeIndices_.insert(
                &shape, static_cast<std::uint32_t>(shapes.size()));
            shapes.push_back(converted);
        }
        if (!uploadInput(&shapeVertexBuffer_, shapeVertices)
            || !uploadInput(&boundaryBuffer_, boundaries)
            || !uploadInput(&triangleBuffer_, triangles)
            || !uploadInput(&shapeBuffer_, shapes)) {
            disable(stats_.error);
            return;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(device_.As(&dxgiDevice))
            && SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC description{};
            if (SUCCEEDED(adapter->GetDesc(&description))) {
                stats_.adapter = QString::fromWCharArray(
                    description.Description);
            }
        }
        available_ = true;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    InputBuffer pointBuffer_;
    InputBuffer polygonBuffer_;
    InputBuffer shapeVertexBuffer_;
    InputBuffer boundaryBuffer_;
    InputBuffer triangleBuffer_;
    InputBuffer shapeBuffer_;
    InputBuffer transformBuffer_;
    InputBuffer taskBuffer_;
    OutputBuffer scratchBuffer_;
    OutputBuffer resultBuffer_;
    QHash<const ShapeMesh *, std::uint32_t> shapeIndices_;
    QVector<GpuPoint> points_;
    QVector<GpuPolygon> polygons_;
    GpuEvaluatorStats stats_;
    int coveredPolygonCount_ = 0;
    bool available_ = false;
};

#else

class UnavailableGpuEvaluator final : public GpuAreaEvaluator {
public:
    bool setSubjects(const Polygons &, const Polygons &) override {
        return false;
    }

    bool evaluate(const QVector<GpuEvaluationRequest> &,
                  QVector<AreaGradient> *) override {
        return false;
    }

    bool available() const override {
        return false;
    }

    bool supportsOptimizerEvaluation() const override {
        return false;
    }

    bool usesDoublePrecision() const override {
        return false;
    }

    GpuEvaluatorStats stats() const override {
        GpuEvaluatorStats result;
        result.backend = QStringLiteral("Direct3D 11 compute");
        result.error = QStringLiteral(
            "Direct3D GPU evaluation is unavailable on this platform");
        return result;
    }
};

#endif

class FallbackGpuEvaluator final : public GpuAreaEvaluator {
public:
    explicit FallbackGpuEvaluator(const QVector<ShapeMesh> &catalog) {
#ifdef FH6_HAS_CUDA
        evaluators_.push_back(createCudaAreaEvaluator(catalog));
#endif
#ifdef Q_OS_WIN
        evaluators_.push_back(std::make_unique<D3dAreaEvaluator>(catalog));
#else
        evaluators_.push_back(std::make_unique<UnavailableGpuEvaluator>());
#endif
        selectAvailable(0);
    }

    bool setSubjects(const Polygons &coveredSubject,
                     const Polygons &legalSubject) override {
        coveredSubject_ = coveredSubject;
        legalSubject_ = legalSubject;
        subjectsSet_ = false;
        while (active_ < evaluators_.size()) {
            if (evaluators_[active_]->available()
                && evaluators_[active_]->setSubjects(
                    coveredSubject_, legalSubject_)) {
                subjectsSet_ = true;
                return true;
            }
            selectAvailable(active_ + 1);
        }

        return false;
    }

    bool evaluate(const QVector<GpuEvaluationRequest> &requests,
                  QVector<AreaGradient> *results) override {
        while (active_ < evaluators_.size()) {
            if (subjectsSet_
                && evaluators_[active_]->evaluate(requests, results)) {
                return true;
            }
            selectAvailable(active_ + 1);
            if (active_ < evaluators_.size()
                && evaluators_[active_]->setSubjects(
                    coveredSubject_, legalSubject_)) {
                subjectsSet_ = true;
                continue;
            }
            subjectsSet_ = false;
        }

        return false;
    }

    bool available() const override {
        return active_ < evaluators_.size();
    }

    bool supportsOptimizerEvaluation() const override {
        return available()
            && evaluators_[active_]->supportsOptimizerEvaluation();
    }

    bool usesDoublePrecision() const override {
        return available()
            && evaluators_[active_]->usesDoublePrecision();
    }

    GpuEvaluatorStats stats() const override {
        GpuEvaluatorStats result;
        QStringList errors;
        for (size_t index = 0; index < evaluators_.size(); ++index) {
            const GpuEvaluatorStats current = evaluators_[index]->stats();
            result.batches += current.batches;
            result.evaluations += current.evaluations;
            result.intersectionTasks += current.intersectionTasks;
            result.wallSeconds += current.wallSeconds;
            if (!current.error.isEmpty()) {
                errors.push_back(
                    QStringLiteral("%1: %2")
                        .arg(current.backend, current.error));
            }
            if (index == active_) {
                result.backend = current.backend;
                result.adapter = current.adapter;
            }
        }
        if (result.backend.isEmpty()) {
            result.backend = QStringLiteral("CPU");
        }
        result.error = errors.join(QStringLiteral("; "));

        return result;
    }

private:
    void selectAvailable(size_t first) {
        active_ = first;
        while (active_ < evaluators_.size()
               && !evaluators_[active_]->available()) {
            ++active_;
        }
        subjectsSet_ = false;
    }

    std::vector<std::unique_ptr<GpuAreaEvaluator>> evaluators_;
    Polygons coveredSubject_;
    Polygons legalSubject_;
    size_t active_ = 0;
    bool subjectsSet_ = false;
};

} // namespace

std::unique_ptr<GpuAreaEvaluator> createGpuAreaEvaluator(
    const QVector<ShapeMesh> &catalog) {
    return std::make_unique<FallbackGpuEvaluator>(catalog);
}

} // namespace gui::cover
