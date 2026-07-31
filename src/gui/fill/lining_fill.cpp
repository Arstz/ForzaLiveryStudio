#include "lining_fill.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace gui {
namespace {

constexpr double kEpsilon = 1e-8;
constexpr int kMaximumPlacements = 512;
constexpr std::array<int, 4> kLiningShapeIds = {2112, 2131, 136, 2133};

QString pointText(const QPointF &point) {
    return QStringLiteral("(%1,%2)")
        .arg(point.x(), 0, 'f', 4)
        .arg(point.y(), 0, 'f', 4);
}

QString transformText(const QTransform &transform) {
    return QStringLiteral("[%1 %2 %3; %4 %5 %6]")
        .arg(transform.m11(), 0, 'f', 6)
        .arg(transform.m21(), 0, 'f', 6)
        .arg(transform.dx(), 0, 'f', 6)
        .arg(transform.m12(), 0, 'f', 6)
        .arg(transform.m22(), 0, 'f', 6)
        .arg(transform.dy(), 0, 'f', 6);
}

class LiningDiagnostic {
public:
    LiningDiagnostic() {
        lines_.push_back(QStringLiteral("Lining fill diagnostic %1")
                             .arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)));
    }

    ~LiningDiagnostic() {
        const QString path = QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("lining_fill.log"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream stream(&file);
            stream << lines_.join(QLatin1Char('\n')) << '\n';
        }
    }

    void add(const QString &line) {
        lines_.push_back(line);
    }

private:
    QStringList lines_;
};

struct PrimitiveProfile {
    const PenPrimitive *primitive = nullptr;
    QPointF start;
    QPointF middle;
    QPointF end;
    QPointF inner;
    QPointF spineStart;
    QPointF spineMiddle;
    QPointF spineEnd;
    QPointF tip;
    QPointF base;
    double sourceSign = 1.0;
};

struct Candidate {
    int start = -1;
    int end = -1;
    const PenPrimitive *primitive = nullptr;
    QTransform transform;
    double score = 0.0;
    double curveError = std::numeric_limits<double>::max();
    double widthError = std::numeric_limits<double>::max();
    double sideSign = 0.0;
    double overlapFactor = 0.0;
    bool tipAtStart = false;
};

QString candidateText(const Candidate &candidate) {
    return QStringLiteral("shape=%1 span=%2..%3 curveError=%4 widthError=%5 "
                          "tipAtStart=%6 transform=%7")
        .arg(candidate.primitive != nullptr ? candidate.primitive->shapeId : -1)
        .arg(candidate.start)
        .arg(candidate.end)
        .arg(candidate.curveError, 0, 'g', 8)
        .arg(candidate.widthError, 0, 'g', 8)
        .arg(candidate.tipAtStart ? QStringLiteral("true") : QStringLiteral("false"))
        .arg(transformText(candidate.transform));
}

double candidateScore(double placementArea,
                      double assignedArea,
                      double width,
                      int shapeId) {
    double result = std::min(placementArea, assignedArea) - width * width * 0.04;
    if (shapeId == 2133) {
        result *= 0.72;
    } else if (shapeId == 2112) {
        result *= 1.08;
    }
    return result;
}

double cross(const QPointF &a, const QPointF &b) {
    return a.x() * b.y() - a.y() * b.x();
}

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        result += cross(polygon[i], polygon[(i + 1) % polygon.size()]);
    }
    return result * 0.5;
}

double pathArea(const QPainterPath &path) {
    double result = 0.0;
    for (const QPolygonF &polygon : path.toFillPolygons()) {
        result += std::abs(signedArea(polygon));
    }
    return result;
}

QPointF quadraticPoint(const PenBoundarySegment &segment, double t) {
    if (!segment.curved) {
        return segment.start * (1.0 - t) + segment.end * t;
    }
    const double u = 1.0 - t;
    return segment.start * (u * u)
        + segment.control * (2.0 * u * t)
        + segment.end * (t * t);
}

QVector<QPointF> sampleCenterline(const LiningPath &path,
                                  double width,
                                  QVector<int> *segmentBoundaries) {
    QVector<QPointF> result;
    if (segmentBoundaries != nullptr) {
        segmentBoundaries->clear();
        segmentBoundaries->push_back(0);
    }
    const double stepLength = std::max(width * 0.45, 0.25);
    for (const PenBoundarySegment &segment : path.segments) {
        double estimatedLength = 0.0;
        QPointF previous = segment.start;
        for (int i = 1; i <= 16; ++i) {
            const QPointF point = quadraticPoint(segment, static_cast<double>(i) / 16.0);
            estimatedLength += QLineF(previous, point).length();
            previous = point;
        }
        const int steps = std::clamp(static_cast<int>(std::ceil(estimatedLength / stepLength)), 1, 96);
        if (result.isEmpty()) {
            result.push_back(segment.start);
        }
        for (int i = 1; i <= steps; ++i) {
            result.push_back(quadraticPoint(segment, static_cast<double>(i) / steps));
        }
        if (segmentBoundaries != nullptr) {
            segmentBoundaries->push_back(result.size() - 1);
        }
    }
    constexpr int maximumSamples = kMaximumPlacements * 2 + 1;
    if (result.size() <= maximumSamples) {
        return result;
    }
    QVector<QPointF> reduced;
    reduced.reserve(maximumSamples);
    for (int i = 0; i < maximumSamples; ++i) {
        const int index = static_cast<int>(std::llround(
            static_cast<double>(i) * (result.size() - 1) / (maximumSamples - 1)));
        reduced.push_back(result[index]);
    }
    if (segmentBoundaries != nullptr) {
        for (int &boundary : *segmentBoundaries) {
            boundary = static_cast<int>(std::llround(
                static_cast<double>(boundary) * (maximumSamples - 1) / (result.size() - 1)));
        }
    }
    return reduced;
}

QVector<double> cumulativeLengths(const QVector<QPointF> &points) {
    QVector<double> result(points.size(), 0.0);
    for (int i = 1; i < points.size(); ++i) {
        result[i] = result[i - 1] + QLineF(points[i - 1], points[i]).length();
    }
    return result;
}

QTransform affineFromTriangles(const QPointF &a,
                               const QPointF &b,
                               const QPointF &c,
                               const QPointF &p,
                               const QPointF &q,
                               const QPointF &r,
                               bool *ok) {
    const QPointF d1 = b - a;
    const QPointF d2 = c - a;
    const QPointF e1 = q - p;
    const QPointF e2 = r - p;
    const double determinant = cross(d1, d2);
    if (std::abs(determinant) <= kEpsilon) {
        *ok = false;
        return {};
    }
    const double m11 = (e1.x() * d2.y() - e2.x() * d1.y()) / determinant;
    const double m21 = (-e1.x() * d2.x() + e2.x() * d1.x()) / determinant;
    const double m12 = (e1.y() * d2.y() - e2.y() * d1.y()) / determinant;
    const double m22 = (-e1.y() * d2.x() + e2.y() * d1.x()) / determinant;
    const double dx = p.x() - m11 * a.x() - m21 * a.y();
    const double dy = p.y() - m12 * a.x() - m22 * a.y();
    *ok = std::isfinite(m11) && std::isfinite(m12) && std::isfinite(m21)
        && std::isfinite(m22) && std::isfinite(dx) && std::isfinite(dy);
    return QTransform(m11, m12, m21, m22, dx, dy);
}

