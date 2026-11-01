#include "contour_partition.h"

#include "pen_fill.h"
#include "contour_mesh_simplify.h"
#include "cubic_contour.h"

#include <CDT.h>
#include <clipper2/clipper.h>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <array>
#include <optional>
#include <queue>
#include <tuple>
#include <stdexcept>

namespace gui {
namespace {

using fls::scene::MeshData;
using fls::scene::MeshPoint;
using Triangulation = CDT::Triangulation<double>;
using RegionPaths = Clipper2Lib::Paths64;

constexpr int kMaximumSubdivisionRecords = 8192;
constexpr int kMaximumPartitionDecisions = 512;
constexpr int kSubdivisionDepth = 24;
const double kCoordinateRoundoff = std::pow(std::numeric_limits<double>::epsilon(), 2.0 / 3.0);
constexpr double kAreaApproximationFactor = 8.0;
const double kClipperScale = 1.0 / kCoordinateRoundoff;
constexpr int kBoundaryLeafSize = 8;
constexpr int kGrowthSubdivisionDepth = 10;
constexpr double kMaximumAxialGradient = 1.0 / 3.0;
constexpr int kMaximumCorridorCuts = 128;

struct SourceSpan {
    PenBoundarySegment curve;
    MeshPoint start;
    MeshPoint end;
    int contour = -1;
    int cutout = -1;
};

struct Constraint {
    QPointF start;
    QPointF end;
    int first = -1;
    int second = -1;
    int span = -1;
    double low = 0.0;
    double high = 1.0;
};

struct PartitionInput {
    QVector<SourceSpan> spans;
    QVector<Constraint> constraints;
    QVector<QPainterPath> material;
    QVector<QPainterPath> sourceHoles;
    QVector<QPainterPath> completedVoids;
    QVector<double> winding;
    QVector<QPointF> positions;
    QHash<int, QPointF> originalCoordinates;
    QSet<int> anchors;
    QHash<quint64, QVector<int>> originals;
    QHash<QPair<qint64, qint64>, int> pointIndex;
    QTransform normalization;
    QTransform restoration;
    double extent = 0.0;
    double flatness = std::cbrt(std::numeric_limits<double>::epsilon());
    double areaTolerance = 0.0;
    double maximumRegionMismatchFraction = 0.0;
};

struct TriangleLabel {
    QVector<int> coverage;
    int owner = -1;
    bool gap = false;
};

struct PartitionEdge {
    int start = -1;
    int end = -1;
    int span = -1;
    double first = 0.0;
    double last = 1.0;
};

struct PartitionResult {
    MeshData mesh;
    QVector<int> sourceLabels;
};

class PartitionReport {
public:
    explicit PartitionReport(const MeshData &mesh) {
        timer_.start();
        report_.insert(QStringLiteral("version"), 7);
        report_.insert(QStringLiteral("pipeline"), QStringLiteral("topology_wavefront"));
        report_.insert(QStringLiteral("inputContours"), mesh.contours.size());
        report_.insert(QStringLiteral("inputVertices"), mesh.vertices.size());
    }

    void set(const QString &name, const QJsonValue &value) { report_.insert(name, value); }

    void decision(const QString &reason, QJsonObject detail = {}) {
        ++counts_[reason];
        detail.insert(QStringLiteral("decision"), reason);
        if (decisions_.size() < kMaximumPartitionDecisions) decisions_.append(detail);
        else ++omitted_;
    }

