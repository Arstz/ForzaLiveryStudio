#pragma once

#include "profile_fit_compute.h"
#include <cmath>
#include <limits>

#ifdef __CUDACC__
#define FLS_PROFILE_NUMERIC __host__ __device__ inline
#else
#define FLS_PROFILE_NUMERIC inline
#endif

namespace gui::profile::compute {

inline constexpr int kIterations = 4;
inline constexpr int kParameters = 6;
inline constexpr double kMinimumLength = 1e-8;
inline constexpr double kMinimumPivot = 1e-12;
inline constexpr double kEndpointWeight = 6.0;
inline constexpr double kEndpointTangentWeight = 0.1;
inline constexpr double kInfinity = std::numeric_limits<double>::infinity();

FLS_PROFILE_NUMERIC Point difference(Point first, Point second) {
    return {first.x - second.x, first.y - second.y};
}

FLS_PROFILE_NUMERIC double dot(Point first, Point second) {
    return first.x * second.x + first.y * second.y;
}

FLS_PROFILE_NUMERIC Point unit(Point point) {
    const double length = hypot(point.x, point.y);

    return length > kMinimumLength ? Point{point.x / length, point.y / length} : Point{};
}

FLS_PROFILE_NUMERIC Point map(const Transform &transform, Point point) {
    return {transform.values[0] * point.x + transform.values[2] * point.y + transform.values[4],
        transform.values[1] * point.x + transform.values[3] * point.y + transform.values[5]};
}

FLS_PROFILE_NUMERIC Point nearest(const Arc &arc, Point point, Point *tangent) {
    Point result{};
    double best = kInfinity;
    for (int index = 0; index + 1 < kSamples; ++index) {
        const auto delta = difference(arc.points[index + 1], arc.points[index]);
        const double squaredLength = dot(delta, delta);
        const double fraction = squaredLength > kMinimumLength
            ? fmax(0.0, fmin(1.0, dot(difference(point, arc.points[index]), delta) / squaredLength)) : 0.0;
        const Point closest{arc.points[index].x + delta.x * fraction, arc.points[index].y + delta.y * fraction};
        const auto displacement = difference(closest, point);
        const double squaredDistance = dot(displacement, displacement);
        if (squaredDistance < best) {
            best = squaredDistance;
            result = closest;
            *tangent = unit(delta);
        }
    }

    return result;
}

FLS_PROFILE_NUMERIC double arcError(const Arc &source, const Arc &target, const Transform &transform) {
    Arc mapped;
    Point tangent{};
    double error = 0.0;
    for (int index = 0; index < kSamples; ++index) {
        mapped.points[index] = map(transform, source.points[index]);
    }
    for (int index = 0; index < kSamples; ++index) {
        const auto first = difference(target.points[index], nearest(mapped, target.points[index], &tangent));
        const auto second = difference(mapped.points[index], nearest(target, mapped.points[index], &tangent));
        error = fmax(error, fmax(hypot(first.x, first.y), hypot(second.x, second.y)));
    }

    return error;
}

FLS_PROFILE_NUMERIC void addRow(double matrix[kParameters][kParameters + 1],
                                const double row[kParameters], double value, double weight) {
    for (int first = 0; first < kParameters; ++first) {
        for (int second = 0; second < kParameters; ++second) {
            matrix[first][second] += weight * row[first] * row[second];
        }
        matrix[first][kParameters] += weight * row[first] * value;
    }
}

FLS_PROFILE_NUMERIC bool solve(double matrix[kParameters][kParameters + 1], Transform *transform) {
    for (int column = 0; column < kParameters; ++column) {
        int pivot = column;
        for (int row = column + 1; row < kParameters; ++row) {
            if (fabs(matrix[row][column]) > fabs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (fabs(matrix[pivot][column]) < kMinimumPivot) {
            return false;
        }
        for (int field = 0; field <= kParameters; ++field) {
            const double temporary = matrix[pivot][field];
            matrix[pivot][field] = matrix[column][field];
            matrix[column][field] = temporary;
        }
        const double divisor = matrix[column][column];
        for (int field = column; field <= kParameters; ++field) {
            matrix[column][field] /= divisor;
        }
        for (int row = 0; row < kParameters; ++row) {
            if (row != column) {
                const double multiplier = matrix[row][column];
                for (int field = column; field <= kParameters; ++field) {
                    matrix[row][field] -= multiplier * matrix[column][field];
                }
            }
        }
    }
    for (int index = 0; index < kParameters; ++index) {
        transform->values[index] = matrix[index][kParameters];
    }

    return true;
}

FLS_PROFILE_NUMERIC bool finite(const Transform &transform) {
    for (double value : transform.values) {
        if (!(fabs(value) < kInfinity)) {
            return false;
        }
    }

    return true;
}

FLS_PROFILE_NUMERIC Fit fit(const Arc &source, const Arc &target, const Job &job) {
    Fit result;
    result.transform = job.initial;
    if (!finite(job.initial) || arcError(source, target, job.initial) > job.initialErrorLimit) {
        return result;
    }
    result.fitted = 1;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        double matrix[kParameters][kParameters + 1]{};
        for (const auto point : source.points) {
            Point tangent{};
            const auto closest = nearest(target, map(result.transform, point), &tangent);
            const Point normal{tangent.y, -tangent.x};
            const double row[kParameters]{normal.x * point.x, normal.y * point.x,
                normal.x * point.y, normal.y * point.y, normal.x, normal.y};
            addRow(matrix, row, dot(normal, closest), 1.0);
        }
        for (int endpoint = 0; endpoint < kSamples; endpoint += kSamples - 1) {
            const auto point = source.points[endpoint];
            const auto targetPoint = target.points[endpoint];
            const int neighbor = endpoint == 0 ? 1 : kSamples - 2;
            const auto direction = unit(difference(source.points[neighbor], point));
            const auto tangent = unit(difference(target.points[neighbor], targetPoint));
            const Point normal{tangent.y, -tangent.x};
            const double horizontal[kParameters]{point.x, 0, point.y, 0, 1, 0};
            const double vertical[kParameters]{0, point.x, 0, point.y, 0, 1};
            const double tangentRow[kParameters]{normal.x * direction.x, normal.y * direction.x,
                normal.x * direction.y, normal.y * direction.y, 0, 0};
            addRow(matrix, horizontal, targetPoint.x, kEndpointWeight);
            addRow(matrix, vertical, targetPoint.y, kEndpointWeight);
            addRow(matrix, tangentRow, 0, kEndpointTangentWeight);
        }
        auto next = result.transform;
        if (!solve(matrix, &next) || !finite(next)) {
            break;
        }
        result.transform = next;
    }
    result.error = arcError(source, target, result.transform);
    for (int endpoint = 0; endpoint < kSamples; endpoint += kSamples - 1) {
        const int neighbor = endpoint == 0 ? 1 : kSamples - 2;
        const auto direction = unit(difference(map(result.transform, source.points[neighbor]),
            map(result.transform, source.points[endpoint])));
        const auto tangent = unit(difference(target.points[neighbor], target.points[endpoint]));
        result.tangentError = fmax(result.tangentError, fabs(direction.x * tangent.y - direction.y * tangent.x));
    }
    result.valid = finite(result.transform) && result.error < kInfinity && result.tangentError < kInfinity;

    return result;
}

} // namespace gui::profile::compute

#undef FLS_PROFILE_NUMERIC
