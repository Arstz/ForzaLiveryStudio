#pragma once

#include <QVector>
#include <queue>
#include <vector>

namespace gui::profile {

template <typename Score, typename Accept, typename Cancelled>
QVector<int> greedyCover(int candidateCount, int limit, Score score, Accept accept, Cancelled cancelled) {
    struct Entry {
        int index = 0;
        int round = 0;
        double score = 0.0;
    };
    const auto lowerPriority = [](const Entry &first, const Entry &second) {
        return first.score == second.score ? first.index > second.index : first.score < second.score;
    };
    // Growing coverage makes earlier marginal scores valid upper bounds.
    std::priority_queue<Entry, std::vector<Entry>, decltype(lowerPriority)> pending(lowerPriority);
    QVector<int> result;
    for (int index = 0; index < candidateCount && !cancelled(); ++index) {
        pending.push({index, 0, score(index)});
    }
    while (!pending.empty() && result.size() < limit && !cancelled()) {
        auto next = pending.top();
        pending.pop();
        if (next.score <= 0.0) {
            break;
        }
        if (next.round != result.size()) {
            next.score = score(next.index);
            next.round = result.size();
            pending.push(next);
            continue;
        }
        result.push_back(next.index);
        accept(next.index);
    }

    return result;
}

} // namespace gui::profile
