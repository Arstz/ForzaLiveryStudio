#pragma once

#include "profile_fit_compute.h"
#include <QJsonObject>
#include <QThreadPool>
#include <functional>

namespace gui::profile {

class ProfileWorkers {
public:
    ProfileWorkers();
    bool run(int count, const std::function<void(int)> &work, const std::function<bool()> &cancelled);
    int count() const;

private:
    QThreadPool pool_;
};

class ProfileFitter {
public:
    ProfileFitter(std::vector<compute::Arc> sources, std::vector<compute::Arc> targets,
                   bool useGpu = true, std::unique_ptr<compute::GpuFitter> gpu = {});
    bool evaluate(const std::vector<compute::Job> &jobs, std::vector<compute::Fit> *results,
                   const std::function<bool()> &cancelled = {});
    QJsonObject diagnostics() const;

private:
    std::vector<compute::Arc> sources_;
    std::vector<compute::Arc> targets_;
    std::unique_ptr<compute::GpuFitter> gpu_;
    ProfileWorkers workers_;
    QString fallbackError_;
    int cpuBatches_ = 0;
    int gpuBatches_ = 0;
    int fallbacks_ = 0;
    bool gpuFailed_ = false;
    double milliseconds_ = 0.0;
};

} // namespace gui::profile
