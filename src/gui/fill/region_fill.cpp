#include "region_fill.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

namespace gui {

void sortRegionFillLayersByDrawOrder(QVector<RegionFillLayer> *layers) {
    if (layers == nullptr) {
        return;
    }
    const auto variantRank = [](RegionFillVariant variant) {
        switch (variant) {
        case RegionFillVariant::Safe:
            return 0;
        case RegionFillVariant::Dangerous:
            return 1;
        }
        return 2;
    };
    std::stable_sort(layers->begin(), layers->end(),
                     [variantRank](const RegionFillLayer &left,
                                   const RegionFillLayer &right) {
                         if (left.variant != right.variant) {
                             return variantRank(left.variant) < variantRank(right.variant);
                         }
                         return left.drawOrder < right.drawOrder;
                     });
}

namespace {

constexpr double kGeometryEpsilon = 1e-9;
constexpr int kBoundarySamplesPerCurve = 32;
constexpr int kCircleShapeId = 102;
constexpr int kTraceSubdivisionDepth = 16;
constexpr double kTraceTangentSlack = 1e-7;
constexpr double kMinimumTraceTolerance = 1.0 / 64.0;
constexpr double kMinimumSmoothJunctionTolerance = 0.125;
constexpr double kMinimumSmoothSpanTolerance = 0.125;

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

struct FittedTraceSpan {
    QPointF start;
    QPointF control;
    QPointF end;
    double error = 0.0;
    bool curved = true;
};

double traceCross(const QPointF &first, const QPointF &second) {
    return first.x() * second.y() - first.y() * second.x();
}

bool traceControl(const QPointF &start, const QPointF &end, const QPointF &incoming,
                  const QPointF &outgoing, QPointF *control) {
    const double firstLength = std::hypot(incoming.x(), incoming.y());
    const double secondLength = std::hypot(outgoing.x(), outgoing.y());
    const double denominator = traceCross(incoming, outgoing);
    if (firstLength <= kGeometryEpsilon || secondLength <= kGeometryEpsilon
        || std::abs(denominator) <= kGeometryEpsilon * firstLength * secondLength) {
        return false;
    }
    const double first = traceCross(end - start, outgoing) / denominator;
    const double second = traceCross(incoming, end - start) / denominator;
    if (first <= 0.0 || second <= 0.0) {
        return false;
    }
    *control = start + incoming * first;

    return std::isfinite(control->x()) && std::isfinite(control->y());
}

double traceInflectionParameter(const QPointF &start, const QPointF &first,
                                const QPointF &second, const QPointF &end) {
    const QPointF cubic = end - second * 3.0 + first * 3.0 - start;
    const QPointF quadratic = (start - first * 2.0 + second) * 3.0;
    const QPointF linear = (first - start) * 3.0;
    const double a = -3.0 * traceCross(cubic, quadratic);
    const double b = 3.0 * traceCross(linear, cubic);
    const double c = traceCross(linear, quadratic);
    const double coefficientScale = std::max({std::abs(a), std::abs(b), std::abs(c), kGeometryEpsilon});
    double parameter = 0.0;
    const auto choose = [&](double root) {
        if (std::isfinite(root) && root > kTraceTangentSlack && root < 1.0 - kTraceTangentSlack) {
            parameter = root;
        }
    };
    if (std::abs(a) <= kGeometryEpsilon * coefficientScale) {
        if (std::abs(b) > kGeometryEpsilon * coefficientScale) {
            choose(-c / b);
        }
    } else if (const double discriminant = b * b - 4.0 * a * c; discriminant >= 0.0) {
        const double numerator = -0.5 * (b + std::copysign(std::sqrt(discriminant), b));
        choose(numerator / a);
        if (numerator != 0.0) {
            choose(c / numerator);
        }
    }

    return parameter;
}

bool fitTraceCubic(const QPointF &start, const QPointF &first, const QPointF &second,
                   const QPointF &end, double tolerance, int depth, QVector<FittedTraceSpan> *spans) {
    const QPointF chord = end - start;
    const double lengthSquared = QPointF::dotProduct(chord, chord);
    const double inflection = traceInflectionParameter(start, first, second, end);
    QPointF control;
    if (QLineF(start, end).length() <= kGeometryEpsilon
        && QLineF(start, first).length() <= kGeometryEpsilon
        && QLineF(start, second).length() <= kGeometryEpsilon) {
        return true;
    }
    if (lengthSquared > kGeometryEpsilon
        && std::abs(traceCross(chord, first - start)) <= kGeometryEpsilon * lengthSquared
        && std::abs(traceCross(chord, second - start)) <= kGeometryEpsilon * lengthSquared
        && QPointF::dotProduct(first - start, chord) >= 0.0
        && QPointF::dotProduct(second - first, chord) >= 0.0
        && QPointF::dotProduct(end - second, chord) >= 0.0) {
        spans->push_back({start, (start + end) * 0.5, end, 0.0, false});
        return true;
    }
    const QPointF incoming = QLineF(start, first).length() > kGeometryEpsilon ? first - start : second - start;
    const QPointF outgoing = QLineF(second, end).length() > kGeometryEpsilon ? end - second : end - first;
    if (inflection == 0.0 && traceControl(start, end, incoming, outgoing, &control)) {
        const double error = std::max(QLineF(first, (start + control * 2.0) / 3.0).length(),
            QLineF(second, (end + control * 2.0) / 3.0).length());
        if (error <= tolerance) {
            spans->push_back({start, control, end, error, true});
            return true;
        }
    }
    if (depth >= kTraceSubdivisionDepth) {
        return false;
    }
    const double parameter = inflection == 0.0 ? 0.5 : inflection;
    const auto mix = [parameter](const QPointF &left, const QPointF &right) {
        return left * (1.0 - parameter) + right * parameter;
    };
    const QPointF left = mix(start, first);
    const QPointF middle = mix(first, second);
    const QPointF right = mix(second, end);
    const QPointF leftControl = mix(left, middle);
    const QPointF rightControl = mix(middle, right);
    const QPointF split = mix(leftControl, rightControl);

    return fitTraceCubic(start, left, leftControl, split, tolerance, depth + 1, spans)
        && fitTraceCubic(split, rightControl, right, end, tolerance, depth + 1, spans);
}

bool mergeTraceSpans(const FittedTraceSpan &first, const FittedTraceSpan &second,
                     double tolerance, FittedTraceSpan *merged) {
    const QPointF incoming = first.end - first.control;
    const QPointF outgoing = second.control - second.start;
    const double tangentProduct = std::hypot(incoming.x(), incoming.y()) * std::hypot(outgoing.x(), outgoing.y());
    const double firstLength = QLineF(first.start, first.end).length();
    const double secondLength = QLineF(second.start, second.end).length();
    QPointF control;
    if (tangentProduct <= kGeometryEpsilon || firstLength + secondLength <= kGeometryEpsilon
        || QPointF::dotProduct(incoming, outgoing) <= 0.0
        || std::abs(traceCross(incoming, outgoing)) > kTraceTangentSlack * tangentProduct
        || traceCross(first.control - first.start, first.end - first.control)
            * traceCross(second.control - second.start, second.end - second.control) < 0.0) {
        return false;
    }
    const bool curved = first.curved || second.curved;
    if (curved) {
        if (!traceControl(first.start, second.end, first.control - first.start,
                second.end - second.control, &control)) {
            return false;
        }
    } else {
        control = (first.start + second.end) * 0.5;
    }
    const double parameter = firstLength / (firstLength + secondLength);
    const QPointF left = first.start * (1.0 - parameter) + control * parameter;
    const QPointF right = control * (1.0 - parameter) + second.end * parameter;
    const QPointF split = left * (1.0 - parameter) + right * parameter;
    const double addedError = std::max({QLineF(first.control, left).length(),
        QLineF(first.end, split).length(), QLineF(second.control, right).length()});
    const double error = std::max(first.error, second.error) + addedError;
    if (error > tolerance) {
        return false;
    }
    *merged = {first.start, control, second.end, error, curved};

    return true;
}

QVector<PenPoint> fitTracedSubpath(const Subpath &subpath, double tolerance, int *curveCount) {
    QVector<FittedTraceSpan> spans;
    QVector<PenPoint> points;
    QPointF start = subpath.start;
    if (!std::isfinite(tolerance) || tolerance <= 0.0) {
        return {};
    }
    for (const auto &op : subpath.ops) {
        if (op.kind == Op::Line) {
            if (QLineF(start, op.end).length() > kGeometryEpsilon) {
                spans.push_back({start, (start + op.end) * 0.5, op.end, 0.0, false});
            }
        } else if (!fitTraceCubic(start, op.control1, op.control2, op.end, tolerance * 0.5, 0, &spans)) {
            return {};
        }
        start = op.end;
    }
    for (int index = 0; index + 1 < spans.size() && spans.size() > 2;) {
        FittedTraceSpan merged;
        if (mergeTraceSpans(spans[index], spans[index + 1], tolerance, &merged)) {
            spans[index] = merged;
            spans.removeAt(index + 1);
            index = std::max(0, index - 1);
        } else {
            ++index;
        }
    }
    if (spans.isEmpty()) {
        return {};
    }
    points.push_back({spans.front().start, PenPointKind::Hard});
    for (int index = 0; index < spans.size(); ++index) {
        if (spans[index].curved) {
            points.push_back({spans[index].control, PenPointKind::Soft});
            ++*curveCount;
        }
        if (index + 1 < spans.size()) {
            points.push_back({spans[index].end, PenPointKind::Hard});
        }
    }

    return points;
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

QPainterPath subpathPainterPath(const Subpath &subpath) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    result.moveTo(subpath.start);
    for (const Op &op : subpath.ops) {
        if (op.kind == Op::Line) {
            result.lineTo(op.end);
        } else {
            result.cubicTo(op.control1, op.control2, op.end);
        }
    }
    if (subpath.closed) {
        result.closeSubpath();
    }
    return result;
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

QSet<int> simplifyIndexChain(const QPolygonF &points,
                             const QVector<int> &chain,
                             double epsilon) {
    QSet<int> keep = {chain.front(), chain.back()};
    QVector<QPair<int, int>> stack = {{0, chain.size() - 1}};
    while (!stack.isEmpty()) {
        const QPair<int, int> slice = stack.takeLast();
        if (slice.second - slice.first <= 1) {
            continue;
        }
        double maximumError = -1.0;
        int splitPosition = -1;
        for (int position = slice.first + 1; position < slice.second; ++position) {
            const int index = chain[position];
            const double distance = perpendicularDistance(
                points[index], points[chain[slice.first]], points[chain[slice.second]]);
            if (distance > maximumError) {
                maximumError = distance;
                splitPosition = position;
            }
        }
        if (maximumError > epsilon) {
            if (splitPosition <= slice.first || splitPosition >= slice.second) {
                splitPosition = (slice.first + slice.second) / 2;
            }
            keep.insert(chain[splitPosition]);
            stack.push_back({slice.first, splitPosition});
            stack.push_back({splitPosition, slice.second});
        }
    }

    return keep;
}

QVector<int> cyclicRdpIndices(const QPolygonF &points, double epsilon) {
    const int count = points.size();
    if (count <= 3) {
        QVector<int> result;
        result.reserve(count);
        for (int index = 0; index < count; ++index) {
            result.push_back(index);
        }
        return result;
    }
    int anchor = 1;
    double maximumDistance = QLineF(points.front(), points[anchor]).length();
    for (int index = 2; index < count; ++index) {
        const double distance = QLineF(points.front(), points[index]).length();
        if (distance > maximumDistance) {
            maximumDistance = distance;
            anchor = index;
        }
    }
    QVector<int> firstChain;
    firstChain.reserve(anchor + 1);
    for (int index = 0; index <= anchor; ++index) {
        firstChain.push_back(index);
    }
    QVector<int> secondChain;
    secondChain.reserve(count - anchor + 1);
    for (int index = anchor; index < count; ++index) {
        secondChain.push_back(index);
    }
    secondChain.push_back(0);
    QSet<int> keep = simplifyIndexChain(points, firstChain, epsilon);
    keep.unite(simplifyIndexChain(points, secondChain, epsilon));
    QVector<int> result;
    result.reserve(keep.size());
    for (const int index : keep) {
        result.push_back(index);
    }
    std::sort(result.begin(), result.end());

    return result;
}

QVector<int> cyclicArcIndices(int count, int start, int end) {
    QVector<int> result = {start};
    int index = start;
    while (index != end && result.size() <= count) {
        index = (index + 1) % count;
        result.push_back(index);
    }

    return result;
}

QPointF quadraticReconstructionControl(const QPolygonF &points,
                                        const QVector<int> &arcIndices) {
    QVector<double> distances(arcIndices.size(), 0.0);
    for (int position = 1; position < arcIndices.size(); ++position) {
        distances[position] = distances[position - 1]
            + QLineF(points[arcIndices[position - 1]],
                     points[arcIndices[position]]).length();
    }
    const QPointF start = points[arcIndices.front()];
    const QPointF end = points[arcIndices.back()];
    const double total = distances.back();
    QPointF numerator;
    double denominator = 0.0;
    for (int position = 0; position < arcIndices.size(); ++position) {
        const double parameter = total > 1e-9
            ? distances[position] / total
            : static_cast<double>(position) / (arcIndices.size() - 1);
        const double remaining = 1.0 - parameter;
        const double weight = 2.0 * remaining * parameter;
        const QPointF fixed = start * (remaining * remaining)
            + end * (parameter * parameter);
        numerator += (points[arcIndices[position]] - fixed) * weight;
        denominator += weight * weight;
    }
    if (denominator <= 1e-12) {
        return (start + end) * 0.5;
    }

    return numerator / denominator;
}

struct CyclicRdpSlice {
    int start = 0;
    int end = 0;
};

QPolygonF approximateClosedPolygon(const QPolygonF &polygon, double epsilon) {
    constexpr int kFarthestPointPasses = 3;
    if (!std::isfinite(epsilon) || epsilon <= 0.0 || polygon.size() <= 4) {
        return polygon;
    }
    QPolygonF source;
    source.reserve(polygon.size());
    for (const QPointF &point : polygon) {
        source.push_back(QPointF(static_cast<float>(point.x()),
                                 static_cast<float>(point.y())));
    }
    QVector<CyclicRdpSlice> stack;
    QPolygonF result;
    const int sourceCount = source.size();
    const double epsilonSquared = epsilon * epsilon;
    int position = 0;
    int rightStart = 0;
    QPointF startPoint;
    bool withinEpsilon = false;

    for (int pass = 0; pass < kFarthestPointPasses; ++pass) {
        position = (position + rightStart) % sourceCount;
        startPoint = source[position];
        position = (position + 1) % sourceCount;
        double maximumDistanceSquared = 0.0;
        rightStart = 0;
        for (int offset = 1; offset < sourceCount; ++offset) {
            const QPointF point = source[position];
            position = (position + 1) % sourceCount;
            const QPointF delta = point - startPoint;
            const double distanceSquared = QPointF::dotProduct(delta, delta);
            if (distanceSquared > maximumDistanceSquared) {
                maximumDistanceSquared = distanceSquared;
                rightStart = offset;
            }
        }
        withinEpsilon = maximumDistanceSquared <= epsilonSquared;
    }
    if (!withinEpsilon) {
        const int seam = position % sourceCount;
        const int opposite = (rightStart + seam) % sourceCount;
        stack.push_back(CyclicRdpSlice{opposite, seam});
        stack.push_back(CyclicRdpSlice{seam, opposite});
    } else {
        result.push_back(startPoint);
    }

    while (!stack.isEmpty()) {
        const CyclicRdpSlice slice = stack.takeLast();
        const QPointF endPoint = source[slice.end];
        position = slice.start;
        startPoint = source[position];
        position = (position + 1) % sourceCount;
        int split = slice.start;
        if (position != slice.end) {
            const QPointF segment = endPoint - startPoint;
            const double segmentLengthSquared = QPointF::dotProduct(segment, segment);
            double maximumScaledDistance = 0.0;
            if (segmentLengthSquared > 0.0) {
                while (position != slice.end) {
                    const QPointF point = source[position];
                    position = (position + 1) % sourceCount;
                    const QPointF fromStart = point - startPoint;
                    const double projection = QPointF::dotProduct(fromStart, segment);
                    double scaledDistance = 0.0;
                    if (projection < 0.0) {
                        scaledDistance = QPointF::dotProduct(fromStart, fromStart)
                            * segmentLengthSquared;
                    } else if (projection > segmentLengthSquared) {
                        const QPointF fromEnd = point - endPoint;
                        scaledDistance = QPointF::dotProduct(fromEnd, fromEnd)
                            * segmentLengthSquared;
                    } else {
                        const double cross = fromStart.y() * segment.x()
                            - fromStart.x() * segment.y();
                        scaledDistance = cross * cross;
                    }
                    if (scaledDistance > maximumScaledDistance) {
                        maximumScaledDistance = scaledDistance;
                        split = (position + sourceCount - 1) % sourceCount;
                    }
                }
            }
            withinEpsilon = segmentLengthSquared <= 0.0
                || maximumScaledDistance <= epsilonSquared * segmentLengthSquared;
        } else {
            withinEpsilon = true;
        }
        if (withinEpsilon) {
            result.push_back(startPoint);
        } else {
            stack.push_back(CyclicRdpSlice{split, slice.end});
            stack.push_back(CyclicRdpSlice{slice.start, split});
        }
    }

    const int cleanupCount = result.size();
    int resultCount = cleanupCount;
    if (cleanupCount <= 2) {
        return polygon;
    }
    position = cleanupCount - 1;
    const auto readPoint = [&]() {
        const QPointF point = result[position];
        position = (position + 1) % cleanupCount;
        return point;
    };
    startPoint = readPoint();
    int writePosition = position;
    QPointF point = readPoint();
    for (int pass = 0; pass < cleanupCount && resultCount > 2; ++pass) {
        const QPointF endPoint = readPoint();
        const double dx = endPoint.x() - startPoint.x();
        const double dy = endPoint.y() - startPoint.y();
        const double distance = std::abs((point.x() - startPoint.x()) * dy
                                         - (point.y() - startPoint.y()) * dx);
        const double successiveInnerProduct =
            (point.x() - startPoint.x()) * (endPoint.x() - point.x())
            + (point.y() - startPoint.y()) * (endPoint.y() - point.y());
        if (distance * distance
                <= 0.5 * epsilonSquared * (dx * dx + dy * dy)
            && dx != 0.0 && dy != 0.0 && successiveInnerProduct >= 0.0) {
            --resultCount;
            startPoint = endPoint;
            result[writePosition] = startPoint;
            writePosition = (writePosition + 1) % cleanupCount;
            point = readPoint();
            ++pass;
            continue;
        }
        result[writePosition] = point;
        startPoint = point;
        writePosition = (writePosition + 1) % cleanupCount;
        point = endPoint;
    }
    result.resize(resultCount);

    return result.size() >= 3 ? result : polygon;
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

QPolygonF largestFlattenedContour(const QPainterPath &outline, int curveSamples = 8) {
    const QVector<Subpath> subpaths = toSubpaths(outline);
    if (subpaths.isEmpty()) {
        return {};
    }
    int outer = 0;
    double outerArea = -1.0;
    QPolygonF outerPolygon;
    for (int i = 0; i < subpaths.size(); ++i) {
        const QPolygonF polygon = flattenSubpath(subpaths[i], curveSamples);
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


} // namespace

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
    if (options.mergeTolerance <= 0.0 && !options.straightenSoftRuns) {
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

int regionOutlinePenPointCount(const QPainterPath &outline) {
    constexpr double kClosureTolerance = 1e-6;
    const OuterSelection outer = selectClosedOuter(outline, kClosureTolerance);
    if (!outer.error.isEmpty()) {
        return 0;
    }

    return initialPenPoints(outer.subpath, kClosureTolerance).size();
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

QPolygonF simplifyClosedPolygonCyclic(const QPolygonF &polygon, double epsilon) {
    return approximateClosedPolygon(polygon, epsilon);
}

QPolygonF quadraticSamples(const QPointF &start, const QPointF &control,
                           const QPointF &end, int samples);
double openPolylineDeviation(const QPolygonF &first, const QPolygonF &second);

QVector<int> mergeQuadraticAnchors(const QPolygonF &polygon,
                                  QVector<int> anchors, double tolerance) {
    constexpr int kCornerSampleOffset = 3;
    constexpr double kSmoothTurnCosine = 0.5;
    constexpr int kMinimumFitSamples = 12;
    constexpr int kMaximumFitSamples = 32;
    struct Candidate {
        double deviation = 0.0;
        int position = 0;
        int version = 0;
    };
    struct GreaterCandidate {
        bool operator()(const Candidate &left, const Candidate &right) const {
            if (left.deviation != right.deviation) {
                return left.deviation > right.deviation;
            }
            return left.position > right.position;
        }
    };
    const int count = anchors.size();
    QVector<int> previous(count);
    QVector<int> next(count);
    QVector<int> versions(count, 0);
    QVector<bool> active(count, true);
    for (int index = 0; index < count; ++index) {
        previous[index] = (index + count - 1) % count;
        next[index] = (index + 1) % count;
    }
    const auto deviationFor = [&](int position) {
        const int middle = anchors[position];
        const QPointF incoming = polygon[middle]
            - polygon[(middle + polygon.size() - kCornerSampleOffset) % polygon.size()];
        const QPointF outgoing = polygon[(middle + kCornerSampleOffset) % polygon.size()]
            - polygon[middle];
        const double lengths = std::hypot(incoming.x(), incoming.y())
            * std::hypot(outgoing.x(), outgoing.y());
        if (lengths <= kGeometryEpsilon
            || QPointF::dotProduct(incoming, outgoing) < kSmoothTurnCosine * lengths) {
            return std::numeric_limits<double>::infinity();
        }
        const int start = anchors[previous[position]];
        const int end = anchors[next[position]];
        const QVector<int> arcIndices = cyclicArcIndices(polygon.size(), start, end);
        QPolygonF arc;
        arc.reserve(arcIndices.size());
        for (int index : arcIndices) {
            arc.push_back(polygon[index]);
        }
        const QPointF control = quadraticReconstructionControl(polygon, arcIndices);
        const int samples = std::clamp(static_cast<int>(arc.size() / 2),
            kMinimumFitSamples, kMaximumFitSamples);
        const QPolygonF fit = quadraticSamples(polygon[start], control, polygon[end], samples);

        return openPolylineDeviation(arc, fit);
    };
    std::priority_queue<Candidate, std::vector<Candidate>, GreaterCandidate> queue;
    const auto enqueue = [&](int position) {
        if (!active[position]) {
            return;
        }
        const double deviation = deviationFor(position);
        if (std::isfinite(deviation)) {
            queue.push({deviation, position, versions[position]});
        }
    };
    for (int position = 0; position < count; ++position) {
        enqueue(position);
    }
    int remaining = count;
    while (remaining > 3 && !queue.empty()) {
        const Candidate candidate = queue.top();
        queue.pop();
        if (!active[candidate.position]
            || versions[candidate.position] != candidate.version) {
            continue;
        }
        if (candidate.deviation > tolerance + kGeometryEpsilon) {
            break;
        }
        const int left = previous[candidate.position];
        const int right = next[candidate.position];
        active[candidate.position] = false;
        next[left] = right;
        previous[right] = left;
        --remaining;
        ++versions[left];
        ++versions[right];
        enqueue(left);
        enqueue(right);
    }
    QVector<int> merged;
    merged.reserve(remaining);
    for (int position = 0; position < count; ++position) {
        if (active[position]) {
            merged.push_back(anchors[position]);
        }
    }

    return merged;
}

QVector<PenPoint> simplifyClosedPolygonRdpHybridQuadratic(
    const QPolygonF &polygon,
    double epsilon,
    double minimumCurveBow,
    double smoothSpanTolerance) {
    QVector<PenPoint> result;
    if (polygon.size() < 3 || !std::isfinite(epsilon) || epsilon <= 0.0
        || !std::isfinite(minimumCurveBow) || minimumCurveBow < 0.0
        || !std::isfinite(smoothSpanTolerance) || smoothSpanTolerance < 0.0) {
        return result;
    }
    QVector<int> anchors = cyclicRdpIndices(polygon, epsilon);
    if (anchors.size() < 3) {
        return result;
    }
    if (smoothSpanTolerance > 0.0) {
        anchors = mergeQuadraticAnchors(polygon, std::move(anchors), smoothSpanTolerance);
    }
    result.reserve(anchors.size() * 2);
    for (int anchorPosition = 0; anchorPosition < anchors.size(); ++anchorPosition) {
        const int startIndex = anchors[anchorPosition];
        const int endIndex = anchors[(anchorPosition + 1) % anchors.size()];
        const QVector<int> arcIndices = cyclicArcIndices(
            polygon.size(), startIndex, endIndex);
        const QPointF start = polygon[startIndex];
        const QPointF end = polygon[endIndex];
        const QPointF control = quadraticReconstructionControl(polygon, arcIndices);
        const double curveBow = perpendicularDistance(control, start, end) * 0.5;
        if (anchorPosition == 0) {
            result.push_back({start, PenPointKind::Hard});
        }
        if (curveBow >= minimumCurveBow) {
            result.push_back({control, PenPointKind::Soft});
        }
        if (anchorPosition + 1 < anchors.size()) {
            result.push_back({end, PenPointKind::Hard});
        }
    }

    return result;
}

QPolygonF quadraticSamples(const QPointF &start, const QPointF &control,
                           const QPointF &end, int samples) {
    QPolygonF result;
    result.reserve(samples + 1);
    for (int step = 0; step <= samples; ++step) {
        const double t = static_cast<double>(step) / samples;
        const double u = 1.0 - t;
        result.push_back(start * (u * u) + control * (2.0 * u * t) + end * (t * t));
    }

    return result;
}

double openPolylineDeviation(const QPolygonF &first, const QPolygonF &second) {
    const auto directed = [](const QPolygonF &source, const QPolygonF &target) {
        double maximum = 0.0;
        for (const QPointF &point : source) {
            double nearest = std::numeric_limits<double>::infinity();
            for (int index = 1; index < target.size(); ++index) {
                nearest = std::min(nearest, perpendicularDistance(point,
                    target[index - 1], target[index]));
            }
            maximum = std::max(maximum, nearest);
        }
        return maximum;
    };

    return std::max(directed(first, second), directed(second, first));
}

double hybridJunctionDeviation(const QVector<PenPoint> &points, int index) {
    constexpr int kJunctionSamples = 12;
    const int count = points.size();
    const int previous = (index + count - 1) % count;
    const int next = (index + 1) % count;
    const PenPoint &before = points[(index + count - 2) % count];
    const PenPoint &after = points[(index + 2) % count];
    const QPointF left = before.kind == PenPointKind::Hard
        ? before.position : (before.position + points[previous].position) * 0.5;
    const QPointF right = after.kind == PenPointKind::Hard
        ? after.position : (points[next].position + after.position) * 0.5;
    const QPointF middle = (points[previous].position + points[next].position) * 0.5;
    QPolygonF original = quadraticSamples(left, points[previous].position,
        points[index].position, kJunctionSamples);
    QPolygonF replacement = quadraticSamples(left, points[previous].position,
        middle, kJunctionSamples);
    original += quadraticSamples(points[index].position, points[next].position,
        right, kJunctionSamples);
    replacement += quadraticSamples(middle, points[next].position,
        right, kJunctionSamples);

    return openPolylineDeviation(original, replacement);
}

QVector<PenPoint> smoothHybridJunctions(const QVector<PenPoint> &points,
                                      double tolerance) {
    constexpr double kSmoothTurnCosine = 0.5;
    constexpr int kGlobalSamples = 8;
    constexpr qint64 kGlobalComparisonLimit = 10'000'000;
    if (points.size() < 3 || tolerance <= 0.0) {
        return points;
    }
    QVector<PenPoint> smoothed = points;
    QVector<int> originalIndices;
    QSet<int> rejected;
    originalIndices.reserve(points.size());
    for (int index = 0; index < points.size(); ++index) {
        originalIndices.push_back(index);
    }
    int hardCount = static_cast<int>(std::count_if(points.cbegin(), points.cend(),
        [](const PenPoint &point) { return point.kind == PenPointKind::Hard; }));
    while (hardCount > 1) {
        QVector<std::pair<double, int>> candidates;
        for (int index = 0; index < smoothed.size(); ++index) {
            if (smoothed[index].kind != PenPointKind::Hard
                || rejected.contains(originalIndices[index])) {
                continue;
            }
            const int previous = (index + smoothed.size() - 1) % smoothed.size();
            const int next = (index + 1) % smoothed.size();
            if (smoothed[previous].kind != PenPointKind::Soft
                || smoothed[next].kind != PenPointKind::Soft) {
                continue;
            }
            const QPointF incoming = smoothed[index].position - smoothed[previous].position;
            const QPointF outgoing = smoothed[next].position - smoothed[index].position;
            const double lengths = std::hypot(incoming.x(), incoming.y())
                * std::hypot(outgoing.x(), outgoing.y());
            if (lengths <= kGeometryEpsilon
                || QPointF::dotProduct(incoming, outgoing) < kSmoothTurnCosine * lengths) {
                continue;
            }
            const double deviation = hybridJunctionDeviation(smoothed, index);
            if (deviation <= tolerance + kGeometryEpsilon) {
                candidates.push_back({deviation, index});
            }
        }
        std::sort(candidates.begin(), candidates.end());
        bool changed = false;
        for (const auto &[deviation, index] : candidates) {
            Q_UNUSED(deviation);
            QVector<PenPoint> trial = smoothed;
            trial.removeAt(index);
            if (!buildPenContour(trial).valid()) {
                rejected.insert(originalIndices[index]);
                continue;
            }
            smoothed = std::move(trial);
            originalIndices.removeAt(index);
            --hardCount;
            changed = true;
            break;
        }
        if (!changed) {
            break;
        }
    }
    if (smoothed.size() == points.size()) {
        return points;
    }
    const PenContour original = buildPenContour(points);
    const PenContour replacement = buildPenContour(smoothed);
    const QPolygonF originalSamples = flattenPenContour(original, kGlobalSamples);
    const QPolygonF replacementSamples = flattenPenContour(replacement, kGlobalSamples);
    if (static_cast<qint64>(originalSamples.size()) * replacementSamples.size() <= kGlobalComparisonLimit
        && boundaryDeviation(originalSamples, replacementSamples) > tolerance * 2.0) {
        return points;
    }

    return smoothed;
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

QPolygonF regionOuterContour(const QPainterPath &outline, int curveSamples) {
    if (curveSamples < 1) {
        return {};
    }

    return largestFlattenedContour(outline, curveSamples);
}

QVector<QPolygonF> regionContours(const QPainterPath &outline, int curveSamples) {
    QVector<QPolygonF> result;
    if (curveSamples < 1) {
        return result;
    }
    const QVector<Subpath> subpaths = toSubpaths(outline);
    result.reserve(subpaths.size());
    for (const Subpath &subpath : subpaths) {
        QPolygonF polygon = flattenSubpath(subpath, curveSamples);
        while (polygon.size() > 1
               && QLineF(polygon.back(), polygon.front()).length() <= 1e-6) {
            polygon.removeLast();
        }
        if (polygon.size() >= 3) {
            result.push_back(std::move(polygon));
        }
    }

    return result;
}

double closedPolylineSeparation(const QPolygonF &left, const QPolygonF &right) {
    if (left.size() < 3 || right.size() < 3) {
        return std::numeric_limits<double>::infinity();
    }
    double result = std::numeric_limits<double>::infinity();
    for (const QPointF &point : left) {
        result = std::min(result, pointToClosedPolylineDistance(point, right));
    }
    for (const QPointF &point : right) {
        result = std::min(result, pointToClosedPolylineDistance(point, left));
    }
    return result;
}

RegionPenLoopConversionResult regionOutlineToPenLoops(
    const QPainterPath &outline,
    const RegionPenLoopConversionOptions &options) {
    RegionPenLoopConversionResult result;
    if (outline.isEmpty()
        || options.curveSamples < 1
        || !std::isfinite(options.simplifyEpsilon)
        || options.simplifyEpsilon <= 0.0
        || !std::isfinite(options.minimumCurveBow)
        || options.minimumCurveBow < 0.0
        || !std::isfinite(options.smoothSpanTolerance)
        || options.smoothSpanTolerance < 0.0
        || !std::isfinite(options.smoothJunctionTolerance)
        || options.smoothJunctionTolerance < 0.0
        || !std::isfinite(options.discardedCutoutAreaCeiling)
        || options.discardedCutoutAreaCeiling < 0.0
        || !std::isfinite(options.discardedCutoutBoundaryClearance)
        || options.discardedCutoutBoundaryClearance < 0.0) {
        result.error = QStringLiteral("The region has no fillable contour");
        return result;
    }

    struct LoopCandidate {
        Subpath subpath;
        QPolygonF sampled;
        double area = 0.0;
    };
    QVector<LoopCandidate> candidates;
    for (Subpath &subpath : toSubpaths(
             outline, options.fallback.closureTolerance)) {
        QPolygonF sampled = flattenSubpath(subpath, options.curveSamples);
        while (sampled.size() > 1
               && QLineF(sampled.back(), sampled.front()).length() <= 1e-6) {
            sampled.removeLast();
        }
        const double area = std::abs(signedArea(sampled));
        if (area > kGeometryEpsilon) {
            candidates.push_back({std::move(subpath), std::move(sampled), area});
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const LoopCandidate &left,
                                                        const LoopCandidate &right) {
        return left.area > right.area;
    });
    if (candidates.isEmpty()) {
        result.error = QStringLiteral("The region has no fillable contour");
        return result;
    }

    result.loops.reserve(candidates.size());
    QVector<int> includedCandidateIndices;
    includedCandidateIndices.reserve(candidates.size());
    for (int candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
        const LoopCandidate &candidate = candidates[candidateIndex];
        if (candidateIndex > 0) {
            const bool belowAreaCeiling = options.discardedCutoutAreaCeiling > 0.0
                && candidate.area
                    <= options.discardedCutoutAreaCeiling + kGeometryEpsilon;
            const bool withinBoundaryGate = !belowAreaCeiling
                && options.discardedCutoutBoundaryClearance > 0.0
                && closedPolylineSeparation(candidate.sampled,
                                            candidates.front().sampled)
                    <= options.discardedCutoutBoundaryClearance + kGeometryEpsilon;
            if (belowAreaCeiling || withinBoundaryGate) {
                ++result.discardedCutoutCount;
                if (belowAreaCeiling) {
                    ++result.discardedCutoutAreaCount;
                } else {
                    ++result.discardedCutoutBoundaryCount;
                }
                continue;
            }
        }
        const Subpath &subpath = candidate.subpath;
        QVector<PenPoint> points;
        if (options.fitTracedCurves) {
            points = fitTracedSubpath(subpath, options.curveFitTolerance, &result.fittedCurveSegments);
            if (points.isEmpty()) {
                result.error = QStringLiteral("The traced curves could not be fitted within the curve tolerance");
                result.loops.clear();
                return result;
            }
        } else if (options.preserveInputCurves) {
            RegionPenConversionOptions directOptions = options.fallback;
            directOptions.mergeTolerance = 0.0;
            directOptions.adaptiveSearchSteps = 0;
            directOptions.straightenSoftRuns = false;
            const RegionPenConversionResult conversion = regionOutlineToPenPoints(
                subpathPainterPath(subpath), directOptions);
            if (!conversion.valid()) {
                result.error = conversion.error.isEmpty()
                    ? QStringLiteral("The source vector boundary is invalid")
                    : conversion.error;
                result.loops.clear();
                return result;
            }
            points = conversion.points;
        } else {
            points = simplifyClosedPolygonRdpHybridQuadratic(
                candidate.sampled, options.simplifyEpsilon, options.minimumCurveBow);
            if (!buildPenContour(points).valid()) {
                const double conservativeBow = RegionPenLoopConversionOptions{}.minimumCurveBow;
                if (options.minimumCurveBow < conservativeBow) {
                    auto retry = options;
                    retry.minimumCurveBow = conservativeBow;
                    return regionOutlineToPenLoops(outline, retry);
                }
                const RegionPenConversionResult conversion = regionOutlineToPenPoints(
                    subpathPainterPath(subpath), options.fallback);
                if (!conversion.valid()) {
                    result.error = conversion.error.isEmpty()
                        ? QStringLiteral("The traced region boundary is invalid")
                        : conversion.error;
                    result.loops.clear();
                    return result;
                }
                points = conversion.points;
            }
        }
        result.loops.push_back({
            std::move(points),
            candidateIndex == 0 ? PenLoopKind::Outer : PenLoopKind::Cutout,
        });
        includedCandidateIndices.push_back(candidateIndex);
    }

    const PenContour compound = buildPenContour(result.loops);
    if (!compound.valid()) {
        const double conservativeBow = RegionPenLoopConversionOptions{}.minimumCurveBow;
        if (!options.fitTracedCurves && !options.preserveInputCurves
            && options.minimumCurveBow < conservativeBow) {
            auto retry = options;
            retry.minimumCurveBow = conservativeBow;
            return regionOutlineToPenLoops(outline, retry);
        }
        if (options.fitTracedCurves && options.curveFitTolerance > kMinimumTraceTolerance) {
            auto retry = options;
            retry.curveFitTolerance *= 0.5;
            return regionOutlineToPenLoops(outline, retry);
        }
        result.error = compound.error.isEmpty()
            ? QStringLiteral("The traced region is not a valid Pen contour")
            : compound.error;
        result.loops.clear();
        return result;
    }
    if (!options.fitTracedCurves && !options.preserveInputCurves
        && (options.smoothSpanTolerance > 0.0 || options.smoothHybridJunctions)) {
        const auto accepts = [&](int index, const QVector<PenPoint> &points) {
            if (!buildPenContour(points).valid()) {
                return false;
            }
            auto trial = result.loops;
            trial[index].points = points;
            if (!buildPenContour(trial).valid()) {
                return false;
            }
            result.loops = std::move(trial);
            return true;
        };
        for (int index = 0; index < result.loops.size(); ++index) {
            bool accepted = false;
            for (double tolerance = options.smoothSpanTolerance;
                 tolerance >= kMinimumSmoothSpanTolerance && !accepted; tolerance *= 0.5) {
                QVector<PenPoint> merged = simplifyClosedPolygonRdpHybridQuadratic(
                    candidates[includedCandidateIndices[index]].sampled, options.simplifyEpsilon,
                    options.minimumCurveBow, tolerance);
                if (options.smoothHybridJunctions && buildPenContour(merged).valid()) {
                    for (double junctionTolerance = options.smoothJunctionTolerance;
                         junctionTolerance >= kMinimumSmoothJunctionTolerance;
                         junctionTolerance *= 0.5) {
                        auto smoothed = smoothHybridJunctions(merged, junctionTolerance);
                        if (smoothed.size() < merged.size() && accepts(index, smoothed)) {
                            accepted = true;
                            break;
                        }
                    }
                }
                if (!accepted && merged.size() != result.loops[index].points.size()) {
                    accepted = accepts(index, merged);
                }
            }
            if (!accepted && options.smoothHybridJunctions) {
                for (double tolerance = options.smoothJunctionTolerance;
                     tolerance >= kMinimumSmoothJunctionTolerance; tolerance *= 0.5) {
                    auto smoothed = smoothHybridJunctions(result.loops[index].points, tolerance);
                    if (smoothed.size() < result.loops[index].points.size()
                        && accepts(index, smoothed)) {
                        break;
                    }
                }
            }
        }
    }
    return result;
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
