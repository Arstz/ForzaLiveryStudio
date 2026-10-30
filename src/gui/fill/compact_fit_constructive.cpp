#include "compact_fit_constructive.h"
#include "compact_fit_catalog_internal.h"
#include "compact_fit_coordinates.h"
#include "polygon_mesh.h"

#include <clipper2/clipper.h>
#include <QElapsedTimer>
#include <QLineF>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gui::compact {
namespace {

constexpr int kConstructionMilliseconds = 10000;
constexpr int kTriangleShapeId = 103;
constexpr int kSquareShapeId = 101;
constexpr int kRawVerificationLimit = 8;
constexpr int kSeamExpansionAttempts = 12;
constexpr int kSeamClearanceMultiplier = 4;
constexpr double kParallelWidthTolerance = 1e-8;
constexpr std::array<double, 6> kConstructionTolerances{0.25, 0.125, 0.05, 0.02, 0.005, 0.0};

void requireConstruction(bool condition, const QString &message) {
    if (!condition)
        throw std::runtime_error(message.toStdString());
}

QVector<catalog::Polygons> constructiveComponents(const catalog::Polygons &polygons) {
    QVector<catalog::Polygons> components;
    for (const auto &polygon : polygons)
        if (catalog::signedArea(polygon) > 0)
            components.push_back({polygon});
    for (const auto &polygon : polygons) {
        if (catalog::signedArea(polygon) >= 0)
            continue;
        int owner = -1;
        double smallest = std::numeric_limits<double>::infinity();
        for (int index = 0; index < components.size(); ++index) {
            const double area = catalog::signedArea(components[index].front());
            if (area < smallest && components[index].front().containsPoint(polygon.front(), Qt::OddEvenFill)) {
                smallest = area;
                owner = index;
            }
        }
        requireConstruction(owner >= 0, QStringLiteral("Cannot assign a hole to a component"));
        components[owner].push_back(polygon);
    }
    return components;
}

catalog::FillResult constructAttempt(const catalog::Region &authored,
    const QVector<catalog::Primitive> &primitives, const FillOptions &options,
    const QString &mode, double epsilon, const std::function<bool()> &cancelled) {
    const bool slabs = mode == QStringLiteral("slabs");
    const bool rawMode = mode == QStringLiteral("rawedges") || slabs;
    catalog::FillResult fitted;
    QVector<QPolygonF> idealTriangles;
    double maximumVertexError = 0;
    const auto appendPlacement = [&](const PenPlacement &placement) {
        requireConstruction(fitted.fill.placements.size() < options.shapeBudget,
            QStringLiteral("Constructive cover exceeds the shape budget"));
        fitted.fill.placements.push_back(placement);
    };
    auto region = catalog::leewayAdjustedRegion(authored, options.leeway, options.boundaryAllowance);
    if (epsilon > 0) {
        catalog::Polygons simplified;
        for (const auto &polygon : region.required) {
            Clipper2Lib::PathD path;
            for (const auto &point : polygon)
                path.emplace_back(point.x(), point.y());
            const auto reduced = Clipper2Lib::SimplifyPath(path, epsilon, true);
            QPolygonF result;
            for (const auto &point : reduced)
                result.push_back({point.x, point.y});
            if (result.size() >= 3)
                simplified.push_back(result);
        }
        region.required = catalog::unite(simplified);
    }
    const auto nativeTriangle = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
        return primitive.shape.shapeId == kTriangleShapeId;
    });
    requireConstruction(nativeTriangle != primitives.end(), QStringLiteral("Native triangle is unavailable"));
    const auto appendRawTriangle = [&](const QPolygonF &target) {
        const auto &source = nativeTriangle->shape.contours.front();
        std::array<int, 3> order{0, 1, 2};
        double bestError = std::numeric_limits<double>::infinity();
        QTransform best;
        do {
            const auto transform = catalog::emittedTransform(catalog::affineFromAnchors(
                {source[0], source[1], source[2]}, {target[order[0]], target[order[1]], target[order[2]]}));
            double error = 0;
            for (int index = 0; index < 3; ++index) {
                const auto delta = transform.map(source[index]) - target[order[index]];
                error = std::max(error, QPointF::dotProduct(delta, delta));
            }
            if (error < bestError) {
                bestError = error;
                best = transform;
            }
        } while (std::next_permutation(order.begin(), order.end()));
        PenPlacement result;
        result.shapeId = kTriangleShapeId;
        result.transform = best;
        appendPlacement(result);
        idealTriangles.push_back(target);
        maximumVertexError = std::max(maximumVertexError, std::sqrt(bestError));
    };
    if (slabs) {
        QVector<double> levels;
        for (const auto &polygon : region.required)
            for (const auto &point : polygon)
                levels.push_back(point.y());
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
        struct Crossing { QPointF start, end; double x; };
        const auto at = [](const Crossing &edge, double y) {
            return QPointF(edge.start.x() + (edge.end.x() - edge.start.x())
                * ((y - edge.start.y()) / (edge.end.y() - edge.start.y())), y);
        };
        const auto square = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
            return primitive.shape.shapeId == kSquareShapeId;
        });
        requireConstruction(square != primitives.end(), QStringLiteral("Native square is unavailable"));
        for (int slab = 1; slab < levels.size(); ++slab) {
            requireConstruction(!cancelled(), QStringLiteral("Construction time budget reached"));
            const double top = levels[slab - 1], bottom = levels[slab], middle = (top + bottom) * 0.5;
            QVector<Crossing> crossings;
            for (const auto &polygon : region.required)
                for (int edge = 0; edge < polygon.size(); ++edge) {
                    const auto first = polygon[edge], second = polygon[(edge + 1) % polygon.size()];
                    if (middle > std::min(first.y(), second.y()) && middle < std::max(first.y(), second.y())) {
                        Crossing crossing{first, second, 0};
                        crossing.x = at(crossing, middle).x();
                        crossings.push_back(crossing);
                    }
                }
            std::sort(crossings.begin(), crossings.end(), [](const auto &a, const auto &b) { return a.x < b.x; });
            requireConstruction(crossings.size() % 2 == 0, QStringLiteral("Unpaired region boundary crossing"));
            for (int index = 0; index < crossings.size(); index += 2) {
                const auto a = at(crossings[index], top), b = at(crossings[index + 1], top);
                const auto c = at(crossings[index + 1], bottom), d = at(crossings[index], bottom);
                const double firstWidth = b.x() - a.x(), secondWidth = c.x() - d.x();
                if (firstWidth <= 0 && secondWidth <= 0)
                    continue;
                if (std::abs(firstWidth - secondWidth) < kParallelWidthTolerance && firstWidth > 0) {
                    const QPolygonF target({a, b, c, d});
                    const auto bounds = square->shape.bounds;
                    const auto transform = catalog::emittedTransform(catalog::affineFromAnchors(
                        {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft()}, {a, b, d}));
                    const QPolygonF source({bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()});
                    for (int vertex = 0; vertex < 4; ++vertex)
                        maximumVertexError = std::max(maximumVertexError, QLineF(transform.map(source[vertex]), target[vertex]).length());
                    PenPlacement placement;
                    placement.shapeId = kSquareShapeId;
                    placement.transform = transform;
                    appendPlacement(placement);
                    idealTriangles.push_back(target);
                } else {
                    if (firstWidth > 0)
                        appendRawTriangle({a, b, d});
                    if (secondWidth > 0)
                        appendRawTriangle({b, c, d});
                }
            }
        }
    } else {
        const auto triangle = nativeTriangle;
        const auto components = constructiveComponents(region.required);
        for (const auto &component : components) {
            const auto center = component.front().boundingRect().center();
            QTransform toLocal;
            toLocal.translate(-center.x(), -center.y());
            QTransform toWorld;
            toWorld.translate(center.x(), center.y());
            PolygonMeshRequest meshRequest;
            meshRequest.mergeSquares = false;
            meshRequest.sources.triangle = triangle->shape.contours.front();
            for (const auto &polygon : component)
                meshRequest.contours.push_back(toLocal.map(polygon));
            const auto mesh = meshPolygon(meshRequest, cancelled);
            requireConstruction(mesh.error.isEmpty(), mesh.error);
            for (const auto &placement : mesh.placements) {
                requireConstruction(!cancelled(), QStringLiteral("Construction time budget reached"));
                auto piece = region;
                piece.required = catalog::mapped(triangle->shape, placement.transform * toWorld);
                if (piece.required.isEmpty() || piece.required.front().size() != 3)
                    continue;
                if (rawMode) {
                    const auto &target = piece.required.front();
                    appendRawTriangle(target);
                } else {
                    const auto candidates = catalog::completeCover(piece, primitives, cancelled);
                    for (const auto &candidate : candidates)
                        appendPlacement(candidate.placement);
                }
            }
        }
    }
    const bool rawAccepted = rawMode && fitted.fill.placements.size() <= kRawVerificationLimit
        && verifyEmittedRegion(authored, primitives, options, &fitted);
    if (rawMode && !rawAccepted) {
        const auto square = std::find_if(primitives.begin(), primitives.end(), [](const auto &primitive) {
            return primitive.shape.shapeId == kSquareShapeId;
        });
        requireConstruction(square != primitives.end(), QStringLiteral("Native square is unavailable"));
        catalog::Polygons coverage;
        for (const auto &placement : fitted.fill.placements) {
            const auto primitive = std::find_if(primitives.begin(), primitives.end(), [&](const auto &candidate) {
                return candidate.shape.shapeId == placement.shapeId;
            });
            coverage += catalog::mapped(primitive->shape, placement.transform);
        }
        coverage = catalog::unite(coverage);
        auto missing = catalog::subtract(region.required, coverage);
        fitted.diagnostics.insert("rawCount", fitted.fill.placements.size());
        fitted.diagnostics.insert("maximumVertexError", maximumVertexError);
        const double radius = maximumVertexError + catalog::kVerificationClearance * kSeamClearanceMultiplier;
        QSet<QString> edges;
        for (const auto &polygon : idealTriangles) {
            for (int edge = 0; edge < polygon.size(); ++edge) {
                requireConstruction(!cancelled(), QStringLiteral("Construction time budget reached"));
                auto start = polygon[edge], end = polygon[(edge + 1) % polygon.size()];
                if (start.x() > end.x() || (start.x() == end.x() && start.y() > end.y()))
                    std::swap(start, end);
                const QString key = QStringLiteral("%1,%2,%3,%4").arg(start.x(), 0, 'f', 6).arg(start.y(), 0, 'f', 6)
                    .arg(end.x(), 0, 'f', 6).arg(end.y(), 0, 'f', 6);
                if (edges.contains(key))
                    continue;
                edges.insert(key);
                const double length = QLineF(start, end).length();
                if (length <= 0)
                    continue;
                const auto horizontal = (end - start) / length;
                const QPointF vertical(-horizontal.y(), horizontal.x());
                const auto rectangle = [&](double padding) {
                    return QPolygonF({start - horizontal * padding - vertical * padding,
                        end + horizontal * padding - vertical * padding,
                        end + horizontal * padding + vertical * padding,
                        start - horizontal * padding + vertical * padding});
                };
                const auto required = catalog::unite({rectangle(radius)});
                if (catalog::intersect(missing, required).isEmpty())
                    continue;
                bool covered = false;
                for (int attempt = 0; attempt < kSeamExpansionAttempts; ++attempt) {
                    const auto target = rectangle(std::ldexp(radius, attempt + 1));
                    const auto bounds = square->shape.bounds;
                    const auto transform = catalog::emittedTransform(catalog::affineFromAnchors(
                        {bounds.topLeft(), bounds.topRight(), bounds.bottomLeft()}, {target[0], target[1], target[3]}));
                    const auto support = catalog::mapped(square->shape, transform);
                    if (catalog::subtract(required, support).isEmpty()
                        && catalog::subtract(support, region.permitted).isEmpty()) {
                        PenPlacement placement;
                        placement.shapeId = kSquareShapeId;
                        placement.transform = transform;
                        appendPlacement(placement);
                        missing = catalog::subtract(missing, support);
                        covered = true;
                        break;
                    }
                }
                requireConstruction(covered, QStringLiteral("Cannot cover a native rounding seam"));
            }
        }
        fitted.diagnostics.insert("remainingSeamArea", catalog::area(missing));

    }

    requireConstruction(verifyEmittedRegion(authored, primitives, options, &fitted),
        QStringLiteral("Constructive cover failed emitted geometry verification"));
    fitted.fill.shapeLimit = options.shapeBudget;
    fitted.diagnostics.insert(QStringLiteral("constructionMode"), mode);
    fitted.diagnostics.insert(QStringLiteral("constructionTolerance"), epsilon);

    return fitted;
}

} // namespace

