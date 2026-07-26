#pragma once

#include "differentiable_cover.h"

#include <memory>

namespace gui::cover {

struct GpuEvaluationRequest {
    const ShapeMesh *shape = nullptr;
    Affine transform;
};

struct GpuEvaluatorStats {
    QString adapter;
    QString error;
    std::uint64_t batches = 0;
    std::uint64_t evaluations = 0;
    std::uint64_t intersectionTasks = 0;
    double wallSeconds = 0.0;
};

class GpuAreaEvaluator {
public:
    virtual ~GpuAreaEvaluator() = default;

    virtual bool setSubjects(const Polygons &coveredSubject,
                             const Polygons &legalSubject) = 0;
    virtual bool evaluate(const QVector<GpuEvaluationRequest> &requests,
                          QVector<AreaGradient> *results) = 0;
    virtual bool available() const = 0;
    virtual GpuEvaluatorStats stats() const = 0;
};

std::unique_ptr<GpuAreaEvaluator> createGpuAreaEvaluator(
    const QVector<ShapeMesh> &catalog);

} // namespace gui::cover
