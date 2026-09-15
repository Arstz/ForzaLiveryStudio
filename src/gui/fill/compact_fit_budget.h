#pragma once

#include <array>
#include <cstdint>

namespace gui::compact {

enum class WorkStage { Recognition, Repair, SpatialReduction, ExposedReduction, Polish };
inline constexpr std::array<int, 5> kStagePercentages{5, 55, 80, 90, 100};

inline int stageLimit(int total, WorkStage stage) {
    return static_cast<int>(static_cast<std::int64_t>(total) * kStagePercentages[static_cast<int>(stage)] / 100);
}

} // namespace gui::compact
