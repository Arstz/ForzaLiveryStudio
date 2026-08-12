#include "curve_fill.h"

#include "differential_cover.h"
#include "region_extract.h"
#include "region_fill.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QMutex>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gui {
namespace {

constexpr double kTemplateRasterScale = 2.0;
constexpr int kTemplateRasterMargin = 2;
constexpr double kBoundaryVertexTolerance = 1e-5;
constexpr double kMaximumHardVertexSnapDistance = 4.0;
constexpr int kCurveTemplateVersion = 1;
constexpr int kMaximumCurveShapeId = 65535;

QStringList curveShapeAssetPaths() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/curve_shapes.json")),
        QDir(cwd).filePath(QStringLiteral("assets/curve_shapes.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/curve_shapes.json")),
    };
}

QVector<int> loadCurveShapeIdsFromFile(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not open curve shape catalog: %1")
                         .arg(path);
        }
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid curve shape catalog: %1")
                         .arg(parseError.errorString());
        }
        return {};
    }
    const QJsonArray values = document.object()
        .value(QStringLiteral("shape_ids")).toArray();
    if (values.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "curve shape catalog must contain a non-empty shape_ids array");
        }
        return {};
    }
    QVector<int> result;
    QSet<int> seen;
    result.reserve(values.size());
    for (qsizetype index = 0; index < values.size(); ++index) {
        const QJsonValue value = values[index];
        const int shapeId = value.toInt(-1);
        if (!value.isDouble()
            || value.toDouble(-1.0) != static_cast<double>(shapeId)
            || shapeId <= 0 || shapeId > kMaximumCurveShapeId
            || seen.contains(shapeId)) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "curve shape catalog entry %1 is invalid or duplicated")
                             .arg(index);
            }
            return {};
        }
        seen.insert(shapeId);
        result.push_back(shapeId);
    }
    if (error != nullptr) {
        error->clear();
    }
    return result;
}

