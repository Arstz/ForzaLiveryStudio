#include "region_fill.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {

void sortRegionFillLayersByDrawOrder(QVector<RegionFillLayer> *layers) {
    if (layers == nullptr) {
        return;
    }
    std::stable_sort(layers->begin(), layers->end(),
                     [](const RegionFillLayer &left, const RegionFillLayer &right) {
                         if (left.variant != right.variant) {
                             return left.variant == RegionFillVariant::Safe;
                         }
                         return left.drawOrder < right.drawOrder;
                     });
}

namespace {

constexpr double kGeometryEpsilon = 1e-9;
constexpr int kBoundarySamplesPerCurve = 32;
constexpr int kCircleShapeId = 102;
constexpr int kEllipseFitIterations = 8;
constexpr int kEllipseGridSize = 3;
constexpr int kMaximumCoreEllipses = 8;
constexpr int kMinimumEllipseMeshPlacements = 12;
constexpr double kMinimumEllipseScale = 0.1;
constexpr double kMaximumCoreEllipseMismatchFraction = 0.005;
constexpr std::array<double, 2> kEllipseAngles = {0.0, 45.0};
constexpr std::array<double, 5> kEllipseScales = {0.8, 0.9, 1.0, 1.1, 1.2};

struct Op {
    enum Kind { Line, Cubic } kind = Line;
    QPointF control1;
    QPointF control2;
    QPointF end;
};

struct Subpath {
    QPointF start;
    QVector<Op> ops;
    bool closed = false;
};

struct ConvertiblePoint {
    PenPoint point;
    bool removable = false;
};

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &a = polygon[i];
        const QPointF &b = polygon[(i + 1) % polygon.size()];
        result += a.x() * b.y() - a.y() * b.x();
    }
    return result * 0.5;
}

QPointF cubicPoint(const QPointF &p0, const QPointF &c1, const QPointF &c2, const QPointF &p3, double t) {
    const double u = 1.0 - t;
    return p0 * (u * u * u)
        + c1 * (3.0 * u * u * t)
        + c2 * (3.0 * u * t * t)
        + p3 * (t * t * t);
}

QVector<Subpath> toSubpaths(const QPainterPath &path, double closureTolerance = 1e-6) {
    QVector<Subpath> subpaths;
    Subpath current;
    bool have = false;
    const auto finishCurrent = [&]() {
        if (!have || current.ops.isEmpty()) {
            return;
        }
        current.closed = QLineF(current.ops.back().end, current.start).length()
            <= closureTolerance;
        subpaths.push_back(current);
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        if (element.isMoveTo()) {
            finishCurrent();
            current = Subpath{};
            current.start = QPointF(element.x, element.y);
            have = true;
        } else if (element.isLineTo()) {
            if (!have) {
                continue;
            }
            Op op;
            op.kind = Op::Line;
            op.end = QPointF(element.x, element.y);
            current.ops.push_back(op);
        } else if (element.type == QPainterPath::CurveToElement) {
            if (!have || i + 2 >= path.elementCount()) {
                current.ops.clear();
                continue;
            }
            Op op;
            op.kind = Op::Cubic;
            op.control1 = QPointF(element.x, element.y);
            op.control2 = QPointF(path.elementAt(i + 1).x, path.elementAt(i + 1).y);
            op.end = QPointF(path.elementAt(i + 2).x, path.elementAt(i + 2).y);
            current.ops.push_back(op);
            i += 2;
        }
    }
    finishCurrent();
    return subpaths;
}

QPolygonF flattenSubpath(const Subpath &subpath, int curveSamples = 8) {
    QPolygonF polygon;
    polygon.push_back(subpath.start);
    QPointF previous = subpath.start;
    for (const Op &op : subpath.ops) {
        if (op.kind == Op::Line) {
            polygon.push_back(op.end);
        } else {
            for (int step = 1; step <= curveSamples; ++step) {
                polygon.push_back(cubicPoint(previous,
                                             op.control1,
                                             op.control2,
                                             op.end,
                                             static_cast<double>(step) / curveSamples));
            }
        }
        previous = op.end;
    }
    return polygon;
}

double perpendicularDistance(const QPointF &point, const QPointF &a, const QPointF &b) {
    const QPointF ab = b - a;
    const double lengthSquared = ab.x() * ab.x() + ab.y() * ab.y();
    if (lengthSquared <= 1e-12) {
        return QLineF(point, a).length();
    }
    const double t = std::clamp(
        ((point.x() - a.x()) * ab.x() + (point.y() - a.y()) * ab.y()) / lengthSquared,
        0.0,
        1.0);
    const QPointF projection = a + t * ab;
    return QLineF(point, projection).length();
}

double pointToClosedPolylineDistance(const QPointF &point, const QPolygonF &polyline) {
    if (polyline.isEmpty()) {
        return std::numeric_limits<double>::infinity();
    }
    double best = std::numeric_limits<double>::infinity();
    for (int i = 0; i < polyline.size(); ++i) {
        best = std::min(best,
                        perpendicularDistance(point,
                                              polyline[i],
                                              polyline[(i + 1) % polyline.size()]));
    }
    return best;
}

double boundaryDeviation(const QPolygonF &left, const QPolygonF &right) {
    if (left.size() < 3 || right.size() < 3) {
        return std::numeric_limits<double>::infinity();
    }
    double result = 0.0;
    for (const QPointF &point : left) {
        result = std::max(result, pointToClosedPolylineDistance(point, right));
    }
    for (const QPointF &point : right) {
        result = std::max(result, pointToClosedPolylineDistance(point, left));
    }
    return result;
}

