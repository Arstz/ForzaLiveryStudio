#include "binary_io.h"
#include "shape_registry.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSaveFile>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace {

using fls::detail::BinaryCursor;

constexpr quint32 kVinylModelType = 1;
constexpr quint32 kVertexBufferType = 2;
constexpr quint32 kIndexStripType = 3;
constexpr quint16 kStripRestart = std::numeric_limits<quint16>::max();
constexpr int kModelUnits = 256;
constexpr int kMeshCountOffset = 52;
constexpr int kMeshBoundsSize = 48;
constexpr int kVertexSize = 36;
constexpr int kVertexPrefixSize = 4;
constexpr int kMaterialHeaderSize = 20;
constexpr int kMaterialDataSize = 64;
constexpr int kModelFooterSize = 72;

void skipString(BinaryCursor &cursor, const QByteArray &model) {
    const quint32 length = cursor.u32();
    if (length > model.size() - cursor.position()) {
        throw std::runtime_error("invalid vinyl model string length");
    }
    cursor.skip(static_cast<int>(length));
}

int appendVertices(QJsonArray &vertices, BinaryCursor &cursor, const QByteArray &model) {
    if (cursor.u32() != kVertexBufferType) {
        throw std::runtime_error("unsupported vinyl model vertex buffer");
    }
    const quint32 count = cursor.u32();
    const quint32 stride = cursor.u32();
    if (stride != kVertexSize || count > kStripRestart
        || count > (model.size() - cursor.position()) / (kVertexPrefixSize + kVertexSize)) {
        throw std::runtime_error("invalid vinyl model vertex buffer size");
    }
    for (quint32 index = 0; index < count; ++index) {
        cursor.skip(kVertexPrefixSize);
        const double x = kModelUnits * double(cursor.f32());
        const double y = kModelUnits * double(cursor.f32());
        cursor.skip(kVertexSize - 3 * sizeof(quint32));
        const double alpha = (cursor.u32() >> 24) / 255.0;
        if (!std::isfinite(x) || !std::isfinite(y)) {
            throw std::runtime_error("invalid vinyl model vertex coordinates");
        }
        vertices.append(QJsonArray{x, y, alpha});
    }

    return static_cast<int>(count);
}

void appendStrip(QJsonArray &triangles, BinaryCursor &cursor, const QByteArray &model,
                 int vertexStart, int vertexCount) {
    if (cursor.u32() != kIndexStripType) {
        throw std::runtime_error("unsupported vinyl model index buffer");
    }
    const quint32 count = cursor.u32();
    const quint32 width = cursor.u32();
    if (width != sizeof(quint16) || count > (model.size() - cursor.position()) / width) {
        throw std::runtime_error("invalid vinyl model index buffer size");
    }
    quint16 first = 0;
    quint16 second = 0;
    quint32 stripLength = 0;
    for (quint32 index = 0; index < count; ++index) {
        const quint16 third = cursor.u16();
        if (third == kStripRestart) {
            stripLength = 0;
            continue;
        }
        if (third >= vertexCount) {
            throw std::runtime_error("vinyl model index is out of bounds");
        }
        if (stripLength >= 2 && first != second && first != third && second != third) {
            const int a = vertexStart + (stripLength % 2 == 0 ? first : second);
            const int b = vertexStart + (stripLength % 2 == 0 ? second : first);
            triangles.append(QJsonArray{a, b, vertexStart + third});
        }
        first = second;
        second = third;
        ++stripLength;
    }
}