catalog::FillResult constructiveCover(const catalog::Region &region,
    const QVector<catalog::Primitive> &primitives, const FillOptions &options,
    const std::function<bool()> &cancelled) {
    QElapsedTimer timer;
    timer.start();
    const auto stop = [&] {
        return (cancelled && cancelled()) || timer.elapsed() >= kConstructionMilliseconds;
    };
    catalog::FillResult result;
    for (const double tolerance : kConstructionTolerances) {
        for (const auto &mode : {QStringLiteral("centered"), QStringLiteral("rawedges"), QStringLiteral("slabs")}) {
            if (stop())
                break;
            try {
                result = constructAttempt(region, primitives, options, mode,
                    std::min(tolerance, options.inwardAllowance * 0.5), stop);
                if (stop())
                    break;
                result.diagnostics.insert(QStringLiteral("constructionMilliseconds"), timer.elapsed());

                return result;
            } catch (const std::exception &error) {
                result.fill.error = QString::fromUtf8(error.what());
                result.fill.placements.clear();
            }
        }
        if (stop())
            break;
    }
    result.fill.cancelled = cancelled && cancelled();
    if (result.fill.cancelled)
        result.fill.placements.clear();
    if (result.fill.error.isEmpty() && result.fill.placements.isEmpty())
        result.fill.error = QStringLiteral("Constructive cover exhausted its time allowance");
    result.diagnostics.insert(QStringLiteral("constructionMilliseconds"), timer.elapsed());

    return result;
}

} // namespace gui::compact
