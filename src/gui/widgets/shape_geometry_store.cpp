#include "shape_geometry_store.h"

#include <zlib.h>

#include <algorithm>
#include <limits>

namespace gui {
namespace {

constexpr int kCenturyGothicLowercaseA = 3801;

QStringList candidateAssetPaths() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/vector/shape_geometry.json.gz")),
        QDir(appDir).filePath(QStringLiteral("assets/vector/shape_geometry.json")),
        QDir(cwd).filePath(QStringLiteral("assets/vector/shape_geometry.json.gz")),
        QDir(cwd).filePath(QStringLiteral("assets/vector/shape_geometry.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/vector/shape_geometry.json.gz")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/vector/shape_geometry.json")),
    };
}

QPointF vertexPoint(const QJsonArray &vertices, int index) {
    const QJsonArray vertex = vertices.at(index).toArray();
    return QPointF(vertex.at(0).toDouble(), vertex.at(1).toDouble());
}

double vertexAlpha(const QJsonArray &vertices, int index) {
    const QJsonArray vertex = vertices.at(index).toArray();
    return std::clamp(vertex.at(2).toDouble(1.0), 0.0, 1.0);
}

bool inflateGzip(const QByteArray &compressed, QByteArray *out, QString *error) {
    z_stream stream = {};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        if (error != nullptr) {
            *error = QStringLiteral("could not initialize gzip decompressor");
        }
        return false;
    }

    QByteArray decompressed;
    char buffer[64 * 1024];
    int ret = Z_OK;
    while (ret == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        ret = inflate(&stream, Z_NO_FLUSH);
        const qsizetype produced = static_cast<qsizetype>(sizeof(buffer) - stream.avail_out);
        if (produced > 0) {
            decompressed.append(buffer, produced);
        }
    }
    inflateEnd(&stream);

    if (ret != Z_STREAM_END) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid compressed geometry asset");
        }
        return false;
    }
    *out = std::move(decompressed);
    return true;
}

} // namespace

bool ShapeGeometryStore::loadDefault(QString *error) {
    for (const QString &path : candidateAssetPaths()) {
        if (QFile::exists(path)) {
            return loadFromFile(path, error);
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("shape_geometry.json was not found");
    }
    return false;
}

bool ShapeGeometryStore::loadFromFile(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not open geometry asset: %1").arg(path);
        }
        return false;
    }

    QByteArray bytes = file.readAll();
    if (path.endsWith(QStringLiteral(".gz"), Qt::CaseInsensitive)) {
        QByteArray decompressed;
        if (!inflateGzip(bytes, &decompressed, error)) {
            return false;
        }
        bytes = std::move(decompressed);
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid geometry asset: %1").arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject shapes = document.object().value(QStringLiteral("shapes")).toObject();
    QHash<int, ShapeGeometry> parsed;
    for (auto it = shapes.begin(); it != shapes.end(); ++it) {
        bool ok = false;
        const int shapeId = it.key().toInt(&ok);
        if (!ok || !it.value().isObject()) {
            continue;
        }

        const QJsonObject raw = it.value().toObject();
        const QJsonArray size = raw.value(QStringLiteral("size")).toArray();
        const QJsonArray vertices = raw.value(QStringLiteral("vertices")).toArray();
        const QJsonArray triangles = raw.value(QStringLiteral("triangles")).toArray();
        ShapeGeometry geometry;
        if (size.size() >= 2) {
            geometry.width = std::max(1, size.at(0).toInt(128));
            geometry.height = std::max(1, size.at(1).toInt(128));
        }
        geometry.source = raw.value(QStringLiteral("source")).toString();

        geometry.triangles.reserve(triangles.size());
        for (const QJsonValue &triangleValue : triangles) {
            const QJsonArray triangle = triangleValue.toArray();
            if (triangle.size() != 3) {
                continue;
            }
            const int i0 = triangle.at(0).toInt(-1);
            const int i1 = triangle.at(1).toInt(-1);
            const int i2 = triangle.at(2).toInt(-1);
            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
                continue;
            }
            ShapeTriangle item;
            item.p0 = vertexPoint(vertices, i0);
            item.p1 = vertexPoint(vertices, i1);
            item.p2 = vertexPoint(vertices, i2);
            item.alpha0 = vertexAlpha(vertices, i0);
            item.alpha1 = vertexAlpha(vertices, i1);
            item.alpha2 = vertexAlpha(vertices, i2);
            if (item.alpha0 > 0.0 || item.alpha1 > 0.0 || item.alpha2 > 0.0) {
                geometry.triangles.push_back(item);
            }
        }
        if (shapeId == kCenturyGothicLowercaseA) {
            geometry.width *= 2;
            for (ShapeTriangle &triangle : geometry.triangles) {
                triangle.p0.setX(triangle.p0.x() * 2.0);
                triangle.p1.setX(triangle.p1.x() * 2.0);
                triangle.p2.setX(triangle.p2.x() * 2.0);
            }
        }
        parsed.insert(shapeId, geometry);
    }

    shapes_ = parsed;
    return true;
}

const ShapeGeometry *ShapeGeometryStore::shape(int shapeId) const {
    const auto it = shapes_.constFind(shapeId);
    return it == shapes_.constEnd() ? nullptr : &it.value();
}

QSizeF ShapeGeometryStore::shapeSize(int shapeId) const {
    const ShapeGeometry *geometry = shape(shapeId);
    if (geometry == nullptr) {
        return QSizeF(128.0, 128.0);
    }
    return QSizeF(geometry->width, geometry->height);
}

QRectF ShapeGeometryStore::shapeInkBounds(int shapeId) const {
    const ShapeGeometry *geometry = shape(shapeId);
    if (geometry == nullptr || geometry->triangles.isEmpty()) {
        const QSizeF size = shapeSize(shapeId);
        return QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
    }

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    for (const ShapeTriangle &tri : geometry->triangles) {
        for (const QPointF &p : {tri.p0, tri.p1, tri.p2}) {
            minX = std::min(minX, p.x());
            minY = std::min(minY, p.y());
            maxX = std::max(maxX, p.x());
            maxY = std::max(maxY, p.y());
        }
    }
    return QRectF(QPointF(minX, minY), QPointF(maxX, maxY));
}

QVector<int> ShapeGeometryStore::shapeIds() const {
    QVector<int> ids;
    ids.reserve(shapes_.size());
    for (auto it = shapes_.constBegin(); it != shapes_.constEnd(); ++it) {
        ids.push_back(it.key());
    }
    return ids;
}

} // namespace gui