    QJsonObject finish(bool accepted, const QString &stage, const QString &reason) {
        QJsonObject counts;
        for (auto entry = counts_.cbegin(); entry != counts_.cend(); ++entry) counts.insert(entry.key(), entry.value());
        report_.insert(QStringLiteral("accepted"), accepted);
        report_.insert(QStringLiteral("stage"), stage);
        report_.insert(QStringLiteral("reason"), reason);
        report_.insert(QStringLiteral("decisions"), decisions_);
        report_.insert(QStringLiteral("decisionCounts"), counts);
        report_.insert(QStringLiteral("omittedDecisions"), omitted_);
        report_.insert(QStringLiteral("elapsedMs"), timer_.elapsed());

        return report_;
    }

private:
    QJsonObject report_;
    QJsonArray decisions_;
    QHash<QString, int> counts_;
    QElapsedTimer timer_;
    int omitted_ = 0;
};

double cross(const QPointF &first, const QPointF &second) {
    return first.x() * second.y() - first.y() * second.x();
}

double magnitude(const QPointF &point) {
    return std::hypot(point.x(), point.y());
}

quint64 edgeKey(int first, int second) {
    return (quint64(std::min(first, second)) << 32) | quint32(std::max(first, second));
}

QPointF position(const Triangulation &triangulation, int vertex) {
    const auto &point = triangulation.vertices[vertex];

    return {point.x, point.y};
}

QPointF triangleCenter(const Triangulation &triangulation, int triangle) {
    const auto &vertices = triangulation.triangles[triangle].vertices;

    return (position(triangulation, vertices[0]) + position(triangulation, vertices[1]) + position(triangulation, vertices[2])) / 3.0;
}

double triangleArea(const Triangulation &triangulation, int triangle) {
    const auto &vertices = triangulation.triangles[triangle].vertices;

    return std::abs(cross(position(triangulation, vertices[1]) - position(triangulation, vertices[0]),
                          position(triangulation, vertices[2]) - position(triangulation, vertices[0]))) * 0.5;
}

int inputVertex(PartitionInput &input, const QPointF &point, bool anchor) {
    const QPair<qint64, qint64> key{std::llround(point.x() / kCoordinateRoundoff), std::llround(point.y() / kCoordinateRoundoff)};
    for (qint64 horizontal = -1; horizontal <= 1; ++horizontal) {
        for (qint64 vertical = -1; vertical <= 1; ++vertical) {
            const auto found = input.pointIndex.constFind({key.first + horizontal, key.second + vertical});
            if (found == input.pointIndex.cend()
                || (found.key() != key && magnitude(input.positions[found.value()] - point) > kCoordinateRoundoff)) continue;
            if (anchor) input.anchors.insert(found.value());

            return found.value();
        }
    }
    const int vertex = static_cast<int>(input.positions.size());

    input.positions.push_back(point);
    input.pointIndex.insert(key, vertex);
    if (anchor) input.anchors.insert(vertex);

    return vertex;
}

void flattenSpan(PartitionInput &input, int span, const PenBoundarySegment &curve,
                  double low, double high, int depth, QPolygonF &polygon) {
    if (curve.flatness() > input.flatness && depth < kSubdivisionDepth) {
        const auto halves = curve.split();
        const double middle = (low + high) * 0.5;
        flattenSpan(input, span, halves.first, low, middle, depth + 1, polygon);
        flattenSpan(input, span, halves.second, middle, high, depth + 1, polygon);
        return;
    }
    const int first = inputVertex(input, curve.start, low == 0.0);
    const int second = inputVertex(input, curve.end, high == 1.0);
    if (first == second) throw std::runtime_error("The boundary contains a degenerate segment");
    const int index = static_cast<int>(input.constraints.size());
    input.constraints.push_back({curve.start, curve.end, first, second, span, low, high});
    input.originals[edgeKey(first, second)].push_back(index);
    polygon.push_back(curve.end);
}

PenContour validateLoop(QVector<PenPoint> points, double flatness) {
    QRectF bounds;

    for (const auto &curve : penSegments(points, true)) {
        QPolygonF controls{curve.start, curve.end};
        if (curve.curved) controls << curve.control << curve.control2;
        bounds = bounds.united(controls.boundingRect());
    }
    const double extent = std::max(bounds.width(), bounds.height());
    if (extent <= 0.0) return buildPenContour(points, flatness);
    const QTransform normalization(1.0 / extent, 0.0, 0.0, 1.0 / extent, -bounds.left() / extent, -bounds.top() / extent);
    for (auto &point : points) transformPenPoint(point, normalization);

    return buildPenContour(points, std::min(flatness, flatness / extent));
}

void addLoop(PartitionInput &input, const QVector<PenPoint> &points, const QVector<MeshPoint> &attributes,
             int contour, int cutout) {
    QVector<PenPoint> normalized = points;
    QPolygonF polygon;
    double area = 0.0;

    if (points.size() < 3) throw std::runtime_error("Closed regions need nondegenerate boundary loops");
    for (auto &point : normalized) transformPenPoint(point, input.normalization);
    if (!validateLoop(normalized, input.flatness).valid()) throw std::runtime_error("Regions must have simple, nondegenerate boundary loops");
    const auto curves = penSegments(normalized, true);
    polygon.push_back(curves.front().start);
    for (int index = 0; index < curves.size(); ++index) {
        const int span = static_cast<int>(input.spans.size());
        input.originalCoordinates.insert(inputVertex(input, normalized[index].position, true), points[index].position);
        input.spans.push_back({curves[index], attributes[index], attributes[(index + 1) % attributes.size()], contour, cutout});
        flattenSpan(input, span, curves[index], 0.0, 1.0, 0, polygon);
        area += curves[index].signedArea();
    }
    if (polygon.front() == polygon.back()) polygon.removeLast();
    if (cutout < 0) input.winding[contour] = area;
    QPainterPath path;
    path.addPolygon(polygon);
    path.closeSubpath();
    if (cutout >= 0) input.sourceHoles.push_back(path);
    input.material[contour].addPath(path);
    input.material[contour].setFillRule(Qt::OddEvenFill);
}


PartitionInput partitionInput(const MeshData &mesh) {
    PartitionInput input;
    QRectF bounds;

    for (int index = 0; index < mesh.contours.size(); ++index) {
        const auto data = mesh.contourData(index);
        const auto addBounds = [&](const QVector<PenPoint> &points) {
            for (const auto &curve : penSegments(points, true)) {
                QPolygonF controls{curve.start, curve.end};
                if (curve.curved) controls << curve.control << curve.control2;
                bounds = bounds.united(controls.boundingRect());
            }
        };
        addBounds(data.points);
        for (const auto &cutout : data.cutouts) addBounds(cutout);
    }
    input.extent = std::max(bounds.width(), bounds.height());
    if (!std::isfinite(input.extent) || input.extent <= 0.0) throw std::runtime_error("Regions must have a finite spatial extent");
    input.normalization = QTransform(1.0 / input.extent, 0.0, 0.0, 1.0 / input.extent,
                                      -bounds.left() / input.extent, -bounds.top() / input.extent);
    input.restoration = input.normalization.inverted();
    input.material.resize(mesh.contours.size());
    input.winding.resize(mesh.contours.size());
    for (int index = 0; index < mesh.contours.size(); ++index) {
        const auto data = mesh.contourData(index);
        const auto &contour = mesh.contours[index];
        if (!data.cutouts.isEmpty() && !data.cutoutClosed) throw std::runtime_error("Region holes must be closed");
        addLoop(input, data.points, contour.points, index, -1);
        for (int hole = 0; hole < data.cutouts.size(); ++hole) addLoop(input, data.cutouts[hole], contour.cutouts[hole], index, hole);
    }
    double perimeter = 0.0;
    for (const auto &boundary : input.constraints) perimeter += magnitude(boundary.end - boundary.start);
    input.areaTolerance = kAreaApproximationFactor * input.flatness * perimeter;

    return input;
}

Triangulation triangulate(const PartitionInput &input, const QVector<QPair<int, int>> &additional = {}) {
    Triangulation triangulation(CDT::VertexInsertionOrder::Auto, CDT::IntersectingConstraintEdges::TryResolve, kCoordinateRoundoff);
    std::vector<CDT::V2d<double>> vertices;
    std::vector<CDT::Edge> constraints;

    for (const auto &point : input.positions) vertices.emplace_back(point.x(), point.y());
    for (const auto &edge : input.constraints) constraints.emplace_back(edge.first, edge.second);
    for (const auto &edge : additional) constraints.emplace_back(edge.first, edge.second);
    triangulation.insertVertices(vertices);
    triangulation.insertEdges(constraints);
    triangulation.eraseSuperTriangle();

    return triangulation;
}


QVector<TriangleLabel> labelTriangles(const Triangulation &triangulation, const PartitionInput &input, PartitionReport &report) {
    QVector<TriangleLabel> labels(triangulation.triangles.size());
    QVector<double> exclusive(input.material.size(), 0.0);
    QJsonArray overlaps;
    std::map<std::pair<int, int>, double> overlapAreas;
    int maximumCoverage = 0;

    for (int index = 0; index < labels.size(); ++index) {
        const QPointF center = triangleCenter(triangulation, index);
        auto &label = labels[index];
        for (int contour = 0; contour < input.material.size(); ++contour) {
            if (input.material[contour].contains(center)) label.coverage.push_back(contour);
        }
        maximumCoverage = std::max(maximumCoverage, static_cast<int>(label.coverage.size()));
        if (!label.coverage.isEmpty()) label.owner = label.coverage.front();
        const double area = triangleArea(triangulation, index);
        if (label.coverage.size() == 1) exclusive[label.owner] += area;
        for (int first = 0; first < label.coverage.size(); ++first) {
            for (int second = first + 1; second < label.coverage.size(); ++second) {
                overlapAreas[{label.coverage[first], label.coverage[second]}] += area;
            }
        }
    }
    for (const auto &[pair, area] : overlapAreas) {
        overlaps.append(QJsonObject{{QStringLiteral("firstContour"), pair.first}, {QStringLiteral("secondContour"), pair.second},
                                    {QStringLiteral("area"), area * input.extent * input.extent}});
    }
    report.set(QStringLiteral("inputOverlaps"), overlaps);
    report.set(QStringLiteral("maximumCoverage"), maximumCoverage);
    for (int contour = 0; contour < exclusive.size(); ++contour) {
        if (exclusive[contour] <= kCoordinateRoundoff) {
            report.decision(QStringLiteral("no_exclusive_region"), {{QStringLiteral("contour"), contour}});
            throw std::runtime_error("Each region needs an unambiguous material interior");
        }
    }

    return labels;
}

QJsonArray jsonLabels(const QVector<int> &labels) {
    QJsonArray result;
    for (const int label : labels) result.append(label);

    return result;
}

void assignOverlaps(const Triangulation &triangulation, const PartitionInput &input, const QVector<TriangleLabel> &labels,
                     PartitionReport &report) {
    QSet<int> visited;
    int count = 0;

    for (int seed = 0; seed < labels.size(); ++seed) {
        const auto &coverage = labels[seed].coverage;
        if (coverage.size() < 2 || visited.contains(seed)) continue;
        QVector<int> component{seed};
        double area = 0.0;
        visited.insert(seed);
        for (int next = 0; next < component.size(); ++next) {
            const int index = component[next];
            area += triangleArea(triangulation, index);
            for (const auto neighbor : triangulation.triangles[index].neighbors) {
                if (neighbor == CDT::noNeighbor || visited.contains(neighbor) || labels[neighbor].coverage != coverage) continue;
                visited.insert(neighbor);
                component.push_back(neighbor);
            }
        }
        report.decision(QStringLiteral("assigned_overlap_region"), {{QStringLiteral("labels"), jsonLabels(coverage)},
                         {QStringLiteral("owner"), coverage.front()}, {QStringLiteral("triangles"), component.size()},
                         {QStringLiteral("area"), area * input.extent * input.extent}});
        ++count;
    }
    report.set(QStringLiteral("overlapRegions"), count);
}

QHash<quint64, QVector<int>> constraintSources(const Triangulation &triangulation, const PartitionInput &input) {
    QHash<quint64, QVector<int>> sources;

    for (const auto &edge : triangulation.fixedEdges) {
        const quint64 key = edgeKey(edge.v1(), edge.v2());
        auto &references = sources[key];
        references = input.originals.value(key);
        const auto pieces = triangulation.pieceToOriginals.find(edge);
        if (pieces != triangulation.pieceToOriginals.end()) {
            for (const auto &original : pieces->second) {
                for (const int reference : input.originals.value(edgeKey(original.v1(), original.v2()))) {
                    if (!references.contains(reference)) references.push_back(reference);
                }
            }
        }
    }

    return sources;
}

PartitionEdge boundaryEdge(int start, int end, const Triangulation &triangulation, const PartitionInput &input,
                            const QHash<quint64, QVector<int>> &sources) {
    PartitionEdge result{start, end};
    int reference = -1;

    for (const int candidate : sources.value(edgeKey(start, end))) {
        if (reference < 0 || input.constraints[candidate].span < input.constraints[reference].span) reference = candidate;
    }
    if (reference >= 0) {
        const auto &constraint = input.constraints[reference];
        const QPointF direction = constraint.end - constraint.start;
        const auto parameter = [&](int vertex) {
            const double fraction = QPointF::dotProduct(position(triangulation, vertex) - constraint.start, direction)
                / QPointF::dotProduct(direction, direction);

            return constraint.low + (constraint.high - constraint.low) * std::clamp(fraction, 0.0, 1.0);
        };
        result.span = constraint.span;
        result.first = parameter(start);
        result.last = parameter(end);
    }

    return result;
}

QVector<QVector<PartitionEdge>> partitionBoundaries(const Triangulation &triangulation, const PartitionInput &input,
                                                   const QVector<TriangleLabel> &labels) {
    const auto sources = constraintSources(triangulation, input);
    QVector<QVector<PartitionEdge>> boundaries(input.material.size());

    for (int index = 0; index < labels.size(); ++index) {
        const int owner = labels[index].owner;
        if (owner < 0) continue;
        const auto &triangle = triangulation.triangles[index];
        for (int edge = 0; edge < 3; ++edge) {
            const auto neighbor = triangle.neighbors[edge];
            if (neighbor != CDT::noNeighbor && labels[neighbor].owner == owner) continue;
            boundaries[owner].push_back(boundaryEdge(triangle.vertices[edge], triangle.vertices[(edge + 1) % 3], triangulation, input, sources));
        }
    }

    return boundaries;
}

QVector<QVector<PartitionEdge>> traceLoops(const QVector<PartitionEdge> &edges, const Triangulation &triangulation) {
    QHash<int, QVector<int>> outgoing;
    QSet<int> visited;
    QVector<QVector<PartitionEdge>> loops;

    for (int index = 0; index < edges.size(); ++index) outgoing[edges[index].start].push_back(index);
    for (int seed = 0; seed < edges.size(); ++seed) {
        if (visited.contains(seed)) continue;
        QVector<PartitionEdge> loop;
        int index = seed;
        do {
            if (visited.contains(index)) throw std::runtime_error("The region boundary has inconsistent incidence");
            visited.insert(index);
            const auto &edge = edges[index];
            loop.push_back(edge);
            const QPointF incoming = position(triangulation, edge.start) - position(triangulation, edge.end);
            int next = -1;
            double turn = std::numeric_limits<double>::infinity();
            for (const int candidate : outgoing.value(edge.end)) {
                const QPointF direction = position(triangulation, edges[candidate].end) - position(triangulation, edge.end);
                double angle = std::atan2(-cross(incoming, direction), QPointF::dotProduct(incoming, direction));
                if (angle <= 0.0) angle += 2.0 * std::acos(-1.0);
                if (angle < turn) {
                    turn = angle;
                    next = candidate;
                }
            }
            if (next < 0) throw std::runtime_error("The region boundary is open");
            index = next;
        } while (index != seed);
        QVector<QVector<PartitionEdge>> pending{std::move(loop)};
        while (!pending.isEmpty()) {
            auto part = pending.takeLast();
            QHash<int, int> encountered;
            bool split = false;
            for (int edge = 0; edge < part.size(); ++edge) {
                const auto previous = encountered.constFind(part[edge].start);
                if (previous == encountered.cend()) {
                    encountered.insert(part[edge].start, edge);
                    continue;
                }
                const int first = previous.value();
                auto interior = part.mid(first, edge - first);
                part.remove(first, edge - first);
                if (interior.size() >= 3) pending.push_back(std::move(interior));
                if (part.size() >= 3) pending.push_back(std::move(part));
                split = true;
                break;
            }
            if (!split && part.size() >= 3) loops.push_back(std::move(part));
        }
    }

    return loops;
}

void simplifyLoop(QVector<PartitionEdge> &loop, const QSet<int> &junctions) {
    bool changed = true;

    while (changed && loop.size() > 3) {
        changed = false;
        for (int index = 0; index < loop.size(); ++index) {
            const int next = (index + 1) % loop.size();
            auto &first = loop[index];
            const auto &second = loop[next];
            if (junctions.contains(first.end) || first.span != second.span
                || (first.span >= 0 && std::abs(first.last - second.first) > kCoordinateRoundoff)) continue;
            first.end = second.end;
            first.last = second.last;
            loop.removeAt(next);
            changed = true;
            break;
        }
    }
}

void retainPolyline(const QVector<int> &chain, int first, int last, const Triangulation &triangulation,
                     double tolerance, QSet<int> &retained) {
    const QPointF start = position(triangulation, chain[first]);
    const QPointF direction = position(triangulation, chain[last]) - start;
    const double squared = QPointF::dotProduct(direction, direction);
    int furthest = -1;
    double distance = tolerance;

    retained.insert(chain[first]);
    retained.insert(chain[last]);
    for (int index = first + 1; index < last; ++index) {
        const QPointF relative = position(triangulation, chain[index]) - start;
        const double parameter = squared > 0.0 ? std::clamp(QPointF::dotProduct(relative, direction) / squared, 0.0, 1.0) : 0.0;
        const double deviation = magnitude(relative - direction * parameter);
        if (deviation > distance) {
            distance = deviation;
            furthest = index;
        }
    }
    if (furthest >= 0) {
        retainPolyline(chain, first, furthest, triangulation, tolerance, retained);
        retainPolyline(chain, furthest, last, triangulation, tolerance, retained);
    }
}

void retainSharedChains(const QVector<QVector<PartitionEdge>> &boundaries, const Triangulation &triangulation,
                        double tolerance, QSet<int> &retained) {
    QHash<int, QSet<int>> adjacency;
    QSet<int> original;
    QSet<quint64> visited;

    for (const auto &boundary : boundaries) {
        for (const auto &edge : boundary) {
            if (edge.span >= 0) {
                original.insert(edge.start);
                original.insert(edge.end);
            } else {
                adjacency[edge.start].insert(edge.end);
                adjacency[edge.end].insert(edge.start);
            }
        }
    }
    for (auto vertex = adjacency.cbegin(); vertex != adjacency.cend(); ++vertex) {
        if (original.contains(vertex.key()) || vertex.value().size() != 2) retained.insert(vertex.key());
    }
    const auto endpoints = retained;
    for (int pass = 0; pass < 2; ++pass) {
        for (auto vertex = adjacency.cbegin(); vertex != adjacency.cend(); ++vertex) {
            if (pass == 0 && !endpoints.contains(vertex.key())) continue;
            for (const int neighbor : vertex.value()) {
                if (visited.contains(edgeKey(vertex.key(), neighbor))) continue;
                QVector<int> chain{vertex.key(), neighbor};
                visited.insert(edgeKey(vertex.key(), neighbor));
                while (!endpoints.contains(chain.back()) && chain.back() != chain.front()) {
                    int next = -1;
                    for (const int candidate : adjacency.value(chain.back())) {
                        if (candidate != chain[chain.size() - 2]) next = candidate;
                    }
                    if (next < 0 || visited.contains(edgeKey(chain.back(), next))) break;
                    visited.insert(edgeKey(chain.back(), next));
                    chain.push_back(next);
                }
                retainPolyline(chain, 0, static_cast<int>(chain.size()) - 1, triangulation, tolerance, retained);
            }
        }
    }
}

PenBoundarySegment subcurve(PenBoundarySegment curve, double first, double last) {
    const double low = std::min(first, last);
    const double high = std::max(first, last);

    if (high < 1.0) curve = curve.split(high).first;
    if (low > 0.0) curve = curve.split(low / high).second;
    if (first > last) {
        std::swap(curve.start, curve.end);
        std::swap(curve.control, curve.control2);
    }

    return curve;
}

QVector<MeshPoint> meshLoop(const QVector<PartitionEdge> &loop, const PartitionInput &input, MeshData &mesh,
                            QHash<int, int> &vertices, const Triangulation &triangulation, const QHash<int, QPointF> &coordinates) {
    QVector<MeshPoint> points;
    const auto vertex = [&](int index) {
        if (!vertices.contains(index)) {
            vertices.insert(index, static_cast<int>(mesh.vertices.size()));
            mesh.vertices.push_back(coordinates.value(index, input.restoration.map(position(triangulation, index))));
        }

        return vertices.value(index);
    };

    for (const auto &edge : loop) {
        MeshPoint point;
        if (edge.span >= 0) {
            const auto &source = input.spans[edge.span];
            if (edge.first == 0.0) point = source.start;
            else if (edge.first == 1.0) point = source.end;
            point.strokeWidth = source.start.strokeWidth * (1.0 - edge.first) + source.end.strokeWidth * edge.first;
        }
        point.vertex = vertex(edge.start);
        point.explicitHandles = true;
        point.incoming = point.outgoing = {};
        points.push_back(point);
    }
    for (int index = 0; index < loop.size(); ++index) {
        const auto &edge = loop[index];
        const int next = (index + 1) % points.size();
        const QPointF chord = mesh.vertices[points[next].vertex] - mesh.vertices[points[index].vertex];
        points[index].outgoing = chord / 3.0;
        points[next].incoming = -chord / 3.0;
        if (edge.span < 0) continue;
        const auto curve = subcurve(input.spans[edge.span].curve, edge.first, edge.last);
        if (!curve.curved) continue;
        points[index].outgoing = (curve.control - curve.start) * input.extent;
        points[next].incoming = (curve.control2 - curve.end) * input.extent;
        points[index].kind = points[next].kind = PenPointKind::Soft;
    }

    return points;
}

void reverseLoop(QVector<MeshPoint> &points) {
    std::reverse(points.begin(), points.end());
    for (auto &point : points) std::swap(point.incoming, point.outgoing);
}

QVector<PenPoint> loopGeometry(const QVector<MeshPoint> &loop, const MeshData &mesh) {
    QVector<PenPoint> points;
    for (const auto &point : loop) {
        points.push_back({mesh.vertices[point.vertex], point.kind, point.incoming, point.outgoing, true, point.strokeWidth, point.endCap});
    }

    return points;
}

PartitionResult reconstructPartition(const MeshData &original, const Triangulation &triangulation, const PartitionInput &input,
                                      const QVector<TriangleLabel> &labels, QSet<int> &polygonal) {
    auto boundaries = partitionBoundaries(triangulation, input, labels);
    QHash<int, QSet<int>> incidence;
    QSet<int> junctions = input.anchors;
    QHash<int, int> vertices;
    QHash<int, int> coordinateOwners;
    QHash<int, QPointF> coordinates = input.originalCoordinates;
    PartitionResult result;
    auto &mesh = result.mesh;
    QSet<int> polygonalSpans;

    for (const int region : polygonal) {
        for (const auto &edge : boundaries[region]) {
            junctions.insert(edge.start);
            junctions.insert(edge.end);
            if (edge.span >= 0 && input.spans[edge.span].curve.curved) polygonalSpans.insert(edge.span);
        }
    }
    for (auto &boundary : boundaries) {
        for (auto &edge : boundary) if (polygonalSpans.contains(edge.span)) edge.span = -1;
    }

    for (const auto &boundary : boundaries) {
        for (const auto &edge : boundary) {
            incidence[edge.start].insert(edge.end);
            incidence[edge.end].insert(edge.start);
            if (edge.span < 0) continue;
            const auto canonical = [&](int vertex, double parameter) {
                if (input.originalCoordinates.contains(vertex) || coordinateOwners.value(vertex, std::numeric_limits<int>::max()) <= edge.span) return;
                coordinates.insert(vertex, input.restoration.map(input.spans[edge.span].curve.point(parameter)));
                coordinateOwners.insert(vertex, edge.span);
            };
            canonical(edge.start, edge.first);
            canonical(edge.end, edge.last);
        }
    }
    for (auto entry = incidence.cbegin(); entry != incidence.cend(); ++entry) if (entry.value().size() != 2) junctions.insert(entry.key());
    retainSharedChains(boundaries, triangulation, input.flatness, junctions);
    for (int contour = 0; contour < boundaries.size(); ++contour) {
        auto loops = traceLoops(boundaries[contour], triangulation);
        QVector<QVector<MeshPoint>> outer;
        QVector<QVector<MeshPoint>> holes;
        for (auto &loop : loops) {
            const QPointF origin = position(triangulation, loop.front().start);
            double sampledArea = 0.0;
            double perimeter = 0.0;
            for (const auto &edge : loop) {
                const QPointF first = position(triangulation, edge.start);
                const QPointF last = position(triangulation, edge.end);
                sampledArea += cross(first - origin, last - origin);
                perimeter += magnitude(last - first);
            }
            if (std::abs(sampledArea) <= perimeter * kCoordinateRoundoff) continue;
            simplifyLoop(loop, junctions);
            auto points = meshLoop(loop, input, mesh, vertices, triangulation, coordinates);
            if (sampledArea > 0.0) outer.push_back(std::move(points));
            else holes.push_back(std::move(points));
        }
        if (outer.isEmpty()) throw std::runtime_error("Reconstruction must retain every region interior");
        QVector<int> holeOwners(holes.size(), 0);
        for (auto &boundary : outer) {
            auto rebuilt = original.contours[contour];
            rebuilt.points = boundary;
            rebuilt.cutouts.clear();
            const auto enclosure = penPath(loopGeometry(boundary, mesh), true);
            for (int hole = 0; hole < holes.size(); ++hole) {
                const bool contained = std::any_of(holes[hole].cbegin(), holes[hole].cend(), [&](const MeshPoint &point) {
                    return enclosure.contains(mesh.vertices[point.vertex]);
                });
                if (contained) {
                    rebuilt.cutouts.push_back(holes[hole]);
                    ++holeOwners[hole];
                }
            }
            if (input.winding[contour] < 0.0) reverseLoop(rebuilt.points);
            rebuilt.style.closed = rebuilt.style.cutoutClosed = true;
            mesh.contours.push_back(std::move(rebuilt));
            result.sourceLabels.push_back(contour);
        }
        if (std::any_of(holeOwners.cbegin(), holeOwners.cend(), [](int owners) { return owners != 1; })) {
            if (!polygonal.contains(contour)) {
                polygonal.insert(contour);
                return reconstructPartition(original, triangulation, input, labels, polygonal);
            }
            throw std::runtime_error("Reconstructed holes must have one containing exterior");
        }
    }

    return result;
}

QPainterPath contourMaterial(const MeshData &mesh, int index, const QTransform &normalization) {
    const auto data = mesh.contourData(index);
    auto path = normalization.map(penPath(data.points, true));

    for (const auto &hole : data.cutouts) path.addPath(normalization.map(penPath(hole, true)));
    path.setFillRule(Qt::OddEvenFill);

    return path;
}

void appendCurve(Clipper2Lib::Path64 &path, const PenBoundarySegment &curve, double flatness, int depth = 0) {
    if (curve.flatness() > flatness && depth < kSubdivisionDepth) {
        const auto halves = curve.split();
        appendCurve(path, halves.first, flatness, depth + 1);
        appendCurve(path, halves.second, flatness, depth + 1);
        return;
    }
    path.emplace_back(std::llround(curve.end.x() * kClipperScale), std::llround(curve.end.y() * kClipperScale));
}

RegionPaths regionPaths(const QPainterPath &path, double flatness) {
    RegionPaths loops;
    QPointF current;

    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        if (element.isMoveTo()) {
            loops.emplace_back();
            loops.back().emplace_back(std::llround(point.x() * kClipperScale), std::llround(point.y() * kClipperScale));
        } else if (element.isLineTo()) {
            loops.back().emplace_back(std::llround(point.x() * kClipperScale), std::llround(point.y() * kClipperScale));
        } else if (element.type == QPainterPath::CurveToElement) {
            const auto control = path.elementAt(++index);
            const auto end = path.elementAt(++index);
            appendCurve(loops.back(), {current, point, {end.x, end.y}, true, {control.x, control.y}}, flatness);
            current = {end.x, end.y};
            continue;
        }
        current = point;
    }