QString existingCurveCatalogPath() {
    for (const QString &path : curveShapeAssetPaths()) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

QString curveTemplatePath(const QString &assetDirectory, int shapeId) {
    return QDir(assetDirectory).filePath(
        QStringLiteral("curve_templates/%1.json").arg(shapeId));
}

QMutex &catalogMutex() {
    static QMutex mutex;
    return mutex;
}

CurveFillCatalog &catalogCache() {
    static CurveFillCatalog catalog;
    return catalog;
}

quint64 &catalogGeneration() {
    static quint64 generation = 0;
    return generation;
}

struct RasterizedTemplate {
    QImage image;
    QTransform sourceToImage;
    QTransform imageToSource;
};

RasterizedTemplate rasterizedTemplate(const QPainterPath &silhouette) {
    RasterizedTemplate result;
    const QRectF bounds = silhouette.boundingRect();
    const int width = std::max(
        1, static_cast<int>(std::ceil(bounds.width() * kTemplateRasterScale))
            + kTemplateRasterMargin * 2);
    const int height = std::max(
        1, static_cast<int>(std::ceil(bounds.height() * kTemplateRasterScale))
            + kTemplateRasterMargin * 2);
    result.sourceToImage.translate(
        kTemplateRasterMargin - bounds.left() * kTemplateRasterScale,
        kTemplateRasterMargin - bounds.top() * kTemplateRasterScale);
    result.sourceToImage.scale(kTemplateRasterScale, kTemplateRasterScale);
    bool invertible = false;
    result.imageToSource = result.sourceToImage.inverted(&invertible);
    if (!invertible) {
        return {};
    }
    result.image = QImage(width, height, QImage::Format_Grayscale8);
    result.image.fill(0);
    QPainter painter(&result.image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.setTransform(result.sourceToImage);
    painter.drawPath(silhouette);
    painter.end();

    return result;
}

QPainterPath highPrecisionTrace(const QImage &image) {
    if (image.isNull()) {
        return {};
    }
    std::vector<std::uint8_t> mask(
        static_cast<size_t>(image.width()) * image.height(), 0);
    for (int y = 0; y < image.height(); ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            mask[static_cast<size_t>(y) * image.width() + x] = line[x] > 0;
        }
    }
    RegionExtractionParams traceOptions;
    traceOptions.traceSpeckle = 0;
    traceOptions.traceOptTolerance = 0.0;

    return traceMaskToPath(mask, image.width(), image.height(),
                           image.rect(), traceOptions);
}

double polygonArea(const QPolygonF &polygon) {
    double twiceArea = 0.0;
    for (int index = 0; index < polygon.size(); ++index) {
        const QPointF &left = polygon[index];
        const QPointF &right = polygon[(index + 1) % polygon.size()];
        twiceArea += left.x() * right.y() - right.x() * left.y();
    }
    return std::abs(twiceArea) * 0.5;
}

QPainterPath largestTracedSubpath(const QPainterPath &source) {
    QVector<QPainterPath> subpaths;
    QPainterPath current;
    for (int index = 0; index < source.elementCount(); ++index) {
        const QPainterPath::Element element = source.elementAt(index);
        if (element.type == QPainterPath::MoveToElement) {
            if (!current.isEmpty()) {
                current.closeSubpath();
                subpaths.push_back(current);
            }
            current = {};
            current.moveTo(element.x, element.y);
        } else if (element.type == QPainterPath::LineToElement) {
            current.lineTo(element.x, element.y);
        } else if (element.type == QPainterPath::CurveToElement
                   && index + 2 < source.elementCount()) {
            const QPainterPath::Element control2 = source.elementAt(index + 1);
            const QPainterPath::Element end = source.elementAt(index + 2);
            current.cubicTo(element.x, element.y,
                            control2.x, control2.y,
                            end.x, end.y);
            index += 2;
        }
    }
    if (!current.isEmpty()) {
        current.closeSubpath();
        subpaths.push_back(current);
    }
    const auto area = [](const QPainterPath &path) {
        double result = 0.0;
        for (const QPolygonF &polygon : path.toFillPolygons()) {
            result += polygonArea(polygon);
        }
        return result;
    };
    const auto largest = std::max_element(
        subpaths.cbegin(), subpaths.cend(),
        [&](const QPainterPath &left, const QPainterPath &right) {
            return area(left) < area(right);
        });
    return largest == subpaths.cend() ? QPainterPath{} : *largest;
}

double pointSegmentDistance(const QPointF &point,
                            const QPointF &start,
                            const QPointF &end) {
    const QPointF segment = end - start;
    const double lengthSquared = QPointF::dotProduct(segment, segment);
    if (lengthSquared <= 0.0) {
        return QLineF(point, start).length();
    }
    const double parameter = std::clamp(
        QPointF::dotProduct(point - start, segment) / lengthSquared,
        0.0, 1.0);
    return QLineF(point, start + segment * parameter).length();
}

bool onPrimitiveBoundary(const QPointF &point,
                         const QVector<QPolygonF> &contours) {
    for (const QPolygonF &contour : contours) {
        for (int index = 0; index < contour.size(); ++index) {
            const QPointF &start = contour[index];
            const QPointF &end = contour[(index + 1) % contour.size()];
            if (pointSegmentDistance(point, start, end)
                <= kBoundaryVertexTolerance) {
                return true;
            }
        }
    }
    return false;
}

QVector<QPointF> boundaryTriangleVertices(
    const ShapeGeometry &geometry,
    const PenPrimitive &primitive) {
    QVector<QPointF> result;
    const auto append = [&](const QPointF &point) {
        if (!onPrimitiveBoundary(point, primitive.contours)) {
            return;
        }
        const bool duplicate = std::any_of(
            result.cbegin(), result.cend(), [&](const QPointF &candidate) {
                return QLineF(candidate, point).length()
                    <= kBoundaryVertexTolerance;
            });
        if (!duplicate) {
            result.push_back(point);
        }
    };
    for (const ShapeTriangle &triangle : geometry.triangles) {
        append(triangle.p0);
        append(triangle.p1);
        append(triangle.p2);
    }
    return result;
}

int maximumSoftRun(const QVector<PenPoint> &points) {
    if (points.isEmpty()) {
        return 0;
    }
    int maximum = 0;
    int current = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (const PenPoint &point : points) {
            if (point.kind == PenPointKind::Soft) {
                maximum = std::max(maximum, ++current);
            } else {
                current = 0;
            }
        }
    }
    return std::min(maximum, static_cast<int>(points.size()));
}

