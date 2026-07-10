#pragma once

// Shared implementation helpers for the ProjectCanvas translation units
// (project_canvas.cpp and its per-concern siblings project_canvas_hit/drag/
// cursor/paint/events.cpp). These are the anonymous-namespace helpers that were
// duplicated across those concerns; keeping them here lets each unit reuse one
// definition. Unit-local helpers stay in their own file's anonymous namespace.

#include "layer.h"
#include "matrix_math.h"
#include "scene_view.h"

#include "canvas_tools.h"
#include "editor_state.h"
#include "gui_constants.h"
#include "theme_manager.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace gui {
namespace pc_detail {

// Reuse the canonical [0,360) wrap from core rather than shadowing it.
using fh6::normalizeRotation;

// --- Selection-box handle geometry ---------------------------------------
constexpr double HandleHalf = 6.0;            // drawn handle marker half-size
// Scale lives in a band that straddles each edge: it reaches ScaleGrabInside into the box
// (so the visible edge itself grabs Scale) and ScaleGrabOutside past it, while the interior
// beyond the band stays Move. Biased outward so the box interior reads as Move almost
// everywhere. Where two edge bands meet (the corners) Scale becomes two-axis.
constexpr double ScaleGrabInside = 12.0;
constexpr double ScaleGrabOutside = 12.0;
// Rotate is the outer-anchor affordance: it lives strictly outside the box, in the diagonal
// region past each corner, out to this reach. Scale is resolved first, so along the sides the
// scale band wins and Rotate only claims the area past the corner anchors.
constexpr double RotateCornerReach = 131.0;
constexpr double SkewHandleOffset = 30.0;
constexpr double ClickDragThreshold = 5.0;

// Selection-flash timing (shared: the canvas ctor seeds the frame interval; the paint unit
// drives the period/duration envelope).
constexpr qint64 SelectionFlashDurationMs = 750;
constexpr qint64 SelectionFlashPeriodMs = 3750;
constexpr int SelectionFlashFrameMs = 33;

// --- Scene-leaf local transforms and rects --------------------------------
// World-space readers use sceneWorldTransform() so group frames are included; these are the
// leaf's own local frame and intrinsic (centred) rect.
inline QTransform flatEntryTransform(const fh6::scene::Shape &layer)
{
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

inline QTransform flatEntryTransform(const fh6::scene::GuideLayer &guide)
{
    QTransform transform;
    transform.translate(guide.x, guide.y);
    transform.rotate(guide.rotation);
    transform.scale(guide.scaleX, guide.scaleY);
    return transform;
}

inline QSizeF flatEntrySize(const fh6::scene::Shape &layer, const QSizeF &vectorSize)
{
    return layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight) : vectorSize;
}

inline QRectF flatEntryRect(const fh6::scene::Shape &layer, const QSizeF &vectorSize)
{
    return sceneLocalRect(flatEntrySize(layer, vectorSize));
}

inline QRectF flatEntryVisualRect(const fh6::scene::Shape &layer, const ShapeGeometryStore &geometry)
{
    if (layer.raster) {
        return sceneLocalRect(QSizeF(layer.rasterWidth, layer.rasterHeight));
    }
    return geometry.shapeInkBounds(layer.shapeId);
}

inline QRectF flatEntryRect(const fh6::scene::GuideLayer &guide)
{
    const QSizeF size = guide.image != nullptr ? QSizeF(guide.image->width, guide.image->height) : QSizeF();
    return sceneLocalRect(size);
}

// --- Selection partitioning ----------------------------------------------
struct EffectiveSelection {
    QVector<QString> groupIds;
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    QSet<QString> looseLayerIds;
    QSet<QString> looseGuideIds;

    int count() const
    {
        return groupIds.size() + looseLayerIds.size() + looseGuideIds.size();
    }
};

inline void collectGuideIds(const fh6::scene::Layer &node, QSet<QString> &out)
{
    if (node.kind() == fh6::scene::LayerKind::Guide) {
        out.insert(node.id);
        return;
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return;
    }
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        collectGuideIds(*child, out);
    }
}

// Unique, order-preserving id list for a transform command: whole groups first, then loose
// shapes, then loose guides.
inline QVector<QString> buildTransformTargetIds(const QVector<QString> &groupIds,
                                                const QVector<fh6::scene::Shape *> &layers,
                                                const QVector<fh6::scene::GuideLayer *> &guides)
{
    QVector<QString> ids;
    ids.reserve(groupIds.size() + layers.size() + guides.size());
    QSet<QString> seen;
    const auto add = [&](const QString &id) {
        if (!id.isEmpty() && !seen.contains(id)) {
            ids.push_back(id);
            seen.insert(id);
        }
    };
    for (const QString &id : groupIds) {
        add(id);
    }
    for (const fh6::scene::Shape *layer : layers) {
        add(layer->id);
    }
    for (const fh6::scene::GuideLayer *guide : guides) {
        add(guide->id);
    }
    return ids;
}

// --- Scale/skew handle axes ----------------------------------------------
// Which box axes a named scale handle drives ("top_left" -> left + top, ...).
struct HandleAxes {
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
};

inline HandleAxes handleAxes(const QString &handle)
{
    return {handle.contains(QStringLiteral("left")), handle.contains(QStringLiteral("right")),
            handle.contains(QStringLiteral("top")), handle.contains(QStringLiteral("bottom"))};
}

// Grabbed handle point and its opposite anchor, in the box's local rect coords. Returns false
// for the skew handle or a non-directional handle, or when handle and anchor coincide.
inline bool handleAnchorLocalPoints(const QString &handle, const QRectF &rect, QPointF *handlePoint, QPointF *anchorPoint)
{
    const HandleAxes axes = handleAxes(handle);
    if ((!axes.left && !axes.right && !axes.top && !axes.bottom) || handle == QStringLiteral("skew")) {
        return false;
    }
    const double centerX = rect.center().x();
    const double centerY = rect.center().y();
    const double handleX = axes.left ? rect.left() : (axes.right ? rect.right() : centerX);
    const double handleY = axes.top ? rect.top() : (axes.bottom ? rect.bottom() : centerY);
    const double anchorX = axes.left ? rect.right() : (axes.right ? rect.left() : centerX);
    const double anchorY = axes.top ? rect.bottom() : (axes.bottom ? rect.top() : centerY);
    *handlePoint = QPointF(handleX, handleY);
    *anchorPoint = QPointF(anchorX, anchorY);
    return QLineF(*handlePoint, *anchorPoint).length() > 1e-9;
}

} // namespace pc_detail
} // namespace gui
