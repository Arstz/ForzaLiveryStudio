#include "contour_mesh.h"
#include "contour_partition.h"

#include "pen_fill.h"
#include "contour_transform.h"
#include "fill_geometry.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>

namespace gui {
namespace {

using fls::scene::MeshData;
using fls::scene::MeshPoint;

constexpr int kProjectionSeeds = 16;
constexpr int kProjectionIterations = 24;
constexpr int kIntersectionDepth = 32;
constexpr int kArcValidationSamples = 8;
constexpr double kArcTangentCosine = 0.8660254037844386;
constexpr int kRegionSubdivisionDepth = 20;
constexpr int kRegionValidationSamples = 16;
constexpr double kRegionAreaRoundoff = 8.0;
constexpr double kBoundaryApproximationCount = 2.0;
constexpr int kMaximumRegionDiagnostics = 512;
constexpr int kMaximumBoundaryDiagnostics = 64;

class MeshDiagnostics {
public:
    explicit MeshDiagnostics(const MeshData &mesh) {
        timer_.start();
        report_.insert(QStringLiteral("version"), 1);
        report_.insert(QStringLiteral("inputContours"), mesh.contours.size());
        report_.insert(QStringLiteral("inputVertices"), mesh.vertices.size());
    }

    void set(const QString &name, const QJsonValue &value) { report_.insert(name, value); }

    void decision(const QString &reason, QJsonObject detail = {}) {
        ++counts_[reason];
        detail.insert(QStringLiteral("decision"), reason);
        if (regions_.size() < kMaximumRegionDiagnostics) regions_.append(detail);
        else ++omitted_;
    }

    QJsonObject finish(bool accepted, const QString &stage, const QString &reason = {}) {
        QJsonObject counts;
        for (auto entry = counts_.cbegin(); entry != counts_.cend(); ++entry) counts.insert(entry.key(), entry.value());
        report_.insert(QStringLiteral("accepted"), accepted);
        report_.insert(QStringLiteral("stage"), stage);
        report_.insert(QStringLiteral("reason"), reason);
        report_.insert(QStringLiteral("elapsedMs"), timer_.elapsed());
        report_.insert(QStringLiteral("decisions"), regions_);
        report_.insert(QStringLiteral("decisionCounts"), counts);
        report_.insert(QStringLiteral("omittedDecisions"), omitted_);

        return report_;
    }

private:
    QJsonObject report_;
    QJsonArray regions_;
    QHash<QString, int> counts_;
    QElapsedTimer timer_;
    int omitted_ = 0;
};

struct Cut {
    int vertex = -1;
    double parameter = 0.0;
};

struct Span {
    PenBoundarySegment curve;
    QVector<Cut> cuts;
    int loop = -1;
    int point = -1;
    int contour = -1;
};

struct Loop {
    QVector<MeshPoint> points;
    QVector<int> spans;
    int contour = -1;
    int cutout = -1;
    bool closed = false;
};

struct Projection {
    QPointF position;
    int span = -1;
    double parameter = 0.0;
    double distance = std::numeric_limits<double>::infinity();
};

class Vertices {
public:
    explicit Vertices(QVector<QPointF> positions) : positions_(std::move(positions)) {
        parents_.resize(positions_.size());
        owners_.resize(positions_.size());
        std::iota(parents_.begin(), parents_.end(), 0);
        weights_.fill(1, positions_.size());
    }

    int root(int vertex) {
        if (parents_[vertex] != vertex) parents_[vertex] = root(parents_[vertex]);

        return parents_[vertex];
    }

    int add(const QPointF &position) {
        const int vertex = static_cast<int>(positions_.size());

        parents_.push_back(vertex);
        positions_.push_back(position);
        weights_.push_back(1);
        owners_.push_back({});

        return vertex;
    }

    void addOwner(int vertex, int contour) { owners_[root(vertex)].insert(contour); }
    bool shared(int vertex) { return owners_[root(vertex)].size() > 1; }

    void join(int first, int second, bool allowCollapse = true) {
        first = root(first);
        second = root(second);
        if (first == second) return;
        if (!allowCollapse && owners_[first].intersects(owners_[second])) return;
        positions_[first] = (positions_[first] * weights_[first] + positions_[second] * weights_[second])
            / (weights_[first] + weights_[second]);
        weights_[first] += weights_[second];
        parents_[second] = first;
        owners_[first].unite(owners_[second]);
    }

