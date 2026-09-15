#include "profile_fit_batch.h"
#include "profile_fit_numeric.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

using namespace gui::profile;

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FailingGpu final : public compute::GpuFitter {
public:
    bool evaluate(const std::vector<compute::Job> &, std::vector<compute::Fit> *results) override {
        results->resize(1);
        results->front().valid = 1;
        results->front().error = -42;
        return false;
    }
    compute::GpuStats stats() const override {
        compute::GpuStats result;
        result.error = "injected batch failure";
        return result;
    }
};

class InvalidGpu final : public compute::GpuFitter {
public:
    bool evaluate(const std::vector<compute::Job> &jobs, std::vector<compute::Fit> *results) override {
        results->resize(jobs.size());
        results->front().valid = 1;
        results->front().error = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    compute::GpuStats stats() const override { return {}; }
};

void same(const std::vector<compute::Fit> &first, const std::vector<compute::Fit> &second, double tolerance) {
    require(first.size() == second.size(), "Batch size differs");
    for (size_t index = 0; index < first.size(); ++index) {
        require(first[index].valid == second[index].valid && first[index].fitted == second[index].fitted,
            "Fit validity differs");
        require(std::abs(first[index].error - second[index].error) <= tolerance, "Fit residual differs");
        for (int parameter = 0; parameter < compute::kParameters; ++parameter) {
            require(std::abs(first[index].transform.values[parameter] - second[index].transform.values[parameter])
                <= tolerance * std::max(1.0, std::abs(first[index].transform.values[parameter])), "Fit transform differs");
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        std::vector<compute::Arc> sources(3);
        std::vector<compute::Arc> targets;
        std::vector<compute::Job> jobs;
        for (int shape = 0; shape < sources.size(); ++shape) {
            for (int index = 0; index < compute::kSamples; ++index) {
                const double angle = -1.1 + index * 2.2 / (compute::kSamples - 1);
                sources[shape].points[index] = {std::sin(angle), std::cos(angle) + shape * std::cos(2 * angle) * 0.1};
            }
            for (int variant = 0; variant < 4; ++variant) {
                compute::Transform transform{{variant % 2 ? -71.0 : 123.0, 9, 11, 67, variant * 193.0, -59}};
                compute::Arc target;
                for (int index = 0; index < compute::kSamples; ++index) {
                    target.points[index] = compute::map(transform, sources[shape].points[index]);
                }
                targets.push_back(target);
                transform.values[4] += 0.02;
                transform.values[5] -= 0.03;
                jobs.push_back({transform, shape, static_cast<int>(targets.size()) - 1, 2});
            }
        }
        const auto base = jobs;
        while (jobs.size() <= compute::kBatchSize * 2) {
            jobs.insert(jobs.end(), base.begin(), base.end());
        }
        std::vector<compute::Fit> expected, actual, repeated;
        ProfileFitter cpu(sources, targets, false);
        require(cpu.evaluate(jobs, &expected), "CPU fitting failed");
        for (const auto &fit : expected) {
            require(fit.valid && fit.error < 1e-6 && fit.tangentError < 1e-6, "Known affine arc was not recovered");
        }
        require(cpu.evaluate(jobs, &repeated), "CPU repeat failed");
        same(expected, repeated, 0);
        ProfileFitter automatic(sources, targets);
        require(automatic.evaluate(jobs, &actual), "Automatic fitting failed");
        same(expected, actual, 1e-7);
        require(automatic.evaluate(jobs, &repeated), "Automatic repeat failed");
        same(actual, repeated, 0);
        ProfileFitter failure(sources, targets, false, std::make_unique<FailingGpu>());
        require(failure.evaluate(jobs, &actual), "GPU failure did not fall back");
        same(expected, actual, 0);
        require(failure.diagnostics().value("fallbacks").toInt() == 1, "Failed GPU retried repeatedly");
        ProfileFitter corrupt(sources, targets, false, std::make_unique<InvalidGpu>());
        require(corrupt.evaluate(jobs, &actual), "Invalid GPU output did not fall back");
        same(expected, actual, 0);
        require(!automatic.evaluate(jobs, &actual, [] { return true; }) && actual.empty(), "Cancellation leaked partial output");
        require(cpu.evaluate({}, &actual) && actual.empty(), "Empty batch failed");
        auto invalid = base.front();
        invalid.source = -1;
        bool rejected = false;
        try { cpu.evaluate({invalid}, &actual); } catch (const std::exception &) { rejected = true; }
        require(rejected, "Invalid arc index accepted");
        auto nonfinite = base.front();
        nonfinite.initial.values[0] = std::numeric_limits<double>::quiet_NaN();
        require(cpu.evaluate({nonfinite}, &actual) && !actual.front().valid, "Nonfinite transform accepted");
        auto distant = base.front();
        distant.initial.values[4] += 100;
        require(cpu.evaluate({distant}, &actual) && !actual.front().fitted, "Coarse rejection failed");
        ProfileWorkers workers;
        rejected = false;
        try { workers.run(100, [](int) { throw std::runtime_error("injected worker failure"); }, {}); }
        catch (const std::exception &) { rejected = true; }
        require(rejected, "Worker exception was lost");
        int cancellationCalls = 0;
        require(!cpu.evaluate(jobs, &actual, [&] { return ++cancellationCalls > 2; }) && actual.empty(),
            "In-flight cancellation leaked partial output");
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"cpu", cpu.diagnostics()}, {"automatic", automatic.diagnostics()},
            {"failure", failure.diagnostics()}}).toJson(QJsonDocument::Compact) << '\n';
        QTextStream(stdout) << "Profile numeric, repeatability, CPU/GPU parity, fallback and cancellation checks passed\n";
        return 0;
    } catch (const std::exception &exception) {
        QTextStream(stderr) << exception.what() << '\n';
        return 1;
    }
}