    return Clipper2Lib::Union(loops, path.fillRule() == Qt::OddEvenFill ? Clipper2Lib::FillRule::EvenOdd : Clipper2Lib::FillRule::NonZero);
}

RegionPaths regionUnion(const RegionPaths &first, const RegionPaths &second) {
    return Clipper2Lib::Union(first, second, Clipper2Lib::FillRule::NonZero);
}

double regionDifferenceArea(const RegionPaths &first, const RegionPaths &second) {
    const auto difference = Clipper2Lib::Difference(first, second, Clipper2Lib::FillRule::NonZero);

    return std::abs(Clipper2Lib::Area(difference)) / (kClipperScale * kClipperScale);
}

double regionOverlapArea(const RegionPaths &first, const RegionPaths &second) {
    const auto intersection = Clipper2Lib::Intersect(first, second, Clipper2Lib::FillRule::NonZero);

    return std::abs(Clipper2Lib::Area(intersection)) / (kClipperScale * kClipperScale);
}

struct GrowthBoundary {
    QPointF start;
    QPointF end;
    QPointF inward;
    int region = -1;
    int loop = -1;
    int index = -1;
    int span = -1;
    double clearance = 0.0;
    double firstParameter = 0.0;
    double lastParameter = 1.0;
    double station = 0.0;
};

struct GrowthLoop {
    QVector<int> boundaries;
    double length = 0.0;
};

struct BoundaryNode {
    QRectF bounds;
    int first = 0;
    int count = 0;
    int left = -1;
    int right = -1;
};

struct Front {
    QPointF point;
    QPointF inward;
    double distance = std::numeric_limits<double>::infinity();
    double clearance = 0.0;
    bool accessible = true;
    int boundary = -1;
    double parameter = 0.0;
};

class GrowthBoundaries {
public:
    QVector<GrowthBoundary> boundaries;

    int index(const QVector<int> &boundaries) {
        const int first = static_cast<int>(order_.size());
        order_ += boundaries;

        return build(first, static_cast<int>(boundaries.size()));
    }

    Front nearest(int root, const QPointF &point) const {
        Front result;
        search(root, point, result);

        return result;
    }

    double clearance(int root, const GrowthBoundary &boundary) const {
        double distance = std::numeric_limits<double>::infinity();
        intersect(root, (boundary.start + boundary.end) * 0.5, boundary.inward, distance);

        return std::isfinite(distance) ? distance : 0.0;
    }

private:
    QVector<int> order_;
    QVector<BoundaryNode> nodes_;

    int build(int first, int count) {
        if (count == 0) return -1;
        QPointF low(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        QPointF high(-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity());
        for (int index = first; index < first + count; ++index) {
            const auto &edge = boundaries[order_[index]];
            for (const auto &point : {edge.start, edge.end}) {
                low.setX(std::min(low.x(), point.x()));
                low.setY(std::min(low.y(), point.y()));
                high.setX(std::max(high.x(), point.x()));
                high.setY(std::max(high.y(), point.y()));
            }
        }
        const int node = static_cast<int>(nodes_.size());
        const QRectF bounds(low, high);
        nodes_.push_back({bounds, first, count});
        if (count <= kBoundaryLeafSize) return node;
        const int middle = first + count / 2;
        const bool horizontal = bounds.width() >= bounds.height();
        std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + first + count,
            [&](int first, int second) {
                const auto &a = boundaries[first];
                const auto &b = boundaries[second];

                return horizontal ? a.start.x() + a.end.x() < b.start.x() + b.end.x()
                                  : a.start.y() + a.end.y() < b.start.y() + b.end.y();
            });
        const int left = build(first, middle - first);
        const int right = build(middle, first + count - middle);
        nodes_[node].left = left;
        nodes_[node].right = right;

        return node;
    }

    double lowerDistance(int node, const QPointF &point) const {
        const auto &bounds = nodes_[node].bounds;
        const double horizontal = std::max({0.0, bounds.left() - point.x(), point.x() - bounds.right()});
        const double vertical = std::max({0.0, bounds.top() - point.y(), point.y() - bounds.bottom()});

        return std::hypot(horizontal, vertical);
    }

    void search(int index, const QPointF &point, Front &best) const {
        if (index < 0 || lowerDistance(index, point) > best.distance + kCoordinateRoundoff) return;
        const auto &node = nodes_[index];
        if (node.left >= 0) {
            const bool leftFirst = lowerDistance(node.left, point) <= lowerDistance(node.right, point);
            search(leftFirst ? node.left : node.right, point, best);
            search(leftFirst ? node.right : node.left, point, best);
            return;
        }
        for (int index = node.first; index < node.first + node.count; ++index) {
            const auto &edge = boundaries[order_[index]];
            const QPointF direction = edge.end - edge.start;
            const double parameter = std::clamp(QPointF::dotProduct(point - edge.start, direction)
                                                  / QPointF::dotProduct(direction, direction), 0.0, 1.0);
            const QPointF projection = edge.start + direction * parameter;
            const double distance = magnitude(point - projection);
            const bool nearer = distance + kCoordinateRoundoff < best.distance;
            const bool outward = QPointF::dotProduct(point - projection, edge.inward)
                < QPointF::dotProduct(point - best.point, best.inward);
            if (nearer || (std::abs(distance - best.distance) <= kCoordinateRoundoff && outward)) {
                best = {projection, edge.inward, distance, edge.clearance};
                best.boundary = order_[index];
                best.parameter = parameter;
            }
        }
    }

    void intersect(int index, const QPointF &point, const QPointF &direction, double &best) const {
        if (index < 0) return;
        const auto &node = nodes_[index];
        double near = 0.0;
        double far = best;
        for (int axis = 0; axis < 2; ++axis) {
            const double origin = axis == 0 ? point.x() : point.y();
            const double velocity = axis == 0 ? direction.x() : direction.y();
            const double low = axis == 0 ? node.bounds.left() : node.bounds.top();
            const double high = axis == 0 ? node.bounds.right() : node.bounds.bottom();
            if (velocity == 0.0) {
                if (origin < low - kCoordinateRoundoff || origin > high + kCoordinateRoundoff) return;
                continue;
            }
            const double first = (low - origin) / velocity;
            const double last = (high - origin) / velocity;
            near = std::max(near, std::min(first, last));
            far = std::min(far, std::max(first, last));
        }
        if (near > far + kCoordinateRoundoff || far <= kCoordinateRoundoff) return;
        if (node.left >= 0) {
            intersect(node.left, point, direction, best);
            intersect(node.right, point, direction, best);
            return;
        }
        for (int index = node.first; index < node.first + node.count; ++index) {
            const auto &edge = boundaries[order_[index]];
            const QPointF tangent = edge.end - edge.start;
            const double denominator = cross(direction, tangent);
            if (std::abs(denominator) <= kCoordinateRoundoff * magnitude(tangent)) continue;
            const double distance = cross(edge.start - point, tangent) / denominator;
            const double parameter = cross(edge.start - point, direction) / denominator;
            if (distance > kCoordinateRoundoff && parameter >= 0.0 && parameter <= 1.0) best = std::min(best, distance);
        }
    }
};

struct GrowingRegion {
    RegionPaths material;
    QPainterPath exterior;
    QPointF center;
    int external = -1;
    int all = -1;
    double time = 0.0;
};

struct GrowingHole {
    QPainterPath path;
    int region = -1;
    int boundary = -1;
    bool occupied = false;
};

struct Contact {
    int first = -1;
    int second = -1;
    double firstTime = std::numeric_limits<double>::infinity();
    double lastTime = 0.0;
    bool accepted = true;
};

struct CollisionSegment {
    QPointF first;
    QPointF last;
    int firstRegion = -1;
    int secondRegion = -1;
};

struct GrowthTopology {
    GrowthBoundaries index;
    QVector<GrowingRegion> regions;
    QVector<GrowingHole> holes;
    QVector<GrowthLoop> loops;
    QVector<CollisionSegment> collisions;
    QVector<RegionPaths> internalClaims;
    QHash<quint64, std::array<RegionPaths, 2>> corridorClaims;
    QHash<quint64, Contact> contacts;
    QVector<QPointF> refinements;
    bool collecting = true;
};

Clipper2Lib::Path64 integerPolygon(const QPolygonF &polygon) {
    Clipper2Lib::Path64 result;
    for (const auto &point : polygon) result.emplace_back(std::llround(point.x() * kClipperScale), std::llround(point.y() * kClipperScale));

    return result;
}

QPainterPath painterRegion(const RegionPaths &paths) {
    QPainterPath result;
    for (const auto &path : paths) {
        QPolygonF polygon;
        for (const auto &point : path) polygon.push_back(QPointF(point.x / kClipperScale, point.y / kClipperScale));
        result.addPolygon(polygon);
        result.closeSubpath();
    }
    result.setFillRule(Qt::WindingFill);

    return result;
}

RegionPaths rootedGrowth(const RegionPaths &paths, const RegionPaths &original, double tolerance, double &removed) {
    Clipper2Lib::PolyTree64 tree;
    QVector<const Clipper2Lib::PolyPath64 *> pending;
    RegionPaths retained;

    Clipper2Lib::BooleanOp(Clipper2Lib::ClipType::Union, Clipper2Lib::FillRule::NonZero, paths, {}, tree);
    for (size_t index = 0; index < tree.Count(); ++index) pending.push_back(tree.Child(index));
    while (!pending.isEmpty()) {
        const auto *outer = pending.takeLast();
        RegionPaths component{outer->Polygon()};
        for (size_t hole = 0; hole < outer->Count(); ++hole) {
            const auto *interior = outer->Child(hole);
            component.push_back(interior->Polygon());
            for (size_t island = 0; island < interior->Count(); ++island) pending.push_back(interior->Child(island));
        }
        bool rooted = regionOverlapArea(component, original) > 0.0;
        if (!rooted) {
            std::optional<Clipper2Lib::Point64> contact;
            const auto &polygon = outer->Polygon();
            double perimeter = 0.0;
            bool connectedArc = false;
            for (size_t index = 0; index < polygon.size(); ++index) {
                const auto &point = polygon[index];
                const auto &next = polygon[(index + 1) % polygon.size()];
                perimeter += std::hypot(double(next.x - point.x), double(next.y - point.y));
                for (const auto &boundary : original) {
                    if (Clipper2Lib::PointInPolygon(point, boundary) != Clipper2Lib::PointInPolygonResult::IsOn) continue;
                    if (!contact.has_value()) contact = point;
                    connectedArc |= std::hypot(double(point.x - contact->x), double(point.y - contact->y)) > tolerance * kClipperScale;
                }
            }
            rooted = connectedArc && std::abs(Clipper2Lib::Area(component)) * 2.0 > perimeter * tolerance * kClipperScale;
        }
        if (rooted) retained.insert(retained.end(), component.begin(), component.end());
        else removed += std::abs(Clipper2Lib::Area(component)) / (kClipperScale * kClipperScale);
    }

    return Clipper2Lib::Union(retained, Clipper2Lib::FillRule::NonZero);
}

GrowthTopology growthTopology(const Triangulation &triangulation, const QVector<TriangleLabel> &labels,
                              const PartitionInput &input) {
    const auto boundaries = partitionBoundaries(triangulation, input, labels);
    GrowthTopology topology;
    QVector<RegionPaths> triangles(input.material.size());

    topology.regions.resize(input.material.size());
    topology.internalClaims.resize(input.material.size());
    for (int index = 0; index < labels.size(); ++index) {
        if (labels[index].owner < 0) continue;
        const auto &triangle = triangulation.triangles[index];
        QPolygonF polygon;
        for (const auto vertex : triangle.vertices) polygon.push_back(position(triangulation, vertex));
        triangles[labels[index].owner].push_back(integerPolygon(polygon));
    }
    for (int region = 0; region < boundaries.size(); ++region) {
        auto &growing = topology.regions[region];
        QVector<int> external;
        QVector<int> all;
        growing.material = Clipper2Lib::Union(triangles[region], Clipper2Lib::FillRule::NonZero);
        double area = 0.0;
        QPointF moment;
        for (const auto &loop : traceLoops(boundaries[region], triangulation)) {
            QPolygonF polygon;
            QVector<int> indices;
            double signedArea = 0.0;
            const int loopIndex = static_cast<int>(topology.loops.size());
            GrowthLoop geometry;
            for (const auto &edge : loop) {
                const QPointF first = position(triangulation, edge.start);
                const QPointF second = position(triangulation, edge.end);
                const QPointF direction = second - first;
                const double product = cross(first, second);
                signedArea += product;
                area += product;
                moment += (first + second) * product;
                polygon.push_back(first);
                indices.push_back(static_cast<int>(topology.index.boundaries.size()));
                GrowthBoundary boundary{first, second, QPointF(-direction.y(), direction.x()) / magnitude(direction), region};
                boundary.loop = loopIndex;
                boundary.index = static_cast<int>(geometry.boundaries.size());
                boundary.span = edge.span;
                boundary.firstParameter = edge.first;
                boundary.lastParameter = edge.last;
                boundary.station = geometry.length;
                geometry.boundaries.push_back(static_cast<int>(topology.index.boundaries.size()));
                geometry.length += magnitude(direction);
                topology.index.boundaries.push_back(boundary);
            }
            topology.loops.push_back(std::move(geometry));
            all += indices;
            QPainterPath path;
            path.addPolygon(polygon);
            path.closeSubpath();
            if (signedArea > 0.0) {
                external += indices;
                growing.exterior.addPath(path);
            } else {
                topology.holes.push_back({path, region, topology.index.index(indices)});
            }
        }
        growing.center = moment / (3.0 * area);
        growing.exterior.setFillRule(Qt::WindingFill);
        growing.external = topology.index.index(external);
        growing.all = topology.index.index(all);
    }
    for (auto &boundary : topology.index.boundaries) {
        boundary.clearance = topology.index.clearance(topology.regions[boundary.region].all, boundary);
    }
    for (auto &hole : topology.holes) {
        const auto material = regionPaths(hole.path, input.flatness);
        for (int region = 0; region < topology.regions.size(); ++region) {
            if (region != hole.region && regionOverlapArea(material, topology.regions[region].material) > 0.0) hole.occupied = true;
        }
    }

    return topology;
}

class ArrivalField {
public:
    explicit ArrivalField(const GrowthTopology &topology) : topology_(topology) {}

