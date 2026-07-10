#pragma once

// A Layer's visual is stored SEPARATELY from the layer node (composition, not
// multiple inheritance). A leaf layer owns exactly one VisualContainer, which is
// either a VectorContainer (a glyph/vector shape addressed by shapeId) or a
// RasterContainer (a logo decal addressed by rasterId, or a guide's inline image).
// Keeping the pixels/geometry reference on a shared container is what lets every
// surface -- the GL canvas AND the CPU thumbnail paths -- draw the same visual.

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
    // Intrinsic local size. Vector geometry lives in the GUI-side geometry store,
    // so the core VectorContainer returns a unit placeholder; callers that need a
    // true vector size resolve it through ShapeGeometryStore as before.
    virtual QSizeF size() const = 0;
    virtual std::unique_ptr<VisualContainer> clone() const = 0;
};

// Vector/glyph shape: geometry resolved from the shape id via ShapeGeometryStore.
class VectorContainer : public VisualContainer {
public:
    quint16 shapeId = 0;

    VisualKind kind() const override { return VisualKind::Vector; }
    QSizeF size() const override { return QSizeF(1.0, 1.0); }
    std::unique_ptr<VisualContainer> clone() const override
    {
        return std::make_unique<VectorContainer>(*this);
    }
};

// Raster visual: a logo decal (rasterId, pixels resolved from the shared decal
// store) or a guide's own inline image (rasterId == 0, pixels/encoded inline).
class RasterContainer : public VisualContainer {
public:
    quint32 rasterId = 0;   // decal id; 0 => inline-only image (guides)
    int width = 0;
    int height = 0;
    QByteArray pixels;      // decoded RGBA8888 (filled from the decal store for decals)
    QByteArray encoded;     // original encoded bytes (guides keep these for round-trip)
    QString format;         // encoded format tag (guides)

    VisualKind kind() const override { return VisualKind::Raster; }
    QSizeF size() const override
    {
        return QSizeF(static_cast<double>(width), static_cast<double>(height));
    }
    std::unique_ptr<VisualContainer> clone() const override
    {
        return std::make_unique<RasterContainer>(*this);
    }
};

} // namespace fh6::scene
