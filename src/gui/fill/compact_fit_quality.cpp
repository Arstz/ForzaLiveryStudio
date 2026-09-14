#include "compact_fit_quality.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace gui::compact {
namespace {

constexpr double kMinimumLength = 1e-8;
constexpr double kTurnSlack = 0.12;
constexpr double kCornerDefectTurn = 0.35;
constexpr double kTargetCornerTurn = 0.6;
constexpr int kMaximumSamples = 2048;
constexpr int kSearchRadius = 3;
constexpr double kCellScale = 8.0;
constexpr double kMaximumGridExtent = 128.0;
constexpr double kSubpixelClosingFraction = 0.15;
constexpr double kPeakTurnWeight = 64.0;
constexpr int kIndexedCornerThreshold = 8;
constexpr double kObservationWindowSlack = 0.1;

QPointF normalized(const QPointF &point) {
    const double length = std::hypot(point.x(), point.y());

    return length > kMinimumLength ? point / length : QPointF();
}

double angle(const QPointF &left, const QPointF &right) {
    return std::atan2(left.x() * right.y() - left.y() * right.x(), QPointF::dotProduct(left, right));
}

catalog::Polygons closeSubpixelGaps(const catalog::Polygons &polygons, double radius) {
    const auto grown = catalog::expanded(polygons, radius);
    const auto bounds = catalog::painterPath(grown).boundingRect().adjusted(-4 * radius, -4 * radius, 4 * radius, 4 * radius);
    QPolygonF frame({bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()});
    const auto outside = catalog::subtract({frame}, grown);

    return catalog::subtract(grown, catalog::expanded(outside, radius));
}

} // namespace

BoundaryModel::BoundaryModel(const catalog::Polygons &target, double observationScale)
    : bounds_(catalog::painterPath(target).boundingRect()),
      scale_(std::max(observationScale, kMinimumLength)),
      cellSize_(std::max(scale_ * kCellScale,
          std::max(bounds_.width(), bounds_.height()) / kMaximumGridExtent)) {
    for (const auto &polygon : target) {
        const auto loop = makeLoop(polygon);
        const int loopIndex = loops_.size();
        loops_.push_back(loop);
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF delta = polygon[(index + 1) % polygon.size()] - polygon[index];
            const QPointF incoming = polygon[index] - polygon[(index + polygon.size() - 1) % polygon.size()];
            if (QLineF({}, incoming).length() > scale_ * 0.5 && QLineF({}, delta).length() > scale_ * 0.5
                && std::abs(angle(normalized(incoming), normalized(delta))) > kTargetCornerTurn) {
                corners_.push_back(polygon[index]);
                cornerCells_[cellKey(static_cast<int>(std::floor((polygon[index].x() - bounds_.left()) / cellSize_)),
                    static_cast<int>(std::floor((polygon[index].y() - bounds_.top()) / cellSize_)))].push_back(polygon[index]);
            }
            const Edge edge{polygon[index], delta, loopIndex, QPointF::dotProduct(delta, delta), loop.lengths[index]};
            const QRectF bounds = QRectF(edge.start, edge.start + edge.delta).normalized();
            const int edgeIndex = edges_.size();
            edges_.push_back(edge);
            const int left = static_cast<int>(std::floor((bounds.left() - bounds_.left()) / cellSize_));
            const int right = static_cast<int>(std::floor((bounds.right() - bounds_.left()) / cellSize_));
            const int top = static_cast<int>(std::floor((bounds.top() - bounds_.top()) / cellSize_));
            const int bottom = static_cast<int>(std::floor((bounds.bottom() - bounds_.top()) / cellSize_));
            for (int row = top; row <= bottom; ++row) {
                for (int column = left; column <= right; ++column) {
                    cells_[cellKey(column, row)].push_back(edgeIndex);
                }
            }
        }
    }
}

BoundaryReference BoundaryModel::reference(const QPointF &point) const {
    const auto location = closest(point);
    BoundaryReference result;
    result.point = location.point;
    result.distance = std::sqrt(location.squaredDistance);
    result.corner = nearCorner(point, scale_ * 2.5);
    if (!loops_.isEmpty()) {
        result.tangent = normalized(pointAt(loops_[location.loop], location.offset + scale_)
            - pointAt(loops_[location.loop], location.offset - scale_));
    }

    return result;
}

