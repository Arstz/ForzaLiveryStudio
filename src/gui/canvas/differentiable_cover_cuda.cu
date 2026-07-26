#include "differentiable_cover_cuda_kernel.h"

#include <cmath>

namespace gui::cover::cuda {
namespace {

constexpr int kCudaThreads = 128;

__device__ Jet zeroJet() {
    Jet result{};

    return result;
}

__device__ Jet constantJet(double value) {
    Jet result{};
    result.value = value;

    return result;
}

__device__ Jet addJet(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value + right.value;
    #pragma unroll
    for (int parameter = 0; parameter < kGradientCount; ++parameter) {
        result.gradient[parameter] =
            left.gradient[parameter] + right.gradient[parameter];
    }

    return result;
}

__device__ Jet subtractJet(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value - right.value;
    #pragma unroll
    for (int parameter = 0; parameter < kGradientCount; ++parameter) {
        result.gradient[parameter] =
            left.gradient[parameter] - right.gradient[parameter];
    }

    return result;
}

__device__ Jet multiplyJet(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value * right.value;
    #pragma unroll
    for (int parameter = 0; parameter < kGradientCount; ++parameter) {
        result.gradient[parameter] =
            left.gradient[parameter] * right.value
            + left.value * right.gradient[parameter];
    }

    return result;
}

__device__ Jet divideJet(const Jet &left, const Jet &right) {
    Jet result;
    const double denominator = right.value * right.value;
    result.value = left.value / right.value;
    #pragma unroll
    for (int parameter = 0; parameter < kGradientCount; ++parameter) {
        result.gradient[parameter] =
            (left.gradient[parameter] * right.value
             - left.value * right.gradient[parameter])
            / denominator;
    }

    return result;
}

__device__ Jet scaleJet(const Jet &value, double scale) {
    Jet result;
    result.value = value.value * scale;
    #pragma unroll
    for (int parameter = 0; parameter < kGradientCount; ++parameter) {
        result.gradient[parameter] =
            value.gradient[parameter] * scale;
    }

    return result;
}

__device__ JetPoint constantPoint(const Point &point) {
    JetPoint result;
    result.x = constantJet(point.x);
    result.y = constantJet(point.y);

    return result;
}

__device__ JetPoint transformedPoint(
    const Point &point,
    const Transform &transform) {
    JetPoint result{};
    result.x.value =
        transform.a * point.x + transform.c * point.y + transform.e;
    result.y.value =
        transform.b * point.x + transform.d * point.y + transform.f;
    result.x.gradient[0] = point.x;
    result.y.gradient[1] = point.x;
    result.x.gradient[2] = point.y;
    result.y.gradient[3] = point.y;
    result.x.gradient[4] = 1.0;
    result.y.gradient[5] = 1.0;

    return result;
}

__device__ JetPoint addPoint(const JetPoint &left,
                             const JetPoint &right) {
    return {
        addJet(left.x, right.x),
        addJet(left.y, right.y),
    };
}

__device__ JetPoint subtractPoint(const JetPoint &left,
                                  const JetPoint &right) {
    return {
        subtractJet(left.x, right.x),
        subtractJet(left.y, right.y),
    };
}

__device__ JetPoint scalePoint(const JetPoint &point,
                               const Jet &scale) {
    return {
        multiplyJet(point.x, scale),
        multiplyJet(point.y, scale),
    };
}

__device__ Jet crossPoint(const JetPoint &left,
                          const JetPoint &right) {
    return subtractJet(
        multiplyJet(left.x, right.y),
        multiplyJet(left.y, right.x));
}

__device__ Jet sideValue(const JetPoint &point,
                         const JetPoint &windowStart,
                         const JetPoint &windowEnd) {
    return crossPoint(
        subtractPoint(windowEnd, windowStart),
        subtractPoint(point, windowStart));
}

__device__ JetPoint intersectionPoint(
    const JetPoint &start,
    const JetPoint &end,
    const Jet &startSide,
    const Jet &endSide) {
    const Jet denominator = subtractJet(startSide, endSide);
    if (abs(denominator.value) <= 1e-10) {
        return end;
    }
    const Jet fraction = divideJet(startSide, denominator);

    return addPoint(
        start, scalePoint(subtractPoint(end, start), fraction));
}

__device__ std::uint32_t triangleVertex(
    const Triangle &triangle,
    std::uint32_t index) {
    if (index == 0) {
        return triangle.first;
    }
    if (index == 1) {
        return triangle.second;
    }

    return triangle.third;
}

__device__ JetPoint windowPoint(
    const Shape &shape,
    const Transform &transform,
    std::uint32_t primitive,
    std::uint32_t index,
    const Point *shapeVertices,
    const Point *boundaries,
    const Triangle *triangles) {
    if (shape.convex != 0) {
        return transformedPoint(
            boundaries[shape.boundaryStart + index], transform);
    }
    const Triangle triangle =
        triangles[shape.triangleStart + primitive];

    return transformedPoint(
        shapeVertices[
            shape.vertexStart + triangleVertex(triangle, index)],
        transform);
}

__device__ std::uint32_t clipEdge(
    std::uint32_t source,
    std::uint32_t destination,
    std::uint32_t count,
    std::uint32_t capacity,
    const JetPoint &windowStart,
    const JetPoint &windowEnd,
    JetPoint *scratch) {
    if (count == 0) {
        return 0;
    }
    std::uint32_t resultCount = 0;
    JetPoint start = scratch[source + count - 1];
    Jet startSide = sideValue(start, windowStart, windowEnd);
    bool startInside = startSide.value >= -1e-10;
    for (std::uint32_t index = 0; index < count; ++index) {
        const JetPoint end = scratch[source + index];
        const Jet endSide =
            sideValue(end, windowStart, windowEnd);
        const bool endInside = endSide.value >= -1e-10;
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

__global__ void evaluateTasks(
    const Point *points,
    const Polygon *polygons,
    const Point *shapeVertices,
    const Point *boundaries,
    const Triangle *triangles,
    const Shape *shapes,
    const Transform *transforms,
    const Task *tasks,
    std::uint32_t taskCount,
    std::uint32_t scratchHalf,
    JetPoint *scratch,
    TaskResult *results) {
    const std::uint32_t taskIndex =
        blockIdx.x * blockDim.x + threadIdx.x;
    if (taskIndex >= taskCount) {
        return;
    }

    const Task task = tasks[taskIndex];
    const Polygon polygon = polygons[task.polygon];
    const Transform transform = transforms[task.evaluation];
    const Shape shape = shapes[transform.shape];
    const std::uint32_t edgeCount =
        shape.convex != 0 ? shape.boundaryCount : 3;
    const std::uint32_t firstBuffer = task.scratchOffset;
    const std::uint32_t secondBuffer =
        scratchHalf + task.scratchOffset;
    for (std::uint32_t index = 0; index < polygon.count; ++index) {
        scratch[firstBuffer + index] =
            constantPoint(points[polygon.start + index]);
    }

    bool reverse = false;
    if (shape.convex != 0) {
        double doubledArea = 0.0;
        for (std::uint32_t edge = 0; edge < edgeCount; ++edge) {
            const JetPoint left = windowPoint(
                shape, transform, task.primitive, edge,
                shapeVertices, boundaries, triangles);
            const JetPoint right = windowPoint(
                shape, transform, task.primitive,
                (edge + 1) % edgeCount,
                shapeVertices, boundaries, triangles);
            doubledArea +=
                left.x.value * right.y.value
                - left.y.value * right.x.value;
        }
        reverse = doubledArea < 0.0;
    } else {
        const JetPoint first = windowPoint(
            shape, transform, task.primitive, 0,
            shapeVertices, boundaries, triangles);
        const JetPoint second = windowPoint(
            shape, transform, task.primitive, 1,
            shapeVertices, boundaries, triangles);
        const JetPoint third = windowPoint(
            shape, transform, task.primitive, 2,
            shapeVertices, boundaries, triangles);
        reverse = crossPoint(
            subtractPoint(second, first),
            subtractPoint(third, first)).value < 0.0;
    }

    std::uint32_t count = polygon.count;
    bool sourceIsFirst = true;
    for (std::uint32_t edge = 0;
         edge < edgeCount && count > 0; ++edge) {
        const std::uint32_t startIndex = reverse
            ? (edgeCount - edge) % edgeCount
            : edge;
        const std::uint32_t endIndex = reverse
            ? (edgeCount - edge - 1 + edgeCount) % edgeCount
            : (edge + 1) % edgeCount;
        const JetPoint windowStart = windowPoint(
            shape, transform, task.primitive, startIndex,
            shapeVertices, boundaries, triangles);
        const JetPoint windowEnd = windowPoint(
            shape, transform, task.primitive, endIndex,
            shapeVertices, boundaries, triangles);
        const std::uint32_t source =
            sourceIsFirst ? firstBuffer : secondBuffer;
        const std::uint32_t destination =
            sourceIsFirst ? secondBuffer : firstBuffer;
        count = clipEdge(
            source, destination, count, task.scratchCapacity,
            windowStart, windowEnd, scratch);
        sourceIsFirst = !sourceIsFirst;
    }

    Jet area = zeroJet();
    if (count >= 3) {
        const std::uint32_t source =
            sourceIsFirst ? firstBuffer : secondBuffer;
        for (std::uint32_t index = 0; index < count; ++index) {
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
    #pragma unroll
    for (int parameter = 0; parameter < kGradientCount; ++parameter) {
        result.gradient[parameter] = area.gradient[parameter];
    }
    result.evaluation = task.evaluation;
    result.legal = task.legal;
    results[taskIndex] = result;
}

} // namespace

cudaError_t launch(
    const Point *points,
    const Polygon *polygons,
    const Point *shapeVertices,
    const Point *boundaries,
    const Triangle *triangles,
    const Shape *shapes,
    const Transform *transforms,
    const Task *tasks,
    std::uint32_t taskCount,
    std::uint32_t scratchHalf,
    JetPoint *scratch,
    TaskResult *results) {
    evaluateTasks<<<
        (taskCount + kCudaThreads - 1) / kCudaThreads,
        kCudaThreads>>>(
            points, polygons, shapeVertices, boundaries,
            triangles, shapes, transforms, tasks,
            taskCount, scratchHalf, scratch, results);

    return cudaGetLastError();
}

} // namespace gui::cover::cuda
