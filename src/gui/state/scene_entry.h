#pragma once

// GUI-side view helpers over the two core scene item types (fh6::ShapeLayer
// and fh6::GuideLayer). The core structs are consumed by the codecs and must
// stay plain data; the shared accessors and the loops-collapsing helpers for
// mixed shape/guide processing live here instead.

#include "core_types.h"
#include "gui_constants.h"

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QTransform>

namespace gui {

// Local->world transform for an item (translate -> rotate -> [shear] -> scale).
inline QTransform entryTransform(const fh6::ShapeLayer &layer)
{
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

inline QTransform entryTransform(const fh6::GuideLayer &guide)
{
    QTransform transform;
    transform.translate(guide.x, guide.y);
    transform.rotate(guide.rotation);
    transform.scale(guide.scaleX, guide.scaleY);
    return transform;
}

// Item-local rectangle centred on the origin (shapes and guides both draw
// centred on their position). Shapes pass their geometry-store size; guides
// their intrinsic pixel size.
inline QRectF entryLocalRect(const QSizeF &size)
{
    return QRectF(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
}

inline QRectF entryLocalRect(const fh6::GuideLayer &guide)
{
    return entryLocalRect(QSizeF(guide.width, guide.height));
}

// World-axis AABB accumulator shared by the project/selection bounds loops.
class BoundsAccumulator {
public:
    void add(const QTransform &transform, const QRectF &localRect)
    {
        const QRectF mapped = transform.mapRect(localRect);
        bounds_ = hasBounds_ ? bounds_.united(mapped) : mapped;
        hasBounds_ = true;
    }

    bool hasBounds() const { return hasBounds_; }
    const QRectF &bounds() const { return bounds_; }

private:
    QRectF bounds_;
    bool hasBounds_ = false;
};

// Non-owning tagged view over a ShapeLayer or GuideLayer for code that walks
// mixed selections with one loop. Only the fields both types carry get plain
// accessors; type-specific fields are reached through layer()/guide() behind a
// hasSkew()/hasColor()-style guard. Exactly one of the two pointers is set.
class EntryRef {
public:
    EntryRef() = default;
    explicit EntryRef(fh6::ShapeLayer *layer) : layer_(layer) {}
    explicit EntryRef(fh6::GuideLayer *guide) : guide_(guide) {}

    bool valid() const { return layer_ != nullptr || guide_ != nullptr; }
    bool isLayer() const { return layer_ != nullptr; }
    bool isGuide() const { return guide_ != nullptr; }
    fh6::ShapeLayer *layer() const { return layer_; }
    fh6::GuideLayer *guide() const { return guide_; }

    QString id() const { return layer_ != nullptr ? layer_->id : guide_->id; }
    QString name() const { return layer_ != nullptr ? layer_->name : guide_->name; }
    double x() const { return layer_ != nullptr ? layer_->x : guide_->x; }
    double y() const { return layer_ != nullptr ? layer_->y : guide_->y; }
    double scaleX() const { return layer_ != nullptr ? layer_->scaleX : guide_->scaleX; }
    double scaleY() const { return layer_ != nullptr ? layer_->scaleY : guide_->scaleY; }
    double rotation() const { return layer_ != nullptr ? layer_->rotation : guide_->rotation; }
    bool visible() const { return layer_ != nullptr ? layer_->visible : guide_->visible; }
    bool locked() const { return layer_ != nullptr ? layer_->locked : guide_->locked; }

    bool hasSkew() const { return layer_ != nullptr; }
    double skew() const { return layer_ != nullptr ? layer_->skew : 0.0; }
    bool hasColor() const { return layer_ != nullptr; }
    // Displayed opacity: the colour alpha for shapes, the guide's own opacity
    // for guides.
    double opacity() const
    {
        return layer_ != nullptr ? static_cast<double>(layer_->color[ColorByteAlpha]) / 255.0
                                 : guide_->opacity;
    }

private:
    fh6::ShapeLayer *layer_ = nullptr;
    fh6::GuideLayer *guide_ = nullptr;
};

} // namespace gui
