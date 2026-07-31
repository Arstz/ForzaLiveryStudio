#pragma once

#include <cuda_runtime_api.h>

#include <cstdint>

namespace gui::cover::cuda {

constexpr int kGradientCount = 6;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Polygon {
    std::uint32_t start = 0;
    std::uint32_t count = 0;
    double minimumX = 0.0;
    double minimumY = 0.0;
    double maximumX = 0.0;
    double maximumY = 0.0;
};

struct Triangle {
    std::uint32_t first = 0;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
};

struct Shape {
    std::uint32_t vertexStart = 0;
    std::uint32_t triangleStart = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t boundaryStart = 0;
    std::uint32_t boundaryCount = 0;
    std::uint32_t convex = 0;
    double area = 0.0;
};

struct Transform {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
    double e = 0.0;
    double f = 0.0;
    std::uint32_t shape = 0;
};

struct Task {
    std::uint32_t evaluation = 0;
    std::uint32_t polygon = 0;
    std::uint32_t primitive = 0;
    std::uint32_t legal = 0;
    std::uint32_t scratchOffset = 0;
    std::uint32_t scratchCapacity = 0;
};

struct Jet {
    double value = 0.0;
    double gradient[kGradientCount]{};
};

struct JetPoint {
    Jet x;
    Jet y;
};

struct TaskResult {
    double value = 0.0;
    double gradient[kGradientCount]{};
    std::uint32_t evaluation = 0;
    std::uint32_t legal = 0;
};

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
    TaskResult *results);

} // namespace gui::cover::cuda
