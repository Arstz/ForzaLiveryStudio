#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QtGlobal>

namespace gui {

// Opt-in timing for hot paths. Set the FORZA_PERF_LOG environment variable and run
// from a console to see how long each scope takes (helps locate selection/transform
// stalls). Zero cost when the variable is unset.
class ScopedPerf {
public:
    explicit ScopedPerf(const char *name)
        : name_(name), enabled_(qEnvironmentVariableIsSet("FORZA_PERF_LOG") || qEnvironmentVariableIsSet("FH6_PERF_LOG"))
    {
        if (enabled_) {
            timer_.start();
        }
    }

    ~ScopedPerf()
    {
        if (enabled_) {
            qInfo("[perf] %s: %lld ms", name_, static_cast<long long>(timer_.elapsed()));
        }
    }

private:
    const char *name_;
    bool enabled_;
    QElapsedTimer timer_;
};

} // namespace gui
