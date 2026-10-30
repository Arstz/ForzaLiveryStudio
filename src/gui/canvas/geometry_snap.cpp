#include "geometry_snap.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace gui {
namespace {

constexpr int kLeafSegmentCount = 8;
constexpr double kDirectionEpsilon = 1e-12;
constexpr double kContactEpsilon = 1e-6;
constexpr double kMinimumSamplingScale = 1e-6;

double cross(const QPointF &first, const QPointF &second) {
    return first.x() * second.y() - first.y() * second.x();
}

double lengthSquared(const QPointF &point) {
    return QPointF::dotProduct(point, point);
}

bool overlaps(const QRectF &first, const QRectF &second) {
    return first.left() <= second.right() && first.right() >= second.left()
        && first.top() <= second.bottom() && first.bottom() >= second.top();
}

QRectF expanded(const QRectF &bounds, double tolerance) {
    return bounds.adjusted(-tolerance, -tolerance, tolerance, tolerance);
}

void sortCoordinates(QVector<double> &coordinates) {
    std::sort(coordinates.begin(), coordinates.end());
    coordinates.erase(std::unique(coordinates.begin(), coordinates.end()), coordinates.end());
}

void extendAlignmentLine(QHash<double, QLineF> &lines, const QLineF &line, bool horizontal) {
    const double coordinate = horizontal ? line.y1() : line.x1();
    const auto found = lines.find(coordinate);

    if (found == lines.end()) {
        lines.insert(coordinate, line);
        return;
    }
    *found = horizontal
        ? QLineF(std::min(found->x1(), line.x1()), coordinate, std::max(found->x2(), line.x2()), coordinate)
        : QLineF(coordinate, std::min(found->y1(), line.y1()), coordinate, std::max(found->y2(), line.y2()));
}

struct MotionPoint {
    QPointF position;
    QPointF first;
    QPointF second;

