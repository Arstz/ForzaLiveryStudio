#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace gui::compact {

enum class WorkStage { Recognition, Repair, SpatialReduction, ExposedReduction, Polish };
inline constexpr std::array<int, 5> kStagePercentages{5, 55, 80, 90, 100};

inline int stageLimit(int total, WorkStage stage) {
    return static_cast<int>(static_cast<std::int64_t>(total) * kStagePercentages[static_cast<int>(stage)] / 100);
}

struct SearchBudget {
    int initialEvaluations;
    int maximumEvaluations;
    int milliseconds;
    int complexity;
};

inline constexpr int kInitialSearchWork = 3000;
inline constexpr int kMinimumSearchCeiling = 12000;
inline constexpr int kInitialComplexityWork = 250;
inline constexpr int kMaximumInitialWork = 12000;
inline constexpr int kComplexityWork = 1500;
inline constexpr int kMinimumSearchMilliseconds = 3000;
inline constexpr int kMaximumSearchMilliseconds = 20000;
inline constexpr int kStagnantSearchRounds = 2;
inline constexpr int kAnchorsPerComplexity = 8;
inline constexpr int kHoleComplexity = 4;
inline constexpr int kComponentComplexity = 4;
inline constexpr int kComplexityMilliseconds = 500;

inline SearchBudget searchBudget(int vertices, int holes, int components, int seedCount, int ceiling) {
    const int complexity = std::max(0, vertices - 4) / kAnchorsPerComplexity + holes * kHoleComplexity
        + std::max(0, components - 1) * kComponentComplexity
        + static_cast<int>(std::log2(std::max(1, seedCount)));
    const int maximum = static_cast<int>(std::min<std::int64_t>(ceiling,
        kMinimumSearchCeiling + static_cast<std::int64_t>(complexity) * kComplexityWork));

    const int initial = static_cast<int>(std::min<std::int64_t>(kMaximumInitialWork,
        kInitialSearchWork + static_cast<std::int64_t>(complexity) * kInitialComplexityWork));

    return {std::min(maximum, initial), maximum,
        static_cast<int>(std::min<std::int64_t>(kMaximumSearchMilliseconds,
            kMinimumSearchMilliseconds + static_cast<std::int64_t>(complexity) * kComplexityMilliseconds)), complexity};
}

} // namespace gui::compact