bool validateCurveTemplate(const PenPrimitive &primitive, QString *error) {
    const int hardCount = static_cast<int>(std::count_if(
        primitive.curvePoints.cbegin(), primitive.curvePoints.cend(),
        [](const PenPoint &point) {
            return point.kind == PenPointKind::Hard;
        }));
    const int softCount = primitive.curvePoints.size() - hardCount;
    if (primitive.curvePoints.size() < 3
        || (softCount > 0 && hardCount < 2)
        || maximumSoftRun(primitive.curvePoints) > 2) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "shape %1 curve template has invalid hard/soft point structure")
                         .arg(primitive.shapeId);
        }
        return false;
    }
    const PenContour contour = buildPenContour(primitive.curvePoints);
    if (!contour.valid()) {
        if (error != nullptr) {
            *error = QStringLiteral("shape %1 curve template is invalid: %2")
                         .arg(primitive.shapeId)
                         .arg(contour.error);
        }
        return false;
    }
    return true;
}

bool loadCurveTemplate(const QString &path,
                       PenPrimitive *primitive,
                       QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("curve template is missing: %1").arg(path);
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid curve template %1: %2")
                         .arg(path, parseError.errorString());
        }
        return false;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt(-1)
            != kCurveTemplateVersion
        || root.value(QStringLiteral("shape_id")).toInt(-1)
            != primitive->shapeId) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "curve template %1 has an incompatible version or shape id")
                         .arg(path);
        }
        return false;
    }
    const QJsonObject storedBounds = root.value(
        QStringLiteral("source_bounds")).toObject();
    const QRectF sourceBounds = primitive->silhouette.boundingRect();
    const auto sameCoordinate = [](double left, double right) {
        return std::isfinite(left)
            && std::abs(left - right) <= 1e-6;
    };
    if (!sameCoordinate(storedBounds.value(QStringLiteral("x")).toDouble(
                            std::numeric_limits<double>::quiet_NaN()),
                        sourceBounds.x())
        || !sameCoordinate(storedBounds.value(QStringLiteral("y")).toDouble(
                                std::numeric_limits<double>::quiet_NaN()),
                            sourceBounds.y())
        || !sameCoordinate(storedBounds.value(QStringLiteral("width")).toDouble(
                                std::numeric_limits<double>::quiet_NaN()),
                            sourceBounds.width())
        || !sameCoordinate(storedBounds.value(QStringLiteral("height")).toDouble(
                                std::numeric_limits<double>::quiet_NaN()),
                            sourceBounds.height())) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "curve template %1 does not match the current shape geometry; "
                "regenerate it with -Force")
                         .arg(path);
        }
        return false;
    }
    const QJsonArray points = root.value(QStringLiteral("points")).toArray();
    primitive->curvePoints.clear();
    primitive->curvePoints.reserve(points.size());
    for (qsizetype index = 0; index < points.size(); ++index) {
        const QJsonObject object = points[index].toObject();
        const double x = object.value(QStringLiteral("x")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const double y = object.value(QStringLiteral("y")).toDouble(
            std::numeric_limits<double>::quiet_NaN());
        const QString kind = object.value(QStringLiteral("kind")).toString();
        if (!std::isfinite(x) || !std::isfinite(y)
            || (kind != QStringLiteral("hard")
                && kind != QStringLiteral("soft"))) {
            if (error != nullptr) {
                *error = QStringLiteral("curve template %1 point %2 is invalid")
                             .arg(path)
                             .arg(index);
            }
            return false;
        }
        primitive->curvePoints.push_back({
            QPointF(x, y), kind == QStringLiteral("hard")
                ? PenPointKind::Hard : PenPointKind::Soft});
    }
    if (!validateCurveTemplate(*primitive, error)) {
        return false;
    }
    primitive->curveSegments = buildPenContour(
        primitive->curvePoints).segments;
    return true;
}

