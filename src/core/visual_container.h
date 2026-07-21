#pragma once


#include <QByteArray>
#include <QSizeF>
#include <QString>

#include <cstdint>
#include <memory>

namespace fh6::scene {

enum class VisualKind { Vector, Raster };

class VisualContainer {
public:
    virtual ~VisualContainer() = default;
    virtual VisualKind kind() const = 0;
    virtual QSizeF size() const = 0;
    virtual std::unique_ptr<VisualContainer> clone() const = 0;
};

class VectorContainer : public VisualContainer {
public:
    quint16 shapeId = 0;

    VisualKind kind() const override { return VisualKind::Vector; }
    QSizeF size() const override { return QSizeF(1.0, 1.0); }
    std::unique_ptr<VisualContainer> clone() const override {
        return std::make_unique<VectorContainer>(*this);
    }
};

class RasterContainer : public VisualContainer {
public:
    quint32 rasterId = 0;
    int width = 0;
    int height = 0;
    QByteArray pixels;
    QByteArray encoded;
    QString format;

    VisualKind kind() const override { return VisualKind::Raster; }
    QSizeF size() const override {
        return QSizeF(static_cast<double>(width), static_cast<double>(height));
    }
    std::unique_ptr<VisualContainer> clone() const override {
        return std::make_unique<RasterContainer>(*this);
    }
};

} // namespace fh6::scene