QPolygonF flattenPenContour(const PenContour &contour, int curveSamples) {
    QPolygonF polygon;
    if (contour.segments.isEmpty()) {
        return polygon;
    }
    polygon.push_back(contour.segments.front().start);
    for (const PenBoundarySegment &segment : contour.segments) {
        if (!segment.curved) {
            polygon.push_back(segment.end);
            continue;
        }
        for (int step = 1; step <= curveSamples; ++step) {
            const double t = static_cast<double>(step) / curveSamples;
            const double u = 1.0 - t;
            polygon.push_back(segment.start * (u * u)
                              + segment.control * (2.0 * u * t)
                              + segment.end * (t * t));
        }
    }
    while (polygon.size() > 1
           && QLineF(polygon.back(), polygon.front()).length() <= kGeometryEpsilon) {
        polygon.removeLast();
    }
    return polygon;
}

struct SsimAccumulation {
    double sum = 0.0;
    qint64 count = 0;
};

QImage renderContourMask(const QPolygonF &polygon,
                         const QRect &sourceRect,
                         int supersample) {
    if (polygon.size() < 3 || sourceRect.isEmpty() || supersample < 1) {
        return {};
    }
    const qint64 highWidth = static_cast<qint64>(sourceRect.width()) * supersample;
    const qint64 highHeight = static_cast<qint64>(sourceRect.height()) * supersample;
    if (highWidth <= 0 || highHeight <= 0
        || highWidth > std::numeric_limits<int>::max()
        || highHeight > std::numeric_limits<int>::max()) {
        return {};
    }
    QImage high(static_cast<int>(highWidth),
                static_cast<int>(highHeight),
                QImage::Format_Grayscale8);
    if (high.isNull()) {
        return {};
    }
    high.fill(0);
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    path.addPolygon(polygon);
    path.closeSubpath();
    QPainter painter(&high);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.scale(supersample, supersample);
    painter.translate(-sourceRect.left(), -sourceRect.top());
    painter.fillPath(path, Qt::white);
    painter.end();
    return high.scaled(sourceRect.size(),
                       Qt::IgnoreAspectRatio,
                       Qt::SmoothTransformation)
        .convertToFormat(QImage::Format_Grayscale8);
}

SsimAccumulation ssimAccumulation(const QImage &left, const QImage &right) {
    SsimAccumulation result;
    if (left.isNull() || right.isNull() || left.size() != right.size()) {
        return result;
    }
    constexpr int window = 7;
    constexpr int radius = window / 2;
    constexpr double sampleCount = window * window;
    constexpr double covarianceDivisor = sampleCount - 1.0;
    constexpr double c1 = 6.5025;   // (0.01 * 255)^2
    constexpr double c2 = 58.5225;  // (0.03 * 255)^2
    const int width = left.width();
    const int height = left.height();
    if (width < window || height < window) {
        double sx = 0.0;
        double sy = 0.0;
        double sxx = 0.0;
        double syy = 0.0;
        double sxy = 0.0;
        for (int y = 0; y < height; ++y) {
            const uchar *a = left.constScanLine(y);
            const uchar *b = right.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                const double av = a[x];
                const double bv = b[x];
                sx += av;
                sy += bv;
                sxx += av * av;
                syy += bv * bv;
                sxy += av * bv;
            }
        }
        const double count = static_cast<double>(width) * height;
        if (count <= 0.0) {
            return result;
        }
        const double mx = sx / count;
        const double my = sy / count;
        const double divisor = std::max(1.0, count - 1.0);
        const double vx = std::max(0.0, (sxx - sx * sx / count) / divisor);
        const double vy = std::max(0.0, (syy - sy * sy / count) / divisor);
        const double covariance = (sxy - sx * sy / count) / divisor;
        result.sum = ((2.0 * mx * my + c1) * (2.0 * covariance + c2))
            / ((mx * mx + my * my + c1) * (vx + vy + c2));
        result.count = 1;
        return result;
    }

    using MomentRows = std::array<QVector<double>, 5>;
    std::array<MomentRows, window> ring;
    MomentRows vertical;
    MomentRows horizontal;
    for (int moment = 0; moment < 5; ++moment) {
        vertical[moment].fill(0.0, width);
        horizontal[moment].fill(0.0, width);
        for (int row = 0; row < window; ++row) {
            ring[row][moment].fill(0.0, width);
        }
    }

    for (int y = 0; y < height; ++y) {
        const uchar *a = left.constScanLine(y);
        const uchar *b = right.constScanLine(y);
        std::array<double, 5> rolling{};
        for (int x = 0; x < window; ++x) {
            const double av = a[x];
            const double bv = b[x];
            rolling[0] += av;
            rolling[1] += bv;
            rolling[2] += av * av;
            rolling[3] += bv * bv;
            rolling[4] += av * bv;
        }
        for (int center = radius; center < width - radius; ++center) {
            for (int moment = 0; moment < 5; ++moment) {
                horizontal[moment][center] = rolling[moment];
            }
            const int remove = center - radius;
            const int add = center + radius + 1;
            if (add < width) {
                const double removeA = a[remove];
                const double removeB = b[remove];
                const double addA = a[add];
                const double addB = b[add];
                rolling[0] += addA - removeA;
                rolling[1] += addB - removeB;
                rolling[2] += addA * addA - removeA * removeA;
                rolling[3] += addB * addB - removeB * removeB;
                rolling[4] += addA * addB - removeA * removeB;
            }
        }

        const int slot = y % window;
        for (int center = radius; center < width - radius; ++center) {
            for (int moment = 0; moment < 5; ++moment) {
                if (y >= window) {
                    vertical[moment][center] -= ring[slot][moment][center];
                }
                ring[slot][moment][center] = horizontal[moment][center];
                vertical[moment][center] += horizontal[moment][center];
            }
        }
        if (y < window - 1) {
            continue;
        }
        for (int center = radius; center < width - radius; ++center) {
            const double sx = vertical[0][center];
            const double sy = vertical[1][center];
            const double mx = sx / sampleCount;
            const double my = sy / sampleCount;
            const double vx = std::max(
                0.0, (vertical[2][center] - sx * sx / sampleCount)
                         / covarianceDivisor);
            const double vy = std::max(
                0.0, (vertical[3][center] - sy * sy / sampleCount)
                         / covarianceDivisor);
            const double covariance =
                (vertical[4][center] - sx * sy / sampleCount) / covarianceDivisor;
            result.sum += ((2.0 * mx * my + c1) * (2.0 * covariance + c2))
                / ((mx * mx + my * my + c1) * (vx + vy + c2));
            ++result.count;
        }
    }
    return result;
}