QJsonObject decodeShapeModel(const QByteArray &model, const QString &name) {
    BinaryCursor cursor(model, "truncated vinyl shape model", fls::detail::ByteOrder::BigEndian);
    QJsonArray vertices;
    QJsonArray triangles;
    if (cursor.u32() != kVinylModelType) {
        throw std::runtime_error("unsupported vinyl shape model type");
    }
    const double minimumX = cursor.f32();
    const double minimumY = cursor.f32();
    cursor.skip(2 * sizeof(float));
    const double maximumX = cursor.f32();
    const double maximumY = cursor.f32();
    const double width = (maximumX - minimumX) * kModelUnits;
    const double height = (maximumY - minimumY) * kModelUnits;
    cursor.skip(kMeshCountOffset - cursor.position());
    const quint32 meshCount = cursor.u32();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0
        || height <= 0.0 || meshCount == 0 || meshCount > (model.size() - cursor.position()) / kMeshBoundsSize) {
        throw std::runtime_error("invalid vinyl shape model header");
    }
    for (quint32 index = 0; index < meshCount; ++index) {
        if (cursor.u32() != kVinylModelType) {
            throw std::runtime_error("unsupported vinyl shape mesh type");
        }
        cursor.skip(kMeshBoundsSize);
        if (cursor.u32() != 1) {
            throw std::runtime_error("invalid vinyl shape mesh name");
        }
        skipString(cursor, model);
        const int vertexStart = static_cast<int>(vertices.size());
        const int vertexCount = appendVertices(vertices, cursor, model);
        cursor.skip(kMaterialHeaderSize);
        if (cursor.u8() != 1) {
            throw std::runtime_error("unsupported vinyl shape material type");
        }
        skipString(cursor, model);
        cursor.skip(kMaterialDataSize);
        appendStrip(triangles, cursor, model, vertexStart, vertexCount);
    }
    if (model.size() - cursor.position() != kModelFooterSize || triangles.isEmpty()) {
        throw std::runtime_error("invalid vinyl shape model geometry");
    }

    return {
        {QStringLiteral("source"), name},
        {QStringLiteral("size"), QJsonArray{width, height}},
        {QStringLiteral("vertices"), vertices},
        {QStringLiteral("triangles"), triangles},
        {QStringLiteral("legacy_size"), QJsonArray{std::round(width), std::round(height)}}
    };
}

QJsonDocument readCatalog(const QString &archive) {
    QMap<QString, quint16> entryIds;
    for (quint16 id = fls::detail::kFirstXbox360ExclusiveShapeId;
         id <= fls::detail::kLastXbox360ExclusiveShapeId; ++id) {
        const QString name = fls::detail::xbox360ExclusiveShapeModelName(id);
        if (!name.isEmpty()) {
            entryIds.insert(QStringLiteral("vinyls/%1.carbin").arg(name.toLower()), id);
        }
    }
    QString error;
    const auto models = fls::readZipEntries(archive, entryIds.keys(), &error);
    if (models.size() != entryIds.size()) {
        throw std::runtime_error(error.toStdString());
    }
    QJsonObject shapes;
    for (auto entry = entryIds.cbegin(); entry != entryIds.cend(); ++entry) {
        const quint16 id = entry.value();
        const QString modelName = fls::detail::xbox360ExclusiveShapeModelName(id);
        try {
            shapes.insert(QString::number(id), decodeShapeModel(models.value(entry.key()), modelName));
        } catch (const std::runtime_error &failure) {
            throw std::runtime_error(QStringLiteral("%1: %2").arg(modelName, QString::fromUtf8(failure.what())).toStdString());
        }
    }

    return QJsonDocument(shapes);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() != 3) {
        qCritical("Usage: fls_pack_xbox360_shapes <Livery.zip> <shapes.json>");
        return 1;
    }
    try {
        const QJsonDocument catalog = readCatalog(arguments[1]);
        const QByteArray bytes = catalog.toJson(QJsonDocument::Compact);
        QSaveFile output(arguments[2]);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
            throw std::runtime_error(output.errorString().toStdString());
        }
        qInfo() << "Wrote" << catalog.object().size() << "Xbox 360 vinyl shapes";
    } catch (const std::exception &failure) {
        qCritical("%s", failure.what());
        return 1;
    }

    return 0;
}