    QVector<Front> at(const QPointF &point) {
        const QPair<qint64, qint64> key{std::llround(point.x() / kCoordinateRoundoff), std::llround(point.y() / kCoordinateRoundoff)};
        const auto found = cache_.constFind(key);
        if (found != cache_.cend()) return found.value();
        QVector<Front> fronts;
        for (const auto &region : topology_.regions) {
            auto front = topology_.index.nearest(region.external, point);
            front.accessible = front.distance <= kCoordinateRoundoff || !region.exterior.contains(point);
            if (front.distance > kCoordinateRoundoff) front.inward = (front.point - point) / front.distance;
            fronts.push_back(front);
        }
        cache_.insert(key, fronts);

        return fronts;
    }

private:
    const GrowthTopology &topology_;
    QHash<QPair<qint64, qint64>, QVector<Front>> cache_;
};

std::optional<double> contactTime(const Front &first, const Front &second) {
    const QPointF separation = second.point - first.point;
    const double time = (first.distance + second.distance) * 0.5;

    if (!first.accessible || !second.accessible || time <= kCoordinateRoundoff
        || time * 2.0 >= first.clearance || time * 2.0 >= second.clearance
        || QPointF::dotProduct(first.inward, second.inward) >= 0.0
        || QPointF::dotProduct(separation, first.inward) >= 0.0
        || QPointF::dotProduct(separation, second.inward) <= 0.0) return std::nullopt;

    return time;
}

QVector<QPair<int, int>> candidateContacts(const QVector<Front> &fronts) {
    QVector<int> nearest;
    QVector<QPair<int, int>> pairs;

    for (int region = 0; region < fronts.size(); ++region) if (fronts[region].accessible) nearest.push_back(region);
    std::sort(nearest.begin(), nearest.end(), [&](int first, int second) { return fronts[first].distance < fronts[second].distance; });
    if (nearest.size() > 3) nearest.resize(3);
    for (int first = 0; first < nearest.size(); ++first) {
        for (int second = first + 1; second < nearest.size(); ++second) {
            if (contactTime(fronts[nearest[first]], fronts[nearest[second]])) pairs.push_back({nearest[first], nearest[second]});
        }
    }

    return pairs;
}

void assignNeighbors(const Triangulation &triangulation, const QVector<TriangleLabel> &labels, GrowthTopology &topology,
                     ArrivalField &arrivals, const PartitionInput &input, PartitionReport &report) {
    for (int index = 0; index < labels.size(); ++index) {
        if (labels[index].owner >= 0) {
            for (const auto neighbor : triangulation.triangles[index].neighbors) {
                if (neighbor == CDT::noNeighbor || labels[neighbor].owner < 0 || labels[neighbor].owner == labels[index].owner) continue;
                const int first = std::min(labels[index].owner, labels[neighbor].owner);
                const int second = std::max(labels[index].owner, labels[neighbor].owner);
                auto &contact = topology.contacts[edgeKey(first, second)];
                contact.first = first;
                contact.second = second;
                contact.firstTime = 0.0;
            }
        }
        if (!labels[index].coverage.isEmpty()) continue;
        const auto fronts = arrivals.at(triangleCenter(triangulation, index));
        for (const auto &pair : candidateContacts(fronts)) {
            const int first = std::min(pair.first, pair.second);
            const int second = std::max(pair.first, pair.second);
            auto &contact = topology.contacts[edgeKey(first, second)];
            const double time = *contactTime(fronts[first], fronts[second]);
            contact.first = first;
            contact.second = second;
            contact.firstTime = std::min(contact.firstTime, time);
            contact.lastTime = std::max(contact.lastTime, time);
        }
    }
    for (auto contact = topology.contacts.begin(); contact != topology.contacts.end(); ++contact) {
        QVector<double> distances(topology.regions.size(), std::numeric_limits<double>::infinity());
        QSet<int> visited;
        distances[contact->first] = 0.0;
        for (int step = 0; step < topology.regions.size(); ++step) {
            int current = -1;
            for (int region = 0; region < distances.size(); ++region) {
                if (!visited.contains(region) && (current < 0 || distances[region] < distances[current])) current = region;
            }
            if (current < 0 || !std::isfinite(distances[current]) || distances[current] >= contact->firstTime) break;
            visited.insert(current);
            for (auto edge = topology.contacts.cbegin(); edge != topology.contacts.cend(); ++edge) {
                if (edge.key() == contact.key()) continue;
                const int neighbor = edge->first == current ? edge->second : edge->second == current ? edge->first : -1;
                if (neighbor >= 0) distances[neighbor] = std::min(distances[neighbor], distances[current] + edge->firstTime);
            }
        }
        contact->accepted = distances[contact->second] + input.flatness >= contact->firstTime;
        if (contact->accepted) {
            topology.regions[contact->first].time = std::max(topology.regions[contact->first].time, contact->lastTime);
            topology.regions[contact->second].time = std::max(topology.regions[contact->second].time, contact->lastTime);
        }
        report.decision(contact->accepted ? QStringLiteral("external_neighbors") : QStringLiteral("indirect_neighbor_pair"),
            {{QStringLiteral("firstContour"), contact->first}, {QStringLiteral("secondContour"), contact->second},
             {QStringLiteral("firstCollisionTime"), contact->firstTime * input.extent},
             {QStringLiteral("lastCollisionTime"), contact->lastTime * input.extent}});
    }
}

double interpolateTriangle(const QPointF &point, const std::array<QPointF, 3> &triangle, const std::array<double, 3> &values) {
    const double denominator = cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
    const double first = cross(point - triangle[0], triangle[2] - triangle[0]) / denominator;
    const double second = cross(triangle[1] - triangle[0], point - triangle[0]) / denominator;

    return values[0] + first * (values[1] - values[0]) + second * (values[2] - values[0]);
}

QPolygonF clipArrival(const QPolygonF &polygon, const std::array<QPointF, 3> &triangle, const std::array<double, 3> &values) {
    QPolygonF clipped;

    for (int index = 0; index < polygon.size(); ++index) {
        const auto first = polygon[index];
        const auto second = polygon[(index + 1) % polygon.size()];
        const double start = interpolateTriangle(first, triangle, values);
        const double end = interpolateTriangle(second, triangle, values);
        if (start <= 0.0) clipped.push_back(first);
        if ((start < 0.0 && end > 0.0) || (start > 0.0 && end < 0.0)) clipped.push_back(first + (second - first) * (start / (start - end)));
    }

    return clipped;
}

void recordCollision(const QPolygonF &cell, const std::array<QPointF, 3> &triangle,
                      const std::array<QVector<Front>, 3> &fronts, const QPair<int, int> &pair, GrowthTopology &topology) {
    std::array<double, 3> differences;
    QPolygonF intersections;
    double length = kCoordinateRoundoff;
    CollisionSegment segment;

    for (int vertex = 0; vertex < 3; ++vertex) differences[vertex] = fronts[vertex][pair.first].distance - fronts[vertex][pair.second].distance;
    for (int edge = 0; edge < cell.size(); ++edge) {
        const auto first = cell[edge];
        const auto last = cell[(edge + 1) % cell.size()];
        const double start = interpolateTriangle(first, triangle, differences);
        const double end = interpolateTriangle(last, triangle, differences);
        if (std::abs(start) <= kCoordinateRoundoff) intersections.push_back(first);
        if ((start < 0.0 && end > 0.0) || (start > 0.0 && end < 0.0)) intersections.push_back(first + (last - first) * (start / (start - end)));
    }
    for (int first = 0; first < intersections.size(); ++first) {
        for (int last = first + 1; last < intersections.size(); ++last) {
            const double distance = magnitude(intersections[first] - intersections[last]);
            if (distance > length) {
                length = distance;
                segment = {intersections[first], intersections[last], pair.first, pair.second};
            }
        }
    }
    if (length > kCoordinateRoundoff) topology.collisions.push_back(segment);
}

void growTriangle(const std::array<QPointF, 3> &triangle, GrowthTopology &topology, ArrivalField &arrivals,
                  const PartitionInput &input, QVector<RegionPaths> &claims, int depth) {
    const QPointF center = (triangle[0] + triangle[1] + triangle[2]) / 3.0;
    const auto fronts = arrivals.at(center);
    auto pairs = candidateContacts(fronts);
    if (!topology.collecting) {
        pairs.clear();
        for (auto contact = topology.contacts.cbegin(); contact != topology.contacts.cend(); ++contact) {
            if (contact->accepted && contact->lastTime > kCoordinateRoundoff
                && fronts[contact->first].accessible && fronts[contact->second].accessible) {
                pairs.push_back({contact->first, contact->second});
            }
        }
    }
    std::array<QVector<Front>, 3> vertices{arrivals.at(triangle[0]), arrivals.at(triangle[1]), arrivals.at(triangle[2])};
    bool refine = false;
    bool active = false;

    for (const auto &pair : pairs) {
        const auto contact = topology.contacts.constFind(edgeKey(pair.first, pair.second));
        if (contact == topology.contacts.cend() || !contact->accepted) continue;
        active = true;
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        double interpolation = 0.0;
        for (const auto &vertex : vertices) {
            const double difference = vertex[pair.first].distance - vertex[pair.second].distance;
            low = std::min(low, difference);
            high = std::max(high, difference);
            interpolation += difference / 3.0;
        }
        refine |= low <= 0.0 && high >= 0.0
            && std::abs(fronts[pair.first].distance - fronts[pair.second].distance - interpolation) > input.flatness;
    }
    QVector<int> holes;
    for (int hole = 0; hole < topology.holes.size(); ++hole) {
        const auto &internal = topology.holes[hole];
        if (!internal.occupied && topology.regions[internal.region].time > 0.0 && internal.path.contains(center)) {
            holes.push_back(hole);
            const double time = topology.index.nearest(internal.boundary, center).distance;
            double interpolation = 0.0;
            double low = time;
            double high = time;
            for (const auto &vertex : triangle) {
                const double distance = topology.index.nearest(internal.boundary, vertex).distance;
                interpolation += distance / 3.0;
                low = std::min(low, distance);
                high = std::max(high, distance);
            }
            const double budget = topology.regions[internal.region].time;
            refine |= low <= budget && high >= budget && std::abs(time - interpolation) > input.flatness;
        }
    }
    if (!active && holes.isEmpty()) return;
    if (topology.collecting && refine && depth < kGrowthSubdivisionDepth) {
        const QPointF first = (triangle[0] + triangle[1]) * 0.5;
        const QPointF second = (triangle[1] + triangle[2]) * 0.5;
        const QPointF third = (triangle[2] + triangle[0]) * 0.5;
        topology.refinements += {first, second, third};
        for (const auto &part : {std::array<QPointF, 3>{triangle[0], first, third}, {first, triangle[1], second},
                                {third, second, triangle[2]}, {first, second, third}}) {
            growTriangle(part, topology, arrivals, input, claims, depth + 1);
        }
        return;
    }
    if (topology.collecting) return;
    const QPolygonF polygon{triangle[0], triangle[1], triangle[2]};
    for (const auto &pair : pairs) {
        const auto contact = topology.contacts.constFind(edgeKey(pair.first, pair.second));
        if (contact == topology.contacts.cend() || !contact->accepted) continue;
        for (const auto &sides : {pair, QPair<int, int>{pair.second, pair.first}}) {
            auto cell = polygon;
            std::array<double, 3> limit;
            for (int predicate = 0; predicate < 3; ++predicate) {
                for (int vertex = 0; vertex < 3; ++vertex) {
                    const auto &first = vertices[vertex][sides.first];
                    const auto &second = vertices[vertex][sides.second];
                    const QPointF separation = second.point - first.point;
                    const double length = magnitude(separation);
                    const QPointF firstNormal = first.distance > kCoordinateRoundoff || length <= kCoordinateRoundoff
                        ? first.inward : -separation / length;
                    const QPointF secondNormal = second.distance > kCoordinateRoundoff || length <= kCoordinateRoundoff
                        ? second.inward : separation / length;
                    limit[vertex] = predicate == 0 ? QPointF::dotProduct(separation, firstNormal)
                        : predicate == 1 ? -QPointF::dotProduct(separation, secondNormal)
                                         : length <= kCoordinateRoundoff ? -1.0 : QPointF::dotProduct(firstNormal, secondNormal);
                }
                cell = clipArrival(cell, triangle, limit);
            }
            for (const int region : {sides.first, sides.second}) {
                for (int vertex = 0; vertex < 3; ++vertex) {
                    const auto &first = vertices[vertex][sides.first];
                    const auto &second = vertices[vertex][sides.second];
                    limit[vertex] = first.distance + second.distance - vertices[vertex][region].clearance;
                }
                cell = clipArrival(cell, triangle, limit);
            }
            for (int region = 0; region < fronts.size() && !cell.isEmpty(); ++region) {
                if (region == sides.first || !fronts[region].accessible) continue;
                for (int vertex = 0; vertex < 3; ++vertex) limit[vertex] = vertices[vertex][sides.first].distance - vertices[vertex][region].distance;
                cell = clipArrival(cell, triangle, limit);
                if (region == sides.second) continue;
                for (int vertex = 0; vertex < 3; ++vertex) limit[vertex] = vertices[vertex][sides.second].distance - vertices[vertex][region].distance;
                cell = clipArrival(cell, triangle, limit);
            }
            if (cell.size() >= 3) {
                const auto path = integerPolygon(cell);
                claims[sides.first].push_back(path);
                topology.corridorClaims[edgeKey(pair.first, pair.second)][sides.first == pair.first ? 0 : 1].push_back(path);
                if (sides.first == pair.first) recordCollision(cell, triangle, vertices, pair, topology);
            }
        }
    }
    for (const int hole : holes) {
        const auto &internal = topology.holes[hole];
        std::array<double, 3> values;
        for (int vertex = 0; vertex < 3; ++vertex) {
            values[vertex] = topology.index.nearest(internal.boundary, triangle[vertex]).distance - topology.regions[internal.region].time;
        }
        const auto cell = clipArrival(polygon, triangle, values);
        if (cell.size() >= 3) topology.internalClaims[internal.region].push_back(integerPolygon(cell));
    }
}

struct CorridorNode {
    QPointF point;
    std::array<Front, 2> fronts;
    QVector<int> edges;
    double radius = 0.0;
    double gradient = 0.0;
};

struct CorridorEdge {
    int first = -1;
    int last = -1;
    bool admissible = false;
};

struct CorridorGraph {
    QVector<CorridorNode> nodes;
    QVector<CorridorEdge> edges;
    QHash<QPair<qint64, qint64>, int> vertices;
    QSet<quint64> connections;
};

QPointF sourceNormal(const Front &front, const QPointF &point, const GrowthTopology &topology, const PartitionInput &input) {
    const auto &boundary = topology.index.boundaries[front.boundary];
    const double parameter = boundary.firstParameter + (boundary.lastParameter - boundary.firstParameter) * front.parameter;

    if (boundary.span >= 0 && parameter > kCoordinateRoundoff && parameter < 1.0 - kCoordinateRoundoff) {
        const QPointF tangent = input.spans[boundary.span].curve.derivative(parameter)
            * (boundary.lastParameter >= boundary.firstParameter ? 1.0 : -1.0);
        const double length = magnitude(tangent);
        if (length > kCoordinateRoundoff) return QPointF(-tangent.y(), tangent.x()) / length;
    }
    if (front.distance > kCoordinateRoundoff) return (front.point - point) / front.distance;

    return front.inward;
}

CorridorNode corridorNode(const QPointF &point, int first, int second, ArrivalField &arrivals,
                          const GrowthTopology &topology, const PartitionInput &input) {
    const auto fronts = arrivals.at(point);
    CorridorNode node;

    node.point = point;
    node.fronts = {fronts[first], fronts[second]};
    node.radius = (fronts[first].distance + fronts[second].distance) * 0.5;
    node.gradient = node.radius > kCoordinateRoundoff
        ? magnitude(sourceNormal(fronts[first], point, topology, input) + sourceNormal(fronts[second], point, topology, input)) * 0.5 : 0.0;

    return node;
}

int corridorVertex(CorridorGraph &graph, const CorridorNode &node) {
    const QPair<qint64, qint64> key{std::llround(node.point.x() / kCoordinateRoundoff), std::llround(node.point.y() / kCoordinateRoundoff)};

    for (qint64 horizontal = -1; horizontal <= 1; ++horizontal) {
        for (qint64 vertical = -1; vertical <= 1; ++vertical) {
            const auto found = graph.vertices.constFind({key.first + horizontal, key.second + vertical});
            if (found != graph.vertices.cend() && (found.key() == key || magnitude(graph.nodes[found.value()].point - node.point) <= kCoordinateRoundoff)) {
                return found.value();
            }
        }
    }
    const int index = static_cast<int>(graph.nodes.size());
    graph.nodes.push_back(node);
    graph.vertices.insert(key, index);

    return index;
}

void corridorEdge(CorridorGraph &graph, const CorridorNode &first, const CorridorNode &last, bool admissible) {
    const int start = corridorVertex(graph, first);
    const int end = corridorVertex(graph, last);
    const quint64 key = edgeKey(start, end);

    if (start == end || graph.connections.contains(key)) return;
    graph.connections.insert(key);
    const int index = static_cast<int>(graph.edges.size());
    graph.edges.push_back({start, end, admissible});
    graph.nodes[start].edges.push_back(index);
    graph.nodes[end].edges.push_back(index);
}

void sampleCorridor(const CorridorNode &first, const CorridorNode &last, int firstRegion, int secondRegion,
                    ArrivalField &arrivals, const GrowthTopology &topology, const PartitionInput &input, CorridorGraph &graph, int depth = 0) {
    const auto middle = corridorNode((first.point + last.point) * 0.5, firstRegion, secondRegion, arrivals, topology, input);
    const double length = magnitude(last.point - first.point);
    const bool firstInside = first.gradient <= kMaximumAxialGradient;
    const bool lastInside = last.gradient <= kMaximumAxialGradient;
    const bool middleInside = middle.gradient <= kMaximumAxialGradient;
    const bool nonlinear = std::abs(middle.gradient - (first.gradient + last.gradient) * 0.5) * length > input.flatness
        || std::abs(middle.radius - (first.radius + last.radius) * 0.5) > input.flatness
        || (firstInside == lastInside && firstInside != middleInside);

    if (nonlinear && depth < kGrowthSubdivisionDepth && length > input.flatness) {
        sampleCorridor(first, middle, firstRegion, secondRegion, arrivals, topology, input, graph, depth + 1);
        sampleCorridor(middle, last, firstRegion, secondRegion, arrivals, topology, input, graph, depth + 1);
        return;
    }
    if (firstInside != lastInside) {
        auto low = first;
        auto high = last;
        for (int iteration = 0; iteration < kSubdivisionDepth && magnitude(high.point - low.point) > input.flatness; ++iteration) {
            const auto candidate = corridorNode((low.point + high.point) * 0.5, firstRegion, secondRegion, arrivals, topology, input);
            if ((candidate.gradient <= kMaximumAxialGradient) == firstInside) low = candidate;
            else high = candidate;
        }
        auto cutoff = corridorNode((low.point + high.point) * 0.5, firstRegion, secondRegion, arrivals, topology, input);
        cutoff.gradient = kMaximumAxialGradient;
        corridorEdge(graph, first, cutoff, firstInside);
        corridorEdge(graph, cutoff, last, lastInside);
    } else {
        corridorEdge(graph, first, last, firstInside && middleInside);
    }
}

QSet<int> shortCorridors(const CorridorGraph &graph, double tolerance, int &rejectedBasins) {
    QSet<int> accepted;
    QSet<int> visited;

    for (int seed = 0; seed < graph.edges.size(); ++seed) {
        if (!graph.edges[seed].admissible || visited.contains(seed)) continue;
        QVector<int> component{seed};
        QSet<int> vertices;
        double minimum = std::numeric_limits<double>::infinity();
        visited.insert(seed);
        for (int next = 0; next < component.size(); ++next) {
            const auto &edge = graph.edges[component[next]];
            for (const int vertex : {edge.first, edge.last}) {
                vertices.insert(vertex);
                minimum = std::min(minimum, graph.nodes[vertex].radius);
                for (const int adjacent : graph.nodes[vertex].edges) {
                    if (!graph.edges[adjacent].admissible || visited.contains(adjacent)) continue;
                    visited.insert(adjacent);
                    component.push_back(adjacent);
                }
            }
        }
        QVector<int> flood;
        QSet<int> reached;
        bool descending = false;
        for (const int vertex : vertices) {
            if (graph.nodes[vertex].radius <= minimum + tolerance) {
                reached.insert(vertex);
                flood.push_back(vertex);
            }
        }
        for (int next = 0; next < flood.size() && !descending; ++next) {
            const int vertex = flood[next];
            if (graph.nodes[vertex].radius < minimum - tolerance) {
                descending = true;
                break;
            }
            for (const int adjacent : graph.nodes[vertex].edges) {
                const auto &edge = graph.edges[adjacent];
                const int neighbor = edge.first == vertex ? edge.last : edge.first;
                if (reached.contains(neighbor) || graph.nodes[neighbor].radius > minimum + tolerance) continue;
                reached.insert(neighbor);
                flood.push_back(neighbor);
            }
        }
        if (!descending) for (const int edge : component) accepted.insert(edge);
        else ++rejectedBasins;
    }

    return accepted;
}

QPolygonF forwardArc(const Front &first, const Front &last, const GrowthTopology &topology) {
    const auto &start = topology.index.boundaries[first.boundary];
    const auto &end = topology.index.boundaries[last.boundary];
    const auto &loop = topology.loops[start.loop];
    QPolygonF arc{first.point};
    int index = start.index;

    for (int step = 0; step <= loop.boundaries.size(); ++step) {
        if (index == end.index && (step > 0 || last.parameter >= first.parameter)) {
            arc.push_back(last.point);
            break;
        }
        arc.push_back(topology.index.boundaries[loop.boundaries[index]].end);
        index = (index + 1) % loop.boundaries.size();
    }

    return arc;
}

QPolygonF projectedArc(const CorridorNode &first, const CorridorNode &last, int side,
                       const GrowthTopology &topology, double tolerance) {
    const auto &a = first.fronts[side];
    const auto &b = last.fronts[side];
    QPolygonF arc;

    if (a.boundary < 0 || b.boundary < 0) return arc;
    const auto &start = topology.index.boundaries[a.boundary];
    const auto &end = topology.index.boundaries[b.boundary];
    if (start.loop != end.loop) return arc;
    if (magnitude(a.point - b.point) <= kCoordinateRoundoff) return {a.point};
    const auto &loop = topology.loops[start.loop];
    const double firstStation = start.station + a.parameter * magnitude(start.end - start.start);
    const double lastStation = end.station + b.parameter * magnitude(end.end - end.start);
    const double distance = lastStation >= firstStation ? lastStation - firstStation : loop.length + lastStation - firstStation;
    if (distance <= loop.length * 0.5) arc = forwardArc(a, b, topology);
    else {
        arc = forwardArc(b, a, topology);
        std::reverse(arc.begin(), arc.end());
    }
    const QPointF direction = last.point - first.point;
    const double squared = QPointF::dotProduct(direction, direction);
    const double reach = (first.radius + last.radius + std::sqrt(squared)) * 0.5 + tolerance;
    for (const auto &point : arc) {
        const double parameter = squared > 0.0 ? std::clamp(QPointF::dotProduct(point - first.point, direction) / squared, 0.0, 1.0) : 0.0;
        if (magnitude(point - first.point - direction * parameter) > reach) return {};
    }

    return arc;
}

void appendCorridor(RegionPaths &mask, QPolygonF arc, const CorridorNode &first, const CorridorNode &last) {
    arc.push_back(last.point);
    arc.push_back(first.point);
    auto polygon = integerPolygon(arc);
    const double area = Clipper2Lib::Area(polygon);

    if (area < 0.0) std::reverse(polygon.begin(), polygon.end());
    if (area != 0.0) mask.push_back(std::move(polygon));
}

QVector<RegionPaths> collisionCorridors(const GrowthTopology &topology, ArrivalField &arrivals,
                                       const PartitionInput &input, PartitionReport &report) {
    std::map<quint64, QVector<CollisionSegment>> segments;
    QVector<RegionPaths> masks(topology.regions.size());
    QJsonArray profiles;
    int totalCuts = 0;
    int interiorCuts = 0;
    double rejectedLength = 0.0;

    for (const auto &segment : topology.collisions) segments[edgeKey(segment.firstRegion, segment.secondRegion)].push_back(segment);
    for (const auto &[key, pieces] : segments) {
        const int firstRegion = pieces.front().firstRegion;
        const int secondRegion = pieces.front().secondRegion;
        CorridorGraph graph;
        for (const auto &piece : pieces) {
            sampleCorridor(corridorNode(piece.first, firstRegion, secondRegion, arrivals, topology, input),
                           corridorNode(piece.last, firstRegion, secondRegion, arrivals, topology, input),
                           firstRegion, secondRegion, arrivals, topology, input, graph);
        }
        int rejectedBasins = 0;
        const auto accepted = shortCorridors(graph, input.flatness, rejectedBasins);
        QHash<int, int> incidence;
        QJsonArray cuts;
        double keptLength = 0.0;
        double discardedLength = 0.0;
        double maximumGradient = 0.0;
        int projectionBreaks = 0;
        for (int index = 0; index < graph.edges.size(); ++index) {
            const auto &edge = graph.edges[index];
            const auto &first = graph.nodes[edge.first];
            const auto &last = graph.nodes[edge.last];
            const double length = magnitude(last.point - first.point);
            maximumGradient = std::max({maximumGradient, first.gradient, last.gradient});
            if (!accepted.contains(index)) {
                discardedLength += length;
                continue;
            }
            auto firstArc = projectedArc(first, last, 0, topology, input.flatness);
            auto secondArc = projectedArc(first, last, 1, topology, input.flatness);
            if (firstArc.isEmpty() || secondArc.isEmpty() || (firstArc.size() == 1 && secondArc.size() == 1)) {
                ++projectionBreaks;
                discardedLength += length;
                continue;
            }
            appendCorridor(masks[firstRegion], std::move(firstArc), first, last);
            appendCorridor(masks[secondRegion], std::move(secondArc), first, last);
            ++incidence[edge.first];
            ++incidence[edge.last];
            keptLength += length;
        }
        const bool complete = discardedLength <= kCoordinateRoundoff && keptLength > kCoordinateRoundoff;
        if (complete) {
            const auto &claims = topology.corridorClaims.constFind(key).value();
            masks[firstRegion].insert(masks[firstRegion].end(), claims[0].begin(), claims[0].end());
            masks[secondRegion].insert(masks[secondRegion].end(), claims[1].begin(), claims[1].end());
            incidence.clear();
        }
        for (auto node = incidence.cbegin(); node != incidence.cend(); ++node) {
            if (node.value() != 1) continue;
            const auto &endpoint = graph.nodes[node.key()];
            const auto point = input.restoration.map(endpoint.point);
            QJsonObject cut{{QStringLiteral("position"), QJsonArray{point.x(), point.y()}},
                            {QStringLiteral("gradient"), endpoint.gradient}};
            for (int side = 0; side < 2; ++side) {
                const auto &front = endpoint.fronts[side];
                const auto &boundary = topology.index.boundaries[front.boundary];
                const double parameter = boundary.firstParameter + (boundary.lastParameter - boundary.firstParameter) * front.parameter;
                cut.insert(side == 0 ? QStringLiteral("firstSpan") : QStringLiteral("secondSpan"), boundary.span);
                cut.insert(side == 0 ? QStringLiteral("firstParameter") : QStringLiteral("secondParameter"), parameter);
                if (boundary.span >= 0 && parameter > kCoordinateRoundoff && parameter < 1.0 - kCoordinateRoundoff) ++interiorCuts;
            }
            if (cuts.size() < kMaximumCorridorCuts) cuts.append(cut);
            ++totalCuts;
        }
        profiles.append(QJsonObject{{QStringLiteral("firstContour"), firstRegion}, {QStringLiteral("secondContour"), secondRegion},
                                     {QStringLiteral("keptLength"), keptLength * input.extent},
                                     {QStringLiteral("discardedLength"), discardedLength * input.extent},
                                     {QStringLiteral("maximumGradient"), maximumGradient}, {QStringLiteral("rejectedBasins"), rejectedBasins},
                                     {QStringLiteral("retainedEntireCorridor"), complete},
                                     {QStringLiteral("projectionBreaks"), projectionBreaks}, {QStringLiteral("cuts"), cuts}});
        rejectedLength += discardedLength;
    }
    for (auto &mask : masks) mask = Clipper2Lib::Union(mask, Clipper2Lib::FillRule::NonZero);
    report.set(QStringLiteral("corridorProfiles"), profiles);
    report.set(QStringLiteral("maximumAxialGradient"), kMaximumAxialGradient);
    report.set(QStringLiteral("discardedCorridorLength"), rejectedLength * input.extent);
    report.set(QStringLiteral("corridorCuts"), totalCuts);
    report.set(QStringLiteral("interiorSourceArcCuts"), interiorCuts);

    return masks;
}

QVector<RegionPaths> growRegions(const Triangulation &triangulation, const QVector<TriangleLabel> &labels,
                                PartitionInput &input, PartitionReport &report) {
    auto topology = growthTopology(triangulation, labels, input);
    ArrivalField arrivals(topology);
    QVector<RegionPaths> claims(input.material.size());

    assignNeighbors(triangulation, labels, topology, arrivals, input, report);
    for (int index = 0; index < labels.size(); ++index) {
        if (!labels[index].coverage.isEmpty()) continue;
        const auto &vertices = triangulation.triangles[index].vertices;
        growTriangle({position(triangulation, vertices[0]), position(triangulation, vertices[1]), position(triangulation, vertices[2])},
                     topology, arrivals, input, claims, 0);
    }
    for (const auto &point : topology.refinements) inputVertex(input, point, false);
    const auto refined = triangulate(input);
    topology.collecting = false;
    for (int index = 0; index < refined.triangles.size(); ++index) {
        const QPointF center = triangleCenter(refined, index);
        bool occupied = false;
        for (const auto &path : input.material) occupied |= path.contains(center);
        if (occupied) continue;
        const auto &vertices = refined.triangles[index].vertices;
        growTriangle({position(refined, vertices[0]), position(refined, vertices[1]), position(refined, vertices[2])},
                     topology, arrivals, input, claims, 0);
    }
    report.set(QStringLiteral("arrivalVertices"), qint64(refined.vertices.size()));
    const auto masks = collisionCorridors(topology, arrivals, input, report);
    for (int region = 0; region < claims.size(); ++region) {
        const auto additions = Clipper2Lib::Union(claims[region], Clipper2Lib::FillRule::NonZero);
        const auto supported = Clipper2Lib::Intersect(additions, masks[region], Clipper2Lib::FillRule::NonZero);
        const auto internal = Clipper2Lib::Union(topology.internalClaims[region], Clipper2Lib::FillRule::NonZero);
        claims[region] = regionUnion(topology.regions[region].material, regionUnion(supported, internal));
    }
    QVector<double> detached(claims.size(), 0.0);
    for (int region = 0; region < claims.size(); ++region) {
        claims[region] = rootedGrowth(claims[region], topology.regions[region].material, input.flatness, detached[region]);
    }
    QVector<double> collapsed(claims.size(), 0.0);
    const auto occupied = claims;
    for (int region = 0; region < claims.size(); ++region) {
        RegionPaths retained;
        for (const auto &path : claims[region]) {
            bool collapse = Clipper2Lib::Area(path) < 0.0;
            if (collapse) {
                RegionPaths hole{path};
                hole = Clipper2Lib::Union(hole, Clipper2Lib::FillRule::EvenOdd);
                QPolygonF polygon;
                for (const auto &point : path) polygon.push_back(QPointF(point.x / kClipperScale, point.y / kClipperScale));
                const auto bounds = polygon.boundingRect();
                collapse = std::min(bounds.width(), bounds.height()) * 0.5 <= topology.regions[region].time;
                for (const auto &original : topology.holes) {
                    if (original.region == region && regionOverlapArea(hole, regionPaths(original.path, input.flatness)) > 0.0) collapse = false;
                }
                for (int other = 0; other < claims.size(); ++other) {
                    if (other != region && regionOverlapArea(hole, occupied[other]) > 0.0) collapse = false;
                }
                if (collapse) collapsed[region] += std::abs(Clipper2Lib::Area(hole)) / (kClipperScale * kClipperScale);
            }
            if (!collapse) retained.push_back(path);
        }
        claims[region] = Clipper2Lib::Union(retained, Clipper2Lib::FillRule::NonZero);
    }
    QJsonArray growth;
    for (int region = 0; region < claims.size(); ++region) {
        const double before = std::abs(Clipper2Lib::Area(topology.regions[region].material)) / (kClipperScale * kClipperScale);
        const double after = std::abs(Clipper2Lib::Area(claims[region])) / (kClipperScale * kClipperScale);
        const QPointF center = input.restoration.map(topology.regions[region].center);
        growth.append(QJsonObject{{QStringLiteral("contour"), region}, {QStringLiteral("beforeArea"), before * input.extent * input.extent},
                                  {QStringLiteral("afterArea"), after * input.extent * input.extent},
                                  {QStringLiteral("growthArea"), (after - before) * input.extent * input.extent},
                                  {QStringLiteral("selfCollapseArea"), collapsed[region] * input.extent * input.extent},
                                  {QStringLiteral("discardedDetachedArea"), detached[region] * input.extent * input.extent},
                                  {QStringLiteral("growthTime"), topology.regions[region].time * input.extent},
                                  {QStringLiteral("center"), QJsonArray{center.x(), center.y()}}});
        if (after > before + kCoordinateRoundoff) {
            report.decision(QStringLiteral("balanced_gap_growth"), {{QStringLiteral("contour"), region},
                             {QStringLiteral("area"), (after - before) * input.extent * input.extent}});
        }
    }
    report.set(QStringLiteral("regionGrowth"), growth);
    report.set(QStringLiteral("ignoredInternalExternalCollisions"), true);
    report.set(QStringLiteral("selfCollisionLimited"), true);

    return claims;
}

Triangulation growthSubdivision(PartitionInput &input, const QVector<RegionPaths> &regions) {
    QVector<QPair<int, int>> constraints;
    QSet<quint64> inserted;

    for (const auto &region : regions) {
        for (const auto &loop : region) {
            for (int point = 0; point < loop.size(); ++point) {
                const auto &first = loop[point];
                const auto &second = loop[(point + 1) % loop.size()];
                const int start = inputVertex(input, QPointF(first.x / kClipperScale, first.y / kClipperScale), false);
                const int end = inputVertex(input, QPointF(second.x / kClipperScale, second.y / kClipperScale), false);
                const auto key = edgeKey(start, end);
                if (start == end || inserted.contains(key) || input.originals.contains(key)) continue;
                inserted.insert(key);
                constraints.push_back({start, end});
            }
        }
    }

    return triangulate(input, constraints);
}

QVector<TriangleLabel> growthLabels(const Triangulation &triangulation, const PartitionInput &input,
                                   const QVector<RegionPaths> &regions) {
    QVector<TriangleLabel> labels(triangulation.triangles.size());
    QVector<QPainterPath> material;

    for (const auto &region : regions) material.push_back(painterRegion(region));
    for (int index = 0; index < labels.size(); ++index) {
        const QPointF center = triangleCenter(triangulation, index);
        auto &label = labels[index];
        for (int region = 0; region < regions.size(); ++region) {
            if (input.material[region].contains(center)) label.coverage.push_back(region);
            if (label.owner < 0 && material[region].contains(center)) label.owner = region;
        }
        if (!label.coverage.isEmpty()) label.owner = label.coverage.front();
        label.gap = label.coverage.isEmpty() && label.owner >= 0;
    }

    return labels;
}

struct VoidBoundary {
    QPointF first;
    QPointF last;
    int owner = -1;
};

struct InteriorVoid {
    QVector<int> triangles;
    QVector<VoidBoundary> boundary;
    QSet<int> owners;
    bool enclosed = true;
    bool sourceHole = false;
    bool growing = false;
    double area = 0.0;
    double perimeter = 0.0;
};

QVector<InteriorVoid> interiorVoids(const Triangulation &triangulation, const PartitionInput &input,
                                   const QVector<TriangleLabel> &labels) {
    const auto sources = constraintSources(triangulation, input);
    QVector<InteriorVoid> voids;
    QSet<int> visited;

    for (int seed = 0; seed < labels.size(); ++seed) {
        if (labels[seed].owner >= 0 || visited.contains(seed)) continue;
        InteriorVoid component;
        component.triangles.push_back(seed);
        visited.insert(seed);
        for (int next = 0; next < component.triangles.size(); ++next) {
            const int index = component.triangles[next];
            const auto &triangle = triangulation.triangles[index];
            component.area += triangleArea(triangulation, index);
            if (!component.sourceHole) for (const auto &hole : input.sourceHoles) {
                if (hole.contains(triangleCenter(triangulation, index))) {
                    component.sourceHole = true;
                    break;
                }
            }
            if (!component.growing) for (const auto &completed : input.completedVoids) {
                if (completed.contains(triangleCenter(triangulation, index))) {
                    component.growing = true;
                    break;
                }
            }
            for (int edge = 0; edge < 3; ++edge) {
                const auto neighbor = triangle.neighbors[edge];
                if (neighbor == CDT::noNeighbor) {
                    component.enclosed = false;
                    continue;
                }
                if (labels[neighbor].owner < 0) {
                    if (!visited.contains(int(neighbor))) {
                        visited.insert(int(neighbor));
                        component.triangles.push_back(int(neighbor));
                    }
                    continue;
                }
                component.owners.insert(labels[neighbor].owner);
                component.growing |= labels[neighbor].gap;
                const int start = triangle.vertices[edge];
                const int end = triangle.vertices[(edge + 1) % 3];
                const QPointF last = position(triangulation, end);
                const QPointF first = position(triangulation, start);
                component.boundary.push_back({first, last, labels[neighbor].owner});
                component.perimeter += magnitude(last - first);
                for (const int reference : sources.value(edgeKey(start, end))) {
                    component.sourceHole |= input.spans[input.constraints[reference].span].cutout >= 0;
                }
            }
        }
        if (component.enclosed && !component.owners.isEmpty()) voids.push_back(std::move(component));
    }

    return voids;
}

class VoidWavefront {
public:
    VoidWavefront(const InteriorVoid &component, const Triangulation &triangulation, PartitionInput &input) {
        const auto vertex = [&](const QPointF &point) {
            const int global = inputVertex(input, point, false);
            const auto found = vertices_.constFind(global);
            if (found != vertices_.cend()) return found.value();
            const int index = static_cast<int>(points_.size());
            points_.push_back(input.positions[global]);
            incidence_.push_back({});
            vertices_.insert(global, index);

            return index;
        };
        for (const int index : component.triangles) {
            const auto &triangle = triangulation.triangles[index];
            const std::array<QPointF, 3> points{position(triangulation, triangle.vertices[0]), position(triangulation, triangle.vertices[1]),
                                              position(triangulation, triangle.vertices[2])};
            const std::array<int, 6> ring{vertex(points[0]), vertex((points[0] + points[1]) * 0.5), vertex(points[1]),
                                         vertex((points[1] + points[2]) * 0.5), vertex(points[2]), vertex((points[2] + points[0]) * 0.5)};
            const int center = vertex((points[0] + points[1] + points[2]) / 3.0);
            for (int edge = 0; edge < ring.size(); ++edge) {
                const int first = ring[edge];
                const int last = ring[(edge + 1) % ring.size()];
                if (first == last || first == center || last == center) continue;
                if (cross(points_[last] - points_[first], points_[center] - points_[first]) == 0.0) continue;
                const int face = static_cast<int>(faces_.size());
                faces_.push_back({first, last, center});
                for (const int point : faces_.back()) incidence_[point].push_back(face);
            }
        }
        for (const auto &edge : component.boundary) {
            auto &seeds = seeds_[edge.owner];
            seeds.insert(vertex(edge.first));
            seeds.insert(vertex((edge.first + edge.last) * 0.5));
            seeds.insert(vertex(edge.last));
        }
    }

