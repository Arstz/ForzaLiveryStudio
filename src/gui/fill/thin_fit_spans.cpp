#include "thin_fit_spans.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace gui::thin {
namespace {

constexpr int kMaximumExtent = 4096;
constexpr double kWidthSamples = 12.0;
constexpr int kBorder = 2;
constexpr int kMaximumThinningRounds = 128;
constexpr int kMinimumNeighbors = 2;
constexpr int kMaximumNeighbors = 6;
constexpr double kMinimumSpanWidthFraction = 0.8;
constexpr int kMaximumSpans = 8192;
constexpr double kJunctionDistanceWidthFraction = 0.35;
constexpr double kJunctionDirectionWidthFraction = 0.75;
constexpr double kJunctionMaximumDirectionDot = -0.6;
constexpr double kJunctionDistancePenalty = 0.1;
constexpr std::array<int, 8> kHorizontal{0, 1, 1, 1, 0, -1, -1, -1};
constexpr std::array<int, 8> kVertical{-1, -1, 0, 1, 1, 1, 0, -1};

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

struct MedialGrid {
    QPointF origin;
    QSize size;
    double step = 0.0;
};

MedialGrid gridFor(const catalog::Region &region, double width) {
    MedialGrid result;
    result.step = std::max(width / kWidthSamples,
        std::max(region.bounds.width(), region.bounds.height()) / kMaximumExtent);
    result.origin = region.bounds.topLeft() - QPointF(kBorder * result.step, kBorder * result.step);
    result.size = QSize(std::max(1, int(std::ceil(region.bounds.width() / result.step))) + kBorder * 2,
        std::max(1, int(std::ceil(region.bounds.height() / result.step))) + kBorder * 2);

    return result;
}

QPointF endpointDirection(const QPolygonF &span, bool reverse, double distance) {
    const auto origin = reverse ? span.back() : span.front();
    QPointF previous = origin;
    QPointF direction;
    double remaining = distance;

    for (int offset = 1; offset < span.size(); ++offset) {
        const auto point = span[reverse ? span.size() - 1 - offset : offset];
        const double length = QLineF(previous, point).length();
        direction = point - origin;
        if (length >= remaining) {
            direction = previous + (point - previous) * (remaining / length) - origin;
            break;
        }
        remaining -= length;
        previous = point;
    }
    const double length = QLineF({}, direction).length();

    return length > 0.0 ? direction / length : QPointF();
}

QVector<QPolygonF> joinedSpans(const QVector<QPolygonF> &spans, double width,
                               const std::function<bool()> &cancelled) {
    struct Endpoint {
        QPointF point;
        QPointF direction;
    };
    struct Connection {
        int first = 0;
        int second = 0;
        double cost = 0.0;
    };
    QVector<Endpoint> endpoints;
    QVector<Connection> connections;
    QVector<int> links(spans.size() * 2, -1);
    QVector<bool> visited(spans.size(), false);
    QVector<QPolygonF> result;
    const double maximumDistance = width * kJunctionDistanceWidthFraction;

    for (const auto &span : spans)
        for (bool reverse : {false, true})
            endpoints.push_back({reverse ? span.back() : span.front(),
                endpointDirection(span, reverse, width * kJunctionDirectionWidthFraction)});
    for (int first = 0; first < endpoints.size() && !stopped(cancelled); ++first)
        for (int second = first + 1; second < endpoints.size(); ++second) {
            if (first / 2 == second / 2)
                continue;
            const double distance = QLineF(endpoints[first].point, endpoints[second].point).length();
            if (distance > maximumDistance)
                continue;
            const double direction = QPointF::dotProduct(endpoints[first].direction, endpoints[second].direction);
            if (direction <= kJunctionMaximumDirectionDot)
                connections.push_back({first, second, 1.0 + direction
                    + distance / maximumDistance * kJunctionDistancePenalty});
        }
    std::stable_sort(connections.begin(), connections.end(), [](const auto &first, const auto &second) {
        return first.cost < second.cost;
    });
    for (const auto &connection : connections)
        if (links[connection.first] < 0 && links[connection.second] < 0) {
            links[connection.first] = connection.second;
            links[connection.second] = connection.first;
        }
    for (bool open : {true, false})
        for (int start = 0; start < endpoints.size() && !stopped(cancelled); ++start) {
            if (visited[start / 2] || (open && links[start] >= 0))
                continue;
            QPolygonF points;
            int endpoint = start;
            int count = 0;
            while (endpoint >= 0 && !visited[endpoint / 2]) {
                auto span = spans[endpoint / 2];
                if (endpoint % 2 != 0)
                    std::reverse(span.begin(), span.end());
                if (!points.isEmpty() && points.back() == span.front())
                    span.removeFirst();
                points += span;
                visited[endpoint / 2] = true;
                ++count;
                endpoint = links[endpoint ^ 1];
            }
            if (count > 1)
                result.push_back(points);
        }

    return result;
}

} // namespace

