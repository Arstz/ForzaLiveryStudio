#pragma once

#include "header_codec.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace fh6::scene {
class Group;
}

namespace fh6 {

struct Project {
    Project();
    ~Project();
    Project(const Project &other);
    Project &operator=(const Project &other);
    Project(Project &&other) noexcept;
    Project &operator=(Project &&other) noexcept;

    QString name = QStringLiteral("Untitled");
    QString sourceFolder;
    QByteArray sourceDecPrefix;
    QByteArray sourceHeader;
    std::optional<HeaderMetadata> headerMetadata; // drives export when sourceHeader is empty (new projects)
    std::unique_ptr<scene::Group> root;            // authoritative runtime scene tree
    QVector<std::array<quint8, 4>> colorSwatches;
    bool isLivery = false;  // imported from a C_livery: root children are the 11 sections
    int carId = 0;          // target car id (C_livery vlrc 0x10); 0 = unset. See livery-car-id-encoding.
    // Decompressed original C_livery payload captured on import. The livery encoder
    // rebuilds the export container from this (applying carId); empty for a livery
    // authored from scratch (artwork-synthesis path not yet implemented).
    QByteArray liverySource;
};

struct LayerData {
    QByteArray data;
    int start = 0;
};

struct Matrix3 {
    double m[3][3] = {
        {1.0, 0.0, 0.0},
        {0.0, 1.0, 0.0},
        {0.0, 0.0, 1.0},
    };
};

struct VinylShape {
    quint16 shapeId = 0;
    bool isLogo = false;
    quint16 logoId = 0;
    quint32 rasterId = 0;
    double rotation = 0.0;
    double posX = 0.0;
    double posY = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double skew = 0.0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    int absPos = 0;
    QByteArray marker;
    int flags = 0;
    bool isMask = false;
};

struct VinylGroup;
using VinylGroupPtr = std::shared_ptr<VinylGroup>;

struct VinylItem {
    std::variant<VinylShape, VinylGroupPtr> value;
    bool isShape() const { return std::holds_alternative<VinylShape>(value); }
};

struct VinylGroup {
    QString nodeType = QStringLiteral("root");
    QString source;
    double px = 0.0;
    double py = 0.0;
    double sx = 1.0;
    double sy = 1.0;
    double rot = 0.0;
    int absPos = 0;
    int flags = 0;
    bool isMask = false;
    std::optional<int> expectedChildren;
    QByteArray childTypeBitmap;
    QByteArray headerMarker;
    QByteArray inlineTransformMarker;
    QByteArray pendingTransformMarker;
    QByteArray effectiveTransformMarker;
    QByteArray headerControlBytes;
    bool rootTransformExempt = false;
    QVector<VinylItem> items;
    int totalChildren() const { return items.size(); }
};

struct FlattenedLayer {
    quint16 shapeId = 0;
    bool raster = false;
    quint32 rasterId = 0;
    int rasterWidth = 256;
    int rasterHeight = 256;
    quint16 sourceLogoId = 0;
    double rotation = 0.0;
    double posX = 0.0;
    double posY = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double skew = 0.0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool isMask = false;
    int flags = 0;
    QByteArray marker;
    int absPos = 0;
    Matrix3 groupMatrix;
};

} // namespace fh6
