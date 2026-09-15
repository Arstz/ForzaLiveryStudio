#include "profile_fit_batch.h"
#include "profile_fit_numeric.h"

#include <QElapsedTimer>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace gui::profile {
namespace {

constexpr int kMaximumWorkers = 8;
constexpr int kWaitMilliseconds = 25;

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

bool validResult(const compute::Fit &fit) {
    if ((fit.valid != 0 && fit.valid != 1) || (fit.fitted != 0 && fit.fitted != 1)) {
        return false;
    }

    return !fit.valid || (compute::finite(fit.transform) && std::isfinite(fit.error) && fit.error >= 0
        && std::isfinite(fit.tangentError) && fit.tangentError >= 0 && fit.tangentError <= 1.0 + 1e-12);
}

} // namespace

ProfileWorkers::ProfileWorkers() {
    pool_.setMaxThreadCount(std::clamp(QThread::idealThreadCount(), 1, kMaximumWorkers));
}

bool ProfileWorkers::run(int count, const std::function<void(int)> &work,
                         const std::function<bool()> &cancelled) {
    std::atomic<int> next{0};
    std::atomic<bool> abort{false};
    std::exception_ptr failure;
    std::mutex failureMutex;
    if (stopped(cancelled)) {
        return false;
    }
    try {
        for (int worker = 0; worker < std::min(count, pool_.maxThreadCount()); ++worker) {
            pool_.start([&] {
                try {
                    while (!abort.load(std::memory_order_relaxed)) {
                        const int index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= count) {
                            break;
                        }
                        work(index);
                    }
                } catch (...) {
                    std::lock_guard guard(failureMutex);
                    failure = std::current_exception();
                    abort.store(true, std::memory_order_relaxed);
                }
            });
        }
        while (!pool_.waitForDone(kWaitMilliseconds)) {
            if (stopped(cancelled)) {
                abort.store(true, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        abort.store(true, std::memory_order_relaxed);
        pool_.waitForDone();
        throw;
    }
    if (failure) {
        std::rethrow_exception(failure);
    }

    return !abort.load(std::memory_order_relaxed) && !stopped(cancelled);
}

int ProfileWorkers::count() const {
    return pool_.maxThreadCount();
}

ProfileFitter::ProfileFitter(std::vector<compute::Arc> sources, std::vector<compute::Arc> targets,
                             bool useGpu, std::unique_ptr<compute::GpuFitter> gpu)
    : sources_(std::move(sources)), targets_(std::move(targets)), gpu_(std::move(gpu)) {
#ifdef FLS_HAS_CUDA
    if (useGpu && !gpu_ && !sources_.empty() && !targets_.empty()) {
        gpu_ = compute::createGpuFitter(sources_, targets_);
    }
#else
    Q_UNUSED(useGpu);
#endif
}

bool ProfileFitter::evaluate(const std::vector<compute::Job> &jobs, std::vector<compute::Fit> *results,
                             const std::function<bool()> &cancelled) {
    QElapsedTimer timer;
    timer.start();
    results->clear();
    for (const auto &job : jobs) {
        if (job.source < 0 || job.source >= sources_.size() || job.target < 0 || job.target >= targets_.size()) {
            throw std::runtime_error("Profile fitting job has an invalid arc index");
        }
    }
    results->reserve(jobs.size());
    for (size_t start = 0; start < jobs.size(); start += compute::kBatchSize) {
        const size_t end = std::min(jobs.size(), start + compute::kBatchSize);
        const std::vector<compute::Job> batch(jobs.begin() + start, jobs.begin() + end);
        std::vector<compute::Fit> fitted;
        if (stopped(cancelled)) {
            results->clear();
            return false;
        }
        bool complete = false;
        if (gpu_ && !gpuFailed_) {
            complete = gpu_->evaluate(batch, &fitted) && fitted.size() == batch.size()
                && std::all_of(fitted.cbegin(), fitted.cend(), validResult);
            if (complete) {
                ++gpuBatches_;
            } else {
                gpuFailed_ = true;
                fallbackError_ = QString::fromStdString(gpu_->stats().error);
                if (fallbackError_.isEmpty()) {
                    fallbackError_ = QStringLiteral("Invalid or incomplete CUDA profile batch");
                }
                ++fallbacks_;
            }
        }
        if (!complete) {
            fitted.assign(batch.size(), {});
            if (!workers_.run(static_cast<int>(batch.size()), [&](int index) {
                const auto &job = batch[index];
                fitted[index] = compute::fit(sources_[job.source], targets_[job.target], job);
            }, cancelled)) {
                results->clear();
                return false;
            }
            ++cpuBatches_;
        }
        if (stopped(cancelled)) {
            results->clear();
            return false;
        }
        results->insert(results->end(), fitted.begin(), fitted.end());
    }
    milliseconds_ += timer.nsecsElapsed() / 1e6;

    return true;
}

QJsonObject ProfileFitter::diagnostics() const {
    const auto stats = gpu_ ? gpu_->stats() : compute::GpuStats{};

    return {{QStringLiteral("backend"), gpuBatches_ > 0
            ? (cpuBatches_ > 0 ? QStringLiteral("CUDA + CPU fallback") : QStringLiteral("CUDA")) : QStringLiteral("CPU")},
        {QStringLiteral("adapter"), QString::fromStdString(stats.adapter)},
        {QStringLiteral("error"), fallbackError_.isEmpty() ? QString::fromStdString(stats.error) : fallbackError_},
        {QStringLiteral("cpuWorkers"), workers_.count()},
        {QStringLiteral("cpuBatches"), cpuBatches_}, {QStringLiteral("gpuBatches"), gpuBatches_},
        {QStringLiteral("fallbacks"), fallbacks_}, {QStringLiteral("numericMilliseconds"), milliseconds_},
        {QStringLiteral("setupMilliseconds"), stats.setupMilliseconds},
        {QStringLiteral("transferMilliseconds"), stats.transferMilliseconds},
        {QStringLiteral("kernelMilliseconds"), stats.kernelMilliseconds}};
}

} // namespace gui::profile
