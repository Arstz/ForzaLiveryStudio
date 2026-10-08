#include "compact_fit_catalog_internal.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace gui::catalog {
namespace {

constexpr int kConfigurationVersion = 1;
constexpr int kSquareShapeId = 101;
constexpr int kCircleShapeId = 102;
constexpr int kTriangleShapeId = 103;
constexpr double kOpaqueEpsilon = 1e-10;
constexpr std::array<const char *, kShapeTaskCount> kTaskNames = {
    "curves", "interior", "corners", "straight_edges", "corner_triangles",
    "replacements", "cutout_replacements", "group_replacements", "whole_region",
    "gap_patches", "residual_repair", "patch_consolidation", "exact_replacements"};

void requireKeys(const QJsonObject &object, const QStringList &allowed, const QString &field) {
    for (auto entry = object.begin(); entry != object.end(); ++entry)
        if (!allowed.contains(entry.key()))
            throw std::runtime_error(QStringLiteral("Unknown %1 field: %2").arg(field, entry.key()).toStdString());
}

QVector<int> readShapeIds(const QJsonValue &value, const ShapeGeometryStore &geometry,
                         const QString &field) {
    QVector<int> result;
    QSet<int> seen;

    if (!value.isArray())
        throw std::runtime_error(QStringLiteral("%1 must be an array of shape IDs").arg(field).toStdString());
    for (const auto &entry : value.toArray()) {
        const int id = entry.toInt(-1);
        if (!entry.isDouble() || id < 0 || entry.toDouble() != id)
            throw std::runtime_error(QStringLiteral("%1 contains an invalid shape ID").arg(field).toStdString());
        if (seen.contains(id))
            throw std::runtime_error(QStringLiteral("%1 contains duplicate shape ID %2").arg(field).arg(id).toStdString());
        if (!geometry.shape(id))
            throw std::runtime_error(QStringLiteral("%1 references unavailable shape ID %2").arg(field).arg(id).toStdString());
        seen.insert(id);
        result.push_back(id);
    }

    return result;
}

bool selectionFlag(const QJsonObject &selection, const QString &field) {
    const auto value = selection.value(field);
    if (!value.isUndefined() && !value.isBool())
        throw std::runtime_error(QStringLiteral("%1 must be a boolean").arg(field).toStdString());

    return value.toBool();
}

QVector<int> taskShapeIds(const QJsonValue &value, const QVector<Primitive> &primitives,
                         const ShapeGeometryStore &geometry, const QString &field) {
    QVector<int> result;
    if (value.isArray())
        return readShapeIds(value, geometry, field);
    if (!value.isObject())
        throw std::runtime_error(QStringLiteral("%1 must be an array or a shape selector").arg(field).toStdString());
    const auto selection = value.toObject();
    requireKeys(selection, {QStringLiteral("all"), QStringLiteral("multiple_contours"), QStringLiteral("exclude")}, field);
    const auto excluded = selection.contains(QStringLiteral("exclude"))
        ? readShapeIds(selection.value(QStringLiteral("exclude")), geometry, field + QStringLiteral(".exclude"))
        : QVector<int>{};
    const bool all = selectionFlag(selection, QStringLiteral("all"));
    const bool multipleContours = selectionFlag(selection, QStringLiteral("multiple_contours"));

    for (const auto &primitive : primitives) {
        if ((all || (multipleContours && primitive.shape.contours.size() > 1))
            && !excluded.contains(primitive.shape.shapeId))
            result.push_back(primitive.shape.shapeId);
    }

    return result;
}

void assignTasks(const QJsonObject &tasks, const ShapeGeometryStore &geometry,
                 QVector<Primitive> *primitives) {
    QStringList names;
    for (const auto *name : kTaskNames)
        names.push_back(QString::fromLatin1(name));
    requireKeys(tasks, names, QStringLiteral("tasks"));
    for (int task = 0; task < kShapeTaskCount; ++task) {
        const auto name = names[task];
        const auto ids = taskShapeIds(tasks.value(name), *primitives, geometry, QStringLiteral("tasks.") + name);
        const auto role = static_cast<ShapeTask>(task);
        const int requiredId = role == ShapeTask::StraightEdges || role == ShapeTask::ExactReplacements ? kSquareShapeId
            : role == ShapeTask::CornerTriangles ? kTriangleShapeId
            : role == ShapeTask::PatchConsolidation ? kCircleShapeId : -1;
        if (requiredId >= 0 && std::any_of(ids.cbegin(), ids.cend(), [&](int id) { return id != requiredId; }))
            throw std::runtime_error(QStringLiteral("tasks.%1 supports only shape ID %2 or an empty array")
                .arg(name).arg(requiredId).toStdString());
        for (auto &primitive : *primitives)
            primitive.taskOrder[task] = ids.indexOf(primitive.shape.shapeId);
    }
}

Primitive catalogPrimitive(const ShapeGeometryStore &geometry, int shapeId, bool reserve) {
    const auto *source = geometry.shape(shapeId);
    Polygons triangles;
    Primitive result;

    for (const auto &triangle : source->triangles) {
        if (triangle.alpha0 < 1.0 - kOpaqueEpsilon || triangle.alpha1 < 1.0 - kOpaqueEpsilon
            || triangle.alpha2 < 1.0 - kOpaqueEpsilon)
            throw std::runtime_error(QStringLiteral("Compact Fit shape %1 is not opaque").arg(shapeId).toStdString());
        QPolygonF polygon({triangle.p0, triangle.p1, triangle.p2});
        if (signedArea(polygon) < 0.0)
            std::reverse(polygon.begin(), polygon.end());
        triangles.push_back(polygon);
    }
    result.shape.shapeId = shapeId;
    result.shape.contours = unite(triangles);
    result.shape.silhouette = painterPath(result.shape.contours);
    result.shape.bounds = result.shape.silhouette.boundingRect();
    result.shape.area = area(result.shape.contours);
    result.reserve = reserve;
    result.taskOrder.fill(-1);
    if (result.shape.area <= 0.0)
        throw std::runtime_error(QStringLiteral("Compact Fit shape %1 has no opaque area").arg(shapeId).toStdString());

    return result;
}

QString defaultConfigurationPath() {
    const auto deployed = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/compact_fit_shapes.json"));
    const auto source = QDir::current().filePath(QStringLiteral("assets/compact_fit_shapes.json"));

    return QFileInfo::exists(deployed) ? deployed : source;
}

} // namespace

