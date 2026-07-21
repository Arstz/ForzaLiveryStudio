#pragma once

#include "core_types.h"
#include "visual_container.h"

#include <QByteArray>
#include <QString>
#include <QVector>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace fh6::scene {

enum class LayerKind { Shape, Guide, Group };

// Transform order is translate, rotate, shear, then scale.
struct Transform2D {
    double x = 0.0;
    double y = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
    double rotation = 0.0;
    double skew = 0.0;

    Matrix3 matrix() const;
};

class Group;

class Layer {
public:
    QString id;
    QString name;
    Transform2D transform;
    double &x = transform.x;
    double &y = transform.y;
    double &scaleX = transform.scaleX;
    double &scaleY = transform.scaleY;
    double &rotation = transform.rotation;
    double &skew = transform.skew;
    double opacity = 1.0;
    bool visible = true;
    bool locked = false;

    virtual ~Layer() = default;
    virtual LayerKind kind() const = 0;
    virtual std::unique_ptr<Layer> clone() const = 0;

    Layer *parent() const { return parent_; }
    Matrix3 worldMatrix() const;

protected:
    void copyBaseTo(Layer &dst) const;

    Layer *parent_ = nullptr;
    friend class Group;
};

class Shape : public Layer {
public:
    Shape() { name = QStringLiteral("Shape"); }

    std::unique_ptr<VisualContainer> visual;
    quint16 shapeId = 0;
    bool raster = false;
    quint32 rasterId = 0;
    int rasterWidth = 256;
    int rasterHeight = 256;
    std::array<quint8, 4> color = {255, 255, 255, 255};
    bool mask = false;

    // Binary round-trip metadata.
    int sourceShape = 0;
    int absOffset = 0;
    QByteArray marker;
    int flags = 0;
    quint16 sourceLogoId = 0;
    bool hasSourceTransform = false;
    Transform2D sourceTransform;

    LayerKind kind() const override { return LayerKind::Shape; }
    std::unique_ptr<Layer> clone() const override;

    bool isRaster() const;
    void setVectorShape(quint16 id);
    void setRasterShape(quint32 id, int width = 256, int height = 256);
};

class GuideLayer : public Layer {
public:
    GuideLayer() {
        name = QStringLiteral("Guide");
        opacity = 0.5;
    }

    std::unique_ptr<RasterContainer> image;
    QString sourcePath;
    int preprocessColorCount = 0;

    LayerKind kind() const override { return LayerKind::Guide; }
    std::unique_ptr<Layer> clone() const override;
};

class Group : public Layer {
public:
    Group() { name = QStringLiteral("Group"); }

    std::vector<std::unique_ptr<Layer>> children;
    bool isLiverySection = false;
    int liverySectionSlot = -1;

    // Binary round-trip metadata.
    int sourceAbsPos = 0;
    QByteArray pendingTransformMarker;
    QByteArray inlineTransformMarker;
    QByteArray effectiveTransformMarker;
    QByteArray headerControlBytes;
    int flags = 0;
    QString sourceParentId;
    QString sourcePreviousSiblingId;
    int sourcePreviousGroupDepth = 0;
    QVector<QString> sourceChildren;

    LayerKind kind() const override { return LayerKind::Group; }
    std::unique_ptr<Layer> clone() const override;

    Layer *append(std::unique_ptr<Layer> child);
    Layer *insert(int index, std::unique_ptr<Layer> child);
    std::unique_ptr<Layer> takeAt(int index);
};

} // namespace fh6::scene