double contourDssim(const QPolygonF &baseline,
                    const QPolygonF &candidate,
                    const QSize &imageSize,
                    int supersample) {
    if (baseline.size() < 3 || candidate.size() < 3
        || !imageSize.isValid() || imageSize.isEmpty()) {
        return 0.0;
    }
    constexpr int ssimRadius = 3;
    constexpr int resamplingMargin = 5;
    const QRect imageRect(QPoint(0, 0), imageSize);
    const QRectF affected = baseline.boundingRect().united(candidate.boundingRect())
        .adjusted(-(ssimRadius + resamplingMargin),
                  -(ssimRadius + resamplingMargin),
                  ssimRadius + resamplingMargin,
                  ssimRadius + resamplingMargin);
    QRect sourceRect = affected.toAlignedRect().intersected(imageRect);
    if (sourceRect.width() < 7 || sourceRect.height() < 7) {
        sourceRect = sourceRect.adjusted(-4, -4, 4, 4).intersected(imageRect);
    }
    const QImage baselineMask = renderContourMask(baseline, sourceRect, supersample);
    const QImage candidateMask = renderContourMask(candidate, sourceRect, supersample);
    const SsimAccumulation local = ssimAccumulation(baselineMask, candidateMask);
    const qint64 fullCount = static_cast<qint64>(std::max(1, imageSize.width() - 6))
        * std::max(1, imageSize.height() - 6);
    if (local.count <= 0 || local.count > fullCount) {
        return std::numeric_limits<double>::infinity();
    }
    const double similarity =
        (local.sum + static_cast<double>(fullCount - local.count)) / fullCount;
    return (1.0 - std::clamp(similarity, -1.0, 1.0)) * 0.5;
}

QVector<PenPoint> penPoints(const QVector<ConvertiblePoint> &points) {
    QVector<PenPoint> result;
    result.reserve(points.size());
    for (const ConvertiblePoint &point : points) {
        result.push_back(point.point);
    }
    return result;
}

double removalDisplacement(const QVector<ConvertiblePoint> &points, int index) {
    if (points.size() < 3 || index < 0 || index >= points.size()
        || !points[index].removable
        || points[index].point.kind != PenPointKind::Hard) {
        return std::numeric_limits<double>::infinity();
    }
    const int previous = (index + points.size() - 1) % points.size();
    const int next = (index + 1) % points.size();
    if (points[previous].point.kind != PenPointKind::Soft
        || points[next].point.kind != PenPointKind::Soft) {
        return std::numeric_limits<double>::infinity();
    }
    const QPointF implied =
        (points[previous].point.position + points[next].point.position) * 0.5;
    return QLineF(points[index].point.position, implied).length();
}

double quadraticMaximumAbsolute(double start, double control, double end) {
    double result = std::max(std::abs(start), std::abs(end));
    const double denominator = start - 2.0 * control + end;
    if (std::abs(denominator) <= kGeometryEpsilon) {
        return result;
    }
    const double at = (start - control) / denominator;
    if (at <= 0.0 || at >= 1.0) {
        return result;
    }
    const double remaining = 1.0 - at;
    const double value = remaining * remaining * start
        + 2.0 * remaining * at * control + at * at * end;

    return std::max(result, std::abs(value));
}

double softRunDeviation(const QPointF &start,
                        const QVector<QPointF> &controls,
                        const QPointF &end) {
    const QPointF chord = end - start;
    const double chordLength = std::hypot(chord.x(), chord.y());
    if (controls.isEmpty()) {
        return 0.0;
    }
    if (chordLength <= kGeometryEpsilon) {
        return std::numeric_limits<double>::infinity();
    }
    const double chordLengthSquared = chordLength * chordLength;
    const auto signedOffset = [&](const QPointF &point) {
        return (chord.x() * (point.y() - start.y())
                - chord.y() * (point.x() - start.x())) / chordLength;
    };
    const auto chordPosition = [&](const QPointF &point) {
        return QPointF::dotProduct(point - start, chord) / chordLengthSquared;
    };

    double result = 0.0;
    QPointF segmentStart = start;
    for (int i = 0; i < controls.size(); ++i) {
        const QPointF &control = controls[i];
        const QPointF segmentEnd = i + 1 == controls.size()
            ? end : (control + controls[i + 1]) * 0.5;
        if (chordPosition(control) < -kGeometryEpsilon
            || chordPosition(control) > 1.0 + kGeometryEpsilon) {
            return std::numeric_limits<double>::infinity();
        }
        result = std::max(result,
                          quadraticMaximumAbsolute(signedOffset(segmentStart),
                                                   signedOffset(control),
                                                   signedOffset(segmentEnd)));
        segmentStart = segmentEnd;
    }

    return result;
}

struct SoftRunCollapseResult {
    QVector<ConvertiblePoint> points;
    int removedSoftPoints = 0;
};