QVector<QPolygonF> medialSpans(const catalog::Region &region, double width,
                              const std::function<bool()> &cancelled) {
    QVector<QPolygonF> result;
    QVector<int> active;
    QVector<int> removed;
    std::vector<unsigned char> mask;
    std::vector<unsigned char> adjacency;
    std::vector<unsigned char> visited;
    std::array<int, 8> offsets;
    const auto grid = gridFor(region, width);
    const auto origin = grid.origin;
    QImage image(grid.size, QImage::Format_ARGB32);
    const int columns = grid.size.width();
    const int rows = grid.size.height();
    const double step = grid.step;

    image.fill(Qt::black);
    {
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.setTransform(QTransform(1.0 / step, 0, 0, 1.0 / step, -origin.x() / step, -origin.y() / step));
        painter.drawPath(catalog::painterPath(region.required));
    }
    mask.resize(columns * rows);
    adjacency.resize(mask.size());
    visited.resize(mask.size());
    for (int direction = 0; direction < 8; ++direction)
        offsets[direction] = kVertical[direction] * columns + kHorizontal[direction];
    for (int row = kBorder; row < rows - kBorder; ++row) {
        const auto pixels = reinterpret_cast<const QRgb *>(image.constScanLine(row));
        for (int column = kBorder; column < columns - kBorder; ++column)
            if (qRed(pixels[column]) != 0) {
                const int index = row * columns + column;
                mask[index] = 1;
                active.push_back(index);
            }
    }
    for (int round = 0; round < kMaximumThinningRounds && !stopped(cancelled); ++round) {
        bool changed = false;
        for (int phase = 0; phase < 2; ++phase) {
            removed.clear();
            for (int index : active) {
                if (!mask[index])
                    continue;
                std::array<int, 8> neighbors;
                int count = 0;
                int transitions = 0;
                for (int direction = 0; direction < 8; ++direction) {
                    neighbors[direction] = mask[index + offsets[direction]];
                    count += neighbors[direction];
                }
                if (count < kMinimumNeighbors || count > kMaximumNeighbors)
                    continue;
                for (int direction = 0; direction < 8; ++direction)
                    transitions += neighbors[direction] == 0 && neighbors[(direction + 1) % 8] != 0;
                if (transitions != 1)
                    continue;
                if (phase == 0) {
                    if (neighbors[0] * neighbors[2] * neighbors[4] != 0
                        || neighbors[2] * neighbors[4] * neighbors[6] != 0)
                        continue;
                } else if (neighbors[0] * neighbors[2] * neighbors[6] != 0
                    || neighbors[0] * neighbors[4] * neighbors[6] != 0) {
                    continue;
                }
                removed.push_back(index);
            }
            for (int index : removed)
                mask[index] = 0;
            changed = changed || !removed.isEmpty();
        }
        if (!changed)
            break;
    }
    for (int index : active) {
        if (!mask[index])
            continue;
        for (int direction = 0; direction < 8; ++direction) {
            if (!mask[index + offsets[direction]])
                continue;
            if (direction % 2 != 0
                && (mask[index + offsets[(direction + 7) % 8]] || mask[index + offsets[(direction + 1) % 8]]))
                continue;
            adjacency[index] |= 1 << direction;
        }
    }
    const auto degree = [&](int index) {
        int count = 0;
        for (int direction = 0; direction < 8; ++direction)
            count += (adjacency[index] >> direction) & 1;

        return count;
    };
    const auto point = [&](int index) {
        return origin + QPointF((index % columns + 0.5) * step, (index / columns + 0.5) * step);
    };
    const auto trace = [&](int start, int direction) {
        QPolygonF points{point(start)};
        int index = start;
        double length = 0.0;
        do {
            const int next = index + offsets[direction];
            visited[index] |= 1 << direction;
            visited[next] |= 1 << ((direction + 4) % 8);
            points.push_back(point(next));
            length += QLineF(points[points.size() - 2], points.back()).length();
            index = next;
            if (index == start || degree(index) != 2)
                break;
            direction = 0;
            while (direction < 8 && !(adjacency[index] & ~visited[index] & (1 << direction)))
                ++direction;
        } while (direction < 8);
        if (length >= width * kMinimumSpanWidthFraction)
            result.push_back(points);
    };
    for (bool endpoints : {true, false})
        for (int index : active) {
            if (stopped(cancelled) || result.size() >= kMaximumSpans)
                return result;
            if (!mask[index] || (endpoints && degree(index) == 2))
                continue;
            for (int direction = 0; direction < 8; ++direction)
                if (adjacency[index] & ~visited[index] & (1 << direction))
                    trace(index, direction);
        }

    return result + joinedSpans(result, width, cancelled);
}

} // namespace gui::thin
