#pragma once

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

using fh6::normalizeRotation;

constexpr double kHandleHalf = 6.0;
constexpr double kScaleGrabInside = 12.0;
constexpr double kScaleGrabOutside = 12.0;
constexpr double kRotateCornerReach = 131.0;
constexpr double kSkewHandleOffset = 30.0;
constexpr double kClickDragThreshold = 5.0;
constexpr double kRulerExtent = 28.0;
constexpr double kGuidelineHitRadius = 5.0;

constexpr qint64 kSelectionFlashDurationMs = 750;
constexpr qint64 kSelectionFlashPeriodMs = 3750;
constexpr int kSelectionFlashFrameMs = 33;

inline QTransform flatEntryTransform(const fh6::scene::Shape &layer) {
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

inline QTransform flatEntryTransform(const fh6::scene::GuideLayer &guide) {
    QTransform transform;
    transform.translate(guide.x, guide.y);
    transform.rotate(guide.rotation);
    transform.scale(guide.scaleX, guide.scaleY);
    return transform;
}

inline QSizeF flatEntrySize(const fh6::scene::Shape &layer, const QSizeF &vectorSize) {
    return layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight) : vectorSize;
}

inline QRectF flatEntryRect(const fh6::scene::Shape &layer, const QSizeF &vectorSize) {
    return sceneLocalRect(flatEntrySize(layer, vectorSize));
}

inline QRectF flatEntryVisualRect(const fh6::scene::Shape &layer, const ShapeGeometryStore &geometry) {
    if (layer.raster) {
        return sceneLocalRect(QSizeF(layer.rasterWidth, layer.rasterHeight));
    }
    return geometry.shapeInkBounds(layer.shapeId);
}

inline QRectF flatEntryRect(const fh6::scene::GuideLayer &guide) {
    const QSizeF size = guide.image != nullptr ? QSizeF(guide.image->width, guide.image->height) : QSizeF();
    return sceneLocalRect(size);
}

struct EffectiveSelection {
    QVector<QString> groupIds;
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    QSet<QString> looseLayerIds;
    QSet<QString> looseGuideIds;

    int count() const {
        return groupIds.size() + looseLayerIds.size() + looseGuideIds.size();
    }
};

inline void collectGuideIds(const fh6::scene::Layer &node, QSet<QString> &out) {
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

inline QVector<QString> buildTransformTargetIds(const QVector<QString> &groupIds,
                                                const QVector<fh6::scene::Shape *> &layers,
                                                const QVector<fh6::scene::GuideLayer *> &guides) {
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

struct HandleAxes {
    bool left = false;
    bool right = false;
    bool top = false;
    bool bottom = false;
};

inline HandleAxes handleAxes(const QString &handle) {
    return {handle.contains(QStringLiteral("left")), handle.contains(QStringLiteral("right")),
            handle.contains(QStringLiteral("top")), handle.contains(QStringLiteral("bottom"))};
}

inline bool handleAnchorLocalPoints(const QString &handle, const QRectF &rect, QPointF *handlePoint, QPointF *anchorPoint) {
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
