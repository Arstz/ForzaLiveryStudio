#pragma once

#include <QtCore>

#include <algorithm>

namespace gui {

class GeometryBoundsIndex {
public:
    explicit GeometryBoundsIndex(const QVector<QRectF> &bounds) {
        entries_.reserve(bounds.size());
        nodes_.reserve(bounds.size() * 2);
        for (int index = 0; index < bounds.size(); ++index) {
            entries_.push_back({bounds[index], index});
        }
        if (!entries_.isEmpty()) {
            buildNode(0, entries_.size());
        }
    }

    template <typename Predicate>
    bool anyIntersecting(const QRectF &bounds, Predicate predicate) const {
        QVector<int> pending;
        if (!nodes_.isEmpty()) {
            pending.push_back(0);
        }
        while (!pending.isEmpty()) {
            const Node &node = nodes_[pending.takeLast()];
            if (!overlaps(node.bounds, bounds)) {
                continue;
            }
            if (node.left >= 0) {
                pending.push_back(node.right);
                pending.push_back(node.left);
                continue;
            }
            for (int index = node.first; index < node.last; ++index) {
                const Entry &entry = entries_[index];
                if (overlaps(entry.bounds, bounds) && predicate(entry.index)) {
                    return true;
                }
            }
        }

        return false;
    }

private:
    struct Entry {
        QRectF bounds;
        int index = 0;
    };

    struct Node {
        QRectF bounds;
        int first = 0;
        int last = 0;
        int left = -1;
        int right = -1;
    };

    static constexpr int kLeafSize = 8;

    static bool overlaps(const QRectF &left, const QRectF &right) {
        return left.left() <= right.right() && left.right() >= right.left()
            && left.top() <= right.bottom() && left.bottom() >= right.top();
    }

    int buildNode(int first, int last) {
        QRectF bounds = entries_[first].bounds;
        const int nodeIndex = nodes_.size();
        for (int index = first + 1; index < last; ++index) {
            const QRectF &entry = entries_[index].bounds;
            bounds.setCoords(std::min(bounds.left(), entry.left()),
                             std::min(bounds.top(), entry.top()),
                             std::max(bounds.right(), entry.right()),
                             std::max(bounds.bottom(), entry.bottom()));
        }
        nodes_.push_back({bounds, first, last});
        if (last - first > kLeafSize) {
            const int middle = first + (last - first) / 2;
            const bool horizontal = bounds.width() >= bounds.height();
            std::nth_element(entries_.begin() + first, entries_.begin() + middle,
                             entries_.begin() + last, [horizontal](const Entry &left,
                                                                   const Entry &right) {
                return horizontal ? left.bounds.center().x() < right.bounds.center().x()
                                  : left.bounds.center().y() < right.bounds.center().y();
            });
            const int left = buildNode(first, middle);
            const int right = buildNode(middle, last);
            nodes_[nodeIndex].left = left;
            nodes_[nodeIndex].right = right;
        }

        return nodeIndex;
    }

    QVector<Entry> entries_;
    QVector<Node> nodes_;
};

} // namespace gui