QVector<PrimitiveProfile> primitiveProfiles(const PenPrimitive &primitive) {
    if (primitive.shapeId == 2133 && primitive.bounds.width() > kEpsilon
        && primitive.bounds.height() > kEpsilon) {
        PrimitiveProfile profile;
        profile.primitive = &primitive;
        profile.start = QPointF(primitive.bounds.left(), primitive.bounds.center().y());
        profile.middle = primitive.bounds.center();
        profile.end = QPointF(primitive.bounds.right(), primitive.bounds.center().y());
        profile.inner = QPointF(primitive.bounds.center().x(), primitive.bounds.bottom());
        profile.spineStart = profile.start;
        profile.spineMiddle = profile.middle;
        profile.spineEnd = profile.end;
        profile.tip = profile.start;
        profile.base = profile.end;
        return {profile};
    }
    QVector<QPointF> points;
    for (const QPolygonF &contour : primitive.contours) {
        for (const QPointF &point : contour) {
            points.push_back(point);
        }
    }
    if (points.size() < 3) {
        return {};
    }

    QPointF centroid;
    for (const QPointF &point : points) {
        centroid += point;
    }
    centroid /= points.size();

    double xx = 0.0;
    double xy = 0.0;
    double yy = 0.0;
    for (const QPointF &point : points) {
        const QPointF delta = point - centroid;
        xx += delta.x() * delta.x();
        xy += delta.x() * delta.y();
        yy += delta.y() * delta.y();
    }
    const double angle = 0.5 * std::atan2(2.0 * xy, xx - yy);
    QPointF axis(std::cos(angle), std::sin(angle));
    QPointF normal(-axis.y(), axis.x());

    auto extents = [&](const QPointF &direction) {
        double minimum = std::numeric_limits<double>::max();
        double maximum = std::numeric_limits<double>::lowest();
        for (const QPointF &point : points) {
            const double value = QPointF::dotProduct(point - centroid, direction);
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        return QPair<double, double>(minimum, maximum);
    };
    QPair<double, double> along = extents(axis);
    QPair<double, double> across = extents(normal);
    if (along.second - along.first < across.second - across.first) {
        std::swap(axis, normal);
        normal = QPointF(-axis.y(), axis.x());
        along = extents(axis);
        across = extents(normal);
    }
    if (axis.x() < -kEpsilon || (std::abs(axis.x()) <= kEpsilon && axis.y() < 0.0)) {
        axis = -axis;
        normal = QPointF(-axis.y(), axis.x());
        along = extents(axis);
        across = extents(normal);
    }

    const double span = along.second - along.first;
    if (span <= kEpsilon) {
        return {};
    }
    const auto crossSection = [&](double position, double radius) {
        double minimum = std::numeric_limits<double>::max();
        double maximum = std::numeric_limits<double>::lowest();
        for (const QPointF &point : points) {
            const QPointF delta = point - centroid;
            const double u = QPointF::dotProduct(delta, axis);
            if (std::abs(u - position) > radius) {
                continue;
            }
            const double v = QPointF::dotProduct(delta, normal);
            minimum = std::min(minimum, v);
            maximum = std::max(maximum, v);
        }
        return QPair<double, double>(minimum, maximum);
    };

    const double endpointRadius = span * 0.02;
    const double radius = span * 0.08;
    const QPair<double, double> startSection = crossSection(along.first, endpointRadius);
    const QPair<double, double> endSection = crossSection(along.second, endpointRadius);
    const QPair<double, double> tipStartSection = crossSection(along.first, span * 0.04);
    const QPair<double, double> tipEndSection = crossSection(along.second, span * 0.04);
    const auto sectionCenter = [&](double position, double radius) {
        QPointF center;
        int count = 0;
        for (const QPointF &point : points) {
            if (std::abs(QPointF::dotProduct(point - centroid, axis) - position)
                <= radius) {
                center += point;
                ++count;
            }
        }
        return count > 0 ? center / count : centroid + axis * position;
    };
    const QPointF tipStartCenter = sectionCenter(along.first, span * 0.04);
    const QPointF tipEndCenter = sectionCenter(along.second, span * 0.04);
    const double middleU = (along.first + along.second) * 0.5;
    const QPair<double, double> middleSection = crossSection(middleU, radius);
    if (!std::isfinite(startSection.first) || !std::isfinite(endSection.first)
        || !std::isfinite(middleSection.first)) {
        return {};
    }
    const double middleThickness = middleSection.second - middleSection.first;
    if (middleThickness <= kEpsilon) {
        return {};
    }

    QVector<PrimitiveProfile> result;
    for (const double sourceSign : {-1.0, 1.0}) {
        PrimitiveProfile profile;
        profile.primitive = &primitive;
        profile.sourceSign = sourceSign;
        profile.start = centroid + axis * along.first
            + normal * (sourceSign > 0.0 ? startSection.second : startSection.first);
        profile.middle = centroid + axis * middleU
            + normal * ((middleSection.first + middleSection.second) * 0.5);
        profile.end = centroid + axis * along.second
            + normal * (sourceSign > 0.0 ? endSection.second : endSection.first);
        profile.inner = centroid + axis * middleU
            + normal * (sourceSign > 0.0 ? middleSection.first : middleSection.second);
        profile.spineStart = centroid + axis * along.first
            + normal * ((startSection.first + startSection.second) * 0.5);
        profile.spineMiddle = profile.middle;
        profile.spineEnd = centroid + axis * along.second
            + normal * ((endSection.first + endSection.second) * 0.5);
        const bool tipAtStart = tipStartSection.second - tipStartSection.first
            < tipEndSection.second - tipEndSection.first;
        profile.tip = tipAtStart ? tipStartCenter : tipEndCenter;
        profile.base = tipAtStart ? tipEndCenter : tipStartCenter;
        result.push_back(profile);
        std::swap(profile.start, profile.end);
        std::swap(profile.spineStart, profile.spineEnd);
        result.push_back(profile);
    }
    return result;
}

bool containedBy(const PenPrimitive &primitive,
                 const QTransform &transform,
                 const QPainterPath &allowed,
                 const QRectF &allowedBounds,
                 bool exact) {
    if (!transform.isAffine() || std::abs(transform.determinant()) <= kEpsilon) {
        return false;
    }
    const QRectF bounds = transform.mapRect(primitive.bounds);
    if (!allowedBounds.contains(bounds)) {
        return false;
    }
    for (const QPolygonF &contour : primitive.contours) {
        for (const QPointF &point : contour) {
            if (!allowed.contains(transform.map(point))) {
                return false;
            }
        }
    }
    if (!exact) {
        return true;
    }
    const QPainterPath placed = transform.map(primitive.silhouette);
    return pathArea(placed.subtracted(allowed))
        <= std::max(1e-8, pathArea(placed) * 1e-6);
}

QPointF tangentAt(const QVector<QPointF> &samples, int index) {
    const int before = std::max(0, index - 1);
    const int after = std::min(static_cast<int>(samples.size()) - 1, index + 1);
    QPointF tangent = samples[after] - samples[before];
    const double length = std::hypot(tangent.x(), tangent.y());
    if (length <= kEpsilon) {
        return QPointF(1.0, 0.0);
    }
    return tangent / length;
}

double sectionWidth(const PenPrimitive &primitive,
                    const QTransform &transform,
                    const QPointF &center,
                    const QPointF &tangent,
                    double window) {
    const QPointF normal(-tangent.y(), tangent.x());
    double minimum = std::numeric_limits<double>::max();
    double maximum = std::numeric_limits<double>::lowest();
    for (const QPolygonF &contour : primitive.contours) {
        for (const QPointF &source : contour) {
            const QPointF delta = transform.map(source) - center;
            if (std::abs(QPointF::dotProduct(delta, tangent)) > window) {
                continue;
            }
            const double across = QPointF::dotProduct(delta, normal);
            minimum = std::min(minimum, across);
            maximum = std::max(maximum, across);
        }
    }
    return minimum <= maximum && std::isfinite(minimum) && std::isfinite(maximum)
        ? maximum - minimum
        : 0.0;
}

std::optional<Candidate> fitCandidate(const PrimitiveProfile &profile,
                                      int start,
                                      int end,
                                      const QVector<QPointF> &samples,
                                      double width,
                                      double assignedArea,
                                      double widthFactor,
                                      double offsetFactor,
                                      double overlapFactor,
                                      double sideSign,
                                      bool curveAligned,
                                      const QPainterPath &allowed,
                                      const QRectF &allowedBounds,
                                      bool requireContained) {
    if (end <= start) {
        return std::nullopt;
    }
    const int middle = (start + end) / 2;
    const QPointF startTangent = tangentAt(samples, start);
    const QPointF middleTangent = tangentAt(samples, middle);
    const QPointF endTangent = tangentAt(samples, end);
    const QPointF startNormal(-startTangent.y(), startTangent.x());
    const QPointF normal(-middleTangent.y(), middleTangent.x());
    const QPointF endNormal(-endTangent.y(), endTangent.x());
    const QPointF offset = normal * (width * offsetFactor * sideSign);
    const double overlap = width * overlapFactor;
    const QPointF targetStartCenter = samples[start]
        - (start > 0 ? startTangent * overlap : QPointF());
    const QPointF targetEndCenter = samples[end]
        + (end + 1 < samples.size() ? endTangent * overlap : QPointF());
    if (profile.primitive->shapeId == 2133) {
        const QPointF pillStart = targetStartCenter + offset;
        const QPointF pillEnd = targetEndCenter + offset;
        const QPointF chord = pillEnd - pillStart;
        const double chordLength = std::hypot(chord.x(), chord.y());
        if (chordLength <= kEpsilon || profile.primitive->bounds.width() <= kEpsilon
            || profile.primitive->bounds.height() <= kEpsilon) {
            return std::nullopt;
        }
        QTransform transform;
        const QPointF center = (pillStart + pillEnd) * 0.5;
        transform.translate(center.x(), center.y());
        transform.rotateRadians(std::atan2(chord.y(), chord.x()));
        transform.scale(chordLength / profile.primitive->bounds.width(),
                        width * widthFactor * 2.0 / profile.primitive->bounds.height());
        transform.translate(-profile.primitive->bounds.center().x(),
                            -profile.primitive->bounds.center().y());
        if (requireContained
            && !containedBy(*profile.primitive, transform, allowed, allowedBounds, false)) {
            return std::nullopt;
        }
        Candidate result;
        result.start = start;
        result.end = end;
        result.primitive = profile.primitive;
        result.transform = transform;
        result.curveError = QLineF(transform.map(profile.middle), samples[middle]).length();
        result.sideSign = sideSign;
        result.overlapFactor = overlapFactor;
        result.tipAtStart = QLineF(transform.map(profile.tip), samples[start]).length()
            < QLineF(transform.map(profile.tip), samples[end]).length();
        const double placementArea =
            profile.primitive->area * std::abs(transform.determinant());
        result.score = candidateScore(placementArea,
                                      assignedArea,
                                      width,
                                      profile.primitive->shapeId);
        return result;
    }
    if (curveAligned) {
        const QPointF sourceAcross = profile.inner - profile.spineMiddle;
        const QPointF targetAcross = normal * (width * 0.5 * sideSign);
        const QPointF curveStart = targetStartCenter
            + startNormal * (width * offsetFactor * sideSign);
        const QPointF curveTargetMiddle = samples[middle]
            + normal * (width * offsetFactor * sideSign);
        const QPointF curveEnd = targetEndCenter
            + endNormal * (width * offsetFactor * sideSign);
        bool curveOk = false;
        const QTransform curveTransform = affineFromTriangles(profile.spineStart,
                                                               profile.spineMiddle,
                                                               profile.spineEnd,
                                                               curveStart,
                                                               curveTargetMiddle,
                                                               curveEnd,
                                                               &curveOk);
        bool widthOk = false;
        const QTransform widthTransform = affineFromTriangles(profile.spineStart,
                                                               profile.spineEnd,
                                                               profile.spineStart
                                                                   + sourceAcross,
                                                               curveStart,
                                                               curveEnd,
                                                               curveStart
                                                                   + targetAcross,
                                                               &widthOk);
        const auto blend = [widthFactor](double curveValue, double widthValue) {
            return curveValue * (1.0 - widthFactor) + widthValue * widthFactor;
        };
        const QTransform transform(blend(curveTransform.m11(), widthTransform.m11()),
                                   blend(curveTransform.m12(), widthTransform.m12()),
                                   blend(curveTransform.m21(), widthTransform.m21()),
                                   blend(curveTransform.m22(), widthTransform.m22()),
                                   blend(curveTransform.dx(), widthTransform.dx()),
                                   blend(curveTransform.dy(), widthTransform.dy()));
        if (!curveOk || !widthOk
            || (requireContained
                && !containedBy(*profile.primitive,
                                transform,
                                allowed,
                                allowedBounds,
                                false))) {
            return std::nullopt;
        }
        Candidate result;
        result.start = start;
        result.end = end;
        result.primitive = profile.primitive;
        result.transform = transform;
        result.curveError = QLineF(transform.map(profile.spineMiddle),
                                  samples[middle]).length();
        const double window = std::max(width * 0.5,
                                       QLineF(samples[start], samples[end]).length() * 0.04);
        result.widthError = std::abs(width
                                     - sectionWidth(*profile.primitive,
                                                    transform,
                                                    samples[middle],
                                                    middleTangent,
                                                    window));
        result.sideSign = sideSign;
        result.overlapFactor = overlapFactor;
        result.tipAtStart = QLineF(transform.map(profile.tip), samples[start]).length()
            < QLineF(transform.map(profile.tip), samples[end]).length();
        const double placementArea =
            profile.primitive->area * std::abs(transform.determinant());
        result.score = candidateScore(placementArea,
                                      assignedArea,
                                      width,
                                      profile.primitive->shapeId);
        return result;
    }
    const QPointF targetStart = targetStartCenter + offset
        + startNormal * (width * widthFactor * sideSign);
    const QPointF targetEnd = targetEndCenter + offset
        + endNormal * (width * widthFactor * sideSign);
    const QPointF targetInner = samples[middle] + offset
        - normal * (width * widthFactor * sideSign);
    bool ok = false;
    const QTransform transform = affineFromTriangles(profile.start,
                                                       profile.end,
                                                       profile.inner,
                                                       targetStart,
                                                       targetEnd,
                                                       targetInner,
                                                       &ok);
    if (!ok
        || (requireContained
            && !containedBy(*profile.primitive,
                            transform,
                            allowed,
                            allowedBounds,
                            false))) {
        return std::nullopt;
    }
    Candidate result;
    result.start = start;
    result.end = end;
    result.primitive = profile.primitive;
    result.transform = transform;
    result.curveError = QLineF(transform.map(profile.middle), samples[middle]).length();
    const double window = std::max(width * 0.5,
                                   QLineF(samples[start], samples[end]).length() * 0.04);
    result.widthError = std::abs(width
                                 - sectionWidth(*profile.primitive,
                                                transform,
                                                samples[middle],
                                                middleTangent,
                                                window));
    result.sideSign = sideSign;
    result.overlapFactor = overlapFactor;
    result.tipAtStart = QLineF(transform.map(profile.tip), samples[start]).length()
        < QLineF(transform.map(profile.tip), samples[end]).length();
    const double placementArea = profile.primitive->area * std::abs(transform.determinant());
    result.score = candidateScore(placementArea,
                                  assignedArea,
                                  width,
                                  profile.primitive->shapeId);
    return result;
}

} // namespace