bool writeCurveTemplate(const QString &path,
                        const PenPrimitive &primitive,
                        QString *error) {
    QJsonObject root;
    root.insert(QStringLiteral("version"), kCurveTemplateVersion);
    root.insert(QStringLiteral("shape_id"), primitive.shapeId);
    root.insert(QStringLiteral("raster_scale"), kTemplateRasterScale);
    QJsonArray points;
    for (const PenPoint &point : primitive.curvePoints) {
        QJsonObject object;
        object.insert(QStringLiteral("x"), point.position.x());
        object.insert(QStringLiteral("y"), point.position.y());
        object.insert(QStringLiteral("kind"),
                      point.kind == PenPointKind::Hard
                          ? QStringLiteral("hard")
                          : QStringLiteral("soft"));
        points.push_back(object);
    }
    root.insert(QStringLiteral("points"), points);
    const QRectF bounds = primitive.silhouette.boundingRect();
    QJsonObject sourceBounds;
    sourceBounds.insert(QStringLiteral("x"), bounds.x());
    sourceBounds.insert(QStringLiteral("y"), bounds.y());
    sourceBounds.insert(QStringLiteral("width"), bounds.width());
    sourceBounds.insert(QStringLiteral("height"), bounds.height());
    root.insert(QStringLiteral("source_bounds"), sourceBounds);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0
        || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("could not save curve template: %1")
                         .arg(path);
        }
        return false;
    }
    return true;
}

PenPrimitive generateCurvePrimitive(int shapeId,
                                    const ShapeGeometry &source,
                                    QString *error) {
    PenPrimitive primitive = buildPenPrimitive(shapeId, source);
    const RasterizedTemplate raster = rasterizedTemplate(primitive.silhouette);
    const QPainterPath traced = largestTracedSubpath(
        highPrecisionTrace(raster.image));
    RegionPenConversionOptions options;
    options.mergeTolerance = kCurveContourMergeTolerance;
    options.maximumDssim = kCurveContourMaximumDssim;
    const QVector<QPointF> preferredVertices = boundaryTriangleVertices(
        source, primitive);
    options.preferredHardPoints.reserve(preferredVertices.size());
    for (const QPointF &point : preferredVertices) {
        options.preferredHardPoints.push_back(
            raster.sourceToImage.map(point));
    }
    options.maximumPreferredHardPointDistance =
        kMaximumHardVertexSnapDistance * kTemplateRasterScale;
    const RegionPenConversionResult conversion =
        optimizeCurveRegionOutline(traced, options);
    if (!conversion.valid()) {
        if (error != nullptr) {
            *error = QStringLiteral("shape %1 curve path failed: %2")
                         .arg(shapeId)
                         .arg(conversion.error);
        }
        return {};
    }
    primitive.curvePoints.reserve(conversion.points.size());
    for (const PenPoint &point : conversion.points) {
        primitive.curvePoints.push_back({
            raster.imageToSource.map(point.position), point.kind});
    }
    if (!validateCurveTemplate(primitive, error)) {
        return {};
    }
    primitive.curveSegments = buildPenContour(primitive.curvePoints).segments;
    return primitive;
}

} // namespace

CurveFillCatalog buildCurveFillCatalog(
    const ShapeGeometryStore &geometry,
    const CurveCatalogProgress &progress,
    const std::function<bool()> &cancelled) {
    CurveFillCatalog catalog;
    const QString catalogPath = existingCurveCatalogPath();
    if (catalogPath.isEmpty()) {
        catalog.error = QStringLiteral("curve_shapes.json was not found");
        return catalog;
    }
    QString catalogError;
    const QVector<int> shapeIds = loadCurveShapeIdsFromFile(
        catalogPath, &catalogError);
    if (shapeIds.isEmpty()) {
        catalog.error = catalogError;
        return catalog;
    }
    const QString assetDirectory = QFileInfo(catalogPath).absolutePath();
    catalog.meshes.reserve(shapeIds.size());
    catalog.primitives.reserve(shapeIds.size());
    for (int shapeIndex = 0; shapeIndex < shapeIds.size(); ++shapeIndex) {
        if (cancelled && cancelled()) {
            catalog.error = QStringLiteral("Curve shape catalog preparation cancelled");
            return catalog;
        }
        if (progress) {
            progress(QStringLiteral("Loading curve templates"),
                     shapeIndex, shapeIds.size());
        }
        const int shapeId = shapeIds[shapeIndex];
        const ShapeGeometry *source = geometry.shape(shapeId);
        if (source == nullptr) {
            catalog.error = QStringLiteral("shape %1 is unavailable").arg(shapeId);
            return catalog;
        }
        cover::ShapeMesh mesh = cover::buildShapeMesh(shapeId, *source);
        if (!mesh.valid()) {
            catalog.error = mesh.error.isEmpty()
                ? QStringLiteral("shape %1 mesh is invalid").arg(shapeId)
                : mesh.error;
            return catalog;
        }
        PenPrimitive primitive = buildPenPrimitive(shapeId, *source);
        if (!loadCurveTemplate(
                curveTemplatePath(assetDirectory, shapeId),
                &primitive, &catalog.error)) {
            return catalog;
        }
        catalog.meshes.push_back(std::move(mesh));
        catalog.primitives.push_back(std::move(primitive));
    }
    if (progress) {
        progress(QStringLiteral("Loading curve templates"),
                 shapeIds.size(), shapeIds.size());
    }

    return catalog;
}