    QVector<double> distances(int owner, double tolerance, qint64 &updates) const {
        using Arrival = std::pair<double, int>;
        std::priority_queue<Arrival, std::vector<Arrival>, std::greater<Arrival>> pending;
        QVector<double> field(points_.size(), std::numeric_limits<double>::infinity());

        for (const int seed : seeds_[owner]) {
            field[seed] = 0.0;
            pending.push({0.0, seed});
        }
        while (!pending.empty()) {
            const auto [time, vertex] = pending.top();
            pending.pop();
            if (time > field[vertex] + tolerance) continue;
            for (const int face : incidence_[vertex]) {
                const auto &triangle = faces_[face];
                for (int corner = 0; corner < triangle.size(); ++corner) {
                    const int point = triangle[corner];
                    const int first = triangle[(corner + 1) % triangle.size()];
                    const int last = triangle[(corner + 2) % triangle.size()];
                    const double arrival = edgeArrival(points_[point], points_[first], points_[last], field[first], field[last]);
                    if (arrival + tolerance >= field[point]) continue;
                    field[point] = arrival;
                    pending.push({arrival, point});
                    ++updates;
                }
            }
        }

        return field;
    }

    void partition(const QVector<int> &owners, QVector<RegionPaths> &patches, double tolerance, qint64 &updates, double &maximum) const {
        QVector<QVector<double>> fields;

        for (const int owner : owners) fields.push_back(distances(owner, tolerance, updates));
        for (const auto &face : faces_) {
            const std::array<QPointF, 3> triangle{points_[face[0]], points_[face[1]], points_[face[2]]};
            const QPolygonF polygon{triangle[0], triangle[1], triangle[2]};
            for (int owner = 0; owner < owners.size(); ++owner) {
                auto cell = polygon;
                for (int other = 0; other < owners.size() && !cell.isEmpty(); ++other) {
                    if (other == owner) continue;
                    std::array<double, 3> values;
                    bool equal = true;
                    for (int point = 0; point < triangle.size(); ++point) {
                        const double own = fields[owner][face[point]];
                        const double competing = fields[other][face[point]];
                        if (!std::isfinite(own) || !std::isfinite(competing)) throw std::runtime_error("Interior wavefronts must reach every enclosed cell");
                        values[point] = own - competing;
                        equal &= std::abs(values[point]) <= kCoordinateRoundoff;
                    }
                    if (equal) {
                        if (other < owner) cell.clear();
                    } else cell = clipArrival(cell, triangle, values);
                }
                if (cell.size() >= 3) patches[owners[owner]].push_back(integerPolygon(cell));
            }
            for (const int point : face) {
                double first = std::numeric_limits<double>::infinity();
                for (const auto &field : fields) first = std::min(first, field[point]);
                maximum = std::max(maximum, first);
            }
        }
    }

private:
    QVector<QPointF> points_;
    QVector<std::array<int, 3>> faces_;
    QVector<QVector<int>> incidence_;
    QHash<int, int> vertices_;
    QHash<int, QSet<int>> seeds_;

