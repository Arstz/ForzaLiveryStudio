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

namespace fh6 {

constexpr int FlatExportLayerLimit = 3000;

struct ShapeLayer {
    QString id;
    QString name = QStringLiteral("Shape");
    quint16 shapeId = 0;
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotation = 0.0;
    double skew = 0.0;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool visible = true;
    bool locked = false;
    bool mask = false;
    int sourceShape = 0;
    int absOffset = 0;
    QByteArray marker;
    int flags = 0;
};

struct GuideLayer {
    QString id;
    QString name = QStringLiteral("Guide");
    QString sourcePath;
    QByteArray imageBytes;
    QByteArray pixelBytes;
    QString imageFormat;
    int width = 0;
    int height = 0;
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotation = 0.0;
    double opacity = 0.5;
    bool visible = true;
    bool locked = false;
};

struct LayerGroup {
    QString id;
    QString name = QStringLiteral("Group");
    QVector<QString> childIds;
    bool locked = false;
    int sourceAbsPos = 0;
    QByteArray pendingTransformMarker;
    QByteArray inlineTransformMarker;
    QByteArray effectiveTransformMarker;
    QByteArray headerControlBytes;
    int flags = 0;
    QString sourceParentId;
    QString sourcePreviousSiblingId;
    int sourcePreviousGroupDepth = 0;
    QVector<QString> sourceChildIds;
    bool isLiverySection = false;  // top-level C_livery panel section group
    int liverySectionSlot = -1;    // 0..10 storage-order slot when isLiverySection
};

struct Project {
    QString name = QStringLiteral("Untitled");
    QString sourceFolder;
    QByteArray sourceDecPrefix;
    QByteArray sourceHeader;
    std::optional<HeaderMetadata> headerMetadata; // drives export when sourceHeader is empty (new projects)
    QVector<ShapeLayer> layers;
    QVector<GuideLayer> guideLayers;
    QVector<LayerGroup> groups;
    QVector<QString> rootChildIds;
    bool isLivery = false;  // imported from a C_livery: rootChildIds are the 11 sections
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