CurveFillCatalog cachedCurveFillCatalog(
    const ShapeGeometryStore &geometry,
    const CurveCatalogProgress &progress,
    const std::function<bool()> &cancelled) {
    quint64 generation = 0;
    {
        QMutexLocker locker(&catalogMutex());
        if (catalogCache().valid()) {
            if (progress) {
                progress(QStringLiteral("Loading curve templates"),
                         catalogCache().primitives.size(),
                         catalogCache().primitives.size());
            }
            return catalogCache();
        }
        generation = catalogGeneration();
    }

    CurveFillCatalog built = buildCurveFillCatalog(
        geometry, progress, cancelled);
    if (!built.valid()) {
        return built;
    }
    QMutexLocker locker(&catalogMutex());
    if (generation == catalogGeneration()) {
        catalogCache() = built;
    }

    return built;
}

void invalidateCurveFillCatalog() {
    QMutexLocker locker(&catalogMutex());
    ++catalogGeneration();
    catalogCache() = {};
}

CurveTemplateGenerationResult generateCurveFillTemplates(
    const ShapeGeometryStore &geometry,
    const QString &assetDirectory,
    bool force,
    const CurveCatalogProgress &progress,
    const std::function<bool()> &cancelled) {
    CurveTemplateGenerationResult result;
    const QString catalogPath = QDir(assetDirectory).filePath(
        QStringLiteral("curve_shapes.json"));
    const QVector<int> shapeIds = loadCurveShapeIdsFromFile(
        catalogPath, &result.error);
    if (shapeIds.isEmpty()) {
        return result;
    }
    const QString templateDirectory = QDir(assetDirectory).filePath(
        QStringLiteral("curve_templates"));
    if (!QDir().mkpath(templateDirectory)) {
        result.error = QStringLiteral(
            "could not create curve template directory: %1")
                           .arg(templateDirectory);
        return result;
    }
    for (int shapeIndex = 0; shapeIndex < shapeIds.size(); ++shapeIndex) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        if (progress) {
            progress(QStringLiteral("Generating curve templates"),
                     shapeIndex, shapeIds.size());
        }
        const int shapeId = shapeIds[shapeIndex];
        const QString path = curveTemplatePath(assetDirectory, shapeId);
        if (!force && QFile::exists(path)) {
            ++result.skipped;
            continue;
        }
        const ShapeGeometry *source = geometry.shape(shapeId);
        if (source == nullptr) {
            result.error = QStringLiteral("shape %1 is unavailable").arg(shapeId);
            return result;
        }
        PenPrimitive primitive = generateCurvePrimitive(
            shapeId, *source, &result.error);
        if (primitive.curvePoints.isEmpty()
            || !writeCurveTemplate(path, primitive, &result.error)) {
            return result;
        }
        ++result.generated;
    }
    if (progress) {
        progress(QStringLiteral("Generating curve templates"),
                 shapeIds.size(), shapeIds.size());
    }
    return result;
}

QVector<PenPrimitive> buildCurvePrimitiveCatalog(
    const ShapeGeometryStore &geometry,
    QString *error) {
    const CurveFillCatalog catalog = buildCurveFillCatalog(geometry);
    if (error != nullptr) {
        *error = catalog.error;
    }

    return catalog.primitives;
}

} // namespace gui