    QPointF adjusted(const QPointF &correction) const {
        return position + first * correction.x() + second * correction.y();
    }
};

MotionPoint interpolate(const MotionPoint &first, const MotionPoint &second, double fraction) {
    return {first.position + (second.position - first.position) * fraction,
            first.first + (second.first - first.first) * fraction,
            first.second + (second.second - first.second) * fraction};
}

struct Constraint {
    QPointF row;
    QPointF correction;
    MotionPoint source;
    GeometrySnapIndicator indicator;
    bool valid = false;
    double gap = 0.0;
};

Constraint constraint(const MotionPoint &point, const QPointF &normal, double gap) {
    const QPointF row(QPointF::dotProduct(point.first, normal), QPointF::dotProduct(point.second, normal));
    const double denominator = lengthSquared(row);

    if (denominator <= kDirectionEpsilon) {
        return {};
    }

    return {row, row * (gap / denominator), {}, {}, true, gap};
}

GeometrySnapIndicator alignmentIndicator(const Constraint &constraint, const QPointF &correction) {
    GeometrySnapIndicator result = constraint.indicator;

    result.point = constraint.source.adjusted(correction);
    if (!result.infiniteLine) {
        result.line = result.line.dx() != 0.0
            ? QLineF(std::min(result.line.x1(), result.point.x()), result.line.y1(),
                     std::max(result.line.x2(), result.point.x()), result.line.y2())
            : QLineF(result.line.x1(), std::min(result.line.y1(), result.point.y()),
                     result.line.x2(), std::max(result.line.y2(), result.point.y()));
    }

    return result;
}

bool solvePair(const Constraint &first, const Constraint &second, QPointF &correction) {
    const double determinant = cross(first.row, second.row);

    if (!first.valid || !second.valid || std::abs(determinant) <= kDirectionEpsilon) {
        return false;
    }
    correction = QPointF(first.gap * second.row.y() - first.row.y() * second.gap,
                         first.row.x() * second.gap - first.gap * second.row.x()) / determinant;

    return true;
}

double closestFraction(const QPointF &point, const QPointF &first, const QPointF &second) {
    const QPointF direction = second - first;
    const double denominator = lengthSquared(direction);

    return denominator > kDirectionEpsilon
        ? std::clamp(QPointF::dotProduct(point - first, direction) / denominator, 0.0, 1.0) : 0.0;
}

class SnapSearch {
public:
    SnapSearch(const GeometrySnapMotion &motion, double tolerance)
        : motion_(motion), tolerance_(tolerance), closestContactSquared_(tolerance * tolerance) {
        const QPointF origin = motion.transform.map(motion.controlPoint);
        const QPointF first = motion.firstStep.map(motion.controlPoint) - origin;
        const QPointF second = motion.secondStep.map(motion.controlPoint) - origin;

        if (motion.twoDimensions) {
            parameters_ = QTransform(first.x(), first.y(), second.x(), second.y(), 0.0, 0.0).inverted(&valid_);
        } else {
            const double length = std::hypot(first.x(), first.y());
            valid_ = length > kDirectionEpsilon;
            parameters_ = QTransform(valid_ ? 1.0 / length : 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
        }
    }

    bool valid() const { return valid_; }

    MotionPoint point(const QPointF &start) const {
        const QPointF position = motion_.transform.map(start);
        const QPointF first = motion_.firstStep.map(start) - position;
        const QPointF second = motion_.secondStep.map(start) - position;

        return {position, first * parameters_.m11() + second * parameters_.m12(),
                first * parameters_.m21() + second * parameters_.m22()};
    }

    void align(const MotionPoint &point, const QVector<double> &coordinates, bool horizontal,
               const GeometrySnapTargets *targets = nullptr) {
        const QPointF normal = horizontal ? QPointF(0.0, 1.0) : QPointF(1.0, 0.0);
        const double coordinate = QPointF::dotProduct(point.position, normal);
        auto &best = horizontal ? horizontal_ : vertical_;

        for (double target : coordinates) {
            const double gap = target - coordinate;
            if (std::abs(gap) > tolerance_) {
                continue;
            }
            Constraint candidate = constraint(point, normal, gap);
            if (candidate.valid && lengthSquared(candidate.correction) <= tolerance_ * tolerance_
                && (!best.valid || lengthSquared(candidate.correction) < lengthSquared(best.correction))) {
                candidate.source = point;
                candidate.indicator.line = targets != nullptr ? targets->alignmentLine(target, horizontal)
                    : horizontal ? QLineF(0.0, target, 1.0, target) : QLineF(target, 0.0, target, 1.0);
                candidate.indicator.infiniteLine = targets == nullptr;
                best = candidate;
            }
        }
    }

    void alignNearest(const MotionPoint &point, const GeometrySnapTargets &targets, bool horizontal) {
        const auto &coordinates = horizontal ? targets.horizontalCoordinates() : targets.verticalCoordinates();
        const double coordinate = horizontal ? point.position.y() : point.position.x();
        const auto next = std::lower_bound(coordinates.cbegin(), coordinates.cend(), coordinate);
        QVector<double> nearby;

        if (next != coordinates.cend()) nearby.push_back(*next);
        if (next != coordinates.cbegin()) nearby.push_back(*std::prev(next));
        align(point, nearby, horizontal, &targets);
    }

    void contact(const MotionPoint &point, const QLineF &segment) {
        const QPointF direction = segment.p2() - segment.p1();
        const double nearest = closestFraction(point.position, segment.p1(), segment.p2());
        const double length = segment.length();

        if (length <= kDirectionEpsilon
            || lengthSquared(point.position - segment.pointAt(nearest)) > tolerance_ * tolerance_) {
            return;
        }
        const QPointF normal(-direction.y() / length, direction.x() / length);
        const double gap = QPointF::dotProduct(segment.p1() - point.position, normal);
        const Constraint candidate = constraint(point, normal, gap);
        if (candidate.valid) {
            const QPointF moved = point.adjusted(candidate.correction);
            const double fraction = closestFraction(moved, segment.p1(), segment.p2());
            if (lengthSquared(moved - segment.pointAt(fraction)) <= kContactEpsilon * kContactEpsilon) {
                if (acceptContact(candidate.correction)) {
                    contactConstraint_ = candidate;
                    contactPoint_ = point;
                    contactSegment_ = segment;
                    contactIndicator_ = {segment, moved, GeometrySnapIndicatorKind::Contact};
                    canPairContact_ = true;
                }
            }
        }
        if (contactPoint(point, segment.p1())) contactIndicator_.line = segment;
        if (contactPoint(point, segment.p2())) contactIndicator_.line = segment;
    }

    void contactEdge(const MotionPoint &first, const MotionPoint &second, const QPointF &target) {
        const double fraction = closestFraction(target, first.position, second.position);
        const MotionPoint nearest = interpolate(first, second, fraction);

        if (lengthSquared(nearest.position - target) > tolerance_ * tolerance_) {
            return;
        }
        if (motion_.twoDimensions) {
            if (contactPoint(nearest, target)) {
                contactIndicator_.line = QLineF(first.adjusted(contact_), second.adjusted(contact_));
            }
            return;
        }
        const QPointF firstOffset = first.position - target;
        const QPointF secondOffset = second.position - target;
        const double quadratic = cross(first.first, second.first);
        const double linear = cross(first.first, secondOffset) + cross(firstOffset, second.first);
        const double constant = cross(firstOffset, secondOffset);
        const auto consider = [&](double adjustment) {
            const QPointF correction(adjustment, 0.0);
            const QPointF movedFirst = first.adjusted(correction);
            const QPointF movedSecond = second.adjusted(correction);
            const double movedFraction = closestFraction(target, movedFirst, movedSecond);

            if (lengthSquared(target - (movedFirst + (movedSecond - movedFirst) * movedFraction))
                <= kContactEpsilon * kContactEpsilon) {
                if (acceptContact(correction)) {
                    contactIndicator_ = {QLineF(movedFirst, movedSecond), target, GeometrySnapIndicatorKind::Contact};
                }
            }
        };

        if (std::abs(quadratic) <= kDirectionEpsilon) {
            if (std::abs(linear) > kDirectionEpsilon) consider(-constant / linear);
            return;
        }
        const double discriminant = linear * linear - 4.0 * quadratic * constant;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            const double numerator = -0.5 * (linear + std::copysign(root, linear));
            if (std::abs(numerator) > kDirectionEpsilon) {
                consider(numerator / quadratic);
                consider(constant / numerator);
            } else {
                consider(-linear / (2.0 * quadratic));
            }
        }
    }

