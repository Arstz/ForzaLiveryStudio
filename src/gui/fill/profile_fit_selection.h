#pragma once

#include <algorithm>
#include <array>
#include <vector>

namespace gui::profile {

struct ProfileAlternative {
    double area = 0.0;
    double error = 0.0;
    double tangent = 0.0;
    double spill = 0.0;
};

inline std::vector<int> profileAlternativeOrder(const std::vector<ProfileAlternative> &alternatives) {
    std::array<std::vector<int>, 4> rankings;
    std::vector<bool> dominated(alternatives.size(), false);
    std::vector<bool> selected(alternatives.size(), false);
    std::vector<int> result;
    for (int index = 0; index < static_cast<int>(alternatives.size()); ++index) {
        const auto &candidate = alternatives[index];
        for (const auto &other : alternatives) {
            if (other.area >= candidate.area && other.error <= candidate.error
                && other.tangent <= candidate.tangent && other.spill <= candidate.spill
                && (other.area > candidate.area || other.error < candidate.error
                    || other.tangent < candidate.tangent || other.spill < candidate.spill)) {
                dominated[index] = true;
                break;
            }
        }
        for (auto &ranking : rankings) {
            ranking.push_back(index);
        }
    }
    for (int metric = 0; metric < static_cast<int>(rankings.size()); ++metric) {
        std::stable_sort(rankings[metric].begin(), rankings[metric].end(), [&](int first, int second) {
            const auto score = [&](int index) {
                const auto &candidate = alternatives[index];
                if (metric == 0) {
                    return -candidate.area / (1.0 + candidate.error * 2.0 + candidate.tangent * 4.0 + candidate.spill);
                }
                if (metric == 1) {
                    return candidate.error;
                }
                if (metric == 2) {
                    return candidate.tangent;
                }

                return candidate.spill;
            };

            return dominated[first] != dominated[second] ? !dominated[first] : score(first) < score(second);
        });
    }
    std::array<size_t, 4> next{};
    for (bool front : {true, false}) {
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t metric = 0; metric < rankings.size(); ++metric) {
                auto &position = next[metric];
                const auto &ranking = rankings[metric];
                while (position < ranking.size() && selected[ranking[position]]) {
                    ++position;
                }
                if (position < ranking.size() && dominated[ranking[position]] != front) {
                    const int index = ranking[position++];
                    selected[index] = true;
                    result.push_back(index);
                    changed = true;
                }
            }
        }
    }

    return result;
}

} // namespace gui::profile