    QPointF position(int vertex) { return positions_[root(vertex)]; }

private:
    QVector<QPointF> positions_;
    QVector<QSet<int>> owners_;
    QVector<int> parents_;
    QVector<int> weights_;
};

double length(const QPointF &point) {
    return std::hypot(point.x(), point.y());
}

QRectF curveBounds(const PenBoundarySegment &curve) {
    QPolygonF hull;

    hull << curve.start << curve.end;
    if (curve.curved) hull << curve.control << curve.control2;

    return hull.boundingRect();
}

Projection projectToSpan(const QPointF &point, const Span &span) {
    const auto &curve = span.curve;
    Projection result;
    const auto consider = [&](double parameter) {
        const QPointF candidate = curve.point(parameter);
        const double distance = length(point - candidate);
        if (distance < result.distance) {
            result.position = candidate;
            result.parameter = parameter;
            result.distance = distance;
        }
    };

    if (!curve.curved) {
        const QPointF chord = curve.end - curve.start;
        const double squaredLength = QPointF::dotProduct(chord, chord);
        consider(squaredLength > 0.0
            ? std::clamp(QPointF::dotProduct(point - curve.start, chord) / squaredLength, 0.0, 1.0) : 0.0);
    } else {
        consider(0.0);
        consider(1.0);
        for (int seed = 0; seed <= kProjectionSeeds; ++seed) {
            double parameter = static_cast<double>(seed) / kProjectionSeeds;
            for (int iteration = 0; iteration < kProjectionIterations; ++iteration) {
                const QPointF delta = curve.point(parameter) - point;
                const QPointF derivative = curve.derivative(parameter);
                const double denominator = QPointF::dotProduct(derivative, derivative)
                    + QPointF::dotProduct(delta, curve.secondDerivative(parameter));
                if (denominator == 0.0) break;
                const double next = std::clamp(parameter - QPointF::dotProduct(delta, derivative) / denominator, 0.0, 1.0);
                if (next == parameter) break;
                parameter = next;
            }
            consider(parameter);
        }
    }

    return result;
}

QVector<Loop> meshLoops(const MeshData &mesh, QVector<Span> &spans) {
    QVector<Loop> loops;

    for (int contourIndex = 0; contourIndex < mesh.contours.size(); ++contourIndex) {
        const auto &contour = mesh.contours[contourIndex];
        const auto data = mesh.contourData(contourIndex);
        const auto append = [&](const QVector<MeshPoint> &points, QVector<PenPoint> geometry, int cutout, bool closed) {
            Loop loop;
            loop.points = points;
            loop.contour = contourIndex;
            loop.cutout = cutout;
            loop.closed = closed;
            if (data.pathKind == fls::scene::ContourPathKind::Lining && cutout < 0 && !geometry.isEmpty()) {
                geometry.front().kind = geometry.back().kind = PenPointKind::Hard;
            }
            const auto segments = penSegments(geometry, closed);
            for (int index = 0; index < segments.size(); ++index) {
                loop.spans.push_back(static_cast<int>(spans.size()));
                spans.push_back({segments[index], {}, static_cast<int>(loops.size()), index, contourIndex});
            }
            loops.push_back(std::move(loop));
        };

        append(contour.points, data.points, -1, data.pathKind == fls::scene::ContourPathKind::Pen && data.closed);
        for (int index = 0; index < contour.cutouts.size(); ++index) {
            append(contour.cutouts[index], data.cutouts[index], index,
                   data.cutoutClosed || index + 1 < contour.cutouts.size());
        }
    }

    return loops;
}

int vertexOnSpan(Span &span, const Loop &loop, double parameter, Vertices &vertices, double tolerance) {
    const QPointF position = span.curve.point(parameter);
    const int next = (span.point + 1) % loop.points.size();

    if (length(position - span.curve.start) <= tolerance) return loop.points[span.point].vertex;
    if (length(position - span.curve.end) <= tolerance) return loop.points[next].vertex;
    for (const auto &cut : span.cuts) {
        if (length(position - span.curve.point(cut.parameter)) <= tolerance) return cut.vertex;
    }
    const int vertex = vertices.add(position);
    vertices.addOwner(vertex, loop.contour);
    span.cuts.push_back({vertex, parameter});

    return vertex;
}

void intersectSpans(Span &first, Span &second, const QVector<Loop> &loops, Vertices &vertices,
                    const PenBoundarySegment &a, const PenBoundarySegment &b,
                    double aStart, double aEnd, double bStart, double bEnd, double tolerance, int depth) {
    const QRectF aBounds = curveBounds(a).adjusted(-tolerance, -tolerance, tolerance, tolerance);
    const QRectF bBounds = curveBounds(b).adjusted(-tolerance, -tolerance, tolerance, tolerance);

    if (!aBounds.intersects(bBounds)) return;
    if ((a.flatness() <= tolerance && b.flatness() <= tolerance) || depth >= kIntersectionDepth) {
        QPointF intersection;
        const QLineF aLine(a.start, a.end);
        const QLineF bLine(b.start, b.end);
        if (aLine.intersects(bLine, &intersection) != QLineF::BoundedIntersection) return;
        const double aLength = aLine.length();
        const double bLength = bLine.length();
        if (aLength == 0.0 || bLength == 0.0) return;
        const double aParameter = aStart + (aEnd - aStart) * QLineF(a.start, intersection).length() / aLength;
        const double bParameter = bStart + (bEnd - bStart) * QLineF(b.start, intersection).length() / bLength;
        const int firstVertex = vertexOnSpan(first, loops[first.loop], aParameter, vertices, tolerance);
        const int secondVertex = vertexOnSpan(second, loops[second.loop], bParameter, vertices, tolerance);
        vertices.join(firstVertex, secondVertex);

        return;
    }
    if (a.flatness() >= b.flatness()) {
        const auto halves = a.split();
        const double middle = (aStart + aEnd) * 0.5;
        intersectSpans(first, second, loops, vertices, halves.first, b, aStart, middle, bStart, bEnd, tolerance, depth + 1);
        intersectSpans(first, second, loops, vertices, halves.second, b, middle, aEnd, bStart, bEnd, tolerance, depth + 1);
    } else {
        const auto halves = b.split();
        const double middle = (bStart + bEnd) * 0.5;
        intersectSpans(first, second, loops, vertices, a, halves.first, aStart, aEnd, bStart, middle, tolerance, depth + 1);
        intersectSpans(first, second, loops, vertices, a, halves.second, aStart, aEnd, middle, bEnd, tolerance, depth + 1);
    }
}

double crossProduct(const QPointF &first, const QPointF &second) {
    return first.x() * second.y() - first.y() * second.x();
}

bool alignedTangents(const QPointF &first, const QPointF &second) {
    const double product = length(first) * length(second);

    return product > 0.0 && std::abs(QPointF::dotProduct(first, second)) >= kArcTangentCosine * product;
}

double smoothArcSupport(int spanIndex, const QVector<Loop> &loops, const QVector<Span> &spans) {
    const auto &span = spans[spanIndex];
    const auto &loop = loops[span.loop];
    const QPointF direction = span.curve.derivative(0.5);
    QSet<int> visited{spanIndex};
    double support = span.curve.controlLength();

    for (const int step : {-1, 1}) {
        int point = span.point;
        while (true) {
            point += step;
            if (!loop.closed && (point < 0 || point >= loop.spans.size())) break;
            point = (point + loop.spans.size()) % loop.spans.size();
            const int adjacent = loop.spans[point];
            const QPointF adjacentDirection = spans[adjacent].curve.derivative(0.5);
            if (visited.contains(adjacent)
                || QPointF::dotProduct(direction, adjacentDirection) < kArcTangentCosine * length(direction) * length(adjacentDirection)) break;
            visited.insert(adjacent);
            support += spans[adjacent].curve.controlLength();
        }
    }

    return support;
}

PenBoundarySegment withLineControls(PenBoundarySegment curve) {
    if (!curve.curved) {
        curve.control = curve.start + (curve.end - curve.start) / 3.0;
        curve.control2 = curve.start + (curve.end - curve.start) * (2.0 / 3.0);
    }

    return curve;
}

PenBoundarySegment arcSubcurve(const PenBoundarySegment &curve, double start, double end) {
    const bool reversed = start > end;
    const double low = std::min(start, end);
    const double high = std::max(start, end);
    auto result = high < 1.0 ? curve.split(high).first : curve;

    if (low > 0.0) result = result.split(low / high).second;
    result = withLineControls(result);
    if (reversed) {
        std::swap(result.start, result.end);
        std::swap(result.control, result.control2);
    }

    return result;
}

double curvatureRadius(const PenBoundarySegment &curve, double parameter) {
    const QPointF derivative = curve.derivative(parameter);
    const double speed = length(derivative);
    const double turn = std::abs(crossProduct(derivative, curve.secondDerivative(parameter)));

    return turn > 0.0 ? speed * speed * speed / turn : std::numeric_limits<double>::infinity();
}

struct ArcStation {
    double first = 0.0;
    double second = 0.0;
};

struct ArcMatch {
    ArcStation start;
    ArcStation end;
    int first = -1;
    int second = -1;
};

bool validArcDirection(const PenBoundarySegment &source, const PenBoundarySegment &target,
                       double support, double tolerance) {
    Span targetSpan;
    QVector<Projection> projections;
    double previous = -1.0;
    double normalSeparation = 0.0;

    targetSpan.curve = target;
    for (int sample = 0; sample <= kArcValidationSamples; ++sample) {
        const double parameter = static_cast<double>(sample) / kArcValidationSamples;
        const QPointF tangent = source.derivative(parameter);
        const auto projection = projectToSpan(source.point(parameter), targetSpan);
        projections.push_back(projection);
        if (length(tangent) > 0.0) {
            normalSeparation = std::max(normalSeparation,
                std::abs(crossProduct(projection.position - source.point(parameter), tangent)) / length(tangent));
        }
    }
    for (int sample = 0; sample <= kArcValidationSamples; ++sample) {
        const double parameter = static_cast<double>(sample) / kArcValidationSamples;
        const QPointF position = source.point(parameter);
        const auto &projection = projections[sample];
        const QPointF sourceTangent = source.derivative(parameter);
        const QPointF targetTangent = target.derivative(projection.parameter);
        const double parameterTolerance = std::sqrt(std::numeric_limits<double>::epsilon());
        const double spacing = std::min({support, curvatureRadius(source, parameter), curvatureRadius(target, projection.parameter)});

        if (projection.parameter + parameterTolerance < previous || projection.distance >= spacing
            || QPointF::dotProduct(sourceTangent, targetTangent) < kArcTangentCosine * length(sourceTangent) * length(targetTangent)
            || std::abs(QPointF::dotProduct(projection.position - position, sourceTangent))
                > (normalSeparation + tolerance) * length(sourceTangent)) return false;
        previous = projection.parameter;
    }

    return true;
}

bool arcFeatureEndpoint(int spanIndex, double parameter, const QVector<Loop> &loops, const QVector<Span> &spans) {
    const auto &span = spans[spanIndex];
    const auto &loop = loops[span.loop];
    const int adjacent = span.point + (parameter == 0.0 ? -1 : 1);

    if (!loop.closed && (adjacent < 0 || adjacent >= loop.spans.size())) return true;
    const auto &other = spans[loop.spans[(adjacent + loop.spans.size()) % loop.spans.size()]];
    const QPointF tangent = span.curve.derivative(parameter);
    const QPointF otherTangent = other.curve.derivative(1.0 - parameter);

    return QPointF::dotProduct(tangent, otherTangent) < kArcTangentCosine * length(tangent) * length(otherTangent);
}

std::optional<ArcMatch> similarArc(int firstIndex, int secondIndex, const QVector<Loop> &loops, const QVector<Span> &spans,
                                 const QVector<double> &supports, double tolerance) {
    const auto &first = spans[firstIndex];
    const auto &second = spans[secondIndex];
    const double support = std::min(supports[firstIndex], supports[secondIndex]);
    const auto firstBounds = curveBounds(first.curve);
    const auto secondBounds = curveBounds(second.curve);
    const double horizontalGap = std::max({0.0, firstBounds.left() - secondBounds.right(), secondBounds.left() - firstBounds.right()});
    const double verticalGap = std::max({0.0, firstBounds.top() - secondBounds.bottom(), secondBounds.top() - firstBounds.bottom()});
    QVector<ArcStation> stations;
    std::optional<ArcMatch> best;
    int featureAgreement = -1;
    double coverage = 0.0;
    const auto append = [&](double firstParameter, double secondParameter) {
        const QPointF delta = second.curve.point(secondParameter) - first.curve.point(firstParameter);
        const QPointF firstTangent = first.curve.derivative(firstParameter);
        const QPointF secondTangent = second.curve.derivative(secondParameter);
        if (length(delta) < support && alignedTangents(firstTangent, secondTangent)) {
            stations.push_back({firstParameter, secondParameter});
        }
    };
    const auto featuresAgree = [&](const ArcStation &station) {
        return (station.first == 0.0 || station.first == 1.0) && (station.second == 0.0 || station.second == 1.0)
            && arcFeatureEndpoint(firstIndex, station.first, loops, spans)
            && arcFeatureEndpoint(secondIndex, station.second, loops, spans);
    };

    if (std::hypot(horizontalGap, verticalGap) >= support) return std::nullopt;
    for (const double endpoint : {0.0, 1.0}) {
        append(endpoint, projectToSpan(first.curve.point(endpoint), second).parameter);
        append(projectToSpan(second.curve.point(endpoint), first).parameter, endpoint);
        if (arcFeatureEndpoint(firstIndex, endpoint, loops, spans)) {
            for (const double other : {0.0, 1.0}) {
                if (arcFeatureEndpoint(secondIndex, other, loops, spans)) append(endpoint, other);
            }
        }
    }
    std::sort(stations.begin(), stations.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (int start = 0; start < stations.size(); ++start) {
        for (int end = start + 1; end < stations.size(); ++end) {
            const auto &a = stations[start];
            const auto &b = stations[end];
            const auto firstArc = arcSubcurve(first.curve, a.first, b.first);
            const auto secondArc = arcSubcurve(second.curve, a.second, b.second);
            const int candidateAgreement = static_cast<int>(featuresAgree(a)) + static_cast<int>(featuresAgree(b));
            const double candidateCoverage = std::min(firstArc.controlLength(), secondArc.controlLength());
            if (candidateCoverage <= tolerance || candidateCoverage + tolerance < coverage
                || (std::abs(candidateCoverage - coverage) <= tolerance && candidateAgreement <= featureAgreement)) continue;
            if (!validArcDirection(firstArc, secondArc, support, tolerance)
                || !validArcDirection(secondArc, firstArc, support, tolerance)) continue;
            best = ArcMatch{a, b, firstIndex, secondIndex};
            featureAgreement = candidateAgreement;
            coverage = candidateCoverage;
        }
    }

    return best;
}

struct RegionContext {
    QVector<QPainterPath> material;
    QVector<QPolygonF> polygons;
    QTransform normalization;
    double extent = 0.0;
    double flatness = 0.0;
    double tolerance = 0.0;
    double areaTolerance = 0.0;
};

enum class RegionOperation { Grow, ResolveOverlap };

struct BoundaryRegion {
    ArcMatch match;
    QPainterPath area;
    RegionOperation operation = RegionOperation::Grow;
    double score = 0.0;
    double measure = 0.0;
};

struct BoundaryEdge {
    PenBoundarySegment curve;
    MeshPoint start;
    MeshPoint end;
};

void flattenCurve(const PenBoundarySegment &curve, QPolygonF &points, double flatness, int depth = 0) {
    if (curve.flatness() <= flatness || depth >= kRegionSubdivisionDepth) {
        points.push_back(curve.end);
        return;
    }
    const auto halves = curve.split();
    flattenCurve(halves.first, points, flatness, depth + 1);
    flattenCurve(halves.second, points, flatness, depth + 1);
}

QPolygonF loopPolygon(const Loop &loop, const QVector<Span> &spans, double flatness) {
    QPolygonF polygon;

    if (loop.spans.isEmpty()) return polygon;
    polygon.push_back(spans[loop.spans.front()].curve.start);
    for (const int span : loop.spans) flattenCurve(spans[span].curve, polygon, flatness);
    if (loop.closed && polygon.front() == polygon.back()) polygon.removeLast();

    return polygon;
}

QPainterPath polygonPath(const QPolygonF &polygon) {
    QPainterPath path;

    path.addPolygon(polygon);
    path.closeSubpath();
    path.setFillRule(Qt::OddEvenFill);

    return path;
}

bool pointOnLine(const QPointF &point, const QLineF &line, double tolerance) {
    const QPointF direction = line.p2() - line.p1();
    const QPointF relative = point - line.p1();
    const double squaredLength = QPointF::dotProduct(direction, direction);
    const double projection = QPointF::dotProduct(relative, direction);

    return std::abs(crossProduct(direction, relative)) <= tolerance * line.length()
        && projection >= -tolerance * line.length() && projection <= squaredLength + tolerance * line.length();
}

bool simpleBoundary(const QPolygonF &polygon, bool closed, double tolerance) {
    struct Edge {
        QLineF line;
        QRectF bounds;
        int index = -1;
    };
    QVector<Edge> edges;
    const int count = static_cast<int>(polygon.size()) - (closed ? 0 : 1);

    if (count < (closed ? 3 : 1)) return false;
    for (int index = 0; index < count; ++index) {
        const QLineF line(polygon[index], polygon[(index + 1) % polygon.size()]);
        if (line.length() <= tolerance) return false;
        edges.push_back({line, QRectF(line.p1(), line.p2()).normalized().adjusted(-tolerance, -tolerance, tolerance, tolerance), index});
    }
    std::sort(edges.begin(), edges.end(), [](const Edge &first, const Edge &second) {
        return first.bounds.left() < second.bounds.left();
    });
    for (int first = 0; first < edges.size(); ++first) {
        for (int second = first + 1; second < edges.size(); ++second) {
            const auto &a = edges[first];
            const auto &b = edges[second];
            if (b.bounds.left() > a.bounds.right()) break;
            if (!a.bounds.intersects(b.bounds)) continue;
            const bool adjacent = std::abs(a.index - b.index) == 1 || (closed && std::abs(a.index - b.index) == count - 1);
            QPointF intersection;
            if (a.line.intersects(b.line, &intersection) == QLineF::BoundedIntersection) {
                const bool sharedEndpoint = (length(intersection - a.line.p1()) <= tolerance || length(intersection - a.line.p2()) <= tolerance)
                    && (length(intersection - b.line.p1()) <= tolerance || length(intersection - b.line.p2()) <= tolerance);
                if (!adjacent || !sharedEndpoint) return false;
            }
            if (std::abs(crossProduct(a.line.p2() - a.line.p1(), b.line.p2() - b.line.p1())) <= tolerance * (a.line.length() + b.line.length())) {
                if (!adjacent && (pointOnLine(a.line.p1(), b.line, tolerance) || pointOnLine(a.line.p2(), b.line, tolerance)
                    || pointOnLine(b.line.p1(), a.line, tolerance) || pointOnLine(b.line.p2(), a.line, tolerance))) return false;
                const QPointF aMiddle = (a.line.p1() + a.line.p2()) * 0.5;
                const QPointF bMiddle = (b.line.p1() + b.line.p2()) * 0.5;
                if (pointOnLine(aMiddle, b.line, tolerance) || pointOnLine(bMiddle, a.line, tolerance)) return false;
            }
        }
    }

    return true;
}

RegionContext regionContext(const MeshData &mesh, const QVector<Loop> &loops, const QVector<Span> &spans) {
    RegionContext context;
    QRectF bounds;

    for (const auto &span : spans) bounds = bounds.united(curveBounds(span.curve));
    context.extent = std::max(bounds.width(), bounds.height());
    if (context.extent <= 0.0) return context;
    context.normalization = QTransform(1.0 / context.extent, 0.0, 0.0, 1.0 / context.extent,
                                       -bounds.left() / context.extent, -bounds.top() / context.extent);
    context.flatness = context.extent * std::cbrt(std::numeric_limits<double>::epsilon());
    context.tolerance = std::sqrt(std::numeric_limits<double>::epsilon());
    context.areaTolerance = kRegionAreaRoundoff * context.flatness / context.extent;
    context.material.resize(mesh.contours.size());
    for (const auto &loop : loops) {
        const auto polygon = context.normalization.map(loopPolygon(loop, spans, context.flatness));
        context.polygons.push_back(polygon);
        const auto &style = mesh.contours[loop.contour].style;
        if (loop.closed && style.pathKind == fls::scene::ContourPathKind::Pen && style.closed) {
            context.material[loop.contour].addPath(polygonPath(polygon));
            context.material[loop.contour].setFillRule(Qt::OddEvenFill);
        }
    }

    return context;
}

double inwardClearance(const QPointF &position, const QPointF &outward, int contour, const QVector<Loop> &loops,
                        const RegionContext &context) {
    const QPointF direction = -outward / length(outward);
    const auto &material = context.material[contour];
    double clearance = std::numeric_limits<double>::infinity();

    for (int loop = 0; loop < loops.size(); ++loop) {
        if (loops[loop].contour != contour) continue;
        const auto &polygon = context.polygons[loop];
        const int count = static_cast<int>(polygon.size()) - (loops[loop].closed ? 0 : 1);
        for (int point = 0; point < count; ++point) {
            const QPointF start = polygon[point];
            const QPointF edge = polygon[(point + 1) % polygon.size()] - start;
            const double denominator = crossProduct(direction, edge);
            if (denominator == 0.0) continue;
            const double distance = crossProduct(start - position, edge) / denominator;
            const double parameter = crossProduct(start - position, direction) / denominator;
            const double boundaryTolerance = std::max(context.tolerance,
                kBoundaryApproximationCount * context.flatness / context.extent);
            if (distance > boundaryTolerance && parameter >= 0.0 && parameter <= 1.0) clearance = std::min(clearance, distance);
        }
    }
    if (material.isEmpty()) return clearance;
    if (!std::isfinite(clearance) || !material.contains(position + direction * (clearance * 0.5))) return 0.0;

    return clearance;
}

QPolygonF gapPolygon(const ArcMatch &match, const QVector<Span> &spans, double flatness) {
    const auto first = arcSubcurve(spans[match.first].curve, match.start.first, match.end.first);
    const auto second = arcSubcurve(spans[match.second].curve, match.end.second, match.start.second);
    QPolygonF polygon{first.start};

    flattenCurve(first, polygon, flatness);
    if (polygon.back() != second.start) polygon.push_back(second.start);
    flattenCurve(second, polygon, flatness);
    if (polygon.front() == polygon.back()) polygon.removeLast();

    return polygon;
}

QJsonObject regionDetail(const ArcMatch &match, const QVector<Span> &spans) {
    return {{QStringLiteral("firstContour"), spans[match.first].contour},
            {QStringLiteral("secondContour"), spans[match.second].contour},
            {QStringLiteral("firstSpan"), match.first}, {QStringLiteral("secondSpan"), match.second},
            {QStringLiteral("firstInterval"), QJsonArray{match.start.first, match.end.first}},
            {QStringLiteral("secondInterval"), QJsonArray{match.start.second, match.end.second}}};
}

QPolygonF cleanRegionPolygon(const QPolygonF &polygon, double tolerance) {
    QPolygonF result;

    for (const auto &point : polygon) {
        if (result.isEmpty() || length(point - result.back()) > tolerance) result.push_back(point);
    }
    if (result.size() > 1 && length(result.front() - result.back()) <= tolerance) result.removeLast();

    return result;
}

double regionMeasure(const ArcMatch &match, const QVector<Span> &spans, const QTransform &normalization) {
    const auto normalized = [&](PenBoundarySegment curve) {
        curve.start = normalization.map(curve.start);
        curve.control = normalization.map(curve.control);
        curve.control2 = normalization.map(curve.control2);
        curve.end = normalization.map(curve.end);

        return curve;
    };
    const auto first = normalized(arcSubcurve(spans[match.first].curve, match.start.first, match.end.first));
    const auto second = normalized(arcSubcurve(spans[match.second].curve, match.end.second, match.start.second));

    return std::abs(first.signedArea() + crossProduct(first.end, second.start) * 0.5
        + second.signedArea() + crossProduct(second.end, first.start) * 0.5);
}

std::optional<BoundaryRegion> validBoundaryRegion(const ArcMatch &match, const QVector<Span> &spans,
                                                  const QVector<Loop> &loops, const RegionContext &context,
                                                  MeshDiagnostics &diagnostics) {
    const auto &first = spans[match.first];
    const auto &second = spans[match.second];
    const auto polygon = cleanRegionPolygon(context.normalization.map(gapPolygon(match, spans, context.flatness)), context.tolerance);
    const auto area = polygonPath(polygon);
    auto detail = regionDetail(match, spans);
    Span target;
    std::optional<RegionOperation> operation;
    double score = 0.0;
    const auto reject = [&](const QString &reason) -> std::optional<BoundaryRegion> {
        diagnostics.decision(reason, detail);

        return std::nullopt;
    };

    detail.insert(QStringLiteral("area"), fill_geometry::pathArea(area) * context.extent * context.extent);
    if (!simpleBoundary(polygon, true, context.tolerance)) return reject(QStringLiteral("invalid_region_boundary"));
    target.curve = arcSubcurve(second.curve, match.start.second, match.end.second);
    for (int sample = 1; sample < kRegionValidationSamples; ++sample) {
        const double fraction = static_cast<double>(sample) / kRegionValidationSamples;
        const double firstParameter = match.start.first + (match.end.first - match.start.first) * fraction;
        const auto projection = projectToSpan(first.curve.point(firstParameter), target);
        const QPointF p = context.normalization.map(first.curve.point(firstParameter));
        const QPointF q = context.normalization.map(projection.position);
        const QPointF middle = (p + q) * 0.5;
        const double separation = length(q - p);
        if (separation <= context.tolerance) continue;
        const bool insideFirst = context.material[first.contour].contains(middle);
        const bool insideSecond = context.material[second.contour].contains(middle);
        detail.insert(QStringLiteral("sampleParameter"), firstParameter);
        if (insideFirst != insideSecond) return reject(QStringLiteral("one_sided_material"));
        const auto label = insideFirst ? RegionOperation::ResolveOverlap : RegionOperation::Grow;
        if (operation && *operation != label) return reject(QStringLiteral("mixed_region_labels"));
        operation = label;
        const double direction = label == RegionOperation::ResolveOverlap ? -1.0 : 1.0;
        const double clearance = std::min(inwardClearance(p, (q - p) * direction, first.contour, loops, context),
                                          inwardClearance(q, (p - q) * direction, second.contour, loops, context));
        if (separation >= clearance) {
            detail.insert(QStringLiteral("separation"), separation * context.extent);
            detail.insert(QStringLiteral("clearance"), clearance * context.extent);
            return reject(QStringLiteral("insufficient_clearance"));
        }
        score = std::max(score, std::isfinite(clearance) ? separation / clearance : separation);
    }
    if (!operation) return reject(QStringLiteral("coincident_region"));
    detail.insert(QStringLiteral("operation"), *operation == RegionOperation::Grow ? QStringLiteral("gap") : QStringLiteral("overlap"));
    for (int contour = 0; contour < context.material.size(); ++contour) {
        const auto &material = context.material[contour];
        if (material.isEmpty()) continue;
        const double covered = fill_geometry::pathArea(area.intersected(material));
        const bool participant = contour == first.contour || contour == second.contour;
        if ((participant && *operation == RegionOperation::ResolveOverlap
                && fill_geometry::pathArea(area.subtracted(material)) > context.areaTolerance)
            || ((!participant || *operation == RegionOperation::Grow) && covered > context.areaTolerance)) {
            detail.insert(QStringLiteral("blockingContour"), contour);
            detail.insert(QStringLiteral("coveredArea"), covered * context.extent * context.extent);
            return reject(QStringLiteral("incompatible_material_coverage"));
        }
    }
    for (int loop = 0; loop < loops.size(); ++loop) {
        const auto &boundary = context.polygons[loop];
        for (int point = 0; point + 1 < boundary.size() + (loops[loop].closed ? 1 : 0); ++point) {
            const QPointF middle = (boundary[point] + boundary[(point + 1) % boundary.size()]) * 0.5;
            if (!area.contains(middle)) continue;
            bool onBoundary = false;
            for (int edge = 0; edge < polygon.size(); ++edge) {
                onBoundary |= pointOnLine(middle, QLineF(polygon[edge], polygon[(edge + 1) % polygon.size()]),
                                          kBoundaryApproximationCount * context.flatness / context.extent);
            }
            if (!onBoundary) {
                detail.insert(QStringLiteral("blockingLoop"), loop);
                return reject(QStringLiteral("intervening_boundary"));
            }
        }
    }
    detail.insert(QStringLiteral("score"), score);
    diagnostics.decision(*operation == RegionOperation::Grow ? QStringLiteral("candidate_gap") : QStringLiteral("candidate_overlap"), detail);

    return BoundaryRegion{match, area, *operation, score, regionMeasure(match, spans, context.normalization)};
}

QVector<ArcMatch> regionIntervals(const ArcMatch &match, const QVector<Span> &spans, double tolerance) {
    const auto &first = spans[match.first];
    const auto &second = spans[match.second];
    QVector<ArcStation> stations{match.start, match.end};
    QVector<ArcMatch> intervals;
    const double low = std::min(match.start.second, match.end.second);
    const double high = std::max(match.start.second, match.end.second);
    const double direction = match.start.second < match.end.second ? 1.0 : -1.0;
    const auto append = [&](double firstParameter, double secondParameter, double distance) {
        if (distance <= tolerance && firstParameter > match.start.first && firstParameter < match.end.first
            && secondParameter >= low && secondParameter <= high) stations.push_back({firstParameter, secondParameter});
    };

    for (const auto &cut : first.cuts) {
        const auto projection = projectToSpan(first.curve.point(cut.parameter), second);
        append(cut.parameter, projection.parameter, projection.distance);
    }
    for (const auto &cut : second.cuts) {
        const auto projection = projectToSpan(second.curve.point(cut.parameter), first);
        append(projection.parameter, cut.parameter, projection.distance);
    }
    std::sort(stations.begin(), stations.end(), [](const ArcStation &a, const ArcStation &b) { return a.first < b.first; });
    ArcStation previous = stations.front();
    for (int index = 1; index < stations.size(); ++index) {
        const auto &next = stations[index];
        if (length(first.curve.point(previous.first) - first.curve.point(next.first)) <= tolerance) continue;
        if ((next.second - previous.second) * direction <= 0.0) continue;
        intervals.push_back({previous, next, match.first, match.second});
        previous = next;
    }

    return intervals;
}

bool overlapInterval(double firstStart, double firstEnd, double secondStart, double secondEnd) {
    const double low = std::max(std::min(firstStart, firstEnd), std::min(secondStart, secondEnd));
    const double high = std::min(std::max(firstStart, firstEnd), std::max(secondStart, secondEnd));

    return high - low > std::sqrt(std::numeric_limits<double>::epsilon());
}

bool compatibleRegions(const BoundaryRegion &first, const BoundaryRegion &second, double areaTolerance) {
    const auto &a = first.match;
    const auto &b = second.match;
    const auto overlaps = [&](int firstSpan, double firstStart, double firstEnd, int secondSpan, double secondStart, double secondEnd) {
        return firstSpan == secondSpan && overlapInterval(firstStart, firstEnd, secondStart, secondEnd);
    };

    if (overlaps(a.second, a.start.second, a.end.second, b.second, b.start.second, b.end.second)
        || overlaps(a.first, a.start.first, a.end.first, b.second, b.start.second, b.end.second)
        || overlaps(a.second, a.start.second, a.end.second, b.first, b.start.first, b.end.first)) return false;

    return fill_geometry::pathArea(first.area.intersected(second.area)) <= areaTolerance;
}

QVector<BoundaryRegion> boundaryRegions(const QVector<Span> &spans, const QVector<Loop> &loops, const RegionContext &context,
                                       double tolerance, MeshDiagnostics &diagnostics) {
    QVector<BoundaryRegion> candidates;
    QVector<BoundaryRegion> selected;
    QVector<double> supports;
    int proposals = 0;
    int intervals = 0;

    for (int index = 0; index < spans.size(); ++index) supports.push_back(smoothArcSupport(index, loops, spans));
    for (int first = 0; first < spans.size(); ++first) {
        for (int second = first + 1; second < spans.size(); ++second) {
            if (spans[first].contour == spans[second].contour) continue;
            const auto match = similarArc(first, second, loops, spans, supports, tolerance);
            if (!match) continue;
            ++proposals;
            for (const auto &interval : regionIntervals(*match, spans, tolerance)) {
                ++intervals;
                if (auto region = validBoundaryRegion(interval, spans, loops, context, diagnostics)) candidates.push_back(std::move(*region));
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const BoundaryRegion &first, const BoundaryRegion &second) {
        return first.score < second.score;
    });
    for (const auto &candidate : candidates) {
        if (std::all_of(selected.cbegin(), selected.cend(), [&](const BoundaryRegion &region) {
            return compatibleRegions(candidate, region, context.areaTolerance);
        })) {
            selected.push_back(candidate);
            diagnostics.decision(QStringLiteral("selected_region"), regionDetail(candidate.match, spans));
        } else {
            diagnostics.decision(QStringLiteral("conflicting_region"), regionDetail(candidate.match, spans));
        }
    }
    diagnostics.set(QStringLiteral("arcProposals"), proposals);
    diagnostics.set(QStringLiteral("regionIntervals"), intervals);
    diagnostics.set(QStringLiteral("selectedRegions"), selected.size());

    return selected;
}

void overlayCoincidentPoints(QVector<Span> &spans, const QVector<Loop> &loops, Vertices &vertices, double tolerance) {
    for (int first = 0; first < spans.size(); ++first) {
        for (int second = first + 1; second < spans.size(); ++second) {
            auto &a = spans[first];
            auto &b = spans[second];
            if (a.contour == b.contour) continue;
            const auto overlay = [&](Span &source, Span &target) {
                auto knots = source.cuts;
                knots.push_back({loops[source.loop].points[source.point].vertex, 0.0});
                knots.push_back({loops[source.loop].points[(source.point + 1) % loops[source.loop].points.size()].vertex, 1.0});
                for (const auto &knot : knots) {
                    const auto projection = projectToSpan(source.curve.point(knot.parameter), target);
                    if (projection.distance > tolerance) continue;
                    const int vertex = vertexOnSpan(target, loops[target.loop], projection.parameter, vertices, tolerance);
                    vertices.join(knot.vertex, vertex);
                }
            };
            overlay(a, b);
            overlay(b, a);
        }
    }
}

QVector<BoundaryRegion> prepareRegions(const QVector<BoundaryRegion> &regions, QVector<Span> &spans, const QVector<Loop> &loops,
                                      Vertices &vertices, double tolerance, MeshDiagnostics &diagnostics) {
    QVector<BoundaryRegion> prepared;

    for (const auto &region : regions) {
        const auto &match = region.match;
        auto &first = spans[match.first];
        auto &second = spans[match.second];
        const double low = std::min(match.start.second, match.end.second);
        const double high = std::max(match.start.second, match.end.second);
        auto knots = second.cuts;
        knots.push_back({loops[second.loop].points[second.point].vertex, 0.0});
        knots.push_back({loops[second.loop].points[(second.point + 1) % loops[second.loop].points.size()].vertex, 1.0});
        bool valid = true;

        for (const auto &knot : knots) {
            if (knot.parameter <= low || knot.parameter >= high || !vertices.shared(knot.vertex)) continue;
            const auto projection = projectToSpan(second.curve.point(knot.parameter), first);
            valid &= projection.distance <= tolerance;
        }
        if (!valid) {
            diagnostics.decision(QStringLiteral("protected_junction"), regionDetail(match, spans));
            continue;
        }
        vertexOnSpan(first, loops[first.loop], match.start.first, vertices, tolerance);
        vertexOnSpan(first, loops[first.loop], match.end.first, vertices, tolerance);
        vertexOnSpan(second, loops[second.loop], low, vertices, tolerance);
        vertexOnSpan(second, loops[second.loop], high, vertices, tolerance);
        for (const auto &knot : knots) {
            if (knot.parameter < low || knot.parameter > high) continue;
            const auto projection = projectToSpan(second.curve.point(knot.parameter), first);
            if (projection.parameter < match.start.first || projection.parameter > match.end.first) continue;
            const int vertex = vertexOnSpan(first, loops[first.loop], projection.parameter, vertices, tolerance);
            if (projection.distance <= tolerance) vertices.join(knot.vertex, vertex);
        }
        prepared.push_back(region);
    }
    diagnostics.set(QStringLiteral("preparedRegions"), prepared.size());

    return prepared;
}

MeshPoint boundaryPoint(const Span &span, const QVector<Loop> &loops, double parameter, int vertex) {
    const auto &loop = loops[span.loop];
    const auto &start = loop.points[span.point];
    const auto &end = loop.points[(span.point + 1) % loop.points.size()];
    MeshPoint point = parameter == 0.0 ? start : parameter == 1.0 ? end : MeshPoint{};

    point.vertex = vertex;
    point.strokeWidth = start.strokeWidth * (1.0 - parameter) + end.strokeWidth * parameter;
    point.explicitHandles = true;

    return point;
}

QVector<BoundaryEdge> boundaryEdges(Span &span, const QVector<Loop> &loops, Vertices &vertices,
                                   double start, double end, double tolerance) {
    QVector<BoundaryEdge> edges;
    QVector<Cut> knots;
    const double low = std::min(start, end);
    const double high = std::max(start, end);
    const int first = vertexOnSpan(span, loops[span.loop], low, vertices, tolerance);
    const int last = vertexOnSpan(span, loops[span.loop], high, vertices, tolerance);

    knots.push_back({first, low});
    for (const auto &cut : span.cuts) {
        if (cut.parameter > low && cut.parameter < high) knots.push_back(cut);
    }
    knots.push_back({last, high});
    std::sort(knots.begin(), knots.end(), [](const Cut &first, const Cut &second) { return first.parameter < second.parameter; });
    if (start > end) std::reverse(knots.begin(), knots.end());
    for (int index = 1; index < knots.size(); ++index) {
        const auto &a = knots[index - 1];
        const auto &b = knots[index];
        if (vertices.root(a.vertex) == vertices.root(b.vertex)) continue;
        edges.push_back({arcSubcurve(span.curve, a.parameter, b.parameter), boundaryPoint(span, loops, a.parameter, a.vertex),
                         boundaryPoint(span, loops, b.parameter, b.vertex)});
    }

    return edges;
}

void appendConnector(QVector<BoundaryEdge> &edges, MeshPoint start, MeshPoint end, Vertices &vertices, double tolerance) {
    const QPointF first = vertices.position(start.vertex);
    const QPointF second = vertices.position(end.vertex);

    if (length(first - second) <= tolerance) {
        vertices.join(start.vertex, end.vertex);
        return;
    }
    edges.push_back({withLineControls({first, {}, second, false, {}}), start, end});
}

bool reverseEdges(const BoundaryEdge &first, const BoundaryEdge &second, Vertices &vertices, double tolerance) {
    return vertices.root(first.start.vertex) == vertices.root(second.end.vertex)
        && vertices.root(first.end.vertex) == vertices.root(second.start.vertex)
        && length(first.curve.control - second.curve.control2) <= tolerance
        && length(first.curve.control2 - second.curve.control) <= tolerance;
}

std::optional<QVector<MeshPoint>> extractBoundary(const QVector<BoundaryEdge> &edges, const Loop &loop,
                                                 Vertices &vertices, double tolerance, MeshDiagnostics &diagnostics) {
    QVector<int> boundary;
    QHash<int, int> outgoing;
    QVector<MeshPoint> points;
    QSet<int> visited;
    QPointF incoming;
    bool previousCurved = false;
    const auto reject = [&](const QString &reason, int vertex = -1) -> std::optional<QVector<MeshPoint>> {
        QJsonArray remaining;
        for (const int index : boundary) {
            if (remaining.size() >= kMaximumBoundaryDiagnostics) break;
            const auto &edge = edges[index];
            remaining.append(QJsonObject{{QStringLiteral("start"), vertices.root(edge.start.vertex)},
                                         {QStringLiteral("end"), vertices.root(edge.end.vertex)},
                                         {QStringLiteral("startPosition"), QJsonArray{edge.curve.start.x(), edge.curve.start.y()}},
                                         {QStringLiteral("endPosition"), QJsonArray{edge.curve.end.x(), edge.curve.end.y()}}});
        }
        diagnostics.decision(reason, {{QStringLiteral("contour"), loop.contour}, {QStringLiteral("cutout"), loop.cutout},
                                     {QStringLiteral("vertex"), vertex}, {QStringLiteral("boundaryEdges"), boundary.size()},
                                     {QStringLiteral("visitedEdges"), visited.size()}, {QStringLiteral("boundary"), remaining}});

        return std::nullopt;
    };

    for (int index = 0; index < edges.size(); ++index) {
        if (vertices.root(edges[index].start.vertex) == vertices.root(edges[index].end.vertex)) continue;
        auto reverse = std::find_if(boundary.begin(), boundary.end(), [&](int other) {
            return reverseEdges(edges[index], edges[other], vertices, tolerance);
        });
        if (reverse != boundary.end()) boundary.erase(reverse);
        else boundary.push_back(index);
    }
    for (const int edge : boundary) {
        const int start = vertices.root(edges[edge].start.vertex);
        if (outgoing.contains(start)) return reject(QStringLiteral("branched_boundary"), start);
        outgoing.insert(start, edge);
    }
    if (boundary.isEmpty()) return reject(QStringLiteral("empty_boundary"));
    int vertex = vertices.root(loop.points.front().vertex);
    if (!outgoing.contains(vertex)) {
        if (!loop.closed) return reject(QStringLiteral("open_boundary_start_removed"), vertex);
        vertex = vertices.root(edges[boundary.front()].start.vertex);
    }
    const int start = vertex;
    while (outgoing.contains(vertex)) {
        const int index = outgoing.value(vertex);
        const auto &edge = edges[index];
        if (visited.contains(index)) break;
        visited.insert(index);
        auto point = edge.start;
        point.vertex = vertex;
        point.outgoing = edge.curve.control - vertices.position(vertex);
        point.explicitHandles = true;
        if (!points.isEmpty()) point.incoming = incoming;
        if (previousCurved || edge.curve.curved) point.kind = PenPointKind::Soft;
        points.push_back(point);
        vertex = vertices.root(edge.end.vertex);
        incoming = edge.curve.control2 - vertices.position(vertex);
        if (loop.closed && vertex == start) {
            points.front().incoming = incoming;
            if (edge.curve.curved) points.front().kind = PenPointKind::Soft;
            break;
        }
        auto next = edge.end;
        next.vertex = vertex;
        next.incoming = incoming;
        if (edge.curve.curved) next.kind = PenPointKind::Soft;
        if (!outgoing.contains(vertex)) {
            points.push_back(next);
            break;
        }
        previousCurved = edge.curve.curved;
    }
    if (visited.size() != boundary.size() || (loop.closed && vertex != start)
        || (!loop.closed && vertex != vertices.root(loop.points.back().vertex))) {
        return reject(QStringLiteral("incomplete_boundary"), vertex);
    }

    return points;
}

bool reconstructRegions(MeshData &mesh, QVector<Span> &spans, const QVector<Loop> &loops,
                         const QVector<BoundaryRegion> &regions, Vertices &vertices, double tolerance, MeshDiagnostics &diagnostics) {
    QVector<QVector<BoundaryEdge>> faces(loops.size());

    for (auto &span : spans) faces[span.loop] += boundaryEdges(span, loops, vertices, 0.0, 1.0, tolerance);
    for (const auto &region : regions) {
        const auto &match = region.match;
        auto &source = spans[match.first];
        auto &recipient = spans[match.second];
        const bool forward = match.start.second < match.end.second;
        const double low = std::min(match.start.second, match.end.second);
        const double high = std::max(match.start.second, match.end.second);
        auto seam = boundaryEdges(source, loops, vertices, forward ? match.start.first : match.end.first,
                                 forward ? match.end.first : match.start.first, tolerance);
        auto previous = boundaryEdges(recipient, loops, vertices, high, low, tolerance);
        if (seam.isEmpty() || previous.isEmpty()) continue;
        for (auto &edge : seam) {
            const auto attributes = [&](const QPointF &position, int vertex) {
                const double sourceParameter = projectToSpan(position, source).parameter;
                const double fraction = (sourceParameter - match.start.first) / (match.end.first - match.start.first);
                const double recipientParameter = match.start.second + (match.end.second - match.start.second) * fraction;

                return boundaryPoint(recipient, loops, recipientParameter, vertex);
            };
            edge.start = attributes(edge.curve.start, edge.start.vertex);
            edge.end = attributes(edge.curve.end, edge.end.vertex);
        }
        auto &face = faces[recipient.loop];
        appendConnector(face, previous.back().end, seam.front().start, vertices, tolerance);
        face += seam;
        appendConnector(face, seam.back().end, previous.front().start, vertices, tolerance);
        face += previous;
    }
    for (int index = 0; index < loops.size(); ++index) {
        const auto &loop = loops[index];
        const auto points = extractBoundary(faces[index], loop, vertices, tolerance, diagnostics);
        if (!points) return false;
        auto &contour = mesh.contours[loop.contour];
        if (loop.cutout < 0) contour.points = *points;
        else contour.cutouts[loop.cutout] = *points;
    }

    return true;
}

double loopArea(const Loop &loop, const QVector<Span> &spans, const QTransform &normalization) {
    double area = 0.0;

    for (const int span : loop.spans) {
        auto curve = spans[span].curve;
        curve.start = normalization.map(curve.start);
        curve.control = normalization.map(curve.control);
        curve.control2 = normalization.map(curve.control2);
        curve.end = normalization.map(curve.end);
        area += curve.signedArea();
    }

    return area;
}

bool validReconstruction(const MeshData &mesh, const QVector<Loop> &originalLoops, const QVector<Span> &originalSpans,
                         const RegionContext &context, const QVector<BoundaryRegion> &regions, MeshDiagnostics &diagnostics) {
    QVector<Span> spans;
    const auto loops = meshLoops(mesh, spans);
    QVector<QPolygonF> polygons;
    QVector<QPainterPath> material(mesh.contours.size());
    QVector<QPainterPath> assignedGaps(mesh.contours.size());
    QVector<QPainterPath> assignedOverlap(mesh.contours.size());
    QVector<double> originalArea(mesh.contours.size(), 0.0);
    QVector<double> newArea(mesh.contours.size(), 0.0);
    QVector<double> allowedLoss(mesh.contours.size(), 0.0);
    QPainterPath originalUnion;
    QPainterPath rebuiltUnion;
    const auto reject = [&](const QString &reason, QJsonObject detail = {}) {
        diagnostics.decision(reason, detail);

        return false;
    };

    for (const auto &region : regions) {
        const int contour = originalSpans[region.match.second].contour;
        if (region.operation == RegionOperation::ResolveOverlap) {
            assignedOverlap[contour] = assignedOverlap[contour].united(region.area);
            allowedLoss[contour] += region.measure;
        } else {
            assignedGaps[contour] = assignedGaps[contour].united(region.area);
        }
    }
    if (loops.size() != originalLoops.size()) return reject(QStringLiteral("loop_count_changed"));
    for (int index = 0; index < loops.size(); ++index) {
        const auto &loop = loops[index];
        const auto polygon = context.normalization.map(loopPolygon(loop, spans, context.flatness));
        if (!simpleBoundary(polygon, loop.closed, context.tolerance)) {
            return reject(QStringLiteral("non_simple_reconstructed_loop"), {{QStringLiteral("loop"), index}});
        }
        polygons.push_back(polygon);
        if (!loop.closed) continue;
        const double before = loopArea(originalLoops[index], originalSpans, context.normalization);
        const double after = loopArea(loop, spans, context.normalization);
        if (before * after <= 0.0 || std::abs(after) <= context.tolerance * context.tolerance) {
            return reject(QStringLiteral("winding_or_area_invalid"), {{QStringLiteral("loop"), index},
                          {QStringLiteral("beforeArea"), before * context.extent * context.extent},
                          {QStringLiteral("afterArea"), after * context.extent * context.extent}});
        }
        const double sign = loop.cutout < 0 ? 1.0 : -1.0;
        originalArea[loop.contour] += std::abs(before) * sign;
        newArea[loop.contour] += std::abs(after) * sign;
        const auto &style = mesh.contours[loop.contour].style;
        if (style.pathKind == fls::scene::ContourPathKind::Pen && style.closed) {
            material[loop.contour].addPath(polygonPath(polygon));
            material[loop.contour].setFillRule(Qt::OddEvenFill);
        }
    }
    for (int contour = 0; contour < mesh.contours.size(); ++contour) {
        if (context.material[contour].isEmpty()) continue;
        const auto expected = context.material[contour].united(assignedGaps[contour]).subtracted(assignedOverlap[contour]);
        const double missing = fill_geometry::pathArea(expected.subtracted(material[contour]));
        const double excess = fill_geometry::pathArea(material[contour].subtracted(expected));
        const double areaScale = context.extent * context.extent;
        const QJsonObject detail{{QStringLiteral("contour"), contour}, {QStringLiteral("beforeArea"), originalArea[contour] * areaScale},
                                 {QStringLiteral("afterArea"), newArea[contour] * areaScale}, {QStringLiteral("allowedOverlapLoss"), allowedLoss[contour] * areaScale},
                                 {QStringLiteral("missingArea"), missing * areaScale}, {QStringLiteral("excessArea"), excess * areaScale}};
        if (newArea[contour] + context.tolerance * std::abs(originalArea[contour]) < originalArea[contour] - allowedLoss[contour]
            || missing > context.areaTolerance || excess > context.areaTolerance) {
            return reject(QStringLiteral("material_assignment_mismatch"), detail);
        }
        originalUnion = originalUnion.united(context.material[contour]);
        rebuiltUnion = rebuiltUnion.united(material[contour]);
        diagnostics.decision(QStringLiteral("validated_material_assignment"), detail);
    }
    const double lostUnion = fill_geometry::pathArea(originalUnion.subtracted(rebuiltUnion));
    diagnostics.set(QStringLiteral("lostUnionArea"), lostUnion * context.extent * context.extent);
    if (lostUnion > context.areaTolerance) return reject(QStringLiteral("combined_material_lost"));
    for (int hole = 0; hole < loops.size(); ++hole) {
        if (loops[hole].cutout < 0 || !loops[hole].closed) continue;
        const auto outer = std::find_if(loops.cbegin(), loops.cend(), [&](const Loop &loop) {
            return loop.contour == loops[hole].contour && loop.cutout < 0 && loop.closed;
        });
        if (outer == loops.cend()) return reject(QStringLiteral("missing_outer_boundary"));
        const auto holePath = polygonPath(polygons[hole]);
        if (fill_geometry::pathArea(holePath.subtracted(polygonPath(polygons[outer - loops.cbegin()]))) > context.areaTolerance) {
            return reject(QStringLiteral("hole_outside_outer_boundary"), {{QStringLiteral("loop"), hole}});
        }
        for (int other = 0; other < hole; ++other) {
            if (loops[other].contour == loops[hole].contour && loops[other].cutout >= 0
                && fill_geometry::pathArea(holePath.intersected(polygonPath(polygons[other]))) > context.areaTolerance) {
                return reject(QStringLiteral("hole_intersection"), {{QStringLiteral("loop"), hole}, {QStringLiteral("otherLoop"), other}});
            }
        }
    }

    return true;
}

void compactVertices(MeshData &mesh, Vertices &vertices) {
    QHash<int, int> remap;
    QVector<QPointF> positions;
    const auto remapPoints = [&](QVector<MeshPoint> &points) {
        for (auto &point : points) {
            const int root = vertices.root(point.vertex);
            if (!remap.contains(root)) {
                remap.insert(root, static_cast<int>(positions.size()));
                positions.push_back(vertices.position(root));
            }
            point.vertex = remap.value(root);
        }
    };

    for (auto &contour : mesh.contours) {
        remapPoints(contour.points);
        for (auto &cutout : contour.cutouts) remapPoints(cutout);
    }
    mesh.vertices = std::move(positions);
}

} // namespace

void appendMeshGeometry(MeshData &destination, const fls::scene::Layer &node, const QTransform &transform) {
    const auto append = [&](fls::scene::ContourData data, const QString &name, bool visible, double opacity) {
        const bool closed = data.pathKind == fls::scene::ContourPathKind::Pen && data.closed;
        materializePenHandles(data.points, closed);
        for (int index = 0; index < data.cutouts.size(); ++index) {
            auto &cutout = data.cutouts[index];
            materializePenHandles(cutout, data.cutoutClosed || index + 1 < data.cutouts.size());
        }
        destination.appendContour(transformedContourData(std::move(data), transform), name, visible, opacity);
    };

    if (node.kind() == fls::scene::LayerKind::Contour) {
        const auto &figure = static_cast<const fls::scene::ContourFigure &>(node);
        append(figure.data, node.name, node.visible, node.opacity);
    } else if (node.kind() == fls::scene::LayerKind::Mesh) {
        const auto &mesh = static_cast<const fls::scene::Mesh &>(node);
        const int offset = static_cast<int>(destination.vertices.size());
        for (const auto &position : mesh.data.vertices) destination.vertices.push_back(transform.map(position));
        for (int index = 0; index < mesh.data.contours.size(); ++index) {
            const auto &source = mesh.data.contours[index];
            MeshData transformed;
            transformed.appendContour(transformedContourData(mesh.data.contourData(index), transform),
                                      source.name, source.visible && node.visible, source.opacity * node.opacity);
            auto contour = transformed.contours.front();
            const auto transformPoints = [&](QVector<MeshPoint> &points, const QVector<MeshPoint> &original) {
                for (int point = 0; point < points.size(); ++point) {
                    points[point].vertex = original[point].vertex + offset;
                }
            };
            transformPoints(contour.points, source.points);
            for (int cutout = 0; cutout < contour.cutouts.size(); ++cutout) {
                transformPoints(contour.cutouts[cutout], source.cutouts[cutout]);
            }
            destination.contours.push_back(std::move(contour));
        }
    }
}

bool connectMeshContours(MeshData &mesh, QString *error, QJsonObject *diagnostics) {
    MeshData rebuilt = mesh;
    QVector<Span> spans;
    MeshDiagnostics trace(mesh);
    QString stage = QStringLiteral("input_validation");
    const auto reject = [&](const QString &reason) {
        if (error != nullptr) *error = reason;
        const auto report = trace.finish(false, stage, reason);
        if (diagnostics != nullptr) *diagnostics = report;

        return false;
    };
    const auto validPoint = [&](const MeshPoint &point) {
        return point.vertex >= 0 && point.vertex < mesh.vertices.size()
            && std::isfinite(point.incoming.x()) && std::isfinite(point.incoming.y())
            && std::isfinite(point.outgoing.x()) && std::isfinite(point.outgoing.y())
            && std::isfinite(point.strokeWidth) && point.strokeWidth >= 0.0 && std::isfinite(point.endCap);
    };

    if (mesh.contours.size() < 2) return reject(QStringLiteral("A mesh requires multiple contours"));
    for (const auto &point : mesh.vertices) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return reject(QStringLiteral("Contour coordinates must be finite"));
    }
    for (const auto &contour : mesh.contours) {
        if (!std::all_of(contour.points.cbegin(), contour.points.cend(), validPoint)) return reject(QStringLiteral("Invalid contour topology"));
        for (const auto &cutout : contour.cutouts) {
            if (!std::all_of(cutout.cbegin(), cutout.cend(), validPoint)) return reject(QStringLiteral("Invalid contour topology"));
        }
    }
    if (std::all_of(mesh.contours.cbegin(), mesh.contours.cend(), [](const auto &contour) {
            return contour.style.pathKind == fls::scene::ContourPathKind::Pen && contour.style.closed;
        })) {
        return connectMeshRegions(mesh, error, diagnostics);
    }
    trace.set(QStringLiteral("pipeline"), QStringLiteral("open_arc_correspondence"));
    const auto loops = meshLoops(mesh, spans);
    const auto context = regionContext(mesh, loops, spans);
    Vertices vertices(mesh.vertices);
    const double tolerance = context.extent * context.tolerance;

    trace.set(QStringLiteral("extent"), context.extent);
    trace.set(QStringLiteral("coordinateTolerance"), tolerance);
    trace.set(QStringLiteral("flatness"), context.flatness);
    trace.set(QStringLiteral("normalizedAreaTolerance"), context.areaTolerance);
    trace.set(QStringLiteral("areaTolerance"), context.areaTolerance * context.extent * context.extent);
    trace.set(QStringLiteral("loopCount"), loops.size());
    trace.set(QStringLiteral("spanCount"), spans.size());

    if (context.extent <= 0.0) return reject(QStringLiteral("Contours must contain valid boundary regions"));
    for (int index = 0; index < loops.size(); ++index) {
        if (!simpleBoundary(context.polygons[index], loops[index].closed, context.tolerance)) {
            return reject(QStringLiteral("Contours must have simple, nondegenerate boundaries"));
        }
        for (const auto &point : loops[index].points) vertices.addOwner(point.vertex, loops[index].contour);
    }
    QJsonArray overlaps;
    for (int first = 0; first < context.material.size(); ++first) {
        for (int second = first + 1; second < context.material.size(); ++second) {
            const double area = fill_geometry::pathArea(context.material[first].intersected(context.material[second]));
            if (area > context.tolerance * context.tolerance) {
                overlaps.append(QJsonObject{{QStringLiteral("firstContour"), first}, {QStringLiteral("secondContour"), second},
                                            {QStringLiteral("area"), area * context.extent * context.extent}});
            }
        }
    }
    trace.set(QStringLiteral("inputOverlaps"), overlaps);
    stage = QStringLiteral("boundary_overlay");
    for (int first = 0; first < spans.size(); ++first) {
        for (int second = first + 1; second < spans.size(); ++second) {
            if (spans[first].contour == spans[second].contour) continue;
            const auto &a = spans[first].curve;
            const auto &b = spans[second].curve;
            if ((a.start == b.start && a.control == b.control && a.control2 == b.control2 && a.end == b.end)
                || (a.start == b.end && a.control == b.control2 && a.control2 == b.control && a.end == b.start)) continue;
            intersectSpans(spans[first], spans[second], loops, vertices, a, b, 0.0, 1.0, 0.0, 1.0, tolerance, 0);
        }
    }
    overlayCoincidentPoints(spans, loops, vertices, tolerance);
    stage = QStringLiteral("region_selection");
    const auto regions = prepareRegions(boundaryRegions(spans, loops, context, tolerance, trace), spans, loops, vertices, tolerance, trace);
    overlayCoincidentPoints(spans, loops, vertices, tolerance);
    stage = QStringLiteral("boundary_reconstruction");
    if (!reconstructRegions(rebuilt, spans, loops, regions, vertices, tolerance, trace)) {
        return reject(QStringLiteral("The inferred arc regions do not form valid contour boundaries"));
    }
    compactVertices(rebuilt, vertices);
    stage = QStringLiteral("connectivity_validation");
    if (!rebuilt.valid()) return reject(QStringLiteral("Contours must touch through valid shared boundary regions"));
    stage = QStringLiteral("topology_validation");
    if (!validReconstruction(rebuilt, loops, spans, context, regions, trace)) {
        return reject(QStringLiteral("Arc reconstruction must preserve contour winding, material regions, and holes"));
    }
    mesh = std::move(rebuilt);
    trace.set(QStringLiteral("outputVertices"), mesh.vertices.size());
    const auto report = trace.finish(true, QStringLiteral("complete"));
    if (diagnostics != nullptr) *diagnostics = report;

    return true;
}

} // namespace gui