    static double edgeArrival(const QPointF &point, const QPointF &first, const QPointF &last, double start, double end) {
        const QPointF direction = last - first;
        const QPointF relative = point - first;
        const double length = magnitude(direction);
        double arrival = std::min(start + magnitude(relative), end + magnitude(point - last));

        if (!std::isfinite(start) || !std::isfinite(end) || length <= kCoordinateRoundoff) return arrival;
        const double slope = (end - start) / length;
        if (std::abs(slope) >= 1.0) return arrival;
        const double along = QPointF::dotProduct(relative, direction) / length;
        const double height = std::abs(cross(relative, direction)) / length;
        const double position = std::clamp(along - slope * height / std::sqrt(1.0 - slope * slope), 0.0, length);
        arrival = std::min(arrival, start + slope * position + std::hypot(along - position, height));

        return arrival;
    }
};

bool fillInteriorVoids(const Triangulation &triangulation, PartitionInput &input, QVector<TriangleLabel> &labels,
                       QVector<RegionPaths> &regions, PartitionReport &report) {
    const auto voids = interiorVoids(triangulation, input, labels);
    QVector<RegionPaths> patches(regions.size());
    int filled = 0;
    int protectedCount = 0;
    bool subdivide = false;
    qint64 updates = 0;
    double area = 0.0;
    double maximum = 0.0;

    for (const auto &component : voids) {
        if (component.sourceHole) {
            ++protectedCount;
            continue;
        }
        const bool numerical = component.owners.size() == 1 && component.area * 2.0 <= component.perimeter * input.flatness;
        if (!component.growing && !numerical) continue;
        QVector<int> owners(component.owners.begin(), component.owners.end());
        std::sort(owners.begin(), owners.end());
        if (owners.size() == 1) {
            for (const int index : component.triangles) {
                const auto &triangle = triangulation.triangles[index];
                labels[index].owner = owners.front();
                labels[index].gap = true;
                patches[owners.front()].push_back(integerPolygon({position(triangulation, triangle.vertices[0]), position(triangulation, triangle.vertices[1]),
                                                                 position(triangulation, triangle.vertices[2])}));
            }
        } else {
            const VoidWavefront field(component, triangulation, input);
            field.partition(owners, patches, input.flatness, updates, maximum);
            subdivide = true;
        }
        report.decision(QStringLiteral("enclosed_gap_partition"), {{QStringLiteral("regions"), jsonLabels(owners)},
            {QStringLiteral("area"), component.area * input.extent * input.extent}, {QStringLiteral("triangles"), component.triangles.size()}});
        RegionPaths completed;
        for (const int index : component.triangles) {
            const auto &triangle = triangulation.triangles[index];
            completed.push_back(integerPolygon({position(triangulation, triangle.vertices[0]), position(triangulation, triangle.vertices[1]),
                                                position(triangulation, triangle.vertices[2])}));
        }
        input.completedVoids.push_back(painterRegion(Clipper2Lib::Union(completed, Clipper2Lib::FillRule::NonZero)));
        area += component.area;
        ++filled;
    }
    if (subdivide) for (int region = 0; region < regions.size(); ++region) {
        regions[region] = regionUnion(regions[region], Clipper2Lib::Union(patches[region], Clipper2Lib::FillRule::NonZero));
    }
    report.set(QStringLiteral("filledInteriorVoids"), filled);
    report.set(QStringLiteral("protectedInteriorVoids"), protectedCount);
    report.set(QStringLiteral("interiorFillArea"), area * input.extent * input.extent);
    report.set(QStringLiteral("interiorMaximumTravel"), maximum * input.extent);
    report.set(QStringLiteral("interiorArrivalUpdates"), updates);

    return subdivide;
}

struct OwnershipComponent {
    QVector<int> triangles;
    int owner = -1;
    double sourceArea = 0.0;
    double perimeter = 0.0;
};

QVector<OwnershipComponent> ownershipComponents(const Triangulation &triangulation, const QVector<TriangleLabel> &labels) {
    QVector<OwnershipComponent> components;
    QSet<int> visited;

    for (int seed = 0; seed < labels.size(); ++seed) {
        if (labels[seed].owner < 0 || visited.contains(seed)) continue;
        OwnershipComponent component;
        component.owner = labels[seed].owner;
        component.triangles.push_back(seed);
        visited.insert(seed);
        for (int next = 0; next < component.triangles.size(); ++next) {
            const int index = component.triangles[next];
            const auto &triangle = triangulation.triangles[index];
            if (!labels[index].coverage.isEmpty() && labels[index].coverage.front() == component.owner) {
                component.sourceArea += triangleArea(triangulation, index);
            }
            for (int edge = 0; edge < 3; ++edge) {
                const auto neighbor = triangle.neighbors[edge];
                if (neighbor != CDT::noNeighbor && labels[neighbor].owner == component.owner) {
                    if (!visited.contains(int(neighbor))) {
                        visited.insert(int(neighbor));
                        component.triangles.push_back(int(neighbor));
                    }
                } else {
                    component.perimeter += magnitude(position(triangulation, triangle.vertices[edge])
                        - position(triangulation, triangle.vertices[(edge + 1) % 3]));
                }
            }
        }
        components.push_back(std::move(component));
    }

    return components;
}

void propagateOwnership(const Triangulation &triangulation, const QVector<bool> &repair,
                         QVector<TriangleLabel> &labels, double &discarded) {
    using Arrival = std::tuple<double, int, int>;
    std::priority_queue<Arrival, std::vector<Arrival>, std::greater<Arrival>> pending;
    QVector<double> distances(labels.size(), std::numeric_limits<double>::infinity());
    QVector<int> owners(labels.size(), -1);
    const auto offer = [&](int triangle, int owner, double time) {
        if (time > distances[triangle] + kCoordinateRoundoff
            || (std::abs(time - distances[triangle]) <= kCoordinateRoundoff && owners[triangle] >= 0 && owner >= owners[triangle])) return;
        distances[triangle] = time;
        owners[triangle] = owner;
        pending.push({time, owner, triangle});
    };

    for (int index = 0; index < labels.size(); ++index) if (repair[index]) labels[index].owner = -1;
    for (int index = 0; index < labels.size(); ++index) {
        if (!repair[index]) continue;
        const auto &triangle = triangulation.triangles[index];
        const QPointF center = triangleCenter(triangulation, index);
        for (int edge = 0; edge < 3; ++edge) {
            const auto neighbor = triangle.neighbors[edge];
            if (neighbor == CDT::noNeighbor || repair[neighbor] || labels[neighbor].owner < 0) continue;
            const QPointF midpoint = (position(triangulation, triangle.vertices[edge]) + position(triangulation, triangle.vertices[(edge + 1) % 3])) * 0.5;
            offer(index, labels[neighbor].owner, magnitude(center - midpoint));
        }
    }
    while (!pending.empty()) {
        const auto [time, owner, index] = pending.top();
        pending.pop();
        if (labels[index].owner >= 0 || time > distances[index] + kCoordinateRoundoff || owner != owners[index]) continue;
        labels[index].owner = owner;
        labels[index].gap = labels[index].coverage.isEmpty();
        const QPointF center = triangleCenter(triangulation, index);
        for (const auto neighbor : triangulation.triangles[index].neighbors) {
            if (neighbor == CDT::noNeighbor || !repair[neighbor] || labels[neighbor].owner >= 0) continue;
            offer(int(neighbor), owner, time + magnitude(triangleCenter(triangulation, int(neighbor)) - center));
        }
    }
    for (int index = 0; index < labels.size(); ++index) {
        if (repair[index] && labels[index].owner < 0) discarded += triangleArea(triangulation, index);
    }
}

void stabilizePartition(const Triangulation &triangulation, const PartitionInput &input,
                         QVector<TriangleLabel> &labels, PartitionReport &report) {
    const auto components = ownershipComponents(triangulation, labels);
    const auto voids = interiorVoids(triangulation, input, labels);
    QVector<int> primary(input.material.size(), -1);
    QVector<bool> repair(labels.size(), false);
    int artifacts = 0;
    int residuals = 0;
    double repairedArea = 0.0;
    double discarded = 0.0;

    for (int index = 0; index < components.size(); ++index) {
        const auto &component = components[index];
        if (primary[component.owner] < 0 || component.sourceArea > components[primary[component.owner]].sourceArea) primary[component.owner] = index;
    }
    for (int index = 0; index < components.size(); ++index) {
        const auto &component = components[index];
        if (primary[component.owner] == index || component.sourceArea * 2.0 > component.perimeter * input.flatness) continue;
        for (const int triangle : component.triangles) {
            repair[triangle] = true;
            repairedArea += triangleArea(triangulation, triangle);
        }
        ++artifacts;
    }
    for (const auto &component : voids) {
        if (component.sourceHole || (!component.growing && component.area * 2.0 > component.perimeter * input.flatness)) continue;
        for (const int triangle : component.triangles) repair[triangle] = true;
        ++residuals;
    }
    propagateOwnership(triangulation, repair, labels, discarded);
    report.set(QStringLiteral("reassignedArtifactComponents"), artifacts);
    report.set(QStringLiteral("reassignedArtifactArea"), repairedArea * input.extent * input.extent);
    report.set(QStringLiteral("resolvedResidualVoids"), residuals);
    report.set(QStringLiteral("discardedUnrootedArea"), discarded * input.extent * input.extent);
}

void validateSourceHoles(const MeshData &mesh, const PartitionInput &input) {
    for (int index = 0; index < mesh.contours.size(); ++index) {
        const auto data = mesh.contourData(index);
        const auto exterior = regionPaths(input.normalization.map(penPath(data.points, true)), input.flatness);
        QVector<RegionPaths> holes;
        for (const auto &hole : data.cutouts) {
            const auto path = regionPaths(input.normalization.map(penPath(hole, true)), input.flatness);
            if (regionDifferenceArea(path, exterior) > input.areaTolerance) {
                throw std::runtime_error("Region holes must be contained by their exterior");
            }
            for (const auto &other : holes) {
                if (regionOverlapArea(path, other) > input.areaTolerance) throw std::runtime_error("Region holes must have disjoint interiors");
            }
            holes.push_back(path);
        }
    }
}

struct PartitionValidation {
    QVector<RegionPaths> expected;
    QVector<RegionPaths> exclusive;
    RegionPaths combined;
};

PartitionValidation partitionValidation(const Triangulation &triangulation, const QVector<TriangleLabel> &labels,
                                        const PartitionInput &input) {
    PartitionValidation reference;
    QVector<RegionPaths> sources;
    QVector<RegionPaths> prefix(input.material.size() + 1);
    QVector<RegionPaths> suffix(input.material.size() + 1);

    reference.expected.resize(input.material.size());
    for (const auto &path : input.material) {
        sources.push_back(regionPaths(path, input.flatness));
        prefix[sources.size()] = regionUnion(prefix[sources.size() - 1], sources.back());
    }
    for (int index = static_cast<int>(sources.size()) - 1; index >= 0; --index) suffix[index] = regionUnion(sources[index], suffix[index + 1]);
    reference.combined = prefix.back();
    for (int index = 0; index < sources.size(); ++index) {
        reference.exclusive.push_back(Clipper2Lib::Difference(sources[index], regionUnion(prefix[index], suffix[index + 1]), Clipper2Lib::FillRule::NonZero));
    }
    for (int index = 0; index < labels.size(); ++index) {
        if (labels[index].owner < 0) continue;
        const auto &triangle = triangulation.triangles[index];
        reference.expected[labels[index].owner].push_back(integerPolygon({position(triangulation, triangle.vertices[0]),
            position(triangulation, triangle.vertices[1]), position(triangulation, triangle.vertices[2])}));
    }
    for (auto &material : reference.expected) material = Clipper2Lib::Union(material, Clipper2Lib::FillRule::NonZero);

    return reference;
}

bool validatePartition(const PartitionResult &result, const PartitionValidation &reference,
                         const PartitionInput &input, PartitionReport &report, QSet<int> *unsafe = nullptr) {
    const auto &mesh = result.mesh;
    QVector<RegionPaths> material;
    QVector<RegionPaths> assigned(input.material.size());
    RegionPaths after;
    double overlap = 0.0;
    double maximumMismatch = 0.0;

    for (int index = 0; index < mesh.contours.size(); ++index) {
        const auto data = mesh.contourData(index);
        auto normalized = data.points;
        for (auto &point : normalized) transformPenPoint(point, input.normalization);
        const auto validation = validateLoop(normalized, input.flatness);
        if (!validation.valid()) {
            if (unsafe != nullptr) unsafe->insert(result.sourceLabels[index]);
            QJsonArray points;
            for (const auto &point : normalized) {
                points.append(QJsonArray{point.position.x(), point.position.y(), point.incoming.x(), point.incoming.y(),
                                          point.outgoing.x(), point.outgoing.y()});
            }
            report.decision(QStringLiteral("invalid_reconstructed_boundary"), {{QStringLiteral("contour"), index},
                             {QStringLiteral("error"), validation.error}, {QStringLiteral("crossings"), validation.crossings.size()},
                             {QStringLiteral("normalizedPoints"), points}});
            return false;
        }
        const auto exterior = regionPaths(penPath(normalized, true), input.flatness);
        for (auto hole : data.cutouts) {
            for (auto &point : hole) transformPenPoint(point, input.normalization);
            const auto validation = validateLoop(hole, input.flatness);
            const double outside = regionDifferenceArea(regionPaths(penPath(hole, true), input.flatness), exterior);
            const double outsideTolerance = input.maximumRegionMismatchFraction > 0.0 ? input.flatness * input.flatness : input.areaTolerance;
            if (!validation.valid() || outside > outsideTolerance) {
                if (unsafe != nullptr) unsafe->insert(result.sourceLabels[index]);
                QJsonArray points;
                for (const auto &point : hole) points.append(QJsonArray{point.position.x(), point.position.y(), point.incoming.x(), point.incoming.y(),
                                                                       point.outgoing.x(), point.outgoing.y()});
                report.decision(QStringLiteral("invalid_reconstructed_hole"), {{QStringLiteral("contour"), index},
                                 {QStringLiteral("error"), validation.error}, {QStringLiteral("crossings"), validation.crossings.size()},
                                 {QStringLiteral("outsideArea"), outside * input.extent * input.extent}, {QStringLiteral("normalizedPoints"), points}});
                return false;
            }
        }
        const auto path = regionPaths(contourMaterial(mesh, index, input.normalization), input.flatness);
        for (const auto &other : material) overlap += regionOverlapArea(other, path);
        material.push_back(path);
        assigned[result.sourceLabels[index]] = regionUnion(assigned[result.sourceLabels[index]], path);
        after = regionUnion(after, path);
    }
    for (int contour = 0; contour < input.material.size(); ++contour) {
        const auto &expectedMaterial = reference.expected[contour];
        const double expectedArea = std::abs(Clipper2Lib::Area(expectedMaterial)) / (kClipperScale * kClipperScale);
        const double mismatch = regionDifferenceArea(expectedMaterial, assigned[contour])
            + regionDifferenceArea(assigned[contour], expectedMaterial);
        if (expectedArea > 0.0) maximumMismatch = std::max(maximumMismatch, mismatch / expectedArea);
        const double regionTolerance = input.maximumRegionMismatchFraction > 0.0
            ? std::min(input.areaTolerance, std::max(input.flatness * input.flatness,
                expectedArea * input.maximumRegionMismatchFraction))
            : input.areaTolerance;
        if (mismatch > regionTolerance) {
            if (unsafe != nullptr) unsafe->insert(contour);
            report.decision(QStringLiteral("assigned_material_mismatch"), {{QStringLiteral("contour"), contour},
                             {QStringLiteral("area"), mismatch * input.extent * input.extent}});
            return false;
        }
        const double loss = regionDifferenceArea(reference.exclusive[contour], assigned[contour]);
        if (loss > regionTolerance) {
            if (unsafe != nullptr) unsafe->insert(contour);
            report.decision(QStringLiteral("exclusive_material_lost"), {{QStringLiteral("contour"), contour},
                             {QStringLiteral("area"), loss * input.extent * input.extent}});
            return false;
        }
    }
    const double lost = regionDifferenceArea(reference.combined, after);
    const double sourceArea = std::abs(Clipper2Lib::Area(reference.combined)) / (kClipperScale * kClipperScale);
    report.set(QStringLiteral("lostUnionArea"), lost * input.extent * input.extent);
    report.set(QStringLiteral("lostUnionFraction"), sourceArea > 0.0 ? lost / sourceArea : 0.0);
    report.set(QStringLiteral("maximumRegionMismatchFraction"), maximumMismatch);
    report.set(QStringLiteral("remainingOverlapArea"), overlap * input.extent * input.extent);
    if (lost > input.areaTolerance || overlap > input.areaTolerance) return false;

    if (!mesh.connected()) {
        QVector<QSet<int>> contours;
        QSet<int> visited;
        QJsonArray components;
        for (const auto &contour : mesh.contours) {
            QSet<int> vertices;
            for (const auto &point : contour.points) vertices.insert(point.vertex);
            for (const auto &hole : contour.cutouts) for (const auto &point : hole) vertices.insert(point.vertex);
            contours.push_back(vertices);
        }
        for (int seed = 0; seed < contours.size(); ++seed) {
            if (visited.contains(seed)) continue;
            QVector<int> group{seed};
            visited.insert(seed);
            for (int next = 0; next < group.size(); ++next) {
                for (int other = 0; other < contours.size(); ++other) {
                    if (visited.contains(other) || !contours[group[next]].intersects(contours[other])) continue;
                    visited.insert(other);
                    group.push_back(other);
                }
            }
            QVector<int> sources;
            for (const int contour : group) if (!sources.contains(result.sourceLabels[contour])) sources.push_back(result.sourceLabels[contour]);
            std::sort(sources.begin(), sources.end());
            components.append(QJsonObject{{QStringLiteral("sources"), jsonLabels(sources)}, {QStringLiteral("pieces"), jsonLabels(group)}});
        }
        report.decision(QStringLiteral("disconnected_reconstruction"), {{QStringLiteral("components"), components}});
        return false;
    }

    return mesh.valid();
}

void recordSubdivision(const Triangulation &triangulation, const QVector<TriangleLabel> &labels, const PartitionInput &input,
                        PartitionReport &report) {
    QJsonArray vertices;
    QJsonArray triangles;

    for (const auto &vertex : triangulation.vertices) {
        const auto point = input.restoration.map(QPointF(vertex.x, vertex.y));
        vertices.append(QJsonArray{point.x(), point.y()});
    }
    for (int index = 0; index < labels.size() && index < kMaximumSubdivisionRecords; ++index) {
        const auto &triangle = triangulation.triangles[index];
        triangles.append(QJsonObject{{QStringLiteral("vertices"), QJsonArray{int(triangle.vertices[0]), int(triangle.vertices[1]), int(triangle.vertices[2])}},
                                      {QStringLiteral("coverage"), jsonLabels(labels[index].coverage)},
                                      {QStringLiteral("owner"), labels[index].owner}, {QStringLiteral("gapEligible"), labels[index].gap}});
    }
    report.set(QStringLiteral("subdivision"), QJsonObject{{QStringLiteral("vertices"), vertices}, {QStringLiteral("triangles"), triangles},
                                                         {QStringLiteral("omittedTriangles"), labels.size() - triangles.size()}});
}

} // namespace

