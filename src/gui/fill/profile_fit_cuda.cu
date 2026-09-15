#include "profile_fit_numeric.h"
#include <cuda_runtime.h>
#include <chrono>

namespace gui::profile::compute {
namespace {

constexpr int kThreads = 64;

__global__ void fitKernel(const Arc *sources, const Arc *targets, const Job *jobs, int count, Fit *results) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < count) {
        const auto job = jobs[index];
        results[index] = fit(sources[job.source], targets[job.target], job);
    }
}

double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

template <typename Value>
class DeviceBuffer {
public:
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;

    ~DeviceBuffer() {
        cudaFree(data_);
    }
    cudaError_t allocate(size_t count) {
        return cudaMalloc(reinterpret_cast<void **>(&data_), count * sizeof(Value));
    }
    Value *data() const {
        return data_;
    }

private:
    Value *data_ = nullptr;
};

class CudaFitter final : public GpuFitter {
public:
    CudaFitter(const std::vector<Arc> &sources, const std::vector<Arc> &targets) {
        cudaDeviceProp properties{};
        int device = 0;
        const auto start = std::chrono::steady_clock::now();
        if (!check(cudaGetDevice(&device)) || !check(cudaGetDeviceProperties(&properties, device))) {
            return;
        }
        stats_.adapter = properties.name;
        if (!check(sources_.allocate(sources.size())) || !check(targets_.allocate(targets.size()))
            || !check(jobs_.allocate(kBatchSize)) || !check(results_.allocate(kBatchSize))
            || !check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking))) {
            return;
        }
        stats_.setupMilliseconds = elapsed(start);
        const auto transferStart = std::chrono::steady_clock::now();
        if (!check(cudaMemcpy(sources_.data(), sources.data(), sources.size() * sizeof(Arc), cudaMemcpyHostToDevice))
            || !check(cudaMemcpy(targets_.data(), targets.data(), targets.size() * sizeof(Arc), cudaMemcpyHostToDevice))) {
            return;
        }
        stats_.transferMilliseconds += elapsed(transferStart);
    }

    ~CudaFitter() override {
        if (stream_) {
            cudaStreamDestroy(stream_);
        }
    }

    bool evaluate(const std::vector<Job> &jobs, std::vector<Fit> *results) override {
        auto start = std::chrono::steady_clock::now();
        results->clear();
        if (!stats_.error.empty() || jobs.empty() || jobs.size() > kBatchSize) {
            return false;
        }
        if (!check(cudaMemcpyAsync(jobs_.data(), jobs.data(), jobs.size() * sizeof(Job), cudaMemcpyHostToDevice, stream_))
            || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        stats_.transferMilliseconds += elapsed(start);
        start = std::chrono::steady_clock::now();
        fitKernel<<<(static_cast<int>(jobs.size()) + kThreads - 1) / kThreads, kThreads, 0, stream_>>>(
            sources_.data(), targets_.data(), jobs_.data(), static_cast<int>(jobs.size()), results_.data());
        if (!check(cudaGetLastError()) || !check(cudaStreamSynchronize(stream_))) {
            return false;
        }
        stats_.kernelMilliseconds += elapsed(start);
        start = std::chrono::steady_clock::now();
        results->resize(jobs.size());
        if (!check(cudaMemcpyAsync(results->data(), results_.data(), jobs.size() * sizeof(Fit), cudaMemcpyDeviceToHost, stream_))
            || !check(cudaStreamSynchronize(stream_))) {
            results->clear();
            return false;
        }
        stats_.transferMilliseconds += elapsed(start);
        ++stats_.batches;

        return true;
    }

    GpuStats stats() const override {
        return stats_;
    }

private:
    bool check(cudaError_t status) {
        if (status != cudaSuccess) {
            stats_.error = cudaGetErrorString(status);
            return false;
        }

        return true;
    }

    DeviceBuffer<Arc> sources_;
    DeviceBuffer<Arc> targets_;
    DeviceBuffer<Job> jobs_;
    DeviceBuffer<Fit> results_;
    GpuStats stats_;
    cudaStream_t stream_ = nullptr;
};

} // namespace

std::unique_ptr<GpuFitter> createGpuFitter(const std::vector<Arc> &sources, const std::vector<Arc> &targets) {
    return std::make_unique<CudaFitter>(sources, targets);
}

} // namespace gui::profile::compute