catalog::Polygons BoundaryModel::observationSupport(const catalog::Polygons &coverage) const {
    QElapsedTimer timer;
    timer.start();
    auto result = closeSubpixelGaps(coverage, scale_ * kSubpixelClosingFraction);
    closingNanoseconds_ += timer.nsecsElapsed();

    return result;
}

BoundaryModel::ObservationWindow BoundaryModel::observationWindow(const catalog::Polygons &unchanged,
                                                                  const catalog::Polygons &unchangedObserved,
                                                                  const QRectF &addedBounds) const {
    ObservationWindow result;
    QElapsedTimer timer;
    const double radius = scale_ * kSubpixelClosingFraction;
    const double padding = radius * 2.0 + catalog::kVerificationClearance * 4.0;
    const double slack = std::max(scale_ * 2.0, std::max(addedBounds.width(), addedBounds.height()) * kObservationWindowSlack);
    const QRectF additionBounds = addedBounds.adjusted(-slack, -slack, slack, slack);
    const QRectF affected = additionBounds.adjusted(-padding, -padding, padding, padding);
    const QRectF window = affected.adjusted(-padding, -padding, padding, padding);
    const catalog::Polygons affectedPolygon{QPolygonF({affected.topLeft(), affected.topRight(), affected.bottomRight(), affected.bottomLeft()})};
    const catalog::Polygons windowPolygon{QPolygonF({window.topLeft(), window.topRight(), window.bottomRight(), window.bottomLeft()})};

    timer.start();
    result.unchanged = catalog::subtract(unchangedObserved, affectedPolygon);
    result.neighborhood = catalog::intersect(unchanged, windowPolygon);
    result.clip = affectedPolygon;
    result.additionBounds = additionBounds;
    closingNanoseconds_ += timer.nsecsElapsed();

    return result;
}

catalog::Polygons BoundaryModel::observationSupport(const catalog::Polygons &addition, const ObservationWindow &window) const {
    QElapsedTimer timer;
    timer.start();
    const auto local = closeSubpixelGaps(catalog::unite(window.neighborhood + addition), scale_ * kSubpixelClosingFraction);
    auto result = catalog::unite(window.unchanged + catalog::intersect(local, window.clip));
    closingNanoseconds_ += timer.nsecsElapsed();

    return result;
}

BoundaryMetrics BoundaryModel::measure(const catalog::Polygons &coverage) const {
    return measure(coverage, observationSupport(coverage));
}