bool connectMeshRegions(MeshData &mesh, QString *error, QJsonObject *diagnostics) {
    PartitionReport report(mesh);
    QString stage = QStringLiteral("region_validation");
    const auto finish = [&](bool accepted, const QString &reason) {
        if (!accepted && error != nullptr) *error = reason;
        if (diagnostics != nullptr) *diagnostics = report.finish(accepted, stage, reason);

        return accepted;
    };

    try {
        auto input = partitionInput(mesh);
        validateSourceHoles(mesh, input);
        report.set(QStringLiteral("extent"), input.extent);
        report.set(QStringLiteral("flatness"), input.flatness * input.extent);
        report.set(QStringLiteral("coordinateTolerance"), kCoordinateRoundoff * input.extent);
        report.set(QStringLiteral("areaTolerance"), input.areaTolerance * input.extent * input.extent);
        report.set(QStringLiteral("constraintCount"), input.constraints.size());
        stage = QStringLiteral("constrained_triangulation");
        auto triangulation = triangulate(input);
        report.set(QStringLiteral("subdivisionVertices"), qint64(triangulation.vertices.size()));
        report.set(QStringLiteral("triangleCount"), qint64(triangulation.triangles.size()));
        stage = QStringLiteral("coverage_labelling");
        auto labels = labelTriangles(triangulation, input, report);
        stage = QStringLiteral("region_assignment");
        assignOverlaps(triangulation, input, labels, report);
        stage = QStringLiteral("external_wavefront_growth");
        auto regions = growRegions(triangulation, labels, input, report);
        stage = QStringLiteral("collision_subdivision");
        triangulation = growthSubdivision(input, regions);
        labels = growthLabels(triangulation, input, regions);
        stage = QStringLiteral("interior_wavefront_partition");
        if (fillInteriorVoids(triangulation, input, labels, regions, report)) {
            triangulation = growthSubdivision(input, regions);
            labels = growthLabels(triangulation, input, regions);
        }
        stabilizePartition(triangulation, input, labels, report);
        report.set(QStringLiteral("subdivisionVertices"), qint64(triangulation.vertices.size()));
        report.set(QStringLiteral("triangleCount"), qint64(triangulation.triangles.size()));
        recordSubdivision(triangulation, labels, input, report);
        stage = QStringLiteral("partition_reconstruction");
        QSet<int> polygonal;
        auto rebuilt = reconstructPartition(mesh, triangulation, input, labels, polygonal);
        stage = QStringLiteral("partition_validation");
        const auto reference = partitionValidation(triangulation, labels, input);
        while (true) {
            QSet<int> unsafe;
            if (validatePartition(rebuilt, reference, input, report, &unsafe)) break;
            unsafe.subtract(polygonal);
            if (unsafe.isEmpty()) return finish(false, QStringLiteral("Region reconstruction must retain material and a valid connected partition"));
            polygonal.unite(unsafe);
            for (const int region : unsafe) report.decision(QStringLiteral("preserved_subdivision_boundary"), {{QStringLiteral("contour"), region}});
            rebuilt = reconstructPartition(mesh, triangulation, input, labels, polygonal);
        }
        stage = QStringLiteral("shared_boundary_simplification");
        QSet<QPair<double, double>> authored;
        QSet<int> anchors;
        QJsonObject simplification;
        QJsonObject simplifiedValidation;
        QJsonArray fittingChecks;
        double perimeter = 0.0;
        for (const auto &contour : mesh.contours) {
            const auto retainCorners = [&](const QVector<MeshPoint> &points) {
                for (const auto &point : points) if (point.kind == PenPointKind::Hard) {
                    const auto &position = mesh.vertices[point.vertex];
                    authored.insert({position.x(), position.y()});
                }
            };
            retainCorners(contour.points);
            for (const auto &hole : contour.cutouts) retainCorners(hole);
        }
        for (int vertex = 0; vertex < rebuilt.mesh.vertices.size(); ++vertex) {
            const auto &point = rebuilt.mesh.vertices[vertex];
            if (authored.contains({point.x(), point.y()})) anchors.insert(vertex);
        }
        for (const auto &contour : rebuilt.mesh.contours) {
            const auto measure = [&](const QVector<MeshPoint> &points) {
                for (int point = 0; point < points.size(); ++point) {
                    perimeter += magnitude(rebuilt.mesh.vertices[points[point].vertex]
                        - rebuilt.mesh.vertices[points[(point + 1) % points.size()].vertex]) / input.extent;
                }
            };
            measure(contour.points);
            for (const auto &hole : contour.cutouts) measure(hole);
        }
        const double overlapTolerance = std::max(input.flatness * input.flatness, kCoordinateRoundoff * perimeter) * input.extent * input.extent;
        auto fittingInput = input;
        fittingInput.areaTolerance = std::max(input.areaTolerance,
            std::abs(Clipper2Lib::Area(reference.combined)) / (kClipperScale * kClipperScale) * kCubicMaskMissingAreaFraction);
        fittingInput.maximumRegionMismatchFraction = kCubicMaskMismatchFraction;
        qsizetype validatedVertices = rebuilt.mesh.vertices.size();
        simplifyMeshBoundaries(rebuilt.mesh, 0.0, anchors,
            [&](const MeshData &candidate, QSet<int> &unsafeContours) {
                PartitionReport verification(candidate);
                const PartitionResult proposed{candidate, rebuilt.sourceLabels};
                QSet<int> unsafeSources;
                const bool valid = validatePartition(proposed, reference, fittingInput, verification, &unsafeSources);
                const auto detail = verification.finish(valid, QStringLiteral("simplification_validation"), {});
                fittingChecks.append(QJsonObject{{QStringLiteral("vertices"), candidate.vertices.size()},
                    {QStringLiteral("valid"), valid}, {QStringLiteral("decisions"), detail.value(QStringLiteral("decisionCounts"))},
                    {QStringLiteral("overlapArea"), detail.value(QStringLiteral("remainingOverlapArea"))},
                    {QStringLiteral("overlapTolerance"), overlapTolerance}});
                for (int contour = 0; contour < rebuilt.sourceLabels.size(); ++contour) {
                    if (unsafeSources.contains(rebuilt.sourceLabels[contour])) unsafeContours.insert(contour);
                }
                if (valid && detail.value(QStringLiteral("remainingOverlapArea")).toDouble() <= overlapTolerance) {
                    if (candidate.vertices.size() < validatedVertices) {
                        simplifiedValidation = detail;
                        validatedVertices = candidate.vertices.size();
                    }
                    return true;
                }

                return false;
            }, &simplification);
        simplification.insert(QStringLiteral("validations"), fittingChecks);
        report.set(QStringLiteral("boundarySimplification"), simplification);
        if (!simplifiedValidation.isEmpty()) {
            report.set(QStringLiteral("lostUnionArea"), simplifiedValidation.value(QStringLiteral("lostUnionArea")));
            report.set(QStringLiteral("lostUnionFraction"), simplifiedValidation.value(QStringLiteral("lostUnionFraction")));
            report.set(QStringLiteral("maximumRegionMismatchFraction"), simplifiedValidation.value(QStringLiteral("maximumRegionMismatchFraction")));
            report.set(QStringLiteral("remainingOverlapArea"), simplifiedValidation.value(QStringLiteral("remainingOverlapArea")));
        }
        QVector<int> polygonalLabels(polygonal.begin(), polygonal.end());
        std::sort(polygonalLabels.begin(), polygonalLabels.end());
        report.set(QStringLiteral("polygonalSourceLabels"), jsonLabels(polygonalLabels));
        report.set(QStringLiteral("outputVertices"), rebuilt.mesh.vertices.size());
        report.set(QStringLiteral("outputContours"), rebuilt.mesh.contours.size());
        report.set(QStringLiteral("outputSourceLabels"), jsonLabels(rebuilt.sourceLabels));
        mesh = std::move(rebuilt.mesh);
        stage = QStringLiteral("complete");

        return finish(true, {});
    } catch (const std::exception &exception) {
        return finish(false, QString::fromUtf8(exception.what()));
    }
}

} // namespace gui
