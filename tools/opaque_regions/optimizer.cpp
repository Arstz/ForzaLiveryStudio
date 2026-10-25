#include "optimizer.h"

#include "clipper2/clipper.h"
#include "fill_geometry.h"
#include "layer.h"
#include "matrix_math.h"
#include "vinyl_encoding.h"
#include "region_fill.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <numbers>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <memory>
#include <cstdio>
#include <array>
#include <QSvgRenderer>
#include <QCryptographicHash>
#include <QRegion>

namespace opaque {
namespace {

using Segment = gui::FillBoundarySegment;
using Loop = QVector<Segment>;
using Loops = QVector<Loop>;
using namespace Clipper2Lib;
constexpr int kBooleanPrecision = 8;

double booleanScale() {
    static const double scale = std::pow(std::numeric_limits<double>::radix,
        std::ilogb(std::pow(10, kBooleanPrecision)) + 1);
    return scale;
}

PathD trimPolygon(const PathD &path) {
    int error = 0;
    const auto integer = ScalePath<int64_t, double>(path, booleanScale(), error);
    const auto trimmed = TrimCollinear(integer);
    auto result = ScalePath<double, int64_t>(trimmed, 1.0 / booleanScale(), error);
    if (error) throw std::runtime_error("Boolean coordinate range exceeded");
    return result;
}

Loops spans(const QPainterPath &path) {
    Loops loops;
    QPointF current;
    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        const QPointF point(element.x, element.y);
        if (element.isMoveTo()) {
            loops.push_back({});
            current = point;
        } else if (element.isLineTo()) {
            loops.back().push_back({current, {}, point, false, {}});
            current = point;
        } else if (element.type == QPainterPath::CurveToElement) {
            const auto control2 = path.elementAt(++index);
            const auto end = path.elementAt(++index);
            loops.back().push_back({current, point, {end.x, end.y}, true,
                                   {control2.x, control2.y}});
            current = {end.x, end.y};
        }
    }
    for (auto &loop : loops) {
        if (!loop.isEmpty() && loop.back().end != loop.front().start)
            loop.push_back({loop.back().end, {}, loop.front().start, false, {}});
    }
    loops.erase(std::remove_if(loops.begin(), loops.end(), [](const Loop &loop) {
        return loop.isEmpty();
    }), loops.end());
    return loops;
}

QPainterPath fromSpans(const Loops &loops, Qt::FillRule rule) {
    QPainterPath path;
    path.setFillRule(rule);
    for (const auto &loop : loops) {
        if (loop.isEmpty()) continue;
        path.moveTo(loop.front().start);
        for (const auto &segment : loop) segment.appendTo(path);
        path.closeSubpath();
    }
    return path;
}

void flatten(const Segment &segment, double tolerance, PathD &points, int depth = 0) {
    if (!segment.curved || segment.flatness() <= tolerance) {
        points.emplace_back(segment.end.x(), segment.end.y());
        return;
    }
    if (depth == 24) throw std::runtime_error("Flattening tolerance could not be met");
    const auto [left, right] = segment.split();
    flatten(left, tolerance, points, depth + 1);
    flatten(right, tolerance, points, depth + 1);
}

PathsD rawPolygons(const QPainterPath &path, double tolerance) {
    PathsD polygons;
    for (const auto &loop : spans(path)) {
        PathD points{{loop.front().start.x(), loop.front().start.y()}};
        for (const auto &segment : loop) flatten(segment, tolerance, points);
        if (points.front() == points.back()) points.pop_back();
        if (points.size() >= 3) polygons.push_back(points);
    }
    return polygons;
}

PathsD polygons(const QPainterPath &path, double tolerance) {
    auto result = Union(rawPolygons(path, tolerance), path.fillRule() == Qt::OddEvenFill
                        ? FillRule::EvenOdd : FillRule::NonZero, kBooleanPrecision);
    for (auto &polygon : result) polygon = trimPolygon(polygon);
    return result;
}

class PolygonIndex {
    PathsD paths_;
    std::vector<RectD> bounds_;

public:
    explicit PolygonIndex(PathsD paths) : paths_(std::move(paths)) {
        for (const auto &path : paths_) bounds_.push_back(GetBounds(path));
    }

    double clipArea(const PathsD &shape, QRectF box, ClipType operation) const {
        PathsD nearby;
        for (size_t index = 0; index < paths_.size(); ++index) {
            const auto &bounds = bounds_[index];
            if (bounds.right < box.left() || bounds.left > box.right()
                || bounds.bottom < box.top() || bounds.top > box.bottom()) continue;
            nearby.push_back(paths_[index]);
        }
        return std::abs(Area(BooleanOp(operation, FillRule::NonZero, shape, nearby, kBooleanPrecision)));
    }

    double intersectArea(const PathsD &shape, QRectF box) const {
        return clipArea(shape, box, ClipType::Intersection);
    }