SoftRunCollapseResult collapseNegligibleSoftRuns(
    const QVector<ConvertiblePoint> &points,
    double tolerance) {
    SoftRunCollapseResult result;
    const int hardPointCount = static_cast<int>(std::count_if(
        points.cbegin(), points.cend(), [](const ConvertiblePoint &point) {
            return point.point.kind == PenPointKind::Hard;
        }));
    if (tolerance <= 0.0 || hardPointCount < 2) {
        result.points = points;
        return result;
    }

    int firstHard = 0;
    while (points[firstHard].point.kind != PenPointKind::Hard) {
        ++firstHard;
    }
    result.points.reserve(points.size());
    int hard = firstHard;
    do {
        result.points.push_back(points[hard]);
        QVector<QPointF> controls;
        QVector<int> softIndices;
        int next = (hard + 1) % points.size();
        while (points[next].point.kind == PenPointKind::Soft) {
            controls.push_back(points[next].point.position);
            softIndices.push_back(next);
            next = (next + 1) % points.size();
        }
        if (!controls.isEmpty()
            && softRunDeviation(points[hard].point.position,
                                controls,
                                points[next].point.position)
                <= tolerance + kGeometryEpsilon) {
            result.removedSoftPoints += static_cast<int>(controls.size());
        } else {
            for (const int index : softIndices) {
                result.points.push_back(points[index]);
            }
        }
        hard = next;
    } while (hard != firstHard);

    return result;
}

bool sameOrientation(double referenceArea, double candidateArea) {
    return (referenceArea > kGeometryEpsilon && candidateArea > kGeometryEpsilon)
        || (referenceArea < -kGeometryEpsilon && candidateArea < -kGeometryEpsilon);
}

QVector<ConvertiblePoint> initialPenPoints(const Subpath &subpath,
                                           double closureTolerance) {
    QVector<ConvertiblePoint> points;
    if (subpath.ops.isEmpty()) {
        return points;
    }
    const int opCount = subpath.ops.size();
    const bool startRemovable = subpath.ops.back().kind == Op::Cubic
        && subpath.ops.front().kind == Op::Cubic;
    points.push_back({{subpath.start, PenPointKind::Hard}, startRemovable});
    QPointF previous = subpath.start;
    for (int i = 0; i < opCount; ++i) {
        const Op &op = subpath.ops[i];
        const bool closesAtStart = i == opCount - 1
            && QLineF(op.end, subpath.start).length() <= closureTolerance;
        if (op.kind == Op::Line) {
            if (!closesAtStart) {
                points.push_back({{op.end, PenPointKind::Hard}, false});
            }
        } else {
            const QPointF control =
                (op.control1 * 3.0 + op.control2 * 3.0 - previous - op.end) * 0.25;
            points.push_back({{control, PenPointKind::Soft}, false});
            if (!closesAtStart) {
                const Op &next = subpath.ops[(i + 1) % opCount];
                points.push_back({{op.end, PenPointKind::Hard},
                                  next.kind == Op::Cubic});
            }
        }
        previous = op.end;
    }
    return points;
}

void protectCyclicSeam(QVector<ConvertiblePoint> *points) {
    const bool hasProtectedHard = std::any_of(points->cbegin(),
                                              points->cend(),
                                              [](const ConvertiblePoint &point) {
        return point.point.kind == PenPointKind::Hard && !point.removable;
    });
    if (hasProtectedHard) {
        return;
    }
    int seam = -1;
    double largestDisplacement = -1.0;
    for (int i = 0; i < points->size(); ++i) {
        const double displacement = removalDisplacement(*points, i);
        if (std::isfinite(displacement) && displacement > largestDisplacement) {
            largestDisplacement = displacement;
            seam = i;
        }
    }
    if (seam >= 0) {
        (*points)[seam].removable = false;
    }
}

struct OuterSelection {
    Subpath subpath;
    QPolygonF polygon;
    QString error;
};

OuterSelection selectClosedOuter(const QPainterPath &outline,
                                 double closureTolerance) {
    OuterSelection result;
    const QVector<Subpath> subpaths = toSubpaths(outline, closureTolerance);
    if (subpaths.isEmpty()) {
        result.error = QStringLiteral("Region outline has no contour");
        return result;
    }
    QVector<QPolygonF> polygons;
    polygons.reserve(subpaths.size());
    for (const Subpath &subpath : subpaths) {
        if (!subpath.closed) {
            result.error = QStringLiteral("Region outline contains an open contour");
            return result;
        }
        QPolygonF polygon = flattenSubpath(subpath, kBoundarySamplesPerCurve);
        while (polygon.size() > 1
               && QLineF(polygon.back(), polygon.front()).length() <= closureTolerance) {
            polygon.removeLast();
        }
        if (polygon.size() < 3 || std::abs(signedArea(polygon)) <= kGeometryEpsilon) {
            result.error = QStringLiteral("Region outline contains a degenerate contour");
            return result;
        }
        polygons.push_back(std::move(polygon));
    }

    QVector<int> outerIndices;
    for (int i = 0; i < polygons.size(); ++i) {
        bool contained = false;
        const QPointF probe = polygons[i].front();
        for (int j = 0; j < polygons.size(); ++j) {
            if (i != j && polygons[j].containsPoint(probe, Qt::OddEvenFill)) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            outerIndices.push_back(i);
        }
    }
    if (outerIndices.size() != 1) {
        result.error = outerIndices.isEmpty()
            ? QStringLiteral("Region outline has no outer contour")
            : QStringLiteral("Region outline contains multiple outer contours");
        return result;
    }
    result.subpath = subpaths[outerIndices.front()];
    result.polygon = polygons[outerIndices.front()];
    return result;
}

void rdpRecurse(const QPolygonF &points, int first, int last, double epsilon, QPolygonF &out) {
    double maxDistance = 0.0;
    int index = first;
    for (int i = first + 1; i < last; ++i) {
        const double distance = perpendicularDistance(points[i], points[first], points[last]);
        if (distance > maxDistance) {
            maxDistance = distance;
            index = i;
        }
    }
    if (maxDistance > epsilon && index > first) {
        rdpRecurse(points, first, index, epsilon, out);
        rdpRecurse(points, index, last, epsilon, out);
    } else {
        out.push_back(points[last]);
    }
}