BoundaryMetrics BoundaryModel::measure(const catalog::Polygons &coverage, const catalog::Polygons &observed) const {
    BoundaryMetrics result;
    QElapsedTimer timer;
    std::unique_ptr<BoundaryModel> outputBoundary;
    timer.start();
    if (loops_.isEmpty()) {
        return result;
    }
    if (corners_.size() > kIndexedCornerThreshold && !coverage.isEmpty()) {
        outputBoundary = std::make_unique<BoundaryModel>(coverage, scale_);
    }
    for (const auto &corner : corners_) {
        double squaredDistance = std::numeric_limits<double>::infinity();
        if (outputBoundary) {
            squaredDistance = outputBoundary->closest(corner).squaredDistance;
        } else {
            for (const auto &polygon : coverage) {
                for (int index = 0; index < polygon.size(); ++index) {
                    const auto delta = polygon[(index + 1) % polygon.size()] - polygon[index];
                    const double squaredLength = QPointF::dotProduct(delta, delta);
                    const double parameter = squaredLength > kMinimumLength
                        ? std::clamp(QPointF::dotProduct(corner - polygon[index], delta) / squaredLength, 0.0, 1.0) : 0.0;
                    const auto offset = polygon[index] + delta * parameter - corner;
                    squaredDistance = std::min(squaredDistance, QPointF::dotProduct(offset, offset));
                }
            }
        }
        result.maximumCornerDistance = std::max(result.maximumCornerDistance, std::sqrt(squaredDistance));
        result.cornerEnergy += squaredDistance;
    }
    cornerNanoseconds_ += timer.nsecsElapsed();
    timer.start();
    for (const auto &polygon : observed) {
        if (catalog::signedArea(polygon) > 0) {
            ++result.components;
        } else {
            ++result.holes;
        }
        const auto loop = makeLoop(polygon);
        const double step = std::max(scale_, loop.perimeter / kMaximumSamples);
        const int samples = std::max(1, static_cast<int>(std::ceil(loop.perimeter / step)));
        const double spacing = loop.perimeter / samples;
        if (loop.perimeter <= kMinimumLength) {
            continue;
        }
        for (int sample = 0; sample < samples; ++sample) {
            const double offset = (sample + 0.5) * spacing;
            const auto point = pointAt(loop, offset);
            const auto reference = closest(point);
            const auto &target = loops_[reference.loop];
            for (double radius : {scale_, scale_ * 2.0}) {
                const auto before = pointAt(loop, offset - radius);
                const auto after = pointAt(loop, offset + radius);
                const auto targetBefore = pointAt(target, reference.offset - radius);
                const auto targetAfter = pointAt(target, reference.offset + radius);
                const double turn = angle(normalized(point - before), normalized(after - point));
                const double targetTurn = angle(normalized(reference.point - targetBefore),
                    normalized(targetAfter - reference.point));
                const double excess = std::max(0.0,
                    std::abs(std::remainder(turn - targetTurn, 2.0 * std::numbers::pi)) - kTurnSlack);
                const double tangentError = angle(normalized(after - before), normalized(targetAfter - targetBefore));
                const bool cornerWindow = nearCorner(point, radius + scale_ * 0.5);
                if (!cornerWindow && std::abs(targetTurn) < kTargetCornerTurn) {
                    result.tangentEnergy += spacing * tangentError * tangentError * 0.5;
                    result.turnEnergy += spacing * excess * excess * 0.5;
                    result.maximumExcessTurn = std::max(result.maximumExcessTurn, excess);
                    result.cornerDefects += excess > kCornerDefectTurn;
                }
            }
            result.maximumDistance = std::max(result.maximumDistance, std::sqrt(reference.squaredDistance));
            ++result.samples;
        }
    }

    samplingNanoseconds_ += timer.nsecsElapsed();
    ++measurements_;

    return result;
}

double BoundaryModel::energy(const BoundaryMetrics &metrics) const {
    const double peakExcess = std::max(0.0, metrics.maximumExcessTurn - kCornerDefectTurn);

    return metrics.tangentEnergy + metrics.turnEnergy * 2.0 + metrics.cornerEnergy * 32.0 / scale_
        + kPeakTurnWeight * scale_ * peakExcess * peakExcess;
}

QJsonObject BoundaryModel::diagnostics(const BoundaryMetrics &metrics) const {
    return {{QStringLiteral("energy"), energy(metrics)},
        {QStringLiteral("protectedCorners"), corners_.size()},
        {QStringLiteral("tangentEnergy"), metrics.tangentEnergy},
        {QStringLiteral("turnEnergy"), metrics.turnEnergy},
        {QStringLiteral("maximumExcessTurnDegrees"), metrics.maximumExcessTurn * 180.0 / std::numbers::pi},
        {QStringLiteral("maximumBoundaryDistance"), metrics.maximumDistance},
        {QStringLiteral("maximumCornerDistance"), metrics.maximumCornerDistance},
        {QStringLiteral("cornerEnergy"), metrics.cornerEnergy},
        {QStringLiteral("cornerDefects"), metrics.cornerDefects},
        {QStringLiteral("components"), metrics.components},
        {QStringLiteral("holes"), metrics.holes},
        {QStringLiteral("samples"), metrics.samples}};
}

QJsonObject BoundaryModel::performance() const {
    return {{QStringLiteral("measurements"), measurements_},
        {QStringLiteral("cornerMilliseconds"), cornerNanoseconds_ / 1e6},
        {QStringLiteral("closingMilliseconds"), closingNanoseconds_ / 1e6},
        {QStringLiteral("samplingMilliseconds"), samplingNanoseconds_ / 1e6}};
}

double BoundaryModel::perimeter() const {
    double result = 0.0;
    for (const auto &loop : loops_) {
        result += loop.perimeter;
    }

    return result;
}