    double differenceArea(const PathsD &shape, QRectF box) const {
        return clipArea(shape, box, ClipType::Difference);
    }
};

QPainterPath fromPolygons(const PathsD &polygons) {
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (const auto &original : polygons) {
        const auto polygon = trimPolygon(original);
        if (polygon.empty()) continue;
        path.moveTo(polygon.front().x, polygon.front().y);
        for (size_t index = 1; index < polygon.size(); ++index)
            path.lineTo(polygon[index].x, polygon[index].y);
        path.closeSubpath();
    }
    return path;
}

std::vector<PathsD> components(const PathsD &polygons) {
    PolyTreeD tree;
    BooleanOp(ClipType::Union, FillRule::NonZero, polygons, {}, tree, kBooleanPrecision);
    std::vector<PathsD> result;
    const std::function<void(const PolyPathD &)> visit = [&](const PolyPathD &node) {
        if (!node.Polygon().empty() && !node.IsHole()) {
            PathsD part{node.Polygon()};
            for (const auto &child : node) part.push_back(child->Polygon());
            result.push_back(std::move(part));
        }
        for (const auto &child : node) visit(*child);
    };
    visit(tree);

    return result;
}

QVector<QPointF> polygonJunctions(const PathsD &shape) {
    struct Edge { QPointF a, b; QRectF bounds; int loop = 0, vertex = 0; };
    std::vector<Edge> edges;
    for (int loop = 0; loop < int(shape.size()); ++loop) {
        for (int vertex = 0; vertex < int(shape[loop].size()); ++vertex) {
            const auto &a = shape[loop][vertex], &b = shape[loop][(vertex + 1) % shape[loop].size()];
            edges.push_back({{a.x, a.y}, {b.x, b.y}, QRectF(QPointF(a.x, a.y), QPointF(b.x, b.y)).normalized(), loop, vertex});
        }
    }
    std::stable_sort(edges.begin(), edges.end(), [](const Edge &a, const Edge &b) { return a.bounds.left() < b.bounds.left(); });
    const double contact = 8.0 / booleanScale();
    std::map<std::pair<qint64, qint64>, QPointF> junctions;
    for (int loop = 0; loop < int(shape.size()); ++loop) for (int vertex = 0; vertex < int(shape[loop].size()); ++vertex) {
        const auto &p = shape[loop][vertex];
        const QPointF point(p.x, p.y);
        for (const auto &edge : edges) {
            if (edge.bounds.left() > p.x + contact) break;
            if (edge.loop == loop && (edge.vertex == vertex || (edge.vertex + 1) % shape[loop].size() == vertex)) continue;
            if (edge.bounds.right() < p.x - contact || edge.bounds.top() > p.y + contact || edge.bounds.bottom() < p.y - contact) continue;
            if (gui::fill_geometry::distanceSquaredToSegment(point, edge.a, edge.b, 1e-30) > contact * contact) continue;
            junctions.emplace(std::pair(qRound64(p.x * booleanScale()), qRound64(p.y * booleanScale())), point);
            break;
        }
    }
    QVector<QPointF> result;
    for (const auto &[key, point] : junctions) result.push_back(point);
    return result;
}

QPainterPath junctionCompletion(const PathsD &shape, const PathsD &domain, const Options &options) {
    const auto junctions = polygonJunctions(shape);
    if (junctions.isEmpty()) return {};
    const double radius = std::min(options.flattenTolerance * .001,
        std::sqrt(options.areaTolerance / (8 * junctions.size())));
    PathsD bridges;
    for (const auto &point : junctions) {
        QPainterPath squarePath;
        squarePath.addRect(QRectF(point.x() - radius, point.y() - radius, radius * 2, radius * 2));
        const auto square = rawPolygons(squarePath, options.flattenTolerance);
        bridges.insert(bridges.end(), square.begin(), square.end());
    }
    const auto repaired = Intersect(Union(shape, bridges, FillRule::NonZero, kBooleanPrecision), domain,
        FillRule::NonZero, kBooleanPrecision);
    if (std::abs(Area(Difference(repaired, shape, FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance
        || std::abs(Area(Difference(shape, repaired, FillRule::NonZero, kBooleanPrecision))) > options.areaTolerance) return {};
    auto path = fromPolygons(repaired);
    return penTargetError(path, options).isEmpty() ? path : QPainterPath{};
}

double geometryCost(const Metrics &metrics, const Options &options) {
    return options.pathWeight * metrics.paths
        + options.segmentWeight * (metrics.lineSegments + metrics.bezierSegments)
        + options.controlPointWeight * 2 * metrics.bezierSegments
        + options.holeWeight * metrics.holes + options.componentWeight * metrics.components
        + options.orderingWeight * metrics.orderingEdges
        + options.nativeItemWeight * metrics.drawItems + options.nativeSegmentWeight * metrics.encodedSegments;
}

QPainterPath booleanPath(const QPainterPath &left, const QPainterPath &right,
                        ClipType operation, double tolerance) {
    return fromPolygons(BooleanOp(operation, FillRule::NonZero,
        polygons(left, tolerance), polygons(right, tolerance), kBooleanPrecision));
}

double area(const QPainterPath &path, double tolerance) {
    return std::abs(Area(polygons(path, tolerance)));
}

double differenceArea(const QPainterPath &left, const QPainterPath &right, double tolerance) {
    return area(booleanPath(left, right, ClipType::Difference, tolerance), tolerance);
}

bool curved(const QPainterPath &path) {
    for (int index = 0; index < path.elementCount(); ++index)
        if (path.elementAt(index).type == QPainterPath::CurveToElement) return true;
    return false;
}

QPainterPath rectangle(const QRectF &bounds) {
    QPainterPath path;
    path.addRect(bounds);
    return path;
}

QPainterPath filledUnion(const QVector<gui::SvgVectorObject> &objects, double tolerance) {
    PathsD combined;
    for (const auto &object : objects) {
        auto normalized = polygons(object.path, tolerance);
        combined.insert(combined.end(), normalized.begin(), normalized.end());
    }
    return fromPolygons(Union(combined, FillRule::NonZero, kBooleanPrecision));
}

QPainterPath hull(const QPainterPath &path, double tolerance) {
    QVector<QPointF> points;
    for (const auto &polygon : polygons(path, tolerance))
        for (const auto &point : polygon) points.push_back({point.x, point.y});
    std::sort(points.begin(), points.end(), [](QPointF a, QPointF b) {
        return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) return {};
    QPainterPath result;
    result.addPolygon(gui::fill_geometry::convexHullFromSortedPoints(points, 1e-12));
    result.closeSubpath();
    return result;
}

bool withinBudget(const QJsonObject &error, const Options &options) {
    return error.value("rmse").toDouble() <= options.renderRmse
        && error.value("max_channel_error").toDouble() <= options.renderMaxError
        && error.value("max_alpha_error").toDouble() <= options.renderMaxError;
}

QTransform encodedTransform(const QTransform &transform, QColor color, int shapeId,
                            QByteArray &records) {
    fls::Matrix3 matrix;
    matrix.m[0][0] = transform.m11(); matrix.m[0][1] = transform.m21();
    matrix.m[1][0] = transform.m12(); matrix.m[1][1] = transform.m22();
    matrix.m[0][2] = transform.dx(); matrix.m[1][2] = transform.dy();
    const auto fields = fls::decomposeTransform2D(matrix);
    fls::detail::VinylShapePlacement placement;
    placement.shapeId = static_cast<quint16>(shapeId);
    placement.color = {static_cast<quint8>(color.red()), static_cast<quint8>(color.green()),
                       static_cast<quint8>(color.blue()), 255};
    placement.x = static_cast<float>(fields.x);
    placement.y = static_cast<float>(fields.y);
    placement.rotation = static_cast<float>(fls::normalizeRotation(fields.rotation));
    placement.scaleX = static_cast<float>(fields.scaleX);
    placement.scaleY = static_cast<float>(fields.scaleY);
    placement.skew = static_cast<float>(fields.skew);
    fls::detail::appendVinylShapeRecord(records, placement, 0, 0, 0);
    fls::scene::Transform2D decoded;
    decoded.x = placement.x; decoded.y = placement.y;
    decoded.rotation = placement.rotation;
    decoded.scaleX = placement.scaleX; decoded.scaleY = placement.scaleY;
    decoded.skew = placement.skew;
    const auto result = decoded.matrix();
    return QTransform(result.m[0][0], result.m[1][0], result.m[0][1], result.m[1][1],
                      result.m[0][2], result.m[1][2]);
}

} // namespace

QJsonObject Metrics::json() const {
    return {{"paths", paths}, {"total_path_count", paths}, {"subpaths", subpaths},
            {"fills", fills}, {"paths_above_one_per_fill", paths - fills},
            {"single_path_fills", singlePathFills}, {"multiple_path_fills", fills - singlePathFills},
            {"max_paths_per_fill", maximumPathsPerFill},
            {"line_segments", lineSegments}, {"bezier_segments", bezierSegments},
            {"svg_segments", lineSegments + bezierSegments},
            {"bezier_control_points", 2 * bezierSegments},
            {"svg_coordinate_pairs", subpaths + lineSegments + 3 * bezierSegments},
            {"max_segments_per_path", maximumSegmentsPerPath},
            {"max_components_per_path", maximumComponentsPerPath}, {"max_holes_per_path", maximumHolesPerPath},
            {"ordering_edges", orderingMeasured ? QJsonValue(orderingEdges) : QJsonValue()},
            {"ordering_depth", orderingMeasured ? QJsonValue(orderingDepth) : QJsonValue()},
            {"native_encoding_measured", encodingMeasured},
            {"draw_items", encodingMeasured ? QJsonValue(drawItems) : QJsonValue()},
            {"encoded_segments", encodingMeasured ? QJsonValue(encodedSegments) : QJsonValue()},
            {"holes", holes}, {"components", components},
            {"shape_record_bytes", encodingMeasured ? QJsonValue(recordBytes) : QJsonValue()},
            {"cost", costMeasured ? QJsonValue(cost) : QJsonValue()}};
}

double booleanCoordinateGrid() {
    return 1.0 / booleanScale();
}

Metrics pathMetrics(const QVector<gui::SvgVectorObject> &objects, double flattenTolerance) {
    Metrics metrics;
    metrics.paths = objects.size();
    std::map<QRgb, int> fills;
    for (const auto &object : objects) {
        metrics.maximumPathsPerFill = std::max(metrics.maximumPathsPerFill, ++fills[object.color.rgb()]);
        int segments = 0, components = 0, holes = 0;
        for (const auto &loop : spans(object.path)) {
            ++metrics.subpaths;
            for (const auto &segment : loop) {
                if (segment.curved) ++metrics.bezierSegments;
                else ++metrics.lineSegments;
                ++segments;
            }
        }
        for (const auto &polygon : polygons(object.path, flattenTolerance)) {
            if (Area(polygon) < 0) ++holes;
            else ++components;
        }
        metrics.components += components;
        metrics.holes += holes;
        metrics.maximumSegmentsPerPath = std::max(metrics.maximumSegmentsPerPath, segments);
        metrics.maximumComponentsPerPath = std::max(metrics.maximumComponentsPerPath, components);
        metrics.maximumHolesPerPath = std::max(metrics.maximumHolesPerPath, holes);
    }
    metrics.fills = fills.size();
    for (const auto &[fill, count] : fills) metrics.singlePathFills += count == 1;
    return metrics;
}

double silhouetteDifference(const QVector<gui::SvgVectorObject> &before,
                            const QVector<gui::SvgVectorObject> &after, double flattenTolerance) {
    const auto left = polygons(filledUnion(before, flattenTolerance), flattenTolerance);
    const auto right = polygons(filledUnion(after, flattenTolerance), flattenTolerance);
    return std::abs(Area(Xor(left, right, FillRule::NonZero, kBooleanPrecision)));
}

QJsonObject Graph::json() const {
    QJsonArray edgeArray, orderArray;
    for (const auto &[from, to] : edges) edgeArray.append(QJsonArray{from, to});
    for (const int item : order) orderArray.append(item);
    return {{"edges", edgeArray}, {"order", orderArray}, {"error", error}};
}

QVector<Item> makeItems(const QVector<gui::SvgVectorObject> &objects) {
    QVector<Item> result;
    for (int index = 0; index < objects.size(); ++index)
        result.push_back({objects[index].path, objects[index].color, {index}});
    return result;
}

QVector<gui::SvgVectorObject> objects(const QVector<Item> &items) {
    QVector<gui::SvgVectorObject> result;
    for (const auto &item : items) result.push_back({item.path, item.color});
    return result;
}

QImage render(const QVector<gui::SvgVectorObject> &objects, QSize size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    for (const auto &object : objects) {
        painter.setBrush(object.color);
        painter.drawPath(object.path);
    }
    return image;
}

QImage renderCrop(const QVector<gui::SvgVectorObject> &objects, QRect rectangle) {
    QVector<gui::SvgVectorObject> local;
    const QTransform translation = QTransform::fromTranslate(-rectangle.x(), -rectangle.y());
    for (const auto &object : objects)
        if (object.path.boundingRect().adjusted(-2, -2, 2, 2).intersects(rectangle))
            local.push_back({translation.map(object.path), object.color});
    QSvgRenderer renderer(svg(local, rectangle.size()));
    QImage image(rectangle.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(QPointF(), rectangle.size()));

    return image;
}

QJsonObject imageError(const QImage &reference, const QImage &actual) {
    if (reference.size() != actual.size() || reference.isNull())
        throw std::runtime_error("Image dimensions do not match");
    const auto left = reference.convertToFormat(QImage::Format_ARGB32);
    const auto right = actual.convertToFormat(QImage::Format_ARGB32);
    double squared = 0.0;
    int maximum = 0, alphaMaximum = 0;
    qint64 changed = 0;
    for (int y = 0; y < left.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        for (int x = 0; x < left.width(); ++x) {
            int pixelMaximum = 0;
            for (int shift : {0, 8, 16}) {
                const double ca = ((a[x] >> shift) & 255) * qAlpha(a[x]) / 255.0
                    + 255 - qAlpha(a[x]);
                const double cb = ((b[x] >> shift) & 255) * qAlpha(b[x]) / 255.0
                    + 255 - qAlpha(b[x]);
                const double delta = std::abs(ca - cb);
                squared += delta * delta;
                pixelMaximum = std::max(pixelMaximum, static_cast<int>(std::ceil(delta)));
            }
            maximum = std::max(maximum, pixelMaximum);
            alphaMaximum = std::max(alphaMaximum, std::abs(qAlpha(a[x]) - qAlpha(b[x])));
            changed += pixelMaximum != 0;
        }
    }
    const double mse = squared / (left.width() * double(left.height()) * 3);
    return {{"rmse", std::sqrt(mse)}, {"max_channel_error", maximum},
            {"max_alpha_error", alphaMaximum}, {"changed_pixels", double(changed)},
            {"psnr_db", mse > 0 ? QJsonValue(10 * std::log10(255 * 255 / mse)) : QJsonValue()}};
}

QJsonObject appearanceError(const QImage &reference, const QImage &actual, int window) {
    auto error = imageError(reference, actual);
    if (window < 1) throw std::runtime_error("Local appearance window must be positive");
    const auto left = reference.convertToFormat(QImage::Format_ARGB32);
    const auto right = actual.convertToFormat(QImage::Format_ARGB32);
    const int width = std::min(window, left.width()), height = std::min(window, left.height());
    std::vector<std::vector<double>> rows(height, std::vector<double>(left.width()));
    std::vector<double> columns(left.width());
    double maximum = 0;
    QPoint worst;
    for (int y = 0; y < left.height(); ++y) {
        auto &row = rows[y % height];
        const auto *a = reinterpret_cast<const QRgb *>(left.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(right.constScanLine(y));
        for (int x = 0; x < left.width(); ++x) {
            double squared = 0;
            for (const int shift : {0, 8, 16}) {
                const double ca = ((a[x] >> shift) & 255) * qAlpha(a[x]) / 255.0 + 255 - qAlpha(a[x]);
                const double cb = ((b[x] >> shift) & 255) * qAlpha(b[x]) / 255.0 + 255 - qAlpha(b[x]);
                squared += (ca - cb) * (ca - cb);
            }
            columns[x] += squared - row[x];
            row[x] = squared;
        }
        if (y + 1 < height) continue;
        double sum = 0;
        for (int x = 0; x < left.width(); ++x) {
            sum += columns[x];
            if (x >= width) sum -= columns[x - width];
            if (x + 1 >= width && sum > maximum) {
                maximum = sum;
                worst = {x + 1 - width, y + 1 - height};
            }
        }
    }
    error.insert("max_local_rmse", std::sqrt(std::max(0.0, maximum) / (width * height * 3.0)));
    error.insert("local_window_pixels", window);
    error.insert("worst_local_rectangle", QJsonArray{worst.x(), worst.y(), width, height});
    return error;
}

QImage difference(const QImage &reference, const QImage &actual) {
    QImage result(reference.size(), QImage::Format_RGB32);
    const auto channel = [](QColor color, int value) {
        return value * color.alphaF() + 255 * (1 - color.alphaF());
    };
    for (int y = 0; y < result.height(); ++y)
        for (int x = 0; x < result.width(); ++x) {
            const auto a = reference.pixelColor(x, y), b = actual.pixelColor(x, y);
            result.setPixelColor(x, y, QColor(qRound(std::abs(channel(a, a.red()) - channel(b, b.red()))),
                qRound(std::abs(channel(a, a.green()) - channel(b, b.green()))),
                qRound(std::abs(channel(a, a.blue()) - channel(b, b.blue())))));
        }
    return result;
}

QByteArray svg(const QVector<gui::SvgVectorObject> &objects, QSize size) {
    QByteArray result;
    QXmlStreamWriter writer(&result);
    writer.writeStartElement("svg");
    writer.writeDefaultNamespace("http://www.w3.org/2000/svg");
    writer.writeAttribute("width", QString::number(size.width()));
    writer.writeAttribute("height", QString::number(size.height()));
    writer.writeAttribute("viewBox", QString("0 0 %1 %2").arg(size.width()).arg(size.height()));
    const auto point = [](QPointF p) {
        return QString("%1 %2").arg(p.x(), 0, 'g', 17).arg(p.y(), 0, 'g', 17);
    };
    for (const auto &object : objects) {
        if (object.color.alpha() != 255) throw std::runtime_error("Nonopaque output rejected");
        QString data;
        for (const auto &loop : spans(object.path)) {
            data += "M" + point(loop.front().start);
            for (const auto &segment : loop) {
                if (segment.curved)
                    data += "C" + point(segment.control) + " " + point(segment.control2)
                        + " " + point(segment.end);
                else data += "L" + point(segment.end);
            }
            data += "Z";
        }
        writer.writeStartElement("path");
        writer.writeAttribute("fill", object.color.name());
        writer.writeAttribute("fill-rule", object.path.fillRule() == Qt::OddEvenFill ? "evenodd" : "nonzero");
        writer.writeAttribute("d", data);
        writer.writeEndElement();
    }
    writer.writeEndElement();
    return result;
}

Encoder meshEncoder(const gui::PolygonMeshSources &sources, const Options &options) {
    return [sources, options, cache = std::map<QByteArray, Encoding>{}](const QPainterPath &path, QColor color) mutable {
        QByteArray key;
        QDataStream stream(&key, QIODevice::WriteOnly);
        stream << path << color;
        if (const auto found = cache.find(key); found != cache.end()) return found->second;
        Encoding result;
        const auto normalized = polygons(path, options.flattenTolerance);
        PolyTreeD tree;
        BooleanOp(ClipType::Union, FillRule::NonZero, normalized, {}, tree, kBooleanPrecision);
        const auto points = [](const PathD &polygon) {
            QVector<QPointF> contour;
            for (const auto &point : polygon) contour.push_back({point.x, point.y});
            return contour;
        };
        const std::function<void(const PolyPathD &)> emitComponent = [&](const PolyPathD &node) {
            if (!result.error.isEmpty()) return;
            if (!node.Polygon().empty() && !node.IsHole()) {
                gui::PolygonMeshRequest request;
                request.sources = sources;
                if (node.Count() == 0) request.points = points(node.Polygon());
                else {
                    request.contours.push_back(points(node.Polygon()));
                    for (const auto &hole : node) request.contours.push_back(points(hole->Polygon()));
                }
                const auto mesh = gui::meshPolygon(request);
                result.error = mesh.error;
                if (!result.error.isEmpty()) return;
                for (const auto &placement : mesh.placements) {
                    const auto transform = encodedTransform(placement.transform, color, placement.shapeId, result.records);
                    const auto source = placement.shapeId == 101 ? sources.square : sources.triangle;
                    QPainterPath native;
                    native.addPolygon(transform.map(source));
                    native.closeSubpath();
                    result.shapes.push_back({native, color});
                    result.placements.push_back({placement.shapeId, transform});
                    result.segments += source.size();
                }
            }
            for (const auto &child : node) emitComponent(*child);
        };
        emitComponent(tree);
        if (cache.size() >= 8192) cache.clear();
        cache.emplace(std::move(key), result);
        return result;
    };
}

Graph topologicalOrder(int count, const std::set<std::pair<int, int>> &edges, const QVector<int> &keys) {
    Graph graph;
    graph.edges = edges;
    QVector<QVector<int>> next(count);
    QVector<int> degree(count, 0);
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> ready;
    for (const auto &[from, to] : edges) {
        if (from < 0 || to < 0 || from >= count || to >= count || from == to) {
            graph.error = "Invalid dependency";
            return graph;
        }
        next[from].push_back(to); ++degree[to];
    }
    for (int index = 0; index < count; ++index)
        if (!degree[index]) ready.push({keys.isEmpty() ? index : keys[index], index});
    while (!ready.empty()) {
        const int item = ready.top().second;
        ready.pop(); graph.order.push_back(item);
        for (const int to : next[item])
            if (--degree[to] == 0) ready.push({keys.isEmpty() ? to : keys[to], to});
    }
    if (graph.order.size() != count) graph.error = "Dependency cycle";
    return graph;
}

Optimizer::Optimizer(QVector<Item> baseline, Options options, Encoder encoder, Renderer renderer,
                     const QVector<QPainterPath> &fixedOwnership, const QPainterPath &fixedDomain)
    : baseline_(std::move(baseline)), options_(options), encoder_(std::move(encoder)),
      renderer_(std::move(renderer)) {
    std::vector<PathsD> sources;
    QVector<QRectF> bounds;
    PathsD complete;
    for (const auto &item : baseline_) {
        sources.push_back(polygons(item.path, options_.flattenTolerance));
        bounds.push_back(fromPolygons(sources.back()).boundingRect());
        complete.insert(complete.end(), sources.back().begin(), sources.back().end());
    }
    if (!fixedOwnership.isEmpty()) {
        if (fixedOwnership.size() != baseline_.size()) throw std::runtime_error("Fixed ownership does not match paint targets");
        ownership_ = fixedOwnership;
    } else {
        ownership_.resize(baseline_.size());
        for (int index = baseline_.size() - 1; index >= 0; --index) {
            PathsD above;
            for (int later = index + 1; later < baseline_.size(); ++later) {
                if (!bounds[index].intersects(bounds[later])) continue;
                above.insert(above.end(), sources[later].begin(), sources[later].end());
            }
            ownership_[index] = fromPolygons(Difference(sources[index],
                Union(above, FillRule::NonZero, kBooleanPrecision), FillRule::NonZero, kBooleanPrecision));
        }
    }
    domain_ = fixedDomain.isEmpty() ? fromPolygons(Union(complete, FillRule::NonZero, kBooleanPrecision)) : fixedDomain;
    if (options_.strategy != "legacy") {
        const auto original = ownership_;
        ownership_.clear();
        for (int source = 0; source < baseline_.size(); ++source) {
            baseline_[source].owners.clear();
            auto parts = components(polygons(original[source], options_.flattenTolerance));
            if (parts.empty()) parts.push_back({});
            for (const auto &part : parts) {
                baseline_[source].owners.insert(ownership_.size());
                ownership_.push_back(fromPolygons(part));
                ownerSources_.push_back(source);
            }
        }
    } else {
        for (int source = 0; source < baseline_.size(); ++source) ownerSources_.push_back(source);
    }
}

Graph Optimizer::dependencies(const QVector<Item> &items) const {
    QVector<int> ownerItem(ownership_.size(), -1), keys;
    std::vector<PathsD> required, shapes;
    for (const auto &path : ownership_) required.push_back(polygons(path, options_.flattenTolerance));
    std::vector<PolygonIndex> regions;
    for (const auto &region : required) regions.emplace_back(region);
    for (const auto &item : items) shapes.push_back(polygons(item.path, options_.flattenTolerance));
    for (int index = 0; index < items.size(); ++index) {
        if (items[index].owners.empty()) return {{}, {}, "Missing owner"};
        keys.push_back(*items[index].owners.begin());
        for (const int owner : items[index].owners) {
            if (owner < 0 || owner >= ownerItem.size() || ownerItem[owner] != -1)
                return {{}, {}, "Invalid ownership partition"};
            ownerItem[owner] = index;
        }
    }
    std::set<std::pair<int, int>> edges;
    for (int index = 0; index < items.size(); ++index) {
        for (int owner = 0; owner < ownership_.size(); ++owner) {
            if (ownerItem[owner] < 0) {
                if (std::abs(Area(required[owner])) > options_.areaTolerance) return {{}, {}, "Missing owner"};
                continue;
            }
            if (ownerItem[owner] == index) continue;
            if (!items[index].path.boundingRect().intersects(ownership_[owner].boundingRect())) continue;
            if (regions[owner].intersectArea(shapes[index], items[index].path.boundingRect()) > options_.areaTolerance)
                edges.insert({index, ownerItem[owner]});
        }
    }
    return topologicalOrder(items.size(), edges, keys);
}

QJsonObject Optimizer::ownershipDiagnostics() const {
    QJsonArray regions, adjacency;
    QPainterPathStroker stroker;
    stroker.setWidth(options_.flattenTolerance * 2);
    const auto boundary = stroker.createStroke(domain_);
    for (int index = 0; index < ownership_.size(); ++index) {
        int locked = 0;
        for (const auto &loop : spans(baseline_[ownerSources_[index]].path))
            for (const auto &span : loop) {
                QPainterPath chain;
                chain.moveTo(span.start);
                span.appendTo(chain);
                if (boundary.intersects(stroker.createStroke(chain))) ++locked;
            }
        regions.append(QJsonObject{{"original_owner", index},
            {"visible_area", area(ownership_[index], options_.flattenTolerance)},
            {"external_silhouette_spans", locked}});
        const auto expanded = stroker.createStroke(ownership_[index]);
        for (int other = index + 1; other < ownership_.size(); ++other) {
            if (!expanded.boundingRect().intersects(ownership_[other].boundingRect())) continue;
            if (expanded.intersects(ownership_[other])) adjacency.append(QJsonArray{index, other});
        }
    }
    return {{"fixed_visible_owners", regions}, {"adjacency", adjacency},
            {"silhouette_lock", "complete filled-domain symmetric difference; original cubic spans retained unless wholly hidden"}};
}

QString Optimizer::validate(const QVector<Item> &items) const {
    PathsD complete;
    const auto domain = polygons(domain_, options_.flattenTolerance);
    std::vector<PathsD> required;
    for (const auto &path : ownership_) required.push_back(polygons(path, options_.flattenTolerance));
    for (const auto &item : items) {
        if (item.owners.empty() || item.path.isEmpty() || item.color.alpha() != 255)
            return "Invalid or nonopaque geometry";
        for (int index = 0; index < item.path.elementCount(); ++index) {
            const auto p = item.path.elementAt(index);
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return "Nonfinite geometry";
        }
        if (options_.strategy != "legacy") {
            const auto error = penTargetError(item.path, options_);
            if (!error.isEmpty()) return "Invalid Bucket target: " + error;
        }
        for (const auto &polygon : options_.strategy == "legacy"
                 ? rawPolygons(item.path, options_.flattenTolerance) : PathsD{}) {
            QVector<QPointF> points;
            for (const auto &point : polygon) points.push_back({point.x, point.y});
            if (!gui::buildPolygonContour(points, 1e-6).valid())
                return "Invalid self-intersecting or degenerate geometry";
        }
        const auto shape = polygons(item.path, options_.flattenTolerance);
        for (const int owner : item.owners) {
            if (owner < 0 || owner >= ownership_.size() || item.color != baseline_[ownerSources_[owner]].color)
                return "Incompatible appearance ownership";
            const double missing = std::abs(Area(Difference(required[owner], shape, FillRule::NonZero, kBooleanPrecision)));
            if (missing > options_.areaTolerance)
                return QString("Required visible territory removed: owner %1, area %2").arg(owner).arg(missing, 0, 'g', 16);
        }
        if (std::abs(Area(Difference(shape, domain, FillRule::NonZero, kBooleanPrecision))) > options_.silhouetteTolerance)
            return "Outside locked silhouette/domain";
        complete.insert(complete.end(), shape.begin(), shape.end());
    }
    const auto combined = Union(complete, FillRule::NonZero, kBooleanPrecision);
    if (std::abs(Area(Difference(combined, domain, FillRule::NonZero, kBooleanPrecision))) > options_.silhouetteTolerance)
        return "Combined geometry exceeds locked silhouette";
    const double missingDomain = std::abs(Area(Difference(domain, combined, FillRule::NonZero, kBooleanPrecision)));
    if (missingDomain > options_.silhouetteTolerance)
        return QString("Silhouette territory removed: area %1").arg(missingDomain, 0, 'g', 16);
    return dependencies(items).error;
}

Metrics Optimizer::measure(const QVector<Item> &items, QVector<gui::SvgVectorObject> *native,
                           QString *error) const {
    Metrics metrics = pathMetrics(objects(items), options_.flattenTolerance);
    for (const auto &item : items) {
        if (!encoder_) break;
        const auto encoded = encoder_(item.path, item.color);
        if (!encoded.error.isEmpty()) { *error = encoded.error; return metrics; }
        *native += encoded.shapes;
        metrics.drawItems += encoded.shapes.size();
        metrics.encodedSegments += encoded.segments;
        metrics.recordBytes += encoded.records.size();
    }
    metrics.encodingMeasured = bool(encoder_);
    const auto graph = dependencies(items);
    if (!graph.error.isEmpty()) { *error = graph.error; return metrics; }
    metrics.orderingEdges = graph.edges.size();
    metrics.orderingMeasured = true;
    QVector<int> depth(items.size(), 1);
    for (const int index : graph.order) {
        metrics.orderingDepth = std::max(metrics.orderingDepth, depth[index]);
        for (const auto &[from, to] : graph.edges)
            if (from == index) depth[to] = std::max(depth[to], depth[from] + 1);
    }
    metrics.cost = geometryCost(metrics, options_);
    metrics.costMeasured = true;
    return metrics;
}

void Optimizer::proposals(const QVector<Item> &items, const std::function<void(Proposal)> &visit) const {
    const auto replace = [&](int index, QPainterPath path, const QString &kind) {
        auto updated = items;
        updated[index].path = std::move(path);
        visit({std::move(updated), kind});
    };
    for (int index = 0; index < items.size(); ++index) {
        const auto &item = items[index];
        const auto loops = spans(item.path);
        for (int loopIndex = 0; loopIndex < loops.size(); ++loopIndex) {
            auto filled = loops;
            filled.removeAt(loopIndex);
            if (!filled.isEmpty()) replace(index, fromSpans(filled, item.path.fillRule()), "fill-hole");
            const auto &loop = loops[loopIndex];
            for (int start = 0; start < loop.size(); ++start) {
                for (int length = 1; length <= options_.maximumChordSpans && length < loop.size(); ++length) {
                    if (length == 1 && !loop[start].curved) continue;
                    Loop shortened;
                    shortened.push_back({loop[start].start, {}, loop[(start + length - 1) % loop.size()].end, false, {}});
                    for (int offset = length; offset < loop.size(); ++offset)
                        shortened.push_back(loop[(start + offset) % loop.size()]);
                    auto updated = loops;
                    updated[loopIndex] = shortened;
                    replace(index, fromSpans(updated, item.path.fillRule()), "chord");
                }
            }
        }
        replace(index, rectangle(item.path.boundingRect()), "rectangle");
        replace(index, hull(item.path, options_.flattenTolerance), "convex-hull");
        for (int other = index + 1; other < items.size(); ++other) {
            if (items[other].color != item.color) continue;
            QPainterPath merged;
            const double overlap = area(booleanPath(item.path, items[other].path, ClipType::Intersection,
                                                    options_.flattenTolerance), options_.flattenTolerance);
            if (!curved(item.path) && !curved(items[other].path)) {
                merged = booleanPath(item.path, items[other].path, ClipType::Union, options_.flattenTolerance);
            } else if (overlap <= options_.areaTolerance && item.path.fillRule() == items[other].path.fillRule()) {
                merged = item.path;
                merged.addPath(items[other].path);
            } else continue;
            auto updated = items;
            updated[index].path = merged;
            updated[index].owners.insert(items[other].owners.begin(), items[other].owners.end());
            updated.removeAt(other);
            visit({updated, "same-fill-merge"});
            updated[index].path = hull(merged, options_.flattenTolerance);
            visit({updated, "hidden-bridge-hull"});
            updated[index].path = rectangle(merged.boundingRect());
            visit({updated, "hidden-bridge-rectangle"});
        }
    }
}

namespace {

struct Difficulty {
    double area = 0.0;
    double score = 0.0;
    int hard = 0;
    int soft = 0;
    int concave = 0;
    int inflections = 0;
    int components = 0;
    int holes = 0;

    QJsonObject json() const {
        return {{"area", area}, {"hard_points", hard}, {"soft_spans", soft},
            {"concave_points", concave}, {"curvature_sign_changes", inflections},
            {"components", components}, {"holes", holes}, {"estimate", score}};
    }
};

Difficulty difficulty(const QPainterPath &path, const Options &options) {
    Difficulty result;
    int authoredCurves = 0, authoredLines = 0;
    double smoothTurning = 0.0;
    for (const auto &loop : spans(path))
        for (const auto &span : loop) {
            authoredCurves += span.curved;
            authoredLines += !span.curved;
        }
    for (const auto &polygon : polygons(path, options.flattenTolerance)) {
        const double signedArea = Area(polygon);
        result.area += signedArea;
        if (signedArea > 0) ++result.components;
        else ++result.holes;
        int previousSign = 0;
        for (size_t index = 0; index < polygon.size(); ++index) {
            const auto &a = polygon[(index + polygon.size() - 1) % polygon.size()];
            const auto &b = polygon[index];
            const auto &c = polygon[(index + 1) % polygon.size()];
            const double ax = b.x - a.x, ay = b.y - a.y, bx = c.x - b.x, by = c.y - b.y;
            const double turn = std::atan2(ax * by - ay * bx, ax * bx + ay * by);
            if (std::abs(turn) > 0.26) {
                ++result.hard;
                result.concave += turn < 0;
            } else smoothTurning += std::abs(turn);
            if (std::abs(turn) > 0.003) {
                const int sign = turn > 0 ? 1 : -1;
                result.inflections += previousSign != 0 && sign != previousSign;
                previousSign = sign;
            }
        }
    }
    result.hard = std::max(authoredLines, result.hard);
    result.soft = std::max(authoredCurves, int(std::ceil(smoothTurning / (std::numbers::pi / 2))));
    result.area = std::abs(result.area);
    result.score = options.pathWeight + options.hardPointWeight * result.hard
        + options.softPointWeight * result.soft + options.concavityWeight * (result.concave + result.inflections)
        + options.componentWeight * result.components + options.holeWeight * result.holes;
    return result;
}

class CurveBank {
    struct Curve { Segment span; PathD points; };
    using Key = std::pair<qint64, qint64>;
    std::map<Key, std::vector<Curve>> curves_;
    std::set<Key> anchors_;
    static Key key(const PointD &point) {
        const double scale = booleanScale();
        return {qRound64(point.x * scale), qRound64(point.y * scale)};
    }

    void add(const Segment &span, double tolerance) {
        PathD points{{span.start.x(), span.start.y()}};
        flatten(span, tolerance, points);
        curves_[key(points.front())].push_back({span, points});
        std::reverse(points.begin(), points.end());
        curves_[key(points.front())].push_back({{span.end, span.control2, span.start, true, span.control}, points});
        if (span.flatness() > tolerance) {
            const auto [left, right] = span.split();
            add(left, tolerance);
            add(right, tolerance);
        }
    }

public:
    CurveBank(const QVector<Item> &items, double tolerance) {
        for (const auto &item : items)
            for (const auto &loop : spans(item.path))
                for (const auto &span : loop) {
                    if (!span.curved) continue;
                    anchors_.insert(key({span.start.x(), span.start.y()}));
                    anchors_.insert(key({span.end.x(), span.end.y()}));
                    add(span, tolerance);
                }
    }

    QPainterPath restore(const PathsD &polygons, const QRectF &curveDomain = {}) const {
        QPainterPath result;
        result.setFillRule(Qt::WindingFill);
        for (const auto &polygon : polygons) {
            if (polygon.empty()) continue;
            size_t start = 0;
            bool anchored = false;
            for (size_t index = 0; index < polygon.size(); ++index)
                if (anchors_.contains(key(polygon[index]))) { start = index; anchored = true; break; }
            if (!anchored) for (size_t index = 0; index < polygon.size(); ++index)
                if (curves_.contains(key(polygon[index]))) { start = index; break; }
            QPointF startPoint(polygon[start].x, polygon[start].y);
            result.moveTo(startPoint);
            const int moveElement = result.elementCount() - 1;
            size_t consumed = 0;
            while (consumed < polygon.size()) {
                const size_t index = (start + consumed) % polygon.size();
                bool restored = false;
                const auto found = curves_.find(key(polygon[index]));
                if (found != curves_.end()) {
                    for (const auto &curve : found->second) {
                        if (!curveDomain.isNull()) {
                            QPainterPath segment;
                            segment.moveTo(curve.span.start);
                            curve.span.appendTo(segment);
                            if (!curveDomain.contains(segment.boundingRect())) continue;
                        }
                        const size_t edges = curve.points.size() - 1;
                        if (!edges || edges > polygon.size() - consumed) continue;
                        bool match = true;
                        for (size_t offset = 0; offset <= edges; ++offset)
                            if (key(polygon[(index + offset) % polygon.size()]) != key(curve.points[offset])) {
                                match = false; break;
                            }
                        if (!match) continue;
                        auto span = curve.span;
                        if (consumed == 0) {
                            startPoint = span.start;
                            result.setElementPositionAt(moveElement, startPoint.x(), startPoint.y());
                        }
                        if (consumed + edges == polygon.size()) span.end = startPoint;
                        span.appendTo(result);
                        consumed += edges;
                        restored = true;
                        break;
                    }
                }
                if (!restored) {
                    const auto &next = polygon[(index + 1) % polygon.size()];
                    if (consumed + 1 == polygon.size()) result.lineTo(startPoint);
                    else result.lineTo(next.x, next.y);
                    ++consumed;
                }
            }
            result.closeSubpath();
        }
        return result;
    }
};

class OwnerIndex {
    QRectF bounds_;
    QVector<QVector<int>> cells_ = QVector<QVector<int>>(4096);
    QVector<QRectF> rectangles_;
    int coordinate(double value, double origin, double extent) const {
        return std::clamp(int((value - origin) / std::max(extent, 1e-9) * 64), 0, 63);
    }

public:
    OwnerIndex(const QVector<QPainterPath> &regions, QRectF bounds) : bounds_(bounds) {
        for (int owner = 0; owner < regions.size(); ++owner) {
            const auto box = regions[owner].boundingRect();
            rectangles_.push_back(box);
            if (box.isEmpty()) continue;
            for (int y = coordinate(box.top(), bounds.top(), bounds.height());
                 y <= coordinate(box.bottom(), bounds.top(), bounds.height()); ++y)
                for (int x = coordinate(box.left(), bounds.left(), bounds.width());
                     x <= coordinate(box.right(), bounds.left(), bounds.width()); ++x)
                    cells_[y * 64 + x].push_back(owner);
        }
    }
    std::vector<int> query(const QRectF &box) const {
        std::set<int> result;
        for (int y = coordinate(box.top(), bounds_.top(), bounds_.height());
             y <= coordinate(box.bottom(), bounds_.top(), bounds_.height()); ++y)
            for (int x = coordinate(box.left(), bounds_.left(), bounds_.width());
                 x <= coordinate(box.right(), bounds_.left(), bounds_.width()); ++x)
                for (const int owner : cells_[y * 64 + x])
                    if (box.intersects(rectangles_[owner])) result.insert(owner);
        return {result.begin(), result.end()};
    }
};

struct Completion {
    Item item;
    QString kind;
    Difficulty complexity;
    double cost = 0.0;
    double saving = 0.0;
    double addedArea = 0.0;
    std::vector<int> above;
};

struct StackPlan {
    QVector<Item> items;
    QJsonArray steps;
    QJsonArray unavailable;
    QString error;
    int renderRevertedItems = 0;
};

} // namespace

#include "pen_targets.inc"
#include "bridges.inc"

QJsonObject regionDifficulty(const QPainterPath &path, const Options &options) {
    return difficulty(path, options).json();
}

void Optimizer::setAppearanceGuard(QImage reference, ColorOptions limits) {
    appearanceReference_ = std::move(reference);
    appearanceLimits_ = std::move(limits);
}

Result Optimizer::runStack() {
    Result result;
    result.items = baseline_;
    result.error = validate(result.items);
    if (!result.error.isEmpty()) return result;
    result.before = measure(result.items, &result.nativeShapes, &result.error);
    result.after = result.before;
    if (!result.error.isEmpty()) return result;
    const auto reference = renderer_(objects(baseline_));
    if (options_.maximumPasses == 0) {
        result.graph = dependencies(result.items);
        return result;
    }
    const auto domain = polygons(domain_, options_.flattenTolerance);
    const CurveBank curves(baseline_, options_.flattenTolerance);
    const OwnerIndex index(ownership_, domain_.boundingRect());
    std::vector<PathsD> required;
    std::vector<PolygonIndex> regions;
    std::vector<double> sourceCosts;
    std::vector<double> originalCosts;
    QVector<double> sourceAreas(baseline_.size(), 0.0);
    std::map<QRgb, std::vector<int>> colors;
    QJsonArray invisible;
    for (const auto &item : baseline_)
        originalCosts.push_back(geometryCost(pathMetrics({{item.path, item.color}}, options_.flattenTolerance), options_));
    for (int owner = 0; owner < ownership_.size(); ++owner)
        sourceAreas[ownerSources_[owner]] += area(ownership_[owner], options_.flattenTolerance);
    for (int owner = 0; owner < ownership_.size(); ++owner) {
        required.push_back(polygons(ownership_[owner], options_.flattenTolerance));
        regions.emplace_back(required.back());
        const int source = ownerSources_[owner];
        const double mass = std::abs(Area(required.back()));
        sourceCosts.push_back(originalCosts[source] * mass / std::max(sourceAreas[source], options_.areaTolerance));
        if (mass > options_.areaTolerance) colors[baseline_[source].color.rgb()].push_back(owner);
        else invisible.append(owner);
    }
    std::set<std::set<int>> groups;
    for (const auto &item : baseline_) {
        std::set<int> owners;
        for (const int owner : item.owners)
            if (std::abs(Area(required[owner])) > options_.areaTolerance) owners.insert(owner);
        if (!owners.empty()) groups.insert(owners);
    }
    for (const auto &[color, members] : colors) {
        for (const int owner : members) groups.insert({owner});
        if (members.size() > 1) groups.insert(std::set<int>(members.begin(), members.end()));
        for (const int owner : members) {
            std::vector<std::pair<double, int>> nearest;
            const auto box = ownership_[owner].boundingRect();
            for (const int other : members) {
                if (owner == other) continue;
                const auto otherBox = ownership_[other].boundingRect();
                const double dx = std::max({0.0, box.left() - otherBox.right(), otherBox.left() - box.right()});
                const double dy = std::max({0.0, box.top() - otherBox.bottom(), otherBox.top() - box.bottom()});
                nearest.push_back({dx * dx + dy * dy, other});
            }
            std::sort(nearest.begin(), nearest.end());
            for (int pair = 0; pair < std::min(int(nearest.size()), options_.groupNeighbors); ++pair)
                groups.insert({owner, nearest[pair].second});
        }
    }
    std::vector<Completion> candidates;
    std::vector<std::vector<int>> singles(ownership_.size()), blockedBy(ownership_.size()), containing(ownership_.size());
    std::map<QString, int> rejected;
    for (const auto &owners : groups) {
        PathsD territory, source;
        std::set<int> sources;
        double originalCost = 0.0;
        for (const int owner : owners) {
            territory.insert(territory.end(), required[owner].begin(), required[owner].end());
            sources.insert(ownerSources_[owner]);
            originalCost += sourceCosts[owner];
        }
        for (const int id : sources) {
            auto shape = polygons(baseline_[id].path, options_.flattenTolerance);
            source.insert(source.end(), shape.begin(), shape.end());
        }
        territory = Union(territory, FillRule::NonZero, kBooleanPrecision);
        source = Union(source, FillRule::NonZero, kBooleanPrecision);
        const auto visible = curves.restore(territory);
        const auto sourcePath = curves.restore(source);
        std::vector<std::pair<QString, QPainterPath>> shapes{
            {"source", sources.size() == 1 ? baseline_[*sources.begin()].path : sourcePath},
            {"visible", visible}, {"rectangle", rectangle(visible.boundingRect())},
            {"convex-hull", hull(visible, options_.flattenTolerance)}, {"visible-polygon", fromPolygons(territory)}};
        PathsD outer;
        for (const auto &loop : territory) if (Area(loop) > 0) outer.push_back(loop);
        shapes.push_back({"fill-holes", curves.restore(Union(outer, FillRule::NonZero, kBooleanPrecision))});
        if (owners.size() == 1 && !penTargetError(fromPolygons(territory), options_).isEmpty()) {
            const auto repaired = junctionCompletion(territory, domain, options_);
            if (!repaired.isEmpty()) shapes.push_back({"junction-completion", repaired});
        }
        auto sourceLoops = spans(sourcePath);
        for (int loop = 0; loop < sourceLoops.size() && int(shapes.size()) < options_.completionCandidates; ++loop) {
            auto filled = sourceLoops;
            filled.removeAt(loop);
            if (!filled.isEmpty()) shapes.push_back({"fill-hole", fromSpans(filled, sourcePath.fillRule())});
            const auto &chain = sourceLoops[loop];
            for (int length = std::min(options_.maximumChordSpans, int(chain.size()) - 1);
                 length > 0 && int(shapes.size()) < options_.completionCandidates; --length) {
                for (int start = 0; start < chain.size() && int(shapes.size()) < options_.completionCandidates; ++start) {
                    if (length == 1 && !chain[start].curved) continue;
                    Loop shortcut{{chain[start].start, {}, chain[(start + length - 1) % chain.size()].end, false, {}}};
                    for (int offset = length; offset < chain.size(); ++offset) shortcut.push_back(chain[(start + offset) % chain.size()]);
                    auto changed = sourceLoops;
                    changed[loop] = shortcut;
                    shapes.push_back({"chord", fromSpans(changed, sourcePath.fillRule())});
                }
            }
        }
        std::set<QByteArray> unique;
        for (auto &[kind, shape] : shapes) {
            const auto shapePolygons = polygons(shape, options_.flattenTolerance);
            if (shapePolygons.empty()) continue;
            if (std::abs(Area(Difference(territory, shapePolygons, FillRule::NonZero, kBooleanPrecision))) > options_.areaTolerance) {
                ++rejected["Visible territory removed"]; continue;
            }
            if (std::abs(Area(Difference(shapePolygons, domain, FillRule::NonZero, kBooleanPrecision))) > options_.silhouetteTolerance) {
                ++rejected["Outside locked silhouette"]; continue;
            }
            const auto penError = penTargetError(shape, options_);
            if (!penError.isEmpty()) { ++rejected["Invalid Bucket target: " + penError]; continue; }
            QByteArray key;
            QDataStream stream(&key, QIODevice::WriteOnly);
            stream << shape;
            if (!unique.insert(key).second) continue;
            Completion candidate;
            candidate.item = {shape, baseline_[ownerSources_[*owners.begin()]].color, owners};
            candidate.kind = owners.size() > 1 ? "group-" + kind : kind;
            candidate.complexity = difficulty(shape, options_);
            const auto metrics = pathMetrics({{shape, candidate.item.color}}, options_.flattenTolerance);
            candidate.cost = geometryCost(metrics, options_);
            candidate.saving = originalCost - candidate.cost;
            candidate.addedArea = std::max(0.0, std::abs(Area(shapePolygons)) - std::abs(Area(territory)));
            for (const int other : index.query(shape.boundingRect())) {
                if (owners.contains(other)) continue;
                if (regions[other].intersectArea(shapePolygons, shape.boundingRect()) > options_.areaTolerance)
                    candidate.above.push_back(other);
            }
            const int id = candidates.size();
            for (const int owner : owners) containing[owner].push_back(id);
            if (owners.size() == 1) singles[*owners.begin()].push_back(id);
            for (const int owner : candidate.above) blockedBy[owner].push_back(id);
            candidates.push_back(std::move(candidate));
        }
    }
    const auto plan = [&](const QString &strategy) {
        StackPlan output;
        std::vector<bool> active(candidates.size(), true), remaining(ownership_.size(), false);
        std::vector<double> best(ownership_.size(), 0.0);
        int left = 0;
        for (const auto &[color, owners] : colors) for (const int owner : owners) { remaining[owner] = true; ++left; }
        const auto cheapest = [&](int owner, const std::set<int> &extraBlocked) {
            double cost = std::numeric_limits<double>::infinity();
            for (const int id : singles[owner]) {
                if (!active[id]) continue;
                const auto &candidate = candidates[id];
                bool blocked = false;
                for (const int other : candidate.above) if (extraBlocked.contains(other)) { blocked = true; break; }
                if (!blocked) cost = std::min(cost, candidate.cost);
            }
            return cost;
        };
        for (int owner = 0; owner < remaining.size(); ++owner) if (remaining[owner]) best[owner] = cheapest(owner, {});
        const auto initialRank = [&](int id) {
            const auto &candidate = candidates[id];
            if (strategy == "current") return -double(ownerSources_[*candidate.item.owners.begin()]);
            if (strategy == "difficulty") return -candidate.complexity.score;
            return candidate.saving;
        };
        std::vector<int> ranked;
        for (int id = 0; id < candidates.size(); ++id) {
            if (strategy == "current") {
                const auto &owners = candidates[id].item.owners;
                const int source = ownerSources_[*owners.begin()];
                if (std::any_of(owners.begin(), owners.end(), [&](int owner) { return ownerSources_[owner] != source; })) continue;
            }
            ranked.push_back(id);
        }
        std::stable_sort(ranked.begin(), ranked.end(), [&](int a, int b) {
            if (initialRank(a) != initialRank(b)) return initialRank(a) > initialRank(b);
            return a < b;
        });
        while (left > 0) {
            std::vector<int> shortlist;
            for (const int id : ranked) {
                if (!active[id]) continue;
                if (!shortlist.empty() && (strategy == "current"
                    ? initialRank(id) != initialRank(shortlist.front()) : shortlist.size() >= size_t(options_.shortlistSize))) break;
                shortlist.push_back(id);
            }
            if (shortlist.empty()) {
                output.error = "No completion available for remaining visible owners";
                for (int owner = 0; owner < remaining.size() && output.unavailable.size() < 8; ++owner) {
                    if (!remaining[owner]) continue;
                    QJsonArray blockers;
                    for (const int id : containing[owner]) for (const int above : candidates[id].above)
                        if (!remaining[above] && blockers.size() < 8) blockers.append(above);
                    output.unavailable.append(QJsonObject{{"owner", owner}, {"source", ownerSources_[owner]},
                        {"area", std::abs(Area(required[owner]))}, {"native_error", penTargetError(fromPolygons(required[owner]), options_)},
                        {"candidate_count", int(containing[owner].size())}, {"committed_blockers", blockers}});
                }
                return output;
            }
            if (strategy == "savings") {
                std::map<int, double> threatened;
                for (const int id : shortlist) {
                    const auto &candidate = candidates[id];
                    std::set<int> affected;
                    for (const int owner : candidate.item.owners)
                        for (const int blocked : blockedBy[owner])
                            if (active[blocked] && candidates[blocked].item.owners.size() == 1)
                                affected.insert(*candidates[blocked].item.owners.begin());
                    for (const int owner : affected) {
                        if (!remaining[owner] || candidate.item.owners.contains(owner)) continue;
                        const double loss = cheapest(owner, candidate.item.owners) - best[owner];
                        if (loss <= 1e-9) continue;
                        for (const int alternative : singles[owner]) {
                            if (!active[alternative] || candidates[alternative].cost > best[owner] + 1e-9) continue;
                            threatened[alternative] = std::max(threatened[alternative], loss);
                            break;
                        }
                    }
                }
                std::vector<std::pair<double, int>> alternatives;
                for (const auto &[id, loss] : threatened) alternatives.push_back({-loss, id});
                std::sort(alternatives.begin(), alternatives.end());
                for (int index = 0; index < std::min(int(alternatives.size()), options_.shortlistSize); ++index) {
                    const int id = alternatives[index].second;
                    if (std::find(shortlist.begin(), shortlist.end(), id) == shortlist.end()) shortlist.push_back(id);
                }
            }
            int selected = -1;
            double selectedScore = -std::numeric_limits<double>::infinity(), selectedRegret = 0.0;
            for (const int id : shortlist) {
                const auto &candidate = candidates[id];
                std::set<int> affected;
                for (const int owner : candidate.item.owners)
                    for (const int blocked : blockedBy[owner])
                        if (active[blocked] && candidates[blocked].item.owners.size() == 1)
                            affected.insert(*candidates[blocked].item.owners.begin());
                double regret = 0.0;
                for (const int owner : affected) {
                    if (!remaining[owner] || candidate.item.owners.contains(owner)) continue;
                    regret += std::max(0.0, cheapest(owner, candidate.item.owners) - best[owner]);
                }
                double score = initialRank(id);
                if (strategy == "current") score = -candidate.cost / candidate.item.owners.size();
                if (strategy == "savings") score -= options_.lookaheadWeight * regret;
                if (selected < 0 || score > selectedScore + 1e-9
                    || (std::abs(score - selectedScore) <= 1e-9
                        && std::tuple(candidate.cost, candidate.complexity.score, candidate.above.size(), candidate.addedArea, id)
                            < std::tuple(candidates[selected].cost, candidates[selected].complexity.score,
                                candidates[selected].above.size(), candidates[selected].addedArea, selected))) {
                    selected = id; selectedScore = score; selectedRegret = regret;
                }
            }
            const auto &choice = candidates[selected];
            output.items.push_back(choice.item);
            QJsonArray owners, above;
            for (const int owner : choice.item.owners) owners.append(owner);
            for (const int owner : choice.above) above.append(owner);
            output.steps.append(QJsonObject{{"position", output.items.size() - 1}, {"owners", owners},
                {"kind", choice.kind}, {"required_above", above}, {"difficulty", choice.complexity.json()},
                {"encoded_cost", choice.cost}, {"added_area", choice.addedArea},
                {"saving", choice.saving}, {"neighbor_regret", selectedRegret}, {"rank", selectedScore}});
            std::set<int> affected;
            QJsonArray conflicts;
            int blockedCount = 0;
            for (const int owner : choice.item.owners) {
                remaining[owner] = false; --left;
                for (const int id : containing[owner]) active[id] = false;
                for (const int id : blockedBy[owner]) {
                    if (active[id]) {
                        ++blockedCount;
                        if (conflicts.size() < 8) conflicts.append(QJsonObject{{"candidate", id},
                            {"kind", candidates[id].kind}, {"committed_owner", owner},
                            {"cause", "Would cover committed visible ownership"}});
                    }
                    active[id] = false;
                    if (candidates[id].item.owners.size() == 1) affected.insert(*candidates[id].item.owners.begin());
                }
            }
            auto step = output.steps.last().toObject();
            step.insert("blocked_completions", blockedCount);
            step.insert("ordering_conflicts", conflicts);
            output.steps.replace(output.steps.size() - 1, step);
            for (const int owner : affected) if (remaining[owner]) best[owner] = cheapest(owner, {});
        }
        return output;
    };
    QStringList strategies{options_.strategy};
    if (options_.compareStrategies) strategies = {"current", "difficulty", "savings"};
    const auto repairCurrent = [&](StackPlan &plan, const QImage &actual, double threshold) {
        const auto left = reference.convertToFormat(QImage::Format_ARGB32);
        const auto right = actual.convertToFormat(QImage::Format_ARGB32);
        const int stride = left.width() + 1;
        QVector<quint32> prefix(stride * (left.height() + 1));
        for (int y = 0; y < left.height(); ++y) {
            const auto *a = reinterpret_cast<const QRgb *>(left.constScanLine(y));
            const auto *b = reinterpret_cast<const QRgb *>(right.constScanLine(y));
            quint32 row = 0;
            for (int x = 0; x < left.width(); ++x) {
                double delta = std::abs(qAlpha(a[x]) - qAlpha(b[x]));
                for (const int shift : {0, 8, 16}) {
                    const double ca = ((a[x] >> shift) & 255) * qAlpha(a[x]) / 255.0 + 255 - qAlpha(a[x]);
                    const double cb = ((b[x] >> shift) & 255) * qAlpha(b[x]) / 255.0 + 255 - qAlpha(b[x]);
                    delta = std::max(delta, std::abs(ca - cb));
                }
                row += std::ceil(delta) > threshold;
                prefix[(y + 1) * stride + x + 1] = prefix[y * stride + x + 1] + row;
            }
        }
        int restored = 0;
        for (int position = 0; position < plan.items.size(); ++position) {
            auto &item = plan.items[position];
            const int owner = *item.owners.begin();
            const int source = ownerSources_[owner];
            if (std::any_of(item.owners.begin(), item.owners.end(), [&](int id) { return ownerSources_[id] != source; })
                || item.path == baseline_[source].path) continue;
            const auto box = item.path.boundingRect().united(baseline_[source].path.boundingRect())
                .adjusted(-2, -2, 2, 2).toAlignedRect().intersected(left.rect());
            if (box.isEmpty()) continue;
            const int x0 = box.left(), x1 = box.right() + 1, y0 = box.top(), y1 = box.bottom() + 1;
            const quint32 count = prefix[y1 * stride + x1] - prefix[y0 * stride + x1]
                - prefix[y1 * stride + x0] + prefix[y0 * stride + x0];
            if (!count) continue;
            item.path = baseline_[source].path;
            auto step = plan.steps[position].toObject();
            step.insert("replaced_completion", step.value("kind"));
            step.insert("kind", "source-render-repair");
            step.insert("encoded_cost", originalCosts[source]);
            step.insert("saving", 0.0);
            step.insert("difficulty", difficulty(item.path, options_).json());
            double visibleArea = 0;
            for (const int id : item.owners) visibleArea += std::abs(Area(required[id]));
            step.insert("added_area", std::max(0.0, area(item.path, options_.flattenTolerance) - visibleArea));
            QJsonArray above;
            const auto shape = polygons(item.path, options_.flattenTolerance);
            for (const int other : index.query(item.path.boundingRect()))
                if (!item.owners.contains(other) && regions[other].intersectArea(shape, item.path.boundingRect()) > options_.areaTolerance)
                    above.append(other);
            step.insert("required_above", above);
            step.remove("rank");
            step.remove("neighbor_regret");
            plan.steps.replace(position, step);
            ++restored;
        }
        plan.renderRevertedItems += restored;
        return restored;
    };
    for (const auto &strategy : strategies) {
        const auto started = std::chrono::steady_clock::now();
        auto proposed = plan(strategy);
        QString error = proposed.error;
        if (error.isEmpty()) error = validate(proposed.items);
        QVector<gui::SvgVectorObject> unused;
        auto metrics = error.isEmpty() ? measure(proposed.items, &unused, &error) : Metrics{};
        QJsonObject renderError;
        if (error.isEmpty()) {
            auto image = renderer_(objects(proposed.items));
            renderError = imageError(reference, image);
            for (int pass = 0; strategy == "current" && !withinBudget(renderError, options_) && pass < 2; ++pass) {
                const double threshold = pass == 0 ? options_.renderMaxError : 0.0;
                if (!repairCurrent(proposed, image, threshold)) continue;
                error = validate(proposed.items);
                if (!error.isEmpty()) break;
                unused.clear();
                metrics = measure(proposed.items, &unused, &error);
                image = renderer_(objects(proposed.items));
                renderError = imageError(reference, image);
            }
            if (!withinBudget(renderError, options_)) error = "Baseline-to-derived SVG rendering budget exceeded";
        }
        const QJsonObject strategyReport{{"strategy", strategy}, {"metrics", metrics.json()},
            {"image_error", renderError}, {"error", error}, {"render_reverted_items", proposed.renderRevertedItems},
            {"unavailable_owners", proposed.unavailable},
            {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
        result.strategies.append(strategyReport);
        if (strategy != "current" && metrics.costMeasured
            && (result.candidateItems.isEmpty() || metrics.cost < result.candidateReport.value("metrics").toObject().value("cost").toDouble())) {
            result.candidateItems = proposed.items;
            result.candidateReport = strategyReport;
        }
        if (!error.isEmpty() || metrics.cost >= result.after.cost - 1e-9) continue;
        result.items = proposed.items;
        result.after = metrics;
        result.diagnostics = proposed.steps;
        result.accepted = 1;
        result.selectedStrategy = strategy;
    }
    QJsonObject rejectionCounts;
    for (const auto &[cause, count] : rejected) rejectionCounts.insert(cause, count);
    result.diagnostics.prepend(QJsonObject{{"candidate_count", int(candidates.size())},
        {"group_count", int(groups.size())}, {"hidden_owners", invisible}, {"rejected", rejectionCounts},
        {"hidden_area_penalty", false}, {"compact_fit_leeway", false}});
    const auto finalDependencies = dependencies(result.items);
    result.graph = topologicalOrder(result.items.size(), finalDependencies.edges);
    if (!finalDependencies.error.isEmpty()) result.graph.error = finalDependencies.error;
    const auto finalDomain = filledUnion(objects(result.items), options_.flattenTolerance);
    result.silhouetteDifference = area(booleanPath(domain_, finalDomain, ClipType::Xor,
                                                  options_.flattenTolerance), options_.flattenTolerance);
    return result;
}

#include "colors.inc"
#include "support_strategy.inc"
#include "absorption_detail.inc"
#include "incremental.inc"

Result Optimizer::run() {
    if (options_.strategy == "supports") return runSupports();
    if (options_.strategy == "incremental") return runIncremental();
    if (options_.strategy != "legacy") return runStack();
    Result result;
    result.items = baseline_;
    result.error = validate(result.items);
    if (!result.error.isEmpty()) return result;
    result.before = measure(result.items, &result.nativeShapes, &result.error);
    result.after = result.before;
    if (!result.error.isEmpty()) return result;
    const auto baselineNative = renderer_(result.nativeShapes);
    const auto baselinePaths = renderer_(objects(baseline_));
    const auto nativeDomain = filledUnion(result.nativeShapes, options_.flattenTolerance);
    result.decompositionError = imageError(baselinePaths, baselineNative);
    if (!withinBudget(result.decompositionError, options_)) {
        result.error = "Baseline FLS decomposition exceeds rendering budget: "
            + QString::fromUtf8(QJsonDocument(result.decompositionError).toJson(QJsonDocument::Compact));
        return result;
    }
    for (int pass = 0; pass < options_.maximumPasses; ++pass) {
        bool found = false;
        Result best;
        double bestArea = 0.0;
        int bestEdges = 0;
        QString bestKind;
        std::map<QString, int> rejects;
        QJsonArray orderingConflicts;
        proposals(result.items, [&](Proposal proposal) {
            auto error = validate(proposal.items);
            if (!error.isEmpty()) {
                ++rejects[error];
                if (error == "Dependency cycle" && orderingConflicts.size() < 16)
                    orderingConflicts.append(QJsonObject{{"kind", proposal.kind},
                        {"proposed_graph", dependencies(proposal.items).json()}});
                return;
            }
            const auto graph = dependencies(proposal.items);
            QVector<Item> ordered;
            for (const int index : graph.order) ordered.push_back(proposal.items[index]);
            QVector<gui::SvgVectorObject> native;
            const auto metrics = measure(ordered, &native, &error);
            if (!error.isEmpty()) { ++rejects["FLS decomposition: " + error]; return; }
            const bool geometryReduced = metrics.lineSegments + metrics.bezierSegments
                    < result.after.lineSegments + result.after.bezierSegments
                || metrics.holes < result.after.holes || metrics.components < result.after.components
                || metrics.drawItems < result.after.drawItems || metrics.encodedSegments < result.after.encodedSegments;
            if (!geometryReduced) { ++rejects["No geometry reduction; SVG repacking is insufficient"]; return; }
            if (metrics.cost >= result.after.cost - 1e-9) { ++rejects["No weighted complexity reduction"]; return; }
            const auto silhouetteError = area(booleanPath(nativeDomain,
                filledUnion(native, options_.flattenTolerance), ClipType::Xor, options_.flattenTolerance),
                options_.flattenTolerance);
            if (silhouetteError > options_.silhouetteTolerance) {
                ++rejects["Encoded silhouette error budget"]; return;
            }
            if (!withinBudget(imageError(baselineNative, renderer_(native)), options_)
                || !withinBudget(imageError(baselinePaths, renderer_(objects(ordered))), options_)) {
                ++rejects["Rendering error budget"]; return;
            }
            double added = 0.0;
            for (const auto &item : ordered) {
                QPainterPath required;
                for (const int owner : item.owners)
                    required = booleanPath(required, ownership_[owner], ClipType::Union, options_.flattenTolerance);
                added += differenceArea(item.path, required, options_.flattenTolerance);
            }
            const int edges = graph.edges.size();
            if (!found || metrics.cost < best.after.cost - 1e-9
                || (metrics.cost == best.after.cost && std::pair(added, edges) < std::pair(bestArea, bestEdges))) {
                found = true;
                best.items = ordered; best.nativeShapes = native; best.after = metrics;
                bestArea = added; bestEdges = edges; bestKind = proposal.kind;
            }
        });
        QJsonObject rejectionCounts;
        for (const auto &[cause, count] : rejects) rejectionCounts.insert(cause, count);
        result.diagnostics.append(QJsonObject{{"pass", pass}, {"rejected", rejectionCounts},
            {"ordering_conflicts", orderingConflicts},
            {"accepted_kind", found ? bestKind : QString()}, {"cost", found ? best.after.cost : result.after.cost}});
        if (!found) break;
        result.items = best.items; result.nativeShapes = best.nativeShapes; result.after = best.after;
        ++result.accepted;
    }
    result.graph = dependencies(result.items);
    QPainterPath complete;
    for (const auto &item : result.items)
        complete = booleanPath(complete, item.path, ClipType::Union, options_.flattenTolerance);
    result.silhouetteDifference = area(booleanPath(domain_, complete, ClipType::Xor,
                                                   options_.flattenTolerance), options_.flattenTolerance);
    result.nativeSilhouetteDifference = area(booleanPath(nativeDomain,
        filledUnion(result.nativeShapes, options_.flattenTolerance), ClipType::Xor,
        options_.flattenTolerance), options_.flattenTolerance);
    return result;
}

} // namespace opaque