void rdpCorridorRecurse(const QPolygonF &points, int first, int last, double epsilon,
                        const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace,
                        QPolygonF &out) {
    double maxDistance = 0.0;
    int index = first;
    for (int i = first + 1; i < last; ++i) {
        const double distance = perpendicularDistance(points[i], points[first], points[last]);
        if (distance > maxDistance) {
            maxDistance = distance;
            index = i;
        }
    }
    const bool accept = maxDistance <= epsilon
        || (chordInFreeSpace && chordInFreeSpace(points[first], points[last]));
    if (!accept && index > first) {
        rdpCorridorRecurse(points, first, index, epsilon, chordInFreeSpace, out);
        rdpCorridorRecurse(points, index, last, epsilon, chordInFreeSpace, out);
    } else {
        out.push_back(points[last]);
    }
}

QPolygonF largestFlattenedContour(const QPainterPath &outline) {
    const QVector<Subpath> subpaths = toSubpaths(outline);
    if (subpaths.isEmpty()) {
        return {};
    }
    int outer = 0;
    double outerArea = -1.0;
    QPolygonF outerPolygon;
    for (int i = 0; i < subpaths.size(); ++i) {
        const QPolygonF polygon = flattenSubpath(subpaths[i]);
        const double area = std::abs(signedArea(polygon));
        if (area > outerArea) {
            outerArea = area;
            outer = i;
            outerPolygon = polygon;
        }
    }
    Q_UNUSED(outer);
    while (outerPolygon.size() > 1
           && QLineF(outerPolygon.back(), outerPolygon.front()).length() <= 1e-6) {
        outerPolygon.removeLast();
    }
    return outerPolygon;
}

QPainterPath closedPolygonPath(const QPolygonF &polygon) {
    QPainterPath path;
    if (polygon.size() >= 3) {
        path.addPolygon(polygon);
        path.closeSubpath();
    }

    return path;
}

double filledPathArea(const QPainterPath &path) {
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        result += std::abs(signedArea(polygon));
    }

    return result;
}

QTransform ellipseTransform(const QRectF &sourceBounds,
                            const QPointF &center,
                            const QSizeF &size,
                            double angle,
                            double scale) {
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(angle);
    transform.scale(size.width() * scale / sourceBounds.width(),
                    size.height() * scale / sourceBounds.height());
    transform.translate(-sourceBounds.center().x(), -sourceBounds.center().y());

    return transform;
}

} // namespace

QVector<PolygonMeshPlacement> optimizePolygonMeshWithEllipses(
    const QVector<PolygonMeshPlacement> &placements,
    const PolygonMeshSources &sources,
    const QPainterPath &corePath,
    const std::function<bool()> &cancelled) {
    struct EllipseCandidate {
        QPainterPath path;
        QTransform transform;
        QVector<int> coveredPlacements;
        double area = 0.0;
    };

    if (placements.size() < kMinimumEllipseMeshPlacements
        || sources.circle.size() < 3 || corePath.isEmpty()) {
        return placements;
    }
    const QPainterPath circlePath = closedPolygonPath(sources.circle);
    const QRectF circleBounds = circlePath.boundingRect();
    const QRectF coreBounds = corePath.boundingRect();
    if (circleBounds.isEmpty() || coreBounds.isEmpty()) {
        return placements;
    }
    const double minimumCoreSide = std::min(coreBounds.width(), coreBounds.height());
    const QVector<QSizeF> sizes = {
        coreBounds.size(),
        QSizeF(minimumCoreSide, minimumCoreSide),
        QSizeF(coreBounds.width(),
               std::min(coreBounds.height(), coreBounds.width() * 0.5)),
        QSizeF(std::min(coreBounds.width(), coreBounds.height() * 0.5),
               coreBounds.height()),
    };
    const double coreArea = filledPathArea(corePath);
    double bestWholeCoreMismatch = std::numeric_limits<double>::max();
    QTransform bestWholeCoreTransform;
    QVector<QPainterPath> placementPaths;
    placementPaths.reserve(placements.size());
    for (const PolygonMeshPlacement &placement : placements) {
        const QPolygonF &source = placement.shapeId == 101
            ? sources.square : sources.triangle;
        placementPaths.push_back(
            placement.transform.map(closedPolygonPath(source)));
    }
    QVector<EllipseCandidate> candidates;
    for (int gridY = 0; gridY < kEllipseGridSize; ++gridY) {
        for (int gridX = 0; gridX < kEllipseGridSize; ++gridX) {
            if (cancelled && cancelled()) {
                return placements;
            }
            const QPointF center(
                coreBounds.left() + coreBounds.width()
                    * (static_cast<double>(gridX) + 0.5) / kEllipseGridSize,
                coreBounds.top() + coreBounds.height()
                    * (static_cast<double>(gridY) + 0.5) / kEllipseGridSize);
            if (!corePath.contains(center)) {
                continue;
            }
            for (const QSizeF &size : sizes) {
                for (const double angle : kEllipseAngles) {
                    for (const double scale : kEllipseScales) {
                        const QTransform transform = ellipseTransform(
                            circleBounds, center, size, angle, scale);
                        const QPainterPath ellipse = transform.map(circlePath);
                        const double mismatch = filledPathArea(
                            corePath.subtracted(ellipse))
                            + filledPathArea(ellipse.subtracted(corePath));
                        if (mismatch < bestWholeCoreMismatch) {
                            bestWholeCoreMismatch = mismatch;
                            bestWholeCoreTransform = transform;
                        }
                    }
                    double lowerScale = 0.0;
                    double upperScale = 1.0;
                    for (int iteration = 0; iteration < kEllipseFitIterations; ++iteration) {
                        const double scale = (lowerScale + upperScale) * 0.5;
                        const QPainterPath ellipse = ellipseTransform(
                            circleBounds, center, size, angle, scale).map(circlePath);
                        if (corePath.contains(ellipse)) {
                            lowerScale = scale;
                        } else {
                            upperScale = scale;
                        }
                    }
                    if (lowerScale < kMinimumEllipseScale) {
                        continue;
                    }
                    EllipseCandidate candidate;
                    candidate.transform = ellipseTransform(
                        circleBounds, center, size, angle, lowerScale);
                    candidate.path = candidate.transform.map(circlePath);
                    candidate.area = candidate.path.boundingRect().width()
                        * candidate.path.boundingRect().height();
                    for (int placementIndex = 0;
                         placementIndex < placementPaths.size(); ++placementIndex) {
                        if (candidate.path.contains(placementPaths[placementIndex])) {
                            candidate.coveredPlacements.push_back(placementIndex);
                        }
                    }
                    if (candidate.coveredPlacements.size() > 1) {
                        candidates.push_back(std::move(candidate));
                    }
                }
            }
        }
    }
    if (coreArea > kGeometryEpsilon
        && bestWholeCoreMismatch / coreArea
            <= kMaximumCoreEllipseMismatchFraction) {
        return {{kCircleShapeId, bestWholeCoreTransform}};
    }
    QVector<bool> covered(placements.size(), false);
    QVector<PolygonMeshPlacement> ellipses;
    for (int ellipseIndex = 0;
         ellipseIndex < kMaximumCoreEllipses; ++ellipseIndex) {
        int bestCandidate = -1;
        int bestSavings = 0;
        double bestArea = 0.0;
        for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            int newlyCovered = 0;
            for (const int placementIndex : candidates[candidateIndex].coveredPlacements) {
                newlyCovered += covered[placementIndex] ? 0 : 1;
            }
            const int savings = newlyCovered - 1;
            if (savings > bestSavings
                || (savings == bestSavings
                    && candidates[candidateIndex].area > bestArea)) {
                bestCandidate = candidateIndex;
                bestSavings = savings;
                bestArea = candidates[candidateIndex].area;
            }
        }
        if (bestCandidate < 0 || bestSavings <= 0) {
            break;
        }
        const EllipseCandidate &selected = candidates[bestCandidate];
        ellipses.push_back({kCircleShapeId, selected.transform});
        for (const int placementIndex : selected.coveredPlacements) {
            covered[placementIndex] = true;
        }
    }
    if (ellipses.isEmpty()) {
        return placements;
    }
    QVector<PolygonMeshPlacement> result = std::move(ellipses);
    result.reserve(result.size() + placements.size());
    for (int placementIndex = 0; placementIndex < placements.size(); ++placementIndex) {
        if (!covered[placementIndex]) {
            result.push_back(placements[placementIndex]);
        }
    }

    return result;
}