LiningPath buildLiningPath(const QVector<PenPoint> &points) {
    LiningPath result;
    if (points.size() < 2) {
        result.error = QStringLiteral("A lining path needs at least two points");
        return result;
    }
    if (points.front().kind != PenPointKind::Hard
        || points.back().kind != PenPointKind::Hard) {
        result.error = QStringLiteral("A lining path needs hard endpoints");
        return result;
    }

    result.centerline.moveTo(points.front().position);
    QPointF current = points.front().position;
    int index = 1;
    while (index < points.size()) {
        const PenPoint &next = points[index];
        PenBoundarySegment segment;
        segment.start = current;
        if (next.kind == PenPointKind::Hard) {
            segment.end = next.position;
            result.centerline.lineTo(segment.end);
            ++index;
        } else {
            if (index + 1 >= points.size()) {
                result.error = QStringLiteral("A lining path ends with an unfinished curve");
                return result;
            }
            const PenPoint &after = points[index + 1];
            segment.control = next.position;
            segment.end = after.kind == PenPointKind::Hard
                ? after.position
                : (next.position + after.position) * 0.5;
            segment.curved = true;
            result.centerline.quadTo(segment.control, segment.end);
            index += after.kind == PenPointKind::Hard ? 2 : 1;
        }
        if (QLineF(segment.start, segment.end).length() > kEpsilon
            || (segment.curved && QLineF(segment.start, segment.control).length() > kEpsilon)) {
            result.segments.push_back(segment);
        }
        current = segment.end;
    }
    if (result.segments.isEmpty()) {
        result.error = QStringLiteral("The lining path has no length");
    }
    return result;
}