bool usesTask(const Primitive &primitive, ShapeTask task) {
    return primitive.taskOrder[static_cast<int>(task)] >= 0;
}

QVector<int> shapeIdsForTask(const QVector<Primitive> &primitives, ShapeTask task) {
    QVector<std::pair<int, int>> ordered;
    QVector<int> result;

    for (const auto &primitive : primitives)
        if (usesTask(primitive, task))
            ordered.push_back({primitive.taskOrder[static_cast<int>(task)], primitive.shape.shapeId});
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &first, const auto &second) {
        return first.first < second.first;
    });
    for (const auto &entry : ordered)
        result.push_back(entry.second);

    return result;
}

QVector<Primitive> primitivesForTask(const QVector<Primitive> &primitives, ShapeTask task) {
    QVector<Primitive> result;

    for (const auto &primitive : primitives)
        if (usesTask(primitive, task))
            result.push_back(primitive);
    std::stable_sort(result.begin(), result.end(), [task](const Primitive &first, const Primitive &second) {
        return first.taskOrder[static_cast<int>(task)] < second.taskOrder[static_cast<int>(task)];
    });

    return result;
}

QVector<Primitive> buildCatalog(const ShapeGeometryStore &geometry, QString *error) {
    return buildCatalog(geometry, defaultConfigurationPath(), error);
}

QVector<Primitive> buildCatalog(const ShapeGeometryStore &geometry,
                               const QString &configurationPath, QString *error) {
    QVector<Primitive> result;
    const auto path = QFileInfo(configurationPath).absoluteFilePath();

    try {
        if (geometry.shapeIds().isEmpty())
            throw std::runtime_error("Shape geometry is not loaded. Restore assets/vector/shape_geometry.json.gz and restart the editor.");
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        const auto bytes = file.readAll();
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        if (parseError.error != QJsonParseError::NoError)
            throw std::runtime_error(QStringLiteral("Invalid JSON at byte %1: %2")
                .arg(parseError.offset).arg(parseError.errorString()).toStdString());
        if (!document.isObject())
            throw std::runtime_error("The configuration root must be an object");
        const auto object = document.object();
        requireKeys(object, {QStringLiteral("schema_version"), QStringLiteral("shape_ids"),
            QStringLiteral("reserve_shape_ids"), QStringLiteral("tasks")}, QStringLiteral("configuration"));
        if (object.value(QStringLiteral("schema_version")).toDouble(-1) != kConfigurationVersion)
            throw std::runtime_error("Unsupported Compact Fit configuration schema_version");
        if (!object.value(QStringLiteral("tasks")).isObject())
            throw std::runtime_error("tasks must be an object");
        QSet<int> seen;
        for (const auto &key : {QStringLiteral("shape_ids"), QStringLiteral("reserve_shape_ids")})
            for (int id : readShapeIds(object.value(key), geometry, key)) {
                if (seen.contains(id))
                    throw std::runtime_error(QStringLiteral("Shape ID %1 appears in both catalog lists").arg(id).toStdString());
                seen.insert(id);
                result.push_back(catalogPrimitive(geometry, id, key == QStringLiteral("reserve_shape_ids")));
            }
        if (result.isEmpty())
            throw std::runtime_error("The Compact Fit shape catalog is empty");
        std::sort(result.begin(), result.end(), [](const Primitive &first, const Primitive &second) {
            return first.shape.shapeId < second.shape.shapeId;
        });
        assignTasks(object.value(QStringLiteral("tasks")).toObject(), geometry, &result);
        auto snapshot = std::make_shared<QJsonObject>();
        snapshot->insert(QStringLiteral("path"), path);
        snapshot->insert(QStringLiteral("sha256"), QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()));
        QJsonObject effective;
        for (int task = 0; task < kShapeTaskCount; ++task) {
            QJsonArray ids;
            for (int id : shapeIdsForTask(result, static_cast<ShapeTask>(task)))
                ids.push_back(id);
            effective.insert(QString::fromLatin1(kTaskNames[task]), ids);
        }
        snapshot->insert(QStringLiteral("tasks"), effective);
        for (auto &primitive : result)
            primitive.configuration = snapshot;
        if (error)
            error->clear();
    } catch (const std::exception &failure) {
        result.clear();
        if (error)
            *error = QStringLiteral("Compact Fit configuration %1: %2").arg(path, QString::fromUtf8(failure.what()));
    }

    return result;
}

} // namespace gui::catalog