RegionPenConversionResult regionOutlineToPenPoints(
    const QPainterPath &outline,
    const RegionPenConversionOptions &options) {
    RegionPenConversionResult result;
    if (!std::isfinite(options.mergeTolerance) || options.mergeTolerance < 0.0
        || !std::isfinite(options.maximumDssim) || options.maximumDssim < 0.0
        || options.maximumDssim > 1.0
        || !std::isfinite(options.closureTolerance) || options.closureTolerance <= 0.0
        || options.dssimSupersample < 1 || options.dssimSupersample > 8
        || options.maxOptimizedPointCount < 0
        || options.adaptiveSearchSteps < 0 || options.adaptiveSearchSteps > 16
        || ((options.comparisonImageSize.width() > 0)
            != (options.comparisonImageSize.height() > 0))) {
        result.error = QStringLiteral("Region Pen conversion options are invalid");
        return result;
    }

    const OuterSelection outer = selectClosedOuter(outline, options.closureTolerance);
    if (!outer.error.isEmpty()) {
        result.error = outer.error;
        return result;
    }

    QVector<ConvertiblePoint> working =
        initialPenPoints(outer.subpath, options.closureTolerance);
    protectCyclicSeam(&working);
    result.originalPointCount = working.size();
    const PenContour baseline = buildPenContour(penPoints(working));
    if (!baseline.valid()) {
        result.error = baseline.error.isEmpty()
            ? QStringLiteral("Region outline does not form a valid Pen contour")
            : baseline.error;
        return result;
    }
    if (options.maxOptimizedPointCount > 0
        && working.size() > options.maxOptimizedPointCount) {
        result.points = penPoints(working);
        result.optimizationSkipped = true;
        return result;
    }

    const QPolygonF baselinePolygon =
        flattenPenContour(baseline, kBoundarySamplesPerCurve);
    const double referenceArea = signedArea(baselinePolygon);
    constexpr qint64 kDeviationComparisonLimit = 2'000'000;
    const bool measureBaselineDeviation =
        static_cast<qint64>(outer.polygon.size()) * baselinePolygon.size()
        <= kDeviationComparisonLimit;
    if (measureBaselineDeviation) {
        result.baselineDeviation = boundaryDeviation(outer.polygon, baselinePolygon);
    }
    result.maximumDeviation = result.baselineDeviation;

    struct EvaluatedCandidate {
        QVector<ConvertiblePoint> points;
        QPolygonF polygon;
        int removedHardPoints = 0;
        int removedSoftPoints = 0;
        double dssim = 0.0;
        double deviation = 0.0;
        bool valid = false;
    };
    const auto evaluate = [&](double tolerance) {
        EvaluatedCandidate evaluated;
        evaluated.points.reserve(working.size());
        for (int i = 0; i < working.size(); ++i) {
            const double displacement = removalDisplacement(working, i);
            if (std::isfinite(displacement)
                && displacement <= tolerance + kGeometryEpsilon) {
                ++evaluated.removedHardPoints;
                continue;
            }
            evaluated.points.push_back(working[i]);
        }
        if (options.straightenSoftRuns) {
            SoftRunCollapseResult collapsed =
                collapseNegligibleSoftRuns(evaluated.points, tolerance);
            evaluated.removedSoftPoints = collapsed.removedSoftPoints;
            evaluated.points = std::move(collapsed.points);
        }
        if (evaluated.points.size() == working.size()) {
            evaluated.polygon = baselinePolygon;
            evaluated.deviation = result.baselineDeviation;
            evaluated.valid = true;
            return evaluated;
        }
        const PenContour contour = buildPenContour(penPoints(evaluated.points));
        if (!contour.valid()) {
            return evaluated;
        }
        evaluated.polygon = flattenPenContour(contour, kBoundarySamplesPerCurve);
        if (!sameOrientation(referenceArea, signedArea(evaluated.polygon))) {
            return evaluated;
        }
        evaluated.dssim = contourDssim(baselinePolygon,
                                       evaluated.polygon,
                                       options.comparisonImageSize,
                                       options.dssimSupersample);
        if (!std::isfinite(evaluated.dssim)
            || evaluated.dssim > options.maximumDssim + kGeometryEpsilon) {
            return evaluated;
        }
        if (static_cast<qint64>(outer.polygon.size()) * evaluated.polygon.size()
            <= kDeviationComparisonLimit) {
            evaluated.deviation = boundaryDeviation(outer.polygon, evaluated.polygon);
        } else {
            evaluated.deviation = result.baselineDeviation;
        }
        evaluated.valid = true;
        return evaluated;
    };

    EvaluatedCandidate best = evaluate(options.mergeTolerance);
    if (!best.valid && options.mergeTolerance > 0.0) {
        double safeTolerance = 0.0;
        double unsafeTolerance = options.mergeTolerance;
        for (int step = 0; step < options.adaptiveSearchSteps; ++step) {
            const double middle = (safeTolerance + unsafeTolerance) * 0.5;
            EvaluatedCandidate candidate = evaluate(middle);
            if (candidate.valid) {
                safeTolerance = middle;
                best = std::move(candidate);
            } else {
                unsafeTolerance = middle;
            }
        }
    }
    if (best.valid) {
        result.removedHardPoints = best.removedHardPoints;
        result.removedSoftPoints = best.removedSoftPoints;
        result.maximumDeviation = best.deviation;
        result.dssim = best.dssim;
        working = std::move(best.points);
    }

    result.points = penPoints(working);
    return result;
}