    GeometrySnapResult result() const {
        QPointF correction;
        QVector<const Constraint *> alignments;
        GeometrySnapResult result;

        if (hasContact_) {
            correction = contact_;
            if (canPairContact_) {
                const Constraint *pairedAlignment = nullptr;
                double closestPairSquared = 2.0 * tolerance_ * tolerance_;
                for (const auto *axis : {&vertical_, &horizontal_}) {
                    QPointF paired;
                    if (!solvePair(contactConstraint_, *axis, paired)
                        || lengthSquared(paired) > closestPairSquared) continue;
                    const QPointF moved = contactPoint_.adjusted(paired);
                    const double fraction = closestFraction(moved, contactSegment_.p1(), contactSegment_.p2());
                    if (lengthSquared(moved - contactSegment_.pointAt(fraction)) <= kContactEpsilon * kContactEpsilon) {
                        correction = paired;
                        pairedAlignment = axis;
                        closestPairSquared = lengthSquared(paired);
                    }
                }
                if (pairedAlignment != nullptr) alignments.push_back(pairedAlignment);
            }
        } else if (vertical_.valid && horizontal_.valid) {
            if (!solvePair(vertical_, horizontal_, correction)
                || lengthSquared(correction) > 2.0 * tolerance_ * tolerance_) {
                const auto &axis = lengthSquared(vertical_.correction) <= lengthSquared(horizontal_.correction)
                    ? vertical_ : horizontal_;
                correction = axis.correction;
                alignments.push_back(&axis);
            } else {
                alignments = {&vertical_, &horizontal_};
            }
        } else if (vertical_.valid || horizontal_.valid) {
            const auto &axis = vertical_.valid ? vertical_ : horizontal_;
            correction = axis.correction;
            alignments.push_back(&axis);
        }
        result.adjustment = parameters_.map(correction);
        if (hasContact_) {
            auto indicator = contactIndicator_;
            if (canPairContact_) indicator.point = contactPoint_.adjusted(correction);
            result.indicators.push_back(indicator);
        }
        for (const auto *alignment : alignments) result.indicators.push_back(alignmentIndicator(*alignment, correction));

        return result;
    }

private:
    bool contactPoint(const MotionPoint &point, const QPointF &target) {
        const QPointF offset = target - point.position;
        const Constraint horizontal = constraint(point, QPointF(1.0, 0.0), offset.x());
        const Constraint vertical = constraint(point, QPointF(0.0, 1.0), offset.y());
        QPointF correction;

        if (lengthSquared(offset) > tolerance_ * tolerance_) {
            return false;
        }
        if (!solvePair(horizontal, vertical, correction)) {
            const auto &candidate = lengthSquared(horizontal.row) >= lengthSquared(vertical.row) ? horizontal : vertical;
            if (!candidate.valid) return false;
            correction = candidate.correction;
        }
        if (lengthSquared(point.adjusted(correction) - target) <= kContactEpsilon * kContactEpsilon
            && acceptContact(correction)) {
            contactIndicator_ = {QLineF(target, target), target, GeometrySnapIndicatorKind::Contact};
            return true;
        }

        return false;
    }