BoundaryModel::Loop BoundaryModel::makeLoop(const QPolygonF &points) {
    Loop loop;
    loop.points = points;
    for (int index = 0; index < points.size(); ++index) {
        loop.lengths.push_back(loop.perimeter);
        loop.perimeter += QLineF(points[index], points[(index + 1) % points.size()]).length();
    }
    loop.lengths.push_back(loop.perimeter);

    return loop;
}

QPointF BoundaryModel::pointAt(const Loop &loop, double offset) {
    if (loop.perimeter <= kMinimumLength) {
        return loop.points.value(0);
    }
    offset = std::fmod(offset, loop.perimeter);
    if (offset < 0) {
        offset += loop.perimeter;
    }
    const auto upper = std::upper_bound(loop.lengths.begin(), loop.lengths.end(), offset);
    const int index = std::clamp(static_cast<int>(upper - loop.lengths.begin()) - 1, 0,
        static_cast<int>(loop.points.size()) - 1);
    const double length = loop.lengths[index + 1] - loop.lengths[index];
    const double parameter = length > kMinimumLength ? (offset - loop.lengths[index]) / length : 0.0;

    return loop.points[index] + (loop.points[(index + 1) % loop.points.size()] - loop.points[index]) * parameter;
}

BoundaryModel::Location BoundaryModel::closest(const QPointF &point) const {
    Location result;
    const int column = static_cast<int>(std::floor((point.x() - bounds_.left()) / cellSize_));
    const int row = static_cast<int>(std::floor((point.y() - bounds_.top()) / cellSize_));
    auto evaluate = [&](int index) {
        const auto &edge = edges_[index];
        const double parameter = edge.squaredLength > kMinimumLength
            ? std::clamp(QPointF::dotProduct(point - edge.start, edge.delta) / edge.squaredLength, 0.0, 1.0) : 0.0;
        const auto closest = edge.start + edge.delta * parameter;
        const auto difference = point - closest;
        const double squaredDistance = QPointF::dotProduct(difference, difference);
        if (squaredDistance < result.squaredDistance) {
            result = {closest, edge.loop, edge.offset + std::sqrt(edge.squaredLength) * parameter, squaredDistance};
        }
    };
    for (int radius = 0; radius <= kSearchRadius; ++radius) {
        for (int y = row - radius; y <= row + radius; ++y) {
            for (int x = column - radius; x <= column + radius; ++x) {
                if (radius > 0 && std::abs(x - column) < radius && std::abs(y - row) < radius) {
                    continue;
                }
                const auto found = cells_.constFind(cellKey(x, y));
                if (found != cells_.cend()) {
                    for (int index : *found) {
                        evaluate(index);
                    }
                }
            }
        }
        const double clearance = std::min({point.x() - (bounds_.left() + (column - radius) * cellSize_),
            bounds_.left() + (column + radius + 1) * cellSize_ - point.x(),
            point.y() - (bounds_.top() + (row - radius) * cellSize_),
            bounds_.top() + (row + radius + 1) * cellSize_ - point.y()});
        if (result.squaredDistance <= clearance * clearance) {
            return result;
        }
    }
    for (int index = 0; index < edges_.size(); ++index) {
        evaluate(index);
    }

    return result;
}

bool BoundaryModel::nearCorner(const QPointF &point, double radius) const {
    const int column = static_cast<int>(std::floor((point.x() - bounds_.left()) / cellSize_));
    const int row = static_cast<int>(std::floor((point.y() - bounds_.top()) / cellSize_));
    const int extent = static_cast<int>(std::ceil(radius / cellSize_));
    for (int y = row - extent; y <= row + extent; ++y) {
        for (int x = column - extent; x <= column + extent; ++x) {
            const auto found = cornerCells_.constFind(cellKey(x, y));
            if (found == cornerCells_.cend()) {
                continue;
            }
            for (const auto &corner : *found) {
                const auto delta = point - corner;
                if (QPointF::dotProduct(delta, delta) <= radius * radius) {
                    return true;
                }
            }
        }
    }

    return false;
}

qint64 BoundaryModel::cellKey(int x, int y) const {
    return static_cast<qint64>((static_cast<quint64>(static_cast<quint32>(x)) << 32) | static_cast<quint32>(y));
}

} // namespace gui::compact