PenFillResult fillRegionOutline(const QPainterPath &outline,
                                const QVector<PenPrimitive> &primitives,
                                double boundaryTolerance,
                                const std::function<bool()> &cancelled,
                                QPolygonF *optimizedContour,
                                RegionFillContourStats *contourStats,
                                QVector<PenPoint> *optimizedPenPoints,
                                const QSize &comparisonImageSize) {
    PenFillResult result;
    if (optimizedContour != nullptr) {
        optimizedContour->clear();
    }
    if (contourStats != nullptr) {
        *contourStats = RegionFillContourStats{};
    }
    if (optimizedPenPoints != nullptr) {
        optimizedPenPoints->clear();
    }
    RegionPenConversionOptions conversionOptions;
    conversionOptions.comparisonImageSize = comparisonImageSize;
    RegionPenConversionResult conversion =
        regionOutlineToPenPoints(outline, conversionOptions);
    if (!conversion.valid()) {
        result.error = conversion.error.isEmpty()
            ? QStringLiteral("Region outline has no fillable contour")
            : conversion.error;
        return result;
    }

    PenFillRequest request;
    request.points = conversion.points;
    request.primitives = primitives;
    request.boundaryTolerance = boundaryTolerance;
    result = fillPenPath(request, cancelled);
    const auto succeeded = [](const PenFillResult &fill) {
        return fill.error.isEmpty() && !fill.placements.isEmpty() && !fill.cancelled;
    };
    const auto canRetry = [&]() {
        return !succeeded(result) && !result.cancelled
            && !(cancelled && cancelled());
    };
    const auto appendRetryError = [&result](const QString &stage, const QString &error) {
        if (error.isEmpty()) {
            return;
        }
        const QString retryError = QStringLiteral("%1 retry: %2").arg(stage, error);
        result.error = result.error.isEmpty()
            ? retryError : result.error + QStringLiteral("; ") + retryError;
    };
    RegionPenConversionResult hardOnlyFallback;
    bool usedSoftRunRetry = false;
    bool usedBaselineRetry = false;
    bool haveHardOnlyFallback = false;
    if (canRetry() && conversion.removedSoftPoints > 0) {
        RegionPenConversionOptions hardOnlyOptions = conversionOptions;
        hardOnlyOptions.straightenSoftRuns = false;
        RegionPenConversionResult hardOnly =
            regionOutlineToPenPoints(outline, hardOnlyOptions);
        if (hardOnly.valid()) {
            hardOnlyFallback = hardOnly;
            haveHardOnlyFallback = true;
        }
        if (hardOnly.valid() && hardOnly.points.size() != conversion.points.size()) {
            request.points = hardOnly.points;
            PenFillResult retry = fillPenPath(request, cancelled);
            if (succeeded(retry)) {
                result = std::move(retry);
                conversion = std::move(hardOnly);
                usedSoftRunRetry = true;
            } else if (retry.cancelled) {
                result = std::move(retry);
            } else {
                appendRetryError(QStringLiteral("hard-only"), retry.error);
            }
        }
    }
    if (canRetry()
        && conversion.removedHardPoints + conversion.removedSoftPoints > 0) {
        RegionPenConversionOptions baselineOptions = conversionOptions;
        baselineOptions.mergeTolerance = 0.0;
        baselineOptions.straightenSoftRuns = false;
        RegionPenConversionResult baseline =
            regionOutlineToPenPoints(outline, baselineOptions);
        if (baseline.valid() && baseline.points.size() != conversion.points.size()) {
            request.points = baseline.points;
            PenFillResult retry = fillPenPath(request, cancelled);
            if (succeeded(retry)) {
                result = std::move(retry);
                conversion = std::move(baseline);
                usedBaselineRetry = true;
            } else if (retry.cancelled) {
                result = std::move(retry);
            } else {
                appendRetryError(QStringLiteral("baseline"), retry.error);
            }
        }
    }
    if (!succeeded(result) && haveHardOnlyFallback) {
        conversion = std::move(hardOnlyFallback);
    }

    if (contourStats != nullptr) {
        contourStats->originalPointCount = conversion.originalPointCount;
        contourStats->optimizedPointCount = conversion.points.size();
        contourStats->removedHardPoints = conversion.removedHardPoints;
        contourStats->removedSoftPoints = conversion.removedSoftPoints;
        contourStats->optimizationSkipped = conversion.optimizationSkipped;
        contourStats->softRunRetry = usedSoftRunRetry;
        contourStats->baselineRetry = usedBaselineRetry;
        contourStats->dssim = conversion.dssim;
    }
    if (optimizedPenPoints != nullptr) {
        *optimizedPenPoints = conversion.points;
    }
    if (optimizedContour != nullptr || contourStats != nullptr) {
        const PenContour contour = buildPenContour(conversion.points);
        if (contour.valid()) {
            const QPolygonF flattened =
                flattenPenContour(contour, kBoundarySamplesPerCurve);
            if (optimizedContour != nullptr) {
                *optimizedContour = flattened;
            }
            if (contourStats != nullptr) {
                contourStats->flattenedPointCount = flattened.size();
            }
        }
    }
    return result;
}

