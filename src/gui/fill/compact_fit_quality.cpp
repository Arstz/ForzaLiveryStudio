#include "compact_fit_quality.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <numeric>

namespace gui::compact {
namespace {

constexpr double kMinimumLength = 1e-8;
constexpr double kMinimumSquaredLength = kMinimumLength * kMinimumLength;
constexpr double kTurnSlack = 0.12;
constexpr double kCornerDefectTurn = 0.35;
constexpr double kTargetCornerTurn = 0.6;
constexpr int kMaximumSamples = 2048;
constexpr int kArcBins = 128;
constexpr int kSearchRadius = 3;
constexpr int kCellPadding = kSearchRadius + 1;
constexpr double kCellScale = 8.0;
constexpr double kMaximumGridExtent = 128.0;
constexpr double kSubpixelClosingFraction = 0.15;
constexpr double kPeakTurnWeight = 64.0;
constexpr double kObservationWindowSlack = 0.1;
constexpr double kCornerSupportFraction = 0.5;
constexpr int kLoopMatchSamples = 8;
constexpr double kJunctionPhaseRadius = 4.0;
constexpr double kMinimumCornerCosine = 0.01;
constexpr double kCornerProjectionSafety = 2.0;

double wrappedOffset(double offset, double perimeter) {
    offset = std::fmod(offset, perimeter);

    return offset < 0 ? offset + perimeter : offset;
}

QPointF normalized(const QPointF &point) {
    const double length = std::hypot(point.x(), point.y());

    return length > kMinimumLength ? point / length : QPointF();
}

double angle(const QPointF &left, const QPointF &right) {
    return std::atan2(left.x() * right.y() - left.y() * right.x(), QPointF::dotProduct(left, right));
}

double projectedOffset(const QPolygonF &points, const QVector<double> &lengths, const QPointF &point) {
    double best = std::numeric_limits<double>::infinity();
    double offset = 0;
    for (int index = 0; index < points.size(); ++index) {
        const auto delta = points[(index + 1) % points.size()] - points[index];
        const double squaredLength = QPointF::dotProduct(delta, delta);
        const double parameter = squaredLength > kMinimumSquaredLength
            ? std::clamp(QPointF::dotProduct(point - points[index], delta) / squaredLength, 0.0, 1.0) : 0.0;
        const auto difference = points[index] + delta * parameter - point;
        const double distance = QPointF::dotProduct(difference, difference);
        if (distance < best) {
            best = distance;
            offset = lengths[index] + (lengths[index + 1] - lengths[index]) * parameter;
        }
    }

    return offset;
}

catalog::Polygons closeSubpixelGaps(const catalog::Polygons &polygons, double radius) {
    const auto grown = catalog::expanded(polygons, radius);
    const auto bounds = catalog::painterPath(grown).boundingRect().adjusted(-4 * radius, -4 * radius, 4 * radius, 4 * radius);
    QPolygonF frame({bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()});
    const auto outside = catalog::subtract({frame}, grown);

    return catalog::subtract(grown, catalog::expanded(outside, radius));
}

} // namespace

std::shared_ptr<const BoundaryOcclusion> boundaryOcclusion(const catalog::Polygons &polygons, double observationScale) {
    if (polygons.isEmpty())
        return {};
    auto result = std::make_shared<BoundaryOcclusion>();
    result->interior = std::make_shared<catalog::PointContainment>(polygons);
    result->boundary = std::make_shared<BoundaryModel>(polygons, observationScale);

    return result;
}

bool BoundaryOcclusion::contains(const QPointF &point, double radius) const {
    return boundary->withinDistance(point, radius) || interior->contains(point);
}

BoundaryModel::BoundaryModel(const catalog::Polygons &target, double observationScale,
                               const QVector<BoundaryExclusion> &exclusions, bool vertexSamples,
                               const QVector<catalog::BoundaryCorner> &authoredCorners,
                               const std::shared_ptr<const BoundaryOcclusion> &occlusion)
    : exclusions_(exclusions), occlusion_(occlusion), target_(target), bounds_(catalog::painterPath(target).boundingRect()),
      vertexSamples_(vertexSamples),
      scale_(std::max(observationScale, kMinimumLength)),
      cellSize_(std::max(scale_ * kCellScale,
          std::max(bounds_.width(), bounds_.height()) / kMaximumGridExtent)) {
    columns_ = static_cast<int>(std::floor(bounds_.width() / cellSize_)) + kCellPadding * 2 + 1;
    rows_ = static_cast<int>(std::floor(bounds_.height() / cellSize_)) + kCellPadding * 2 + 1;
    cells_.resize(columns_ * rows_);
    cornerCells_.resize(cells_.size());
    QVector<bool> sharpCandidates;
    for (const auto &polygon : target) {
        const auto loop = makeLoop(polygon);
        const int loopIndex = loops_.size();
        loops_.push_back(loop);
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF delta = polygon[(index + 1) % polygon.size()] - polygon[index];
            const double radius = scale_ * kCornerSupportFraction;
            const double turn = supportedTurn(loop, loop.lengths[index], radius);
            if (loop.perimeter > radius * 4 && std::abs(turn) > kTargetCornerTurn
                && visibleCorner(loop, loop.lengths[index]) && !excluded(polygon[index])) {
                const auto incoming = polygon[index] - polygon[(index + polygon.size() - 1) % polygon.size()];
                const bool sharp = std::abs(angle(normalized(incoming), normalized(delta))) > kTargetCornerTurn;
                const double neighborhood = std::max(std::abs(supportedTurn(loop, loop.lengths[index] - radius, radius)),
                    std::abs(supportedTurn(loop, loop.lengths[index] + radius, radius)));
                if (sharp || std::abs(turn) > neighborhood + kTurnSlack) {
                    cornerFeatures_.push_back({polygon[index], normalized(polygon[index] - pointAt(loop, loop.lengths[index] - radius)),
                        normalized(pointAt(loop, loop.lengths[index] + radius) - polygon[index]), loopIndex, loop.lengths[index]});
                    sharpCandidates.push_back(sharp);
                }
            }
            const double squaredLength = QPointF::dotProduct(delta, delta);
            const Edge edge{polygon[index], delta, loopIndex, squaredLength, std::sqrt(squaredLength), loop.lengths[index]};
            const QRectF bounds = QRectF(edge.start, edge.start + edge.delta).normalized();
            const int edgeIndex = edges_.size();
            edges_.push_back(edge);
            const int left = static_cast<int>(std::floor((bounds.left() - bounds_.left()) / cellSize_));
            const int right = static_cast<int>(std::floor((bounds.right() - bounds_.left()) / cellSize_));
            const int top = static_cast<int>(std::floor((bounds.top() - bounds_.top()) / cellSize_));
            const int bottom = static_cast<int>(std::floor((bounds.bottom() - bounds_.top()) / cellSize_));
            for (int row = top; row <= bottom; ++row) {
                for (int column = left; column <= right; ++column) {
                    cells_[(row + kCellPadding) * columns_ + column + kCellPadding].push_back(edgeIndex);
                }
            }
        }
    }
    const auto arcDistance = [&](int loop, double first, double second) {
        const double difference = wrappedOffset(first - second, loops_[loop].perimeter);

        return std::min(difference, loops_[loop].perimeter - difference);
    };
    auto candidates = std::move(cornerFeatures_);
    QVector<int> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int firstIndex, int secondIndex) {
        if (sharpCandidates[firstIndex] != sharpCandidates[secondIndex])
            return sharpCandidates[firstIndex];
        const auto &left = candidates[firstIndex];
        const auto &right = candidates[secondIndex];
        const double first = std::abs(angle(left.incoming, left.outgoing));
        const double second = std::abs(angle(right.incoming, right.outgoing));
        if (first != second)
            return first > second;
        return left.point.x() == right.point.x() ? left.point.y() < right.point.y() : left.point.x() < right.point.x();
    });
    for (int index : order) {
        const auto &candidate = candidates[index];
        const auto duplicate = std::find_if(cornerFeatures_.begin(), cornerFeatures_.end(), [&](const auto &corner) {
            return candidate.loop == corner.loop && angle(candidate.incoming, candidate.outgoing) * angle(corner.incoming, corner.outgoing) > 0
                && arcDistance(candidate.loop, candidate.offset, corner.offset) <= scale_ * kCornerSupportFraction;
        });
        if (duplicate == cornerFeatures_.end())
            cornerFeatures_.push_back(candidate);
    }
    const auto observedFeatures = cornerFeatures_;
    QVector<double> authoredDistances(cornerFeatures_.size(), std::numeric_limits<double>::infinity());
    for (const auto &authored : authoredCorners) {
        const auto incoming = normalized(authored.incoming), outgoing = normalized(authored.outgoing);
        if (incoming.isNull() || outgoing.isNull() || excluded(authored.point)
            || std::abs(angle(incoming, outgoing)) <= kTargetCornerTurn)
            continue;
        const auto location = closest(authored.point);
        const double tolerance = std::max(authored.tolerance, catalog::kVerificationClearance);
        if (location.squaredDistance > tolerance * tolerance || !visibleCorner(loops_[location.loop], location.offset))
            continue;
        int duplicate = -1;
        double distance = std::numeric_limits<double>::infinity();
        for (int index = 0; index < observedFeatures.size(); ++index) {
            const auto &corner = observedFeatures[index];
            if (corner.loop != location.loop || angle(corner.incoming, corner.outgoing) * angle(incoming, outgoing) <= 0)
                continue;
            const double candidateDistance = arcDistance(corner.loop, corner.offset, location.offset);
            if (candidateDistance <= scale_ * kCornerSupportFraction && candidateDistance < distance) {
                duplicate = index;
                distance = candidateDistance;
            }
        }
        if (duplicate < 0 || distance > authoredDistances[duplicate])
            continue;
        if (distance == authoredDistances[duplicate]) {
            const auto &previous = cornerFeatures_[duplicate].point;
            if (authored.point.x() > previous.x() || (authored.point.x() == previous.x() && authored.point.y() >= previous.y()))
                continue;
        }
        const Corner corner{authored.point, incoming, outgoing, location.loop, location.offset};
        cornerFeatures_[duplicate] = corner;
        authoredDistances[duplicate] = distance;
    }
    std::stable_sort(cornerFeatures_.begin(), cornerFeatures_.end(), [](const auto &left, const auto &right) {
        return left.loop == right.loop ? left.offset < right.offset : left.loop < right.loop;
    });
    for (const auto &corner : cornerFeatures_) {
        corners_.push_back(corner.point);
        const int column = static_cast<int>(std::floor((corner.point.x() - bounds_.left()) / cellSize_));
        const int row = static_cast<int>(std::floor((corner.point.y() - bounds_.top()) / cellSize_));
        cornerCells_[(row + kCellPadding) * columns_ + column + kCellPadding].push_back(corner.point);
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

bool BoundaryModel::withinDistance(const QPointF &point, double radius) const {
    if (!bounds_.adjusted(-radius, -radius, radius, radius).contains(point))
        return false;
    const int left = std::max(-kCellPadding, int(std::floor((point.x() - radius - bounds_.left()) / cellSize_)));
    const int right = std::min(columns_ - kCellPadding - 1, int(std::floor((point.x() + radius - bounds_.left()) / cellSize_)));
    const int top = std::max(-kCellPadding, int(std::floor((point.y() - radius - bounds_.top()) / cellSize_)));
    const int bottom = std::min(rows_ - kCellPadding - 1, int(std::floor((point.y() + radius - bounds_.top()) / cellSize_)));
    for (int row = top; row <= bottom; ++row) {
        for (int column = left; column <= right; ++column) {
            for (int index : cells_[(row + kCellPadding) * columns_ + column + kCellPadding]) {
                const auto &edge = edges_[index];
                const double fraction = edge.squaredLength > kMinimumSquaredLength
                    ? std::clamp(QPointF::dotProduct(point - edge.start, edge.delta) / edge.squaredLength, 0.0, 1.0) : 0.0;
                const auto difference = edge.start + edge.delta * fraction - point;
                if (QPointF::dotProduct(difference, difference) <= radius * radius)
                    return true;
            }
        }
    }

    return false;
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
    return measure(coverage, observed, QRectF());
}

BoundaryMetrics BoundaryModel::measure(const catalog::Polygons &coverage, const catalog::Polygons &observed,
                                        const QRectF &window, QVector<BoundaryDefect> *defects) const {
    BoundaryMetrics result;
    QElapsedTimer timer;
    std::unique_ptr<BoundaryModel> outputBoundary;
    timer.start();
    if (loops_.isEmpty()) {
        return result;
    }
    if (!observedReference_) {
        observedReference_ = std::make_shared<BoundaryModel>(
            observationSupport(target_), scale_, QVector<BoundaryExclusion>{}, false,
            QVector<catalog::BoundaryCorner>{}, occlusion_);
    }
    const auto &referenceModel = observedReference_->loops_.isEmpty() ? *this : *observedReference_;
    if (!cornerFeatures_.isEmpty() && !coverage.isEmpty()) {
        outputBoundary = std::make_unique<BoundaryModel>(coverage, scale_, QVector<BoundaryExclusion>{}, false,
            QVector<catalog::BoundaryCorner>{}, occlusion_);
    }
    struct CornerPair { int target; int output; double cost; };
    QVector<CornerPair> pairs;
    QVector<int> matches(cornerFeatures_.size(), -1);
    QVector<int> outputLoops;
    if (outputBoundary) {
        for (const auto &loop : outputBoundary->loops_)
            outputLoops.push_back(correspondingLoop(loop));
        for (int target = 0; target < cornerFeatures_.size(); ++target) {
            const auto &corner = cornerFeatures_[target];
            const double turn = angle(corner.incoming, corner.outgoing);
            for (int output = 0; output < outputBoundary->cornerFeatures_.size(); ++output) {
                const auto &candidate = outputBoundary->cornerFeatures_[output];
                const double outputTurn = angle(candidate.incoming, candidate.outgoing);
                if (outputLoops[candidate.loop] != corner.loop || turn * outputTurn <= 0)
                    continue;
                const double distance = QLineF(corner.point, candidate.point).length();
                const double difference = std::remainder(outputTurn - turn, 2 * std::numbers::pi);
                pairs.push_back({target, output, distance * distance + scale_ * scale_ * difference * difference});
            }
        }
        std::stable_sort(pairs.begin(), pairs.end(), [](const auto &left, const auto &right) {
            return left.cost < right.cost;
        });
        QVector<bool> used(outputBoundary->cornerFeatures_.size(), false);
        for (const auto &pair : pairs) {
            if (matches[pair.target] < 0 && !used[pair.output]) {
                matches[pair.target] = pair.output;
                used[pair.output] = true;
            }
        }
    }
    for (int index = 0; index < cornerFeatures_.size(); ++index) {
        const auto &corner = cornerFeatures_[index];
        if (!window.isNull() && !window.contains(corner.point)) {
            result.cornerDistances.push_back(0.0);
            result.cornerOffsets.push_back({});
            continue;
        }
        double distance = scale_;
        double excess = std::numbers::pi;
        QPointF offset;
        if (matches[index] >= 0) {
            const auto &candidate = outputBoundary->cornerFeatures_[matches[index]];
            offset = candidate.point - corner.point;
            distance = QLineF(corner.point, candidate.point).length();
            excess = 0;
            for (double radius : {scale_, scale_ * 2}) {
                const auto &targetLoop = loops_[corner.loop];
                const auto &outputLoop = outputBoundary->loops_[candidate.loop];
                const double targetTurn = angle(normalized(corner.point - pointAt(targetLoop, corner.offset - radius)),
                    normalized(pointAt(targetLoop, corner.offset + radius) - corner.point));
                const double outputTurn = angle(normalized(candidate.point - pointAt(outputLoop, candidate.offset - radius)),
                    normalized(pointAt(outputLoop, candidate.offset + radius) - candidate.point));
                excess = std::max(excess, std::max(0.0, std::abs(std::remainder(outputTurn - targetTurn,
                    2 * std::numbers::pi)) - kTurnSlack));
            }
            ++result.matchedCorners;
        } else {
            ++result.unmatchedCorners;
            if (outputBoundary)
                distance += std::sqrt(outputBoundary->closest(corner.point).squaredDistance);
        }
        result.cornerDistances.push_back(distance);
        result.cornerOffsets.push_back(offset);
        result.maximumCornerDistance = std::max(result.maximumCornerDistance, distance);
        result.maximumCornerAngleError = std::max(result.maximumCornerAngleError, excess);
        result.cornerEnergy += distance * distance;
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
        if (loop.perimeter <= kMinimumLength) {
            continue;
        }
        const int targetLoop = referenceModel.correspondingLoop(loop);
        const int rawTargetLoop = correspondingLoop(loop);
        struct Anchor { QPointF point; double output; double target; };
        QVector<Anchor> anchors;
        if (targetLoop >= 0 && outputBoundary) {
            for (int index = 0; index < matches.size(); ++index) {
                if (matches[index] < 0 || cornerFeatures_[index].loop != rawTargetLoop)
                    continue;
                const auto &candidate = outputBoundary->cornerFeatures_[matches[index]];
                const double output = projectedOffset(loop.points, loop.lengths, candidate.point);
                const auto observedCorner = pointAt(loop, output);
                const double cosine = std::max(kMinimumCornerCosine,
                    std::cos(std::abs(angle(candidate.incoming, candidate.outgoing)) / 2));
                const double projectionLimit = std::max(scale_ * kCornerSupportFraction,
                    scale_ * kSubpixelClosingFraction * std::sqrt(2.0) * kCornerProjectionSafety / cosine);
                if (QLineF(observedCorner, candidate.point).length() > projectionLimit)
                    continue;
                anchors.push_back({observedCorner, output,
                    referenceModel.closest(cornerFeatures_[index].point, targetLoop).offset});
            }
            std::sort(anchors.begin(), anchors.end(), [](const auto &left, const auto &right) { return left.output < right.output; });
            if (anchors.size() > 1) {
                double total = 0;
                bool degenerate = false;
                const double perimeter = referenceModel.loops_[targetLoop].perimeter;
                for (int index = 0; index < anchors.size(); ++index) {
                    const double span = wrappedOffset(anchors[(index + 1) % anchors.size()].target - anchors[index].target, perimeter);
                    degenerate = degenerate || span <= kMinimumLength;
                    total += span;
                }
                if (degenerate || std::abs(total - perimeter) > catalog::kVerificationClearance * anchors.size()) {
                    result.maximumCornerAngleError = std::numbers::pi;
                    anchors.clear();
                }
            }
        }
        for (const auto &sample : loopSamples(loop, step, vertexSamples_)) {
            const double offset = sample.offset;
            const double spacing = sample.spacing;
            const auto point = pointAt(loop, offset);
            if (excluded(point) || (!window.isNull() && !window.contains(point))) {
                continue;
            }
            double spanStart = 0;
            double spanLength = std::numeric_limits<double>::infinity();
            double alignedOffset = std::numeric_limits<double>::quiet_NaN();
            if (anchors.size() > 1) {
                const auto upper = std::upper_bound(anchors.begin(), anchors.end(), offset,
                    [](double value, const auto &anchor) { return value < anchor.output; });
                const int next = upper == anchors.end() ? 0 : int(upper - anchors.begin());
                const int previous = (next + anchors.size() - 1) % anchors.size();
                spanStart = anchors[previous].target;
                spanLength = wrappedOffset(anchors[next].target - spanStart, referenceModel.loops_[targetLoop].perimeter);
                const double outputLength = wrappedOffset(anchors[next].output - anchors[previous].output, loop.perimeter);
                const double progress = wrappedOffset(offset - anchors[previous].output, loop.perimeter);
                const double radius = scale_ * kJunctionPhaseRadius;
                if (outputLength > kMinimumLength) {
                    if (outputLength <= radius * 2)
                        alignedOffset = spanStart + progress * spanLength / outputLength;
                    else if (progress < radius)
                        alignedOffset = spanStart + std::min(progress, spanLength);
                    else if (outputLength - progress < radius)
                        alignedOffset = spanStart + std::max(0.0, spanLength - (outputLength - progress));
                }
            }
            auto reference = referenceModel.closest(point, targetLoop, spanStart, spanLength);
            const auto &target = referenceModel.loops_[reference.loop];
            if (std::isfinite(alignedOffset)) {
                reference.offset = alignedOffset;
                reference.point = pointAt(target, alignedOffset);
            }
            double sampleEnergy = 0.0;
            double sampleTurn = 0.0;
            for (double radius : {scale_, scale_ * 2.0}) {
                if (std::any_of(anchors.begin(), anchors.end(), [&](const auto &anchor) {
                    return QLineF(point, anchor.point).length() <= radius + scale_ * kCornerSupportFraction;
                }))
                    continue;
                const auto before = pointAt(loop, offset - radius);
                const auto after = pointAt(loop, offset + radius);
                const auto targetBefore = pointAt(target, reference.offset - radius);
                const auto targetAfter = pointAt(target, reference.offset + radius);
                const double observerMargin = scale_ * kSubpixelClosingFraction * std::sqrt(2.0)
                    + catalog::kVerificationClearance;
                if (occlusion_ && (occlusion_->contains(before, observerMargin) || occlusion_->contains(after, observerMargin)
                    || occlusion_->contains(targetBefore, observerMargin) || occlusion_->contains(targetAfter, observerMargin)))
                    continue;
                const double turn = angle(normalized(point - before), normalized(after - point));
                const double targetTurn = angle(normalized(reference.point - targetBefore),
                    normalized(targetAfter - reference.point));
                const double excess = std::max(0.0,
                    std::abs(std::remainder(turn - targetTurn, 2.0 * std::numbers::pi)) - kTurnSlack);
                const double tangentError = angle(normalized(after - before), normalized(targetAfter - targetBefore));
                if (std::abs(targetTurn) < kTargetCornerTurn) {
                    result.tangentEnergy += spacing * tangentError * tangentError * 0.5;
                    result.turnEnergy += spacing * excess * excess * 0.5;
                    result.maximumExcessTurn = std::max(result.maximumExcessTurn, excess);
                    sampleTurn = std::max(sampleTurn, excess);
                    result.cornerDefects += excess > kCornerDefectTurn;
                    sampleEnergy += spacing * (tangentError * tangentError * 0.5 + excess * excess);
                }
            }
            result.maximumDistance = std::max(result.maximumDistance, std::sqrt(reference.squaredDistance));
            ++result.samples;
            if (defects && sampleEnergy > kMinimumLength)
                defects->push_back({point, sampleEnergy, sampleTurn});
        }
    }

    samplingNanoseconds_ += timer.nsecsElapsed();
    ++measurements_;

    return result;
}

double BoundaryModel::energy(const BoundaryMetrics &metrics) const {
    const double peakExcess = std::max(0.0, metrics.maximumExcessTurn - kCornerDefectTurn);
    const double cornerExcess = std::max(0.0, metrics.maximumCornerAngleError - kCornerDefectTurn);

    return metrics.tangentEnergy + metrics.turnEnergy * 2.0 + metrics.cornerEnergy * 32.0 / scale_
        + kPeakTurnWeight * scale_ * (peakExcess * peakExcess + cornerExcess * cornerExcess);
}

QJsonObject BoundaryModel::diagnostics(const BoundaryMetrics &metrics) const {
    return {{QStringLiteral("energy"), energy(metrics)},
        {QStringLiteral("vertexSamples"), vertexSamples_},
        {QStringLiteral("protectedCorners"), corners_.size()},
        {QStringLiteral("tangentEnergy"), metrics.tangentEnergy},
        {QStringLiteral("turnEnergy"), metrics.turnEnergy},
        {QStringLiteral("maximumExcessTurnDegrees"), metrics.maximumExcessTurn * 180.0 / std::numbers::pi},
        {QStringLiteral("maximumBoundaryDistance"), metrics.maximumDistance},
        {QStringLiteral("maximumCornerDistance"), metrics.maximumCornerDistance},
        {QStringLiteral("maximumCornerAngleErrorDegrees"), metrics.maximumCornerAngleError * 180 / std::numbers::pi},
        {QStringLiteral("matchedCorners"), metrics.matchedCorners},
        {QStringLiteral("unmatchedCorners"), metrics.unmatchedCorners},
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

const QVector<QPointF> &BoundaryModel::protectedCorners() const {
    return corners_;
}

BoundaryModel::Loop BoundaryModel::makeLoop(const QPolygonF &points) {
    Loop loop;
    loop.points = points;
    for (int index = 0; index < points.size(); ++index) {
        loop.lengths.push_back(loop.perimeter);
        loop.perimeter += QLineF(points[index], points[(index + 1) % points.size()]).length();
    }
    loop.lengths.push_back(loop.perimeter);
    loop.bins.reserve(kArcBins + 1);
    for (int bin = 0; bin <= kArcBins; ++bin) {
        const double offset = loop.perimeter * bin / kArcBins;
        loop.bins.push_back(static_cast<int>(std::lower_bound(loop.lengths.cbegin(), loop.lengths.cend(), offset)
            - loop.lengths.cbegin()));
    }

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
    const int bin = std::clamp(static_cast<int>(offset / loop.perimeter * kArcBins), 0, kArcBins - 1);
    const int first = std::max(0, loop.bins[bin] - 1);
    const int last = std::min(static_cast<int>(loop.lengths.size()), loop.bins[bin + 1] + 1);
    const bool bounded = loop.lengths[first] <= offset
        && (last == loop.lengths.size() || offset < loop.lengths[last]);
    const auto upper = std::upper_bound(loop.lengths.cbegin() + (bounded ? first : 0),
        loop.lengths.cbegin() + (bounded ? last : loop.lengths.size()), offset);
    const int index = std::clamp(static_cast<int>(upper - loop.lengths.begin()) - 1, 0,
        static_cast<int>(loop.points.size()) - 1);
    const int next = index + 1 == loop.points.size() ? 0 : index + 1;
    const double length = loop.lengths[index + 1] - loop.lengths[index];
    const double parameter = length > kMinimumLength ? (offset - loop.lengths[index]) / length : 0.0;

    return loop.points[index] + (loop.points[next] - loop.points[index]) * parameter;
}

double BoundaryModel::supportedTurn(const Loop &loop, double offset, double radius) {
    const auto point = pointAt(loop, offset);

    return angle(normalized(point - pointAt(loop, offset - radius)), normalized(pointAt(loop, offset + radius) - point));
}

QVector<BoundaryModel::Sample> BoundaryModel::loopSamples(const Loop &loop, double step, bool vertices) {
    QVector<Sample> result;

    if (!vertices) {
        const int count = std::max(1, static_cast<int>(std::ceil(loop.perimeter / step)));
        const double spacing = loop.perimeter / count;
        for (int index = 0; index < count; ++index)
            result.push_back({(index + 0.5) * spacing, spacing});
    } else {
        for (int edge = 0; edge < loop.points.size(); ++edge) {
            const double length = loop.lengths[edge + 1] - loop.lengths[edge];
            const int count = std::max(1, static_cast<int>(std::ceil(length / step)));
            const double spacing = length / count;
            if (length <= kMinimumLength)
                continue;
            for (int index = 0; index < count; ++index)
                result.push_back({loop.lengths[edge] + index * spacing, spacing});
        }
        if (!result.isEmpty()) {
            double previous = result.back().spacing;
            for (auto &sample : result) {
                const double current = sample.spacing;
                sample.spacing = (previous + current) * 0.5;
                previous = current;
            }
        }
    }

    return result;
}

BoundaryModel::Location BoundaryModel::closest(const QPointF &point, int loop, double spanStart, double spanLength) const {
    Location result;
    const int column = static_cast<int>(std::floor((point.x() - bounds_.left()) / cellSize_));
    const int row = static_cast<int>(std::floor((point.y() - bounds_.top()) / cellSize_));
    auto evaluate = [&](int index) {
        const auto &edge = edges_[index];
        if (loop >= 0 && edge.loop != loop)
            return;
        double minimum = 0, maximum = 1;
        if (loop >= 0 && spanLength < loops_[loop].perimeter && edge.length > kMinimumLength) {
            const double perimeter = loops_[loop].perimeter;
            double relative = wrappedOffset(edge.offset - spanStart, perimeter);
            if (relative + edge.length >= perimeter)
                relative -= perimeter;
            minimum = std::max(0.0, -relative / edge.length);
            maximum = std::min(1.0, (spanLength - relative) / edge.length);
            if (minimum > maximum)
                return;
        }
        const double parameter = edge.squaredLength > kMinimumSquaredLength
            ? std::clamp(QPointF::dotProduct(point - edge.start, edge.delta) / edge.squaredLength, minimum, maximum) : 0.0;
        const auto closest = edge.start + edge.delta * parameter;
        const auto difference = point - closest;
        const double squaredDistance = QPointF::dotProduct(difference, difference);
        if (squaredDistance < result.squaredDistance) {
            result = {closest, edge.loop, edge.offset + edge.length * parameter, squaredDistance};
        }
    };
    for (int radius = 0; radius <= kSearchRadius; ++radius) {
        for (int y = row - radius; y <= row + radius; ++y) {
            for (int x = column - radius; x <= column + radius; ++x) {
                if (radius > 0 && std::abs(x - column) < radius && std::abs(y - row) < radius) {
                    continue;
                }
                if (x >= -kCellPadding && x + kCellPadding < columns_
                    && y >= -kCellPadding && y + kCellPadding < rows_) {
                    for (int index : cells_[(y + kCellPadding) * columns_ + x + kCellPadding])
                        evaluate(index);
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

int BoundaryModel::correspondingLoop(const Loop &loop) const {
    if (loops_.size() == 1)
        return (catalog::signedArea(loops_.front().points) > 0) == (catalog::signedArea(loop.points) > 0) ? 0 : -1;
    QVector<double> costs(loops_.size(), std::numeric_limits<double>::infinity());
    const bool positive = catalog::signedArea(loop.points) > 0;
    for (int index = 0; index < loops_.size(); ++index) {
        if ((catalog::signedArea(loops_[index].points) > 0) != positive)
            continue;
        double cost = 0;
        for (int sample = 0; sample < kLoopMatchSamples; ++sample)
            cost += closest(pointAt(loop, loop.perimeter * (sample + 0.5) / kLoopMatchSamples), index).squaredDistance;
        costs[index] = cost;
    }
    const auto best = std::min_element(costs.begin(), costs.end());

    return best == costs.end() || !std::isfinite(*best) ? -1 : int(best - costs.begin());
}

bool BoundaryModel::nearCorner(const QPointF &point, double radius) const {
    const int column = static_cast<int>(std::floor((point.x() - bounds_.left()) / cellSize_));
    const int row = static_cast<int>(std::floor((point.y() - bounds_.top()) / cellSize_));
    const int extent = static_cast<int>(std::ceil(radius / cellSize_));
    for (int y = row - extent; y <= row + extent; ++y) {
        for (int x = column - extent; x <= column + extent; ++x) {
            if (x < -kCellPadding || x + kCellPadding >= columns_
                || y < -kCellPadding || y + kCellPadding >= rows_)
                continue;
            for (const auto &corner : cornerCells_[(y + kCellPadding) * columns_ + x + kCellPadding]) {
                const auto delta = point - corner;
                if (QPointF::dotProduct(delta, delta) <= radius * radius) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool BoundaryModel::visibleCorner(const Loop &loop, double offset) const {
    if (!occlusion_)
        return true;
    if (occlusion_->contains(pointAt(loop, offset), scale_))
        return false;
    for (double radius : {scale_, scale_ * 2})
        for (double sign : {-1.0, 1.0})
            if (occlusion_->contains(pointAt(loop, offset + sign * radius), catalog::kVerificationClearance * 4))
                return false;

    return true;
}

bool BoundaryModel::excluded(const QPointF &point) const {
    if (occlusion_ && occlusion_->contains(point, catalog::kVerificationClearance * 4))
        return true;
    return std::any_of(exclusions_.cbegin(), exclusions_.cend(), [&](const auto &exclusion) {
        const auto offset = point - exclusion.point;

        return QPointF::dotProduct(offset, offset) <= exclusion.radius * exclusion.radius;
    });
}

} // namespace gui::compact