    bool acceptContact(const QPointF &correction) {
        const double distanceSquared = lengthSquared(correction);

        if (std::isfinite(distanceSquared) && distanceSquared <= tolerance_ * tolerance_
            && (!hasContact_ || distanceSquared < closestContactSquared_)) {
            closestContactSquared_ = distanceSquared;
            contact_ = correction;
            hasContact_ = true;
            canPairContact_ = false;
            return true;
        }

        return false;
    }

    const GeometrySnapMotion &motion_;
    QTransform parameters_;
    Constraint horizontal_;
    Constraint vertical_;
    Constraint contactConstraint_;
    MotionPoint contactPoint_;
    QLineF contactSegment_;
    GeometrySnapIndicator contactIndicator_;
    QPointF contact_;
    bool valid_ = false;
    bool hasContact_ = false;
    bool canPairContact_ = false;
    double tolerance_;
    double closestContactSquared_;
};

void alignOutline(SnapSearch &search, const QVector<MotionPoint> &points,
                  const GeometrySnapTargets *targets, const QVector<double> &verticalGuidelines,
                  const QVector<double> &horizontalGuidelines) {
    const auto horizontalOrder = [](const MotionPoint &first, const MotionPoint &second) {
        return first.position.x() < second.position.x();
    };
    const auto verticalOrder = [](const MotionPoint &first, const MotionPoint &second) {
        return first.position.y() < second.position.y();
    };
    const auto [left, right] = std::minmax_element(points.cbegin(), points.cend(), horizontalOrder);
    const auto [top, bottom] = std::minmax_element(points.cbegin(), points.cend(), verticalOrder);

    for (const auto &point : {*left, interpolate(*left, *right, 0.5), *right}) {
        search.align(point, verticalGuidelines, false);
        if (targets != nullptr) search.alignNearest(point, *targets, false);
    }
    for (const auto &point : {*top, interpolate(*top, *bottom, 0.5), *bottom}) {
        search.align(point, horizontalGuidelines, true);
        if (targets != nullptr) search.alignNearest(point, *targets, true);
    }
}

} // namespace

GeometrySnapOutline geometrySnapOutline(const QPainterPath &worldPath, double pixelsPerWorld) {
    const double scale = std::max(pixelsPerWorld, kMinimumSamplingScale);
    const QTransform sampling = QTransform::fromScale(scale, scale);
    const QTransform inverse = QTransform::fromScale(1.0 / scale, 1.0 / scale);
    GeometrySnapOutline result;

    for (const auto &polygon : worldPath.toSubpathPolygons(sampling)) {
        if (polygon.size() > 1) result.contours.push_back(inverse.map(polygon));
    }

    return result;
}

void GeometrySnapTargets::setOutlines(const QVector<GeometrySnapOutline> &outlines) {
    segments_.clear();
    nodes_.clear();
    horizontalCoordinates_.clear();
    verticalCoordinates_.clear();
    horizontalLines_.clear();
    verticalLines_.clear();
    for (const auto &outline : outlines) {
        QRectF bounds;
        bool hasBounds = false;

        for (const auto &polygon : outline.contours) {
            const QRectF polygonBounds = polygon.boundingRect();
            bounds = hasBounds ? bounds.united(polygonBounds) : polygonBounds;
            hasBounds = true;
            for (qsizetype index = 1; index < polygon.size(); ++index) {
                const QLineF line(polygon[index - 1], polygon[index]);
                segments_.push_back({line, QRectF(line.p1(), line.p2()).normalized()});
            }
        }
        if (hasBounds) {
            verticalCoordinates_.append({bounds.left(), bounds.center().x(), bounds.right()});
            horizontalCoordinates_.append({bounds.top(), bounds.center().y(), bounds.bottom()});
            for (double coordinate : {bounds.left(), bounds.center().x(), bounds.right()}) {
                extendAlignmentLine(verticalLines_, QLineF(coordinate, bounds.top(), coordinate, bounds.bottom()), false);
            }
            for (double coordinate : {bounds.top(), bounds.center().y(), bounds.bottom()}) {
                extendAlignmentLine(horizontalLines_, QLineF(bounds.left(), coordinate, bounds.right(), coordinate), true);
            }
        }
    }
    sortCoordinates(horizontalCoordinates_);
    sortCoordinates(verticalCoordinates_);
    if (!segments_.isEmpty()) buildNode(0, static_cast<int>(segments_.size()));
}