QPolygonF simplifyClosedPolygon(const QPolygonF &polygon, double epsilon) {
    if (epsilon <= 0.0 || polygon.size() <= 4) {
        return polygon;
    }
    int anchorA = 0;
    int anchorB = 0;
    double maxSpan = -1.0;
    for (int i = 1; i < polygon.size(); ++i) {
        const double span = QLineF(polygon[0], polygon[i]).length();
        if (span > maxSpan) {
            maxSpan = span;
            anchorB = i;
        }
    }
    Q_UNUSED(anchorA);
    QPolygonF chain = polygon;
    QPolygonF result;
    result.push_back(chain[0]);
    rdpRecurse(chain, 0, anchorB, epsilon, result);
    QPolygonF secondHalf;
    for (int i = anchorB; i < chain.size(); ++i) {
        secondHalf.push_back(chain[i]);
    }
    secondHalf.push_back(chain[0]);
    QPolygonF secondSimplified;
    secondSimplified.push_back(secondHalf.front());
    rdpRecurse(secondHalf, 0, secondHalf.size() - 1, epsilon, secondSimplified);
    for (int i = 1; i + 1 < secondSimplified.size(); ++i) {
        result.push_back(secondSimplified[i]);
    }
    if (result.size() < 3) {
        return polygon;
    }
    return result;
}

QPolygonF simplifyClosedPolygonCorridor(
    const QPolygonF &polygon, double epsilon,
    const std::function<bool(const QPointF &, const QPointF &)> &chordInFreeSpace) {
    if (polygon.size() <= 4) {
        return polygon;
    }
    int anchorB = 0;
    double maxSpan = -1.0;
    for (int i = 1; i < polygon.size(); ++i) {
        const double span = QLineF(polygon[0], polygon[i]).length();
        if (span > maxSpan) {
            maxSpan = span;
            anchorB = i;
        }
    }
    QPolygonF result;
    result.push_back(polygon[0]);
    rdpCorridorRecurse(polygon, 0, anchorB, epsilon, chordInFreeSpace, result);
    QPolygonF secondHalf;
    for (int i = anchorB; i < polygon.size(); ++i) {
        secondHalf.push_back(polygon[i]);
    }
    secondHalf.push_back(polygon[0]);
    QPolygonF secondSimplified;
    secondSimplified.push_back(secondHalf.front());
    rdpCorridorRecurse(secondHalf, 0, secondHalf.size() - 1, epsilon, chordInFreeSpace, secondSimplified);
    for (int i = 1; i + 1 < secondSimplified.size(); ++i) {
        result.push_back(secondSimplified[i]);
    }
    if (result.size() < 3) {
        return polygon;
    }
    return result;
}

QPolygonF regionOuterContour(const QPainterPath &outline) {
    return largestFlattenedContour(outline);
}

PenFillResult fillPolygonMesh(const QPolygonF &polygon,
                              const PolygonMeshSources &sources,
                              const std::function<bool()> &cancelled) {
    PenFillResult result;
    if (!sources.valid()) {
        result.error = QStringLiteral("Square/Triangle mesh geometry is unavailable");
        return result;
    }
    if (polygon.size() < 3) {
        result.error = QStringLiteral("Region outline has no fillable contour");
        return result;
    }
    PolygonMeshRequest request;
    request.points = QVector<QPointF>(polygon.begin(), polygon.end());
    request.sources = sources;
    request.mergeSquares = true;
    const PolygonMeshResult mesh = meshPolygon(request, cancelled);
    if (mesh.cancelled) {
        result.cancelled = true;
        return result;
    }
    if (!mesh.error.isEmpty()) {
        result.error = mesh.error;
        return result;
    }
    const QVector<PolygonMeshPlacement> placements = optimizePolygonMeshWithEllipses(
        mesh.placements, sources, mesh.contour, cancelled);
    if (cancelled && cancelled()) {
        result.cancelled = true;
        return result;
    }
    for (const PolygonMeshPlacement &placement : placements) {
        PenPlacement penPlacement;
        penPlacement.shapeId = placement.shapeId;
        penPlacement.transform = placement.transform;
        penPlacement.coreEllipse = placement.shapeId == kCircleShapeId;
        result.placements.push_back(penPlacement);
    }
    return result;
}

PenFillResult fillRegionOutlineMesh(const QPainterPath &outline,
                                    const PolygonMeshSources &sources,
                                    double simplifyEpsilon,
                                    const std::function<bool()> &cancelled) {
    QPolygonF polygon = largestFlattenedContour(outline);
    if (polygon.size() >= 3) {
        polygon = simplifyClosedPolygon(polygon, simplifyEpsilon);
    }
    return fillPolygonMesh(polygon, sources, cancelled);
}

} // namespace gui
