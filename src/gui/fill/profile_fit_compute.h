#pragma once

#include <memory>
#include <string>
#include <vector>

namespace gui::profile::compute {

inline constexpr int kSamples = 17;
inline constexpr int kBatchSize = 4096;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Arc {
    Point points[kSamples]{};
};

struct Transform {
    double values[6]{1, 0, 0, 1, 0, 0};
};

struct Job {
    Transform initial;
    int source = 0;
    int target = 0;
    double initialErrorLimit = 0.0;
};

struct Fit {
    Transform transform;
    int fitted = 0;
    int valid = 0;
    double error = 0.0;
    double tangentError = 0.0;
};

struct GpuStats {
    std::string adapter;
    std::string error;
    int batches = 0;
    double setupMilliseconds = 0.0;
    double transferMilliseconds = 0.0;
    double kernelMilliseconds = 0.0;
};

class GpuFitter {
public:
    virtual ~GpuFitter() = default;
    virtual bool evaluate(const std::vector<Job> &jobs, std::vector<Fit> *results) = 0;
    virtual GpuStats stats() const = 0;
};

#ifdef FLS_HAS_CUDA
std::unique_ptr<GpuFitter> createGpuFitter(const std::vector<Arc> &sources, const std::vector<Arc> &targets);
#endif

} // namespace gui::profile::compute