QPainterPath buildLiningRibbon(const QPainterPath &centerline, double width) {
    if (centerline.isEmpty() || !std::isfinite(width) || width <= 0.0) {
        return {};
    }
    QPainterPathStroker stroker;
    stroker.setWidth(width);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    return stroker.createStroke(centerline).simplified();
}

QVector<PenPrimitive> buildLiningPrimitiveCatalog(const ShapeGeometryStore &geometry) {
    QVector<PenPrimitive> result;
    result.reserve(kLiningShapeIds.size());
    for (const int shapeId : kLiningShapeIds) {
        const ShapeGeometry *shape = geometry.shape(shapeId);
        if (shape == nullptr) {
            continue;
        }
        PenPrimitive primitive = buildPenPrimitive(shapeId, *shape);
        if (!primitive.silhouette.isEmpty() && primitive.area > kEpsilon) {
            result.push_back(std::move(primitive));
        }
    }
    return result;
}

PenFillResult fillLiningPath(const LiningFillRequest &request,
                             const std::function<bool()> &cancelled) {
    LiningDiagnostic diagnostic;
    diagnostic.add(QStringLiteral("request width=%1 points=%2 primitives=%3")
                       .arg(request.width, 0, 'f', 4)
                       .arg(request.points.size())
                       .arg(request.primitives.size()));
    for (int index = 0; index < request.points.size(); ++index) {
        diagnostic.add(QStringLiteral("point[%1] kind=%2 position=%3")
                           .arg(index)
                           .arg(request.points[index].kind == PenPointKind::Hard
                                    ? QStringLiteral("hard")
                                    : QStringLiteral("soft"))
                           .arg(pointText(request.points[index].position)));
    }
    PenFillResult result;
    if (cancelled && cancelled()) {
        diagnostic.add(QStringLiteral("cancelled before path construction"));
        result.cancelled = true;
        return result;
    }
    const LiningPath path = buildLiningPath(request.points);
    if (!path.valid()) {
        diagnostic.add(QStringLiteral("path error=%1").arg(path.error));
        result.error = path.error;
        return result;
    }
    diagnostic.add(QStringLiteral("path segments=%1").arg(path.segments.size()));
    for (int index = 0; index < path.segments.size(); ++index) {
        const PenBoundarySegment &segment = path.segments[index];
        diagnostic.add(QStringLiteral("pathSegment[%1] curved=%2 start=%3 control=%4 end=%5")
                           .arg(index)
                           .arg(segment.curved ? QStringLiteral("true")
                                               : QStringLiteral("false"))
                           .arg(pointText(segment.start))
                           .arg(pointText(segment.control))
                           .arg(pointText(segment.end)));
    }
    if (!std::isfinite(request.width) || request.width <= 0.0) {
        diagnostic.add(QStringLiteral("invalid width"));
        result.error = QStringLiteral("Lining width must be positive");
        return result;
    }

    const QPainterPath ribbon = buildLiningRibbon(path.centerline, request.width);
    result.targetArea = pathArea(ribbon);
    const int pointCount = static_cast<int>(request.points.size());
    result.shapeLimit = std::min(kMaximumPlacements,
                                 std::max(2, pointCount + std::max(1, pointCount / 2)));
    diagnostic.add(QStringLiteral("ribbon area=%1 shapeLimit=%2")
                       .arg(result.targetArea, 0, 'f', 4)
                       .arg(result.shapeLimit));
    if (ribbon.isEmpty() || result.targetArea <= kEpsilon) {
        diagnostic.add(QStringLiteral("ribbon has no fillable area"));
        result.error = QStringLiteral("The lining path has no fillable area");
        return result;
    }

    QVector<PrimitiveProfile> profiles;
    for (const PenPrimitive &primitive : request.primitives) {
        if (std::find(kLiningShapeIds.cbegin(), kLiningShapeIds.cend(), primitive.shapeId)
            == kLiningShapeIds.cend()) {
            continue;
        }
        profiles += primitiveProfiles(primitive);
    }
    if (profiles.isEmpty()) {
        diagnostic.add(QStringLiteral("primitive profiles unavailable"));
        result.error = QStringLiteral("Lining Primitive geometry is unavailable");
        return result;
    }

    QVector<int> segmentBoundaries;
    const QVector<QPointF> samples = sampleCenterline(path,
                                                      request.width,
                                                      &segmentBoundaries);
    const QVector<double> lengths = cumulativeLengths(samples);
    if (samples.size() < 2 || lengths.back() <= kEpsilon
        || segmentBoundaries.size() != path.segments.size() + 1) {
        diagnostic.add(QStringLiteral("sampled path has no length"));
        result.error = QStringLiteral("The lining path has no length");
        return result;
    }
    diagnostic.add(QStringLiteral("samples=%1 length=%2 boundaries=%3")
                       .arg(samples.size())
                       .arg(lengths.back(), 0, 'f', 4)
                       .arg(segmentBoundaries.size()));
    QStringList boundaryText;
    for (const int boundary : segmentBoundaries) {
        boundaryText.push_back(QString::number(boundary));
    }
    diagnostic.add(QStringLiteral("segmentBoundaries=%1").arg(boundaryText.join(',')));
    for (int index = 0; index < samples.size(); ++index) {
        diagnostic.add(QStringLiteral("sample[%1] length=%2 position=%3")
                           .arg(index)
                           .arg(lengths[index], 0, 'f', 4)
                           .arg(pointText(samples[index])));
    }

    const QPainterPath allowed = ribbon;
    const QRectF allowedBounds = allowed.boundingRect();

    constexpr std::array<double, 5> widthFactors = {0.50, 0.47, 0.43, 0.36, 0.30};
    constexpr std::array<double, 7> curveBlendFactors = {1.0, 0.70, 0.45, 0.25, 0.12, 0.06, 0.0};
    constexpr std::array<double, 7> offsetFactors = {
        0.0, -0.04, 0.04, -0.08, 0.08, -0.12, 0.12,
    };
    constexpr std::array<double, 3> overlapFactors = {0.35, 0.22, 0.0};
    constexpr double chainedCurveOverlap = 0.45;

    const auto unitDirection = [](const QPointF &vector) {
        const double length = std::hypot(vector.x(), vector.y());
        return length <= kEpsilon ? QPointF() : vector / length;
    };
    const auto dominantCurveSide = [&](int start, int end) {
        const QPointF chord = samples[end] - samples[start];
        int positive = 0;
        int negative = 0;
        double signedDistance = 0.0;
        for (int index = start + 1; index < end; ++index) {
            const double side = cross(chord, samples[index] - samples[start]);
            signedDistance += side;
            if (side > kEpsilon) {
                ++positive;
            } else if (side < -kEpsilon) {
                ++negative;
            }
        }
        if (positive != negative) {
            return positive > negative ? 1.0 : -1.0;
        }
        if (std::abs(signedDistance) > kEpsilon) {
            return signedDistance > 0.0 ? 1.0 : -1.0;
        }
        return 0.0;
    };
    const double pathSideSign = [&] {
        const double side = dominantCurveSide(0, samples.size() - 1);
        return side != 0.0 ? side : 1.0;
    }();
    QPointF pathDirection = unitDirection(samples.back() - samples.front());
    if (pathDirection.isNull()) {
        pathDirection = unitDirection(samples[samples.size() / 2] - samples.front());
    }
    if (pathDirection.isNull()) {
        pathDirection = QPointF(1.0, 0.0);
    }
    const QPointF hairDirection(-pathDirection.y() * pathSideSign,
                                pathDirection.x() * pathSideSign);
    diagnostic.add(QStringLiteral("pathSide=%1 hairDirection=%2 chord=%3 -> %4")
                       .arg(pathSideSign, 0, 'f', 0)
                       .arg(pointText(hairDirection))
                       .arg(pointText(samples.front()))
                       .arg(pointText(samples.back())));
    const auto absoluteIndex = [&](double position) {
        const auto found = std::lower_bound(lengths.cbegin(), lengths.cend(), position);
        if (found == lengths.cbegin()) {
            return 0;
        }
        if (found == lengths.cend()) {
            return static_cast<int>(lengths.size()) - 1;
        }
        const int upper = static_cast<int>(found - lengths.cbegin());
        const int lower = upper - 1;
        return position - lengths[lower] <= lengths[upper] - position ? lower : upper;
    };

    const auto bestFit = [&](int start,
                             int end,
                             const QVector<int> &allowedShapeIds,
                             double fitWidth,
                             bool curvePriority,
                             double forcedSideSign = 0.0,
                             int forcedHairTipAtStart = -1,
                             int forcedOffsetSign = 0,
                             bool requireContained = true,
                             double minimumOverlapFactor = 0.0) {
        if (end <= start) {
            return std::optional<Candidate>{};
        }
        if (cancelled && cancelled()) {
            return std::optional<Candidate>{};
        }
        std::optional<Candidate> best;
        for (const double overlapFactor : overlapFactors) {
            if (overlapFactor + kEpsilon < minimumOverlapFactor) {
                continue;
            }
            const int factorCount = curvePriority
                ? static_cast<int>(curveBlendFactors.size())
                : static_cast<int>(widthFactors.size());
            for (int factorIndex = 0; factorIndex < factorCount; ++factorIndex) {
                const double widthFactor = curvePriority
                    ? curveBlendFactors[factorIndex]
                    : widthFactors[factorIndex];
                for (const PrimitiveProfile &profile : profiles) {
                    const int shapePriority = allowedShapeIds.indexOf(
                        profile.primitive->shapeId);
                    if (shapePriority < 0) {
                        continue;
                    }
                    const bool curveAligned = curvePriority
                        && (profile.primitive->shapeId != 2112 || fitWidth <= 3.0);
                    if (curvePriority && !curveAligned && widthFactor > 0.45) {
                        continue;
                    }
                    if (curveAligned && profile.sourceSign < 0.0) {
                        continue;
                    }
                    for (const double sideSign : {-1.0, 1.0}) {
                        if (forcedSideSign != 0.0 && sideSign != forcedSideSign) {
                            continue;
                        }
                        for (const double offsetFactor : offsetFactors) {
                            if (forcedOffsetSign != 0
                                && offsetFactor * forcedOffsetSign <= 0.0) {
                                continue;
                            }
                            if (curvePriority && offsetFactor != 0.0
                                && best.has_value() && best->curveError <= 1e-6) {
                                continue;
                            }
                            if (profile.primitive->shapeId == 2133
                                && (sideSign < 0.0 || offsetFactor != 0.0)) {
                                continue;
                            }
                            const std::optional<Candidate> candidate =
                                fitCandidate(profile,
                                             start,
                                             end,
                                             samples,
                                             fitWidth,
                                             (lengths[end] - lengths[start]) * fitWidth,
                                             widthFactor,
                                             offsetFactor,
                                             overlapFactor,
                                             sideSign,
                                             curveAligned,
                                             allowed,
                                             allowedBounds,
                                             requireContained);
                            if (!candidate.has_value()) {
                                continue;
                            }
                            if (forcedHairTipAtStart >= 0
                                && candidate->primitive->shapeId == 2112
                                && candidate->tipAtStart
                                    != (forcedHairTipAtStart != 0)) {
                                continue;
                            }
                            if (forcedHairTipAtStart < 0
                                && candidate->primitive->shapeId == 2112) {
                                const QPointF placedDirection =
                                    candidate->transform.map(profile.tip)
                                    - candidate->transform.map(profile.base);
                                if (QPointF::dotProduct(placedDirection, hairDirection)
                                    <= kEpsilon) {
                                    continue;
                                }
                            }
                            bool replace = !best.has_value();
                            if (best.has_value() && curvePriority) {
                                const double tolerance = std::max(0.20,
                                                                  fitWidth);
                                if (candidate->curveError + tolerance < best->curveError) {
                                    replace = true;
                                } else if (std::abs(candidate->curveError - best->curveError)
                                               <= tolerance) {
                                    const double widthTolerance = std::max(0.05,
                                                                           fitWidth * 0.02);
                                    if (candidate->widthError + widthTolerance
                                        < best->widthError) {
                                        replace = true;
                                    } else if (std::abs(candidate->widthError
                                                        - best->widthError)
                                                   <= widthTolerance
                                               && (candidate->overlapFactor
                                                       > best->overlapFactor
                                                   || (candidate->overlapFactor
                                                           == best->overlapFactor
                                                       && candidate->score > best->score))) {
                                        replace = true;
                                    }
                                }
                            } else if (best.has_value()
                                       && (candidate->score > best->score
                                           || (std::abs(candidate->score - best->score) <= 1e-6
                                               && candidate->overlapFactor
                                                   > best->overlapFactor))) {
                                replace = true;
                            }
                            if (replace
                                && (!requireContained
                                    || containedBy(*candidate->primitive,
                                                   candidate->transform,
                                                   allowed,
                                                   allowedBounds,
                                                   true))) {
                                best = candidate;
                            }
                        }
                    }
                }
            }
        }
        return best;
    };

    const auto isAuthoredHard = [&](const QPointF &position) {
        return std::any_of(request.points.cbegin(),
                           request.points.cend(),
                           [&](const PenPoint &point) {
            return point.kind == PenPointKind::Hard
                && QLineF(point.position, position).length() <= 1e-5;
        });
    };
    QVector<bool> sharpJoins(path.segments.size() + 1, false);
    for (int join = 1; join < path.segments.size(); ++join) {
        const QPointF position = path.segments[join].start;
        if (!isAuthoredHard(position)) {
            continue;
        }
        const PenBoundarySegment &before = path.segments[join - 1];
        const PenBoundarySegment &after = path.segments[join];
        const QPointF incoming = unitDirection(before.end
                                               - (before.curved
                                                      ? before.control
                                                      : before.start));
        const QPointF outgoing = unitDirection((after.curved
                                                    ? after.control
                                                    : after.end)
                                               - after.start);
        const double dot = std::clamp(QPointF::dotProduct(incoming, outgoing),
                                      -1.0,
                                      1.0);
        sharpJoins[join] = std::acos(dot) >= std::numbers::pi / 3.0;
        diagnostic.add(QStringLiteral("join[%1] hard=true sharp=%2 dot=%3")
                           .arg(join)
                           .arg(sharpJoins[join] ? QStringLiteral("true")
                                                : QStringLiteral("false"))
                           .arg(dot, 0, 'f', 6));
    }
    QVector<int> curveRunSizes(path.segments.size(), 1);
    QVector<int> curveRunOffsets(path.segments.size(), 0);
    QVector<int> curveRunStarts(path.segments.size(), 0);
    QVector<int> curveRunEnds(path.segments.size(), 0);
    for (int runStart = 0; runStart < path.segments.size();) {
        if (!path.segments[runStart].curved) {
            ++runStart;
            continue;
        }
        int runEnd = runStart + 1;
        while (runEnd < path.segments.size()
               && path.segments[runEnd].curved
               && !isAuthoredHard(path.segments[runEnd].start)) {
            ++runEnd;
        }
        const int runSize = runEnd - runStart;
        for (int index = runStart; index < runEnd; ++index) {
            curveRunSizes[index] = runSize;
            curveRunOffsets[index] = index - runStart;
            curveRunStarts[index] = runStart;
            curveRunEnds[index] = runEnd;
        }
        runStart = runEnd;
    }

    struct SegmentFit {
        int start = 0;
        int end = 0;
        QVector<int> shapeIds;
        bool curvePriority = false;
        bool shallowCurve = false;
        double curveSideSign = 0.0;
        double minimumOverlapFactor = 0.0;
        int forcedHairTipAtStart = -1;
    };
    QVector<SegmentFit> segmentFits;
    segmentFits.reserve(path.segments.size());
    const auto shapeIdsText = [](const QVector<int> &shapeIds) {
        QStringList values;
        for (const int shapeId : shapeIds) {
            values.push_back(QString::number(shapeId));
        }
        return values.join(',');
    };
    for (int index = 0; index < path.segments.size(); ++index) {
        const PenBoundarySegment &segment = path.segments[index];
        SegmentFit fit;
        fit.start = segmentBoundaries[index];
        fit.end = segmentBoundaries[index + 1];
        if (!segment.curved) {
            if (sharpJoins[index] != sharpJoins[index + 1]) {
                fit.forcedHairTipAtStart = sharpJoins[index + 1] ? 1 : 0;
            }
            fit.shapeIds = (sharpJoins[index] || sharpJoins[index + 1])
                ? QVector<int>{2112, 2133}
                : QVector<int>{2133};
            diagnostic.add(QStringLiteral("fitSegment[%1] samples=%2..%3 curved=false "
                                          "forcedTip=%4 shapes=%5")
                               .arg(index)
                               .arg(fit.start)
                               .arg(fit.end)
                               .arg(fit.forcedHairTipAtStart)
                               .arg(shapeIdsText(fit.shapeIds)));
            segmentFits.push_back(std::move(fit));
            continue;
        }

        fit.curvePriority = true;
        fit.curveSideSign = pathSideSign;
        if (curveRunSizes[index] > 1) {
            const int runStart = curveRunStarts[index];
            const int runEnd = curveRunEnds[index];
            const int runOffset = curveRunOffsets[index];
            const double runStartLength = lengths[segmentBoundaries[runStart]];
            const double runEndLength = lengths[segmentBoundaries[runEnd]];
            const double runLength = runEndLength - runStartLength;
            const double strideFactor = 1.0 - chainedCurveOverlap;
            const double windowLength = runLength
                / (1.0 + (curveRunSizes[index] - 1) * strideFactor);
            const double stride = windowLength * strideFactor;
            const double windowStart = runStartLength + runOffset * stride;
            const double windowEnd = runOffset + 1 == curveRunSizes[index]
                ? runEndLength
                : windowStart + windowLength;
            fit.start = runOffset == 0
                ? segmentBoundaries[runStart]
                : absoluteIndex(windowStart);
            fit.end = runOffset + 1 == curveRunSizes[index]
                ? segmentBoundaries[runEnd]
                : absoluteIndex(windowEnd);
        }
        const QPointF startDirection = unitDirection(segment.control - segment.start);
        const QPointF endDirection = unitDirection(segment.end - segment.control);
        const double tangentDot = std::clamp(QPointF::dotProduct(startDirection,
                                                                 endDirection),
                                             -1.0,
                                             1.0);
        const double turn = std::acos(tangentDot);
        const double spanLength = lengths[fit.end] - lengths[fit.start];
        const bool shortCurve = spanLength <= request.width * 5.0;
        fit.shallowCurve = turn <= std::numbers::pi * 0.30;
        if (curveRunSizes[index] > 1) {
            fit.shapeIds = {136};
            fit.minimumOverlapFactor = overlapFactors.front();
        } else if (shortCurve) {
            fit.shapeIds = {136, 2131, 2112};
        } else if (fit.shallowCurve) {
            fit.shapeIds = {2112};
        } else {
            fit.shapeIds = {2131, 136, 2112};
        }
        diagnostic.add(QStringLiteral("fitSegment[%1] samples=%2..%3 curved=true "
                                      "side=%4 shallow=%5 runSlot=%6/%7 turn=%8 length=%9 "
                                      "overlap=%10 forcedTip=%11 shapes=%12")
                           .arg(index)
                           .arg(fit.start)
                           .arg(fit.end)
                           .arg(fit.curveSideSign, 0, 'f', 0)
                           .arg(fit.shallowCurve ? QStringLiteral("true")
                                                 : QStringLiteral("false"))
                           .arg(curveRunOffsets[index] + 1)
                           .arg(curveRunSizes[index])
                           .arg(turn, 0, 'f', 6)
                           .arg(spanLength, 0, 'f', 4)
                           .arg(curveRunSizes[index] > 1
                                    ? chainedCurveOverlap
                                    : fit.minimumOverlapFactor,
                                0,
                                'f',
                                2)
                           .arg(fit.forcedHairTipAtStart)
                           .arg(shapeIdsText(fit.shapeIds)));
        segmentFits.push_back(std::move(fit));
    }

    const auto placedPath = [](const Candidate &candidate) {
        return candidate.transform.map(candidate.primitive->silhouette);
    };
    const double minimumOverlapArea = std::max(1e-4,
                                                request.width * request.width * 0.001);
    const auto overlaps = [&](const Candidate &a, const Candidate &b) {
        return pathArea(placedPath(a).intersected(placedPath(b))) >= minimumOverlapArea;
    };

    QVector<Candidate> selected;
    bool usedForcedCandidate = false;
    for (int segmentIndex = 0; segmentIndex < segmentFits.size(); ++segmentIndex) {
        const SegmentFit &fit = segmentFits[segmentIndex];
        if (cancelled && cancelled()) {
            diagnostic.add(QStringLiteral("cancelled before fitSegment[%1]").arg(segmentIndex));
            result.cancelled = true;
            return result;
        }
        bool forced = false;
        std::optional<Candidate> candidate = bestFit(fit.start,
                                                     fit.end,
                                                     fit.shapeIds,
                                                     request.width,
                                                     fit.curvePriority,
                                                     fit.curveSideSign,
                                                     fit.forcedHairTipAtStart,
                                                     0,
                                                     true,
                                                     fit.minimumOverlapFactor);
        if (!candidate.has_value()) {
            candidate = bestFit(fit.start,
                                fit.end,
                                fit.shapeIds,
                                request.width,
                                fit.curvePriority,
                                fit.curveSideSign,
                                fit.forcedHairTipAtStart,
                                0,
                                false,
                                fit.minimumOverlapFactor);
            forced = candidate.has_value();
            usedForcedCandidate = usedForcedCandidate || candidate.has_value();
        }
        if (candidate.has_value()) {
            diagnostic.add(QStringLiteral("selected[%1] forced=%2 %3")
                               .arg(segmentIndex)
                               .arg(forced ? QStringLiteral("true")
                                           : QStringLiteral("false"))
                               .arg(candidateText(*candidate)));
            selected.push_back(*candidate);
            continue;
        }
        diagnostic.add(QStringLiteral("fitSegment[%1] produced no geometric candidate")
                           .arg(segmentIndex));
        result.error = QStringLiteral("No contained Primitive fits a lining span");
        result.unfilled = ribbon;
        return result;
    }

    QVector<Candidate> overlays;
    if (segmentFits.size() == 1 && segmentFits.front().shallowCurve
        && selected.size() == 1 && selected.front().primitive->shapeId == 2112
        && selected.size() < result.shapeLimit) {
        const std::optional<Candidate> complement = bestFit(selected.front().start,
                                                            selected.front().end,
                                                            {2112},
                                                            request.width,
                                                            true,
                                                            segmentFits.front().curveSideSign,
                                                            selected.front().tipAtStart ? 0 : 1);
        if (complement.has_value() && overlaps(selected.front(), *complement)) {
            diagnostic.add(QStringLiteral("complement %1").arg(candidateText(*complement)));
            overlays.push_back(*complement);
        } else {
            diagnostic.add(QStringLiteral("complement unavailable or disconnected"));
        }
    }
    for (int join = 1; join < selected.size(); ++join) {
        const Candidate &left = selected[join - 1];
        const Candidate &right = selected[join];
        if (overlaps(left, right)) {
            diagnostic.add(QStringLiteral("corner[%1] already overlaps left=%2 right=%3")
                               .arg(join)
                               .arg(left.primitive->shapeId)
                               .arg(right.primitive->shapeId));
            continue;
        }
        if (selected.size() + overlays.size() >= result.shapeLimit) {
            result.error = QStringLiteral("The lining requires more overlapping corner shapes");
            result.unfilled = ribbon;
            return result;
        }
        bool sharpCorner = false;
        for (int boundary = 1; boundary + 1 < segmentBoundaries.size(); ++boundary) {
            if (sharpJoins[boundary]
                && left.end == segmentBoundaries[boundary]
                && right.start == segmentBoundaries[boundary]) {
                sharpCorner = true;
                break;
            }
        }
        const bool curvedJoin = segmentFits[join - 1].curvePriority
            || segmentFits[join].curvePriority;
        const QVector<int> connectorShapeIds = sharpCorner
            ? QVector<int>{2133}
            : (curvedJoin ? QVector<int>{2112} : QVector<int>{2112, 2133});
        const double joinLength = (lengths[left.end] + lengths[right.start]) * 0.5;
        const QPointF corner = samples[absoluteIndex(joinLength)];
        diagnostic.add(QStringLiteral("corner[%1] position=%2 sharp=%3 curved=%4 shapes=%5")
                           .arg(join)
                           .arg(pointText(corner))
                           .arg(sharpCorner ? QStringLiteral("true")
                                            : QStringLiteral("false"))
                           .arg(curvedJoin ? QStringLiteral("true")
                                           : QStringLiteral("false"))
                           .arg(shapeIdsText(connectorShapeIds)));
        bool bridged = false;
        std::optional<Candidate> forcedConnector;
        double forcedCoverage = -1.0;
        for (const double spanFactor : {3.0, 5.0, 7.0}) {
            const double connectorLength = request.width * spanFactor;
            const int connectorStart = std::max(left.start,
                                                absoluteIndex(joinLength - connectorLength * 0.5));
            const int connectorEnd = std::min(right.end,
                                              absoluteIndex(joinLength + connectorLength * 0.5));
            std::optional<Candidate> connector = bestFit(connectorStart,
                                                         connectorEnd,
                                                         connectorShapeIds,
                                                         request.width,
                                                         false,
                                                         0.0,
                                                         -1);
            if (connector.has_value() && overlaps(left, *connector)
                && overlaps(*connector, right)) {
                diagnostic.add(QStringLiteral("corner[%1] contained spanFactor=%2 %3")
                                   .arg(join)
                                   .arg(spanFactor, 0, 'f', 1)
                                   .arg(candidateText(*connector)));
                overlays.push_back(*connector);
                bridged = true;
                break;
            }
            if (!connector.has_value()) {
                connector = bestFit(connectorStart,
                                    connectorEnd,
                                    connectorShapeIds,
                                    request.width,
                                    false,
                                    0.0,
                                    -1,
                                    0,
                                    false);
            }
            if (!connector.has_value()) {
                continue;
            }
            Candidate nudged = *connector;
            const QPointF center = nudged.transform.map(
                nudged.primitive->bounds.center());
            const QPointF shift = corner - center;
            const QTransform transform = nudged.transform;
            nudged.transform = QTransform(transform.m11(),
                                          transform.m12(),
                                          transform.m21(),
                                          transform.m22(),
                                          transform.dx() + shift.x(),
                                          transform.dy() + shift.y());
            const double leftCoverage = pathArea(
                placedPath(left).intersected(placedPath(nudged)));
            const double rightCoverage = pathArea(
                placedPath(right).intersected(placedPath(nudged)));
            const double coverage = std::min(leftCoverage, rightCoverage)
                + (leftCoverage + rightCoverage) * 0.01;
            diagnostic.add(QStringLiteral("corner[%1] nudge spanFactor=%2 leftArea=%3 "
                                          "rightArea=%4 rank=%5 %6")
                               .arg(join)
                               .arg(spanFactor, 0, 'f', 1)
                               .arg(leftCoverage, 0, 'f', 6)
                               .arg(rightCoverage, 0, 'f', 6)
                               .arg(coverage, 0, 'f', 6)
                               .arg(candidateText(nudged)));
            if (!forcedConnector.has_value() || coverage > forcedCoverage) {
                forcedConnector = nudged;
                forcedCoverage = coverage;
            }
        }
        if (!bridged && forcedConnector.has_value()) {
            diagnostic.add(QStringLiteral("corner[%1] forced %2")
                               .arg(join)
                               .arg(candidateText(*forcedConnector)));
            overlays.push_back(*forcedConnector);
            usedForcedCandidate = true;
            bridged = true;
        }
        if (!bridged) {
            diagnostic.add(QStringLiteral("corner[%1] produced no connector").arg(join));
            result.error = QStringLiteral("No contained Primitive can overlap a lining corner");
            result.unfilled = ribbon;
            return result;
        }
    }
    const int primaryCount = selected.size();
    selected += overlays;
    if (selected.size() > result.shapeLimit) {
        result.error = QStringLiteral("The lining requires too many Primitive shapes");
        result.unfilled = ribbon;
        return result;
    }

    const auto placementWidth = [&](const Candidate &candidate) {
        if (candidate.primitive->shapeId == 2133) {
            return std::hypot(candidate.transform.m21(), candidate.transform.m22())
                * candidate.primitive->bounds.height();
        }
        const int middle = (candidate.start + candidate.end) / 2;
        const double window = std::max(
            request.width * 0.5,
            QLineF(samples[candidate.start], samples[candidate.end]).length() * 0.04);
        return sectionWidth(*candidate.primitive,
                            candidate.transform,
                            samples[middle],
                            tangentAt(samples, middle),
                            window);
    };
    QVector<double> inheritedWidths(primaryCount, 0.0);
    for (int index = 0; index < primaryCount; ++index) {
        if (selected[index].primitive->shapeId != 2133) {
            inheritedWidths[index] = std::min(placementWidth(selected[index]),
                                              request.width);
            diagnostic.add(QStringLiteral("widthAnchor[%1] shape=%2 width=%3")
                               .arg(index)
                               .arg(selected[index].primitive->shapeId)
                               .arg(inheritedWidths[index], 0, 'f', 6));
        }
    }
    for (int runStart = 0; runStart < primaryCount;) {
        if (selected[runStart].primitive->shapeId != 2133) {
            ++runStart;
            continue;
        }
        int runEnd = runStart;
        while (runEnd < primaryCount
               && selected[runEnd].primitive->shapeId == 2133) {
            ++runEnd;
        }
        const double leftWidth = runStart > 0 ? inheritedWidths[runStart - 1] : 0.0;
        const double rightWidth = runEnd < primaryCount ? inheritedWidths[runEnd] : 0.0;
        const int runLength = runEnd - runStart;
        for (int offset = 0; offset < runLength; ++offset) {
            if (leftWidth > kEpsilon && rightWidth > kEpsilon) {
                const double ratio = static_cast<double>(offset + 1)
                    / static_cast<double>(runLength + 1);
                inheritedWidths[runStart + offset] = leftWidth * (1.0 - ratio)
                    + rightWidth * ratio;
            } else if (leftWidth > kEpsilon) {
                inheritedWidths[runStart + offset] = leftWidth;
            } else if (rightWidth > kEpsilon) {
                inheritedWidths[runStart + offset] = rightWidth;
            }
            diagnostic.add(QStringLiteral("widthInherited[%1] left=%2 right=%3 result=%4")
                               .arg(runStart + offset)
                               .arg(leftWidth, 0, 'f', 6)
                               .arg(rightWidth, 0, 'f', 6)
                               .arg(inheritedWidths[runStart + offset], 0, 'f', 6));
        }
        runStart = runEnd;
    }
    const auto applyInheritedWidth = [&](Candidate &candidate, double targetWidth) {
        if (candidate.primitive->shapeId != 2133 || targetWidth <= kEpsilon) {
            return;
        }
        const double currentWidth = placementWidth(candidate);
        if (currentWidth <= targetWidth + kEpsilon) {
            diagnostic.add(QStringLiteral("widthApply shape=%1 span=%2..%3 current=%4 "
                                          "target=%5 changed=false")
                               .arg(candidate.primitive->shapeId)
                               .arg(candidate.start)
                               .arg(candidate.end)
                               .arg(currentWidth, 0, 'f', 6)
                               .arg(targetWidth, 0, 'f', 6));
            return;
        }
        const double scale = targetWidth / currentWidth;
        const double centerY = candidate.primitive->bounds.center().y();
        const QTransform transform = candidate.transform;
        candidate.transform = QTransform(transform.m11(),
                                         transform.m12(),
                                         transform.m21() * scale,
                                         transform.m22() * scale,
                                         transform.dx()
                                             + transform.m21() * centerY * (1.0 - scale),
                                         transform.dy()
                                             + transform.m22() * centerY * (1.0 - scale));
        diagnostic.add(QStringLiteral("widthApply shape=%1 span=%2..%3 current=%4 target=%5 "
                                      "scale=%6 transform=%7")
                           .arg(candidate.primitive->shapeId)
                           .arg(candidate.start)
                           .arg(candidate.end)
                           .arg(currentWidth, 0, 'f', 6)
                           .arg(targetWidth, 0, 'f', 6)
                           .arg(scale, 0, 'f', 6)
                           .arg(transformText(candidate.transform)));
    };
    for (int index = 0; index < primaryCount; ++index) {
        applyInheritedWidth(selected[index], inheritedWidths[index]);
    }
    for (int index = primaryCount; index < selected.size(); ++index) {
        Candidate &candidate = selected[index];
        if (candidate.primitive->shapeId != 2133) {
            continue;
        }
        const double position = (lengths[candidate.start] + lengths[candidate.end]) * 0.5;
        int left = -1;
        int right = -1;
        for (int primary = 0; primary < primaryCount; ++primary) {
            if (inheritedWidths[primary] <= kEpsilon) {
                continue;
            }
            const double primaryPosition = (lengths[selected[primary].start]
                                            + lengths[selected[primary].end])
                * 0.5;
            if (primaryPosition <= position) {
                left = primary;
            }
            if (primaryPosition >= position) {
                right = primary;
                break;
            }
        }
        double targetWidth = 0.0;
        if (left >= 0 && right >= 0 && left != right) {
            const double leftPosition = (lengths[selected[left].start]
                                         + lengths[selected[left].end])
                * 0.5;
            const double rightPosition = (lengths[selected[right].start]
                                          + lengths[selected[right].end])
                * 0.5;
            const double distance = rightPosition - leftPosition;
            const double ratio = distance > kEpsilon
                ? std::clamp((position - leftPosition) / distance, 0.0, 1.0)
                : 0.5;
            targetWidth = inheritedWidths[left] * (1.0 - ratio)
                + inheritedWidths[right] * ratio;
        } else if (left >= 0) {
            targetWidth = inheritedWidths[left];
        } else if (right >= 0) {
            targetWidth = inheritedWidths[right];
        }
        applyInheritedWidth(candidate, targetWidth);
    }

    for (const Candidate &candidate : std::as_const(selected)) {
        if (candidate.primitive->shapeId != 2112) {
            continue;
        }
        const auto profile = std::find_if(profiles.cbegin(),
                                          profiles.cend(),
                                          [&](const PrimitiveProfile &value) {
            return value.primitive == candidate.primitive;
        });
        if (profile == profiles.cend()) {
            continue;
        }
        const QPointF placedTip = candidate.transform.map(profile->tip);
        const QPointF sourceBase = profile->base;
        const QPointF placedBase = candidate.transform.map(sourceBase);
        const int middle = (candidate.start + candidate.end) / 2;
        const QPointF tangent = tangentAt(samples, middle);
        const double side = cross(tangent, placedTip - placedBase);
        const double directionAlignment = QPointF::dotProduct(placedTip - placedBase,
                                                               hairDirection);
        const double finalCurveError = QLineF(candidate.transform.map(profile->spineMiddle),
                                              samples[middle]).length();
        diagnostic.add(QStringLiteral("hairFinal span=%1..%2 tip=%3 base=%4 tangent=%5 "
                                      "side=%6 directionAlignment=%7 tipAtStart=%8 "
                                      "curveError=%9 transform=%10")
                           .arg(candidate.start)
                           .arg(candidate.end)
                           .arg(pointText(placedTip))
                           .arg(pointText(placedBase))
                           .arg(pointText(tangent))
                           .arg(side, 0, 'f', 6)
                           .arg(directionAlignment, 0, 'f', 6)
                           .arg(candidate.tipAtStart ? QStringLiteral("true")
                                                     : QStringLiteral("false"))
                           .arg(finalCurveError, 0, 'f', 6)
                           .arg(transformText(candidate.transform)));
    }

    QPainterPath coverage;
    for (int index = 0; index < selected.size(); ++index) {
        const Candidate &candidate = selected[index];
        PenPlacement placement;
        placement.shapeId = candidate.primitive->shapeId;
        placement.transform = candidate.transform;
        placement.area = candidate.primitive->area * std::abs(candidate.transform.determinant());
        result.placements.push_back(placement);
        coverage = coverage.united(candidate.transform.map(candidate.primitive->silhouette));
        diagnostic.add(QStringLiteral("final[%1] %2 area=%3")
                           .arg(index)
                           .arg(candidateText(candidate))
                           .arg(placement.area, 0, 'f', 6));
    }
    result.coveredArea = pathArea(coverage.intersected(ribbon));
    result.outsideArea = pathArea(coverage.subtracted(ribbon));
    result.unfilled = ribbon.subtracted(coverage);
    diagnostic.add(QStringLiteral("summary placements=%1 targetArea=%2 coveredArea=%3 "
                                  "outsideArea=%4 forced=%5")
                       .arg(result.placements.size())
                       .arg(result.targetArea, 0, 'f', 6)
                       .arg(result.coveredArea, 0, 'f', 6)
                       .arg(result.outsideArea, 0, 'f', 6)
                       .arg(usedForcedCandidate ? QStringLiteral("true")
                                                : QStringLiteral("false")));
    if (!usedForcedCandidate
        && result.outsideArea > std::max(1e-6, result.targetArea * 1e-5)) {
        diagnostic.add(QStringLiteral("final rejection outside tolerance"));
        result.placements.clear();
        result.error = QStringLiteral("No Primitive sequence remains within the lining border");
    }
    return result;
}

} // namespace gui