QLineF GeometrySnapTargets::alignmentLine(double coordinate, bool horizontal) const {
    return horizontal ? horizontalLines_.value(coordinate) : verticalLines_.value(coordinate);
}

int GeometrySnapTargets::buildNode(int first, int count) {
    QRectF bounds = segments_[first].bounds;
    const int nodeIndex = static_cast<int>(nodes_.size());

    for (int index = first + 1; index < first + count; ++index) {
        const auto &segmentBounds = segments_[index].bounds;
        bounds.setCoords(std::min(bounds.left(), segmentBounds.left()), std::min(bounds.top(), segmentBounds.top()),
                         std::max(bounds.right(), segmentBounds.right()), std::max(bounds.bottom(), segmentBounds.bottom()));
    }
    nodes_.push_back({bounds, first, count});
    if (count > kLeafSegmentCount) {
        const int middle = first + count / 2;
        const bool horizontal = bounds.width() >= bounds.height();
        std::nth_element(segments_.begin() + first, segments_.begin() + middle, segments_.begin() + first + count,
            [horizontal](const Segment &left, const Segment &right) {
                return horizontal ? left.bounds.center().x() < right.bounds.center().x()
                                  : left.bounds.center().y() < right.bounds.center().y();
            });
        const int left = buildNode(first, middle - first);
        const int right = buildNode(middle, first + count - middle);
        nodes_[nodeIndex].left = left;
        nodes_[nodeIndex].right = right;
    }

    return nodeIndex;
}

void GeometrySnapTargets::collectSegments(int nodeIndex, const QRectF &bounds, QVector<QLineF> &result) const {
    const auto &node = nodes_[nodeIndex];

    if (!overlaps(node.bounds, bounds)) return;
    if (node.left >= 0) {
        collectSegments(node.left, bounds, result);
        collectSegments(node.right, bounds, result);
        return;
    }
    for (int index = node.first; index < node.first + node.count; ++index) {
        if (overlaps(segments_[index].bounds, bounds)) result.push_back(segments_[index].line);
    }
}

QVector<QLineF> GeometrySnapTargets::nearbySegments(const QRectF &bounds) const {
    QVector<QLineF> result;

    if (!nodes_.isEmpty()) collectSegments(0, bounds, result);

    return result;
}

GeometrySnapResult snapGeometry(const QVector<GeometrySnapOutline> &outlines,
                              const GeometrySnapTargets *targets, const GeometrySnapMotion &motion,
                              const QVector<double> &verticalGuidelines,
                              const QVector<double> &horizontalGuidelines, double tolerance) {
    SnapSearch search(motion, tolerance);

    if (!search.valid()) return {};
    for (const auto &outline : outlines) {
        QVector<MotionPoint> points;

        for (const auto &polygon : outline.contours) {
            for (const auto &start : polygon) points.push_back(search.point(start));
            if (targets == nullptr) continue;
            for (qsizetype index = 0; index < polygon.size(); ++index) {
                const MotionPoint point = search.point(polygon[index]);
                const QRectF query = expanded(QRectF(point.position, point.position), tolerance);

                for (const auto &segment : targets->nearbySegments(query)) search.contact(point, segment);
                if (index == 0) continue;
                const MotionPoint first = search.point(polygon[index - 1]);
                const QRectF edgeQuery = expanded(QRectF(first.position, point.position).normalized(), tolerance);
                for (const auto &segment : targets->nearbySegments(edgeQuery)) {
                    search.contactEdge(first, point, segment.p1());
                    search.contactEdge(first, point, segment.p2());
                }
            }
        }
        if (!points.isEmpty()) alignOutline(search, points, targets, verticalGuidelines, horizontalGuidelines);
    }

    return search.result();
}

} // namespace gui
