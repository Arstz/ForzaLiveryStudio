#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

// Shared helpers (flatEntry*, EffectiveSelection, collectGuideIds, handle axes,
// handle-box geometry constants, buildTransformTargetIds, normalizeRotation) live in
// project_canvas_internal.h so every ProjectCanvas translation unit reuses one definition.
using namespace pc_detail;


QVector<ProjectCanvas::HitEntry> ProjectCanvas::hitEntries()
{
    if (!hitCacheDirty_) {
        return hitCache_;
    }
    updateViewTransform();
    hitCache_.clear();
    // Topmost-first so the cache lists candidates in front-to-back pick order. Invisible
    // leaves (e.g. an inactive C_livery section) are skipped => not pickable.
    forEachSceneShape([&](const fh6::scene::Shape &shape, const QTransform &world, int drawOrder) {
        if (shape.visible) {
            const QPolygonF polygon = screenQuad(world, sceneLocalRect(shape, geometry_));
            hitCache_.push_back({drawOrder, shape.id, polygon, polygon.boundingRect()});
        }
        return true;
    }, /*reverse=*/true);
    hitCacheDirty_ = false;
    return hitCache_;
}

QString ProjectCanvas::guideAtScreenPoint(const QPointF &point)
{
    if (sceneTree() == nullptr) {
        return {};
    }
    updateViewTransform();
    QString hit;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (!guide.visible || !isSectionActive(sectionGroupId)) {
            return true;
        }
        const QPolygonF polygon = screenQuad(world, sceneLocalRect(guide, geometry_));
        if (polygon.boundingRect().contains(point) && pointInPolygon(point, polygon)) {
            hit = guide.id;
            return false;  // topmost hit wins
        }
        return true;
    }, /*reverse=*/true);
    return hit;
}

std::optional<QColor> ProjectCanvas::guideColorAtScreenPoint(const QPointF &point) const
{
    if (project_ == nullptr || sceneTree() == nullptr) {
        return std::nullopt;
    }
    const_cast<ProjectCanvas *>(this)->updateViewTransform();
    std::optional<QColor> result;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (!guide.visible || !isSectionActive(sectionGroupId)) {
            return true;
        }
        const QPolygonF polygon = screenQuad(world, sceneLocalRect(guide, geometry_));
        if (!polygon.boundingRect().contains(point) || !pointInPolygon(point, polygon)) {
            return true;
        }
        const QImage image = guideImage(guide).convertToFormat(QImage::Format_ARGB32);
        if (image.isNull()) {
            return true;
        }
        const QTransform localToScreen = world * worldToScreen_;
        bool invertible = false;
        const QTransform screenToLocal = localToScreen.inverted(&invertible);
        if (!invertible) {
            return true;
        }
        const QSizeF guideSize = sceneNodeSize(guide, geometry_);
        const QPointF local = screenToLocal.map(point);
        const int x = static_cast<int>(std::floor(local.x() + guideSize.width() * 0.5));
        const int y = static_cast<int>(std::floor(local.y() + guideSize.height() * 0.5));
        if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
            return true;
        }
        const QColor color = QColor::fromRgba(image.pixel(x, y));
        if (color.alpha() == 0) {
            return true;  // transparent here => keep looking at guides below
        }
        result = color;
        return false;
    }, /*reverse=*/true);
    return result;
}

std::optional<QColor> ProjectCanvas::layerColorAtScreenPoint(const QPointF &point) const
{
    if (project_ == nullptr) {
        return std::nullopt;
    }
    const fh6::scene::Group *root = sceneTree();
    if (root == nullptr) {
        return std::nullopt;
    }
    for (const HitEntry &entry : const_cast<ProjectCanvas *>(this)->hitEntries()) {
        if (!entry.screenBounds.contains(point) || !pointInPolygon(point, entry.screenPolygon)) {
            continue;
        }
        if (state_ != nullptr) {
            if (auto *shape = dynamic_cast<const fh6::scene::Shape *>(state_->sceneNode(entry.layerId))) {
                return QColor(shape->color[ColorByteRed],
                              shape->color[ColorByteGreen],
                              shape->color[ColorByteBlue],
                              shape->color[ColorByteAlpha]);
            }
            continue;
        }
        for (const fh6::scene::Shape *shape : sceneShapeLeaves(*root)) {
            if (shape->id == entry.layerId) {
                return QColor(shape->color[ColorByteRed],
                              shape->color[ColorByteGreen],
                              shape->color[ColorByteBlue],
                              shape->color[ColorByteAlpha]);
            }
        }
    }
    return std::nullopt;
}

std::optional<QColor> ProjectCanvas::colorAtScreenPoint(const QPointF &point) const
{
    if (guideLayersOnTop_) {
        if (const std::optional<QColor> guide = guideColorAtScreenPoint(point)) {
            return guide;
        }
        return layerColorAtScreenPoint(point);
    }
    if (const std::optional<QColor> layer = layerColorAtScreenPoint(point)) {
        return layer;
    }
    return guideColorAtScreenPoint(point);
}

QVector<QString> ProjectCanvas::layersAtScreenPoint(const QPointF &point)
{
    QVector<QString> ids;
    for (const HitEntry &entry : hitEntries()) {
        if (entry.screenBounds.contains(point) && pointInPolygon(point, entry.screenPolygon)) {
            ids.push_back(entry.layerId);
        }
    }
    return ids;
}

QString ProjectCanvas::selectTargetAtScreenPoint(const QPointF &point, Qt::KeyboardModifiers modifiers)
{
    const QVector<QString> hits = layersAtScreenPoint(point);
    if (hits.isEmpty()) {
        return {};
    }
    if (modifiers & Qt::ControlModifier) {
        return hits.front();
    }

    QString target = hits.front();
    if (state_ != nullptr) {
        const QSet<QString> selected = state_->selectedLayerIds();
        for (int i = 0; i < hits.size(); ++i) {
            if (selected.contains(hits[i])) {
                target = hits[(i + 1) % hits.size()];
                break;
            }
        }
    }
    return target;
}

const QRectF &ProjectCanvas::cachedSelectionWorldBounds() const
{
    // Cache the world-axis union of the selection's bounds (invalidated on any
    // selection/geometry change) so the selection-flash repaints don't rescan every layer each
    // frame. Computed inline from the selection id sets - NOT via selectedLayers(), which calls
    // lockedLayerIds() (a full buildEntryMaps) and would drop the box entirely if any selected
    // layer were locked; this path runs every paint/drag frame.
    if (!selectionWorldBoundsCache_.has_value()) {
        BoundsAccumulator acc;
        if (project_ != nullptr && state_ != nullptr) {
            const QSet<QString> selected = state_->selectedLayerIds();
            if (!selected.isEmpty()) {
                forEachSceneShape([&](const fh6::scene::Shape &shape, const QTransform &world, int) {
                    if (selected.contains(shape.id)) {
                        acc.add(world, flatEntryVisualRect(shape, geometry_));
                    }
                    return true;
                }, /*reverse=*/false);
            }
            const QSet<QString> selectedGuides = state_->selectedGuideLayerIds();
            if (!selectedGuides.isEmpty()) {
                forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &) {
                    if (selectedGuides.contains(guide.id)) {
                        acc.add(world, flatEntryRect(guide));
                    }
                    return true;
                }, /*reverse=*/false);
            }
        }
        selectionWorldBoundsCache_ = acc.hasBounds() ? acc.bounds() : QRectF();
    }
    return *selectionWorldBoundsCache_;
}

QRectF ProjectCanvas::selectedScreenBounds() const
{
    if (project_ == nullptr || state_ == nullptr) {
        return {};
    }
    // The world-AABB box and the view have no rotation, so the screen bounds are just the world
    // bounds mapped through the current view transform.
    const QRectF worldBounds = cachedSelectionWorldBounds();
    if (!worldBounds.isValid()) {
        return {};
    }
    return worldToScreen_.mapRect(worldBounds);
}

ProjectCanvas::SelectionBox ProjectCanvas::currentSelectionBox() const
{
    SelectionBox box;
    if (project_ == nullptr || state_ == nullptr) {
        return box;
    }
    const QSet<QString> selLayers = state_->selectedLayerIds();
    const QSet<QString> selGuides = state_->selectedGuideLayerIds();
    if (selLayers.isEmpty() && selGuides.isEmpty()) {
        return box;
    }

    EffectiveSelection effective;
    effective.groupIds = state_->fullySelectedTopGroupIds();
    for (const QString &groupId : effective.groupIds) {
        for (const QString &leafId : state_->leafLayerIdsForEntry(groupId)) {
            effective.groupedLayerIds.insert(leafId);
        }
        if (const fh6::scene::Group *group = state_->groupForId(groupId)) {
            collectGuideIds(*group, effective.groupedGuideIds);
        }
    }
    effective.looseLayerIds = selLayers;
    effective.looseLayerIds.subtract(effective.groupedLayerIds);
    effective.looseGuideIds = selGuides;
    effective.looseGuideIds.subtract(effective.groupedGuideIds);
    const int count = effective.count();
    if (count == 0) {
        return box;
    }

    // Detect a selection change so the Relative multi-selection box's reference rotation can be
    // recaptured (lazy, so it never resets mid-drag where the selection is stable).
    const bool signatureChanged = selLayers != frameLayerSignature_ || selGuides != frameGuideSignature_;
    if (signatureChanged) {
        frameLayerSignature_ = selLayers;
        frameGuideSignature_ = selGuides;
    }

    // Absolute mode (default): identity frame over the world-axis AABB - reduces to legacy.
    if (!transformRelativeMode_) {
        const QRectF worldBounds = cachedSelectionWorldBounds();
        if (!worldBounds.isValid()) {
            return box;
        }
        box.valid = true;
        box.localRect = worldBounds;
        box.localToWorld = QTransform();
        return box;
    }

    // Relative mode, single item: the box is the shape's own transformed rect (parallelogram,
    // skew included), so scaling is a pure per-axis scale of the shape's local axes.
    if (count == 1) {
        if (!effective.groupIds.isEmpty()) {
            const QString id = effective.groupIds.front();
            if (auto *group = dynamic_cast<fh6::scene::Group *>(state_->sceneNode(id))) {
                const QTransform groupWorld = sceneWorldTransform(*group);
                bool invertible = false;
                const QTransform groupWorldInverse = groupWorld.inverted(&invertible);
                if (!invertible) {
                    return box;
                }
                BoundsAccumulator acc;
                for (const SceneRenderEntry &entry : state_->renderEntries()) {
                    if (entry.shape != nullptr && effective.groupedLayerIds.contains(entry.shape->id)) {
                        acc.add(entry.worldTransform * groupWorldInverse,
                                flatEntryVisualRect(*entry.shape, geometry_));
                    } else if (entry.guide != nullptr && effective.groupedGuideIds.contains(entry.guide->id)) {
                        acc.add(entry.worldTransform * groupWorldInverse, flatEntryRect(*entry.guide));
                    }
                }
                if (acc.hasBounds()) {
                    box.valid = true;
                    box.localRect = acc.bounds();
                    box.localToWorld = groupWorld;
                }
                return box;
            }
        } else if (!effective.looseLayerIds.isEmpty()) {
            const QString id = *effective.looseLayerIds.constBegin();
            if (auto *layer = dynamic_cast<fh6::scene::Shape *>(state_->sceneNode(id))) {
                box.valid = true;
                box.localRect = flatEntryVisualRect(*layer, geometry_);
                box.localToWorld = sceneWorldTransform(*layer);
                return box;
            }
        } else {
            const QString id = *effective.looseGuideIds.constBegin();
            if (auto *guide = dynamic_cast<fh6::scene::GuideLayer *>(state_->sceneNode(id))) {
                box.valid = true;
                box.localRect = flatEntryRect(*guide);
                box.localToWorld = sceneWorldTransform(*guide);
                return box;
            }
        }
        return box;
    }

    // Relative mode, multi-selection/group: oriented bounding box at the derived frame angle.
    const QRectF worldBounds = cachedSelectionWorldBounds();
    if (!worldBounds.isValid()) {
        return box;
    }
    // The frame angle follows the primary selected item's rotation since selection time. Read it
    // live from layer data so undo/redo restore the box orientation along with the shapes. The
    // primary is the first selected item in project order, for deterministic, stable choice.
    double primaryRotation = 0.0;
    bool hasPrimary = false;
    if (project_->root) {
        for (const QString &groupId : effective.groupIds) {
            if (const auto *group = dynamic_cast<const fh6::scene::Group *>(state_->sceneNode(groupId))) {
                primaryRotation = group->rotation;
                hasPrimary = true;
                break;
            }
        }
        for (const fh6::scene::Shape *layer : sceneShapeLeaves(*project_->root)) {
            if (!hasPrimary && effective.looseLayerIds.contains(layer->id)) {
                primaryRotation = layer->rotation;
                hasPrimary = true;
                break;
            }
        }
    }
    if (!hasPrimary && project_->root) {
        for (const fh6::scene::GuideLayer *guide : sceneGuideLeaves(*project_->root)) {
            if (effective.looseGuideIds.contains(guide->id)) {
                primaryRotation = guide->rotation;
                hasPrimary = true;
                break;
            }
        }
    }
    if (signatureChanged) {
        frameReferenceRotation_ = primaryRotation;
    }
    double frameAngle = normalizeRotation(primaryRotation - frameReferenceRotation_);
    // A non-uniform group scale (or shear) applies a world affine to every child, and
    // decomposeScaleResult() re-extracts each child's rotation from the result - which drifts for
    // any child rotated relative to the box axes, the primary included. Deriving the frame angle
    // from the primary's live rotation would then spin the whole box while a scale/skew handle is
    // dragged (and leave it tilted afterwards): an artifact carried over from the image_transformer
    // port, not an intended rotation. While such a drag is active, pin the frame to the angle
    // captured at press (held in dragStartBox_) and rebase the reference so the pinned angle also
    // survives the release. The extent below still recomputes live, so the box keeps tightly
    // bounding the shapes as they scale; only its orientation is held steady.
    if ((dragMode_ == DragMode::Scale || dragMode_ == DragMode::Skew) && dragStartBox_.valid) {
        frameAngle = normalizeRotation(std::atan2(dragStartBox_.localToWorld.m12(),
                                                  dragStartBox_.localToWorld.m11())
                                       * 180.0 / kPi);
        frameReferenceRotation_ = normalizeRotation(primaryRotation - frameAngle);
    }
    const QPointF center = worldBounds.center();
    const double theta = frameAngle * kPi / 180.0;
    const QPointF axisU(std::cos(theta), std::sin(theta));
    const QPointF axisV(-std::sin(theta), std::cos(theta));
    bool hasExtent = false;
    double minA = 0.0;
    double maxA = 0.0;
    double minB = 0.0;
    double maxB = 0.0;
    const auto accumulate = [&](const QTransform &xform, const QRectF &local) {
        const QPointF corners[4] = {
            xform.map(local.topLeft()), xform.map(local.topRight()),
            xform.map(local.bottomRight()), xform.map(local.bottomLeft()),
        };
        for (const QPointF &corner : corners) {
            const QPointF rel = corner - center;
            const double a = rel.x() * axisU.x() + rel.y() * axisU.y();
            const double b = rel.x() * axisV.x() + rel.y() * axisV.y();
            if (!hasExtent) {
                minA = maxA = a;
                minB = maxB = b;
                hasExtent = true;
            } else {
                minA = std::min(minA, a);
                maxA = std::max(maxA, a);
                minB = std::min(minB, b);
                maxB = std::max(maxB, b);
            }
        }
    };
    if (project_->root) {
        for (const fh6::scene::Shape *layer : sceneShapeLeaves(*project_->root)) {
            if (!selLayers.contains(layer->id)) {
                continue;
            }
            accumulate(sceneWorldTransform(*layer), flatEntryVisualRect(*layer, geometry_));
        }
        for (const fh6::scene::GuideLayer *guide : sceneGuideLeaves(*project_->root)) {
            if (!selGuides.contains(guide->id)) {
                continue;
            }
            accumulate(sceneWorldTransform(*guide), flatEntryRect(*guide));
        }
    }
    if (!hasExtent) {
        return box;
    }
    box.valid = true;
    box.localRect = QRectF(minA, minB, maxA - minA, maxB - minB);
    QTransform frame;
    frame.translate(center.x(), center.y());
    frame.rotate(frameAngle);
    box.localToWorld = frame;
    return box;
}

QTransform ProjectCanvas::boxToScreen(const SelectionBox &box) const
{
    return box.localToWorld * worldToScreen_;
}

bool ProjectCanvas::currentTransformBox(QPointF *center,
                                        double *width,
                                        double *height,
                                        QTransform *boxFrame) const
{
    const SelectionBox box = currentSelectionBox();
    if (!box.valid || box.localRect.isEmpty()) {
        return false;
    }
    if (center != nullptr) {
        *center = box.localToWorld.map(box.localRect.center());
    }
    if (width != nullptr) {
        *width = box.localRect.width();
    }
    if (height != nullptr) {
        *height = box.localRect.height();
    }
    if (boxFrame != nullptr) {
        *boxFrame = box.localToWorld;
    }
    return true;
}

bool ProjectCanvas::boxContainsScreenPoint(const SelectionBox &box, const QPointF &screenPoint) const
{
    if (!box.valid) {
        return false;
    }
    bool invertible = false;
    const QTransform inv = boxToScreen(box).inverted(&invertible);
    if (!invertible) {
        return false;
    }
    return box.localRect.contains(inv.map(screenPoint));
}

QRectF ProjectCanvas::selectedWorldBounds() const
{
    BoundsAccumulator acc;
    for (const fh6::scene::Shape *layer : selectedLayers()) {
        acc.add(flatEntryTransform(*layer), flatEntryVisualRect(*layer, geometry_));
    }
    for (const fh6::scene::GuideLayer *guide : selectedGuideLayers()) {
        acc.add(flatEntryTransform(*guide), flatEntryRect(*guide));
    }
    return acc.bounds();
}

QVector<fh6::scene::Shape *> ProjectCanvas::selectedLayers() const
{
    if (state_ == nullptr) {
        return {};
    }
    const QSet<QString> locked = state_->lockedLayerIds();
    for (const QString &id : state_->selectedLayerIds()) {
        if (locked.contains(id)) {
            return {};
        }
    }
    QVector<fh6::scene::Shape *> result;
    for (fh6::scene::Shape *layer : state_->selectedLayers()) {
        if (!locked.contains(layer->id)) {
            result.push_back(layer);
        }
    }
    return result;
}

QVector<fh6::scene::GuideLayer *> ProjectCanvas::selectedGuideLayers() const
{
    if (state_ == nullptr) {
        return {};
    }
    for (fh6::scene::GuideLayer *guide : state_->selectedGuideLayers()) {
        if (guide->locked) {
            return {};
        }
    }
    return state_->selectedGuideLayers();
}

QString ProjectCanvas::transformHandleAt(const QPointF &point, const SelectionBox &box) const
{
    if (!box.valid) {
        return {};
    }
    bool invertible = false;
    const QTransform toScreen = boxToScreen(box);
    const QTransform toLocal = toScreen.inverted(&invertible);
    if (!invertible) {
        return {};
    }
    // Work in the box's local frame: the box is its axis-aligned localRect there, so the legacy
    // band logic applies verbatim once the screen-pixel reach constants are converted to local
    // units using the per-axis screen length of each local unit vector (handles rotation/skew).
    const QPointF local = toLocal.map(point);
    const QPointF originScreen = toScreen.map(box.localRect.center());
    const double lenX = QLineF(originScreen, toScreen.map(box.localRect.center() + QPointF(1.0, 0.0))).length();
    const double lenY = QLineF(originScreen, toScreen.map(box.localRect.center() + QPointF(0.0, 1.0))).length();
    if (lenX < 1e-9 || lenY < 1e-9) {
        return {};
    }
    const double insideX = ScaleGrabInside / lenX;
    const double outsideX = ScaleGrabOutside / lenX;
    const double insideY = ScaleGrabInside / lenY;
    const double outsideY = ScaleGrabOutside / lenY;
    const double handleHalfX = HandleHalf / lenX;
    const double handleHalfY = HandleHalf / lenY;

    // Skew handle sits just above the top edge, for single shapes and groups alike.
    {
        const QPointF skew(box.localRect.center().x(), box.localRect.top() - SkewHandleOffset / lenY);
        if (std::abs(local.x() - skew.x()) <= handleHalfX && std::abs(local.y() - skew.y()) <= handleHalfY) {
            return QStringLiteral("skew");
        }
    }

    // Scale band straddling each edge: it reaches ScaleGrabInside into the box and
    // ScaleGrabOutside past it. The interior beyond the band is left to Move; the area past a
    // corner is left to Rotate (rotateZoneAt). Where two edge bands meet, Scale is two-axis.
    const double left = box.localRect.left();
    const double right = box.localRect.right();
    const double top = box.localRect.top();
    const double bottom = box.localRect.bottom();
    const bool nearLeft = local.x() >= left - outsideX && local.x() <= left + insideX;
    const bool nearRight = local.x() >= right - insideX && local.x() <= right + outsideX;
    const bool nearTop = local.y() >= top - outsideY && local.y() <= top + insideY;
    const bool nearBottom = local.y() >= bottom - insideY && local.y() <= bottom + outsideY;
    // Single-axis scale only counts when the orthogonal coordinate lies within the box's
    // span, so it never bleeds into the diagonal corner-proximity (rotate) regions.
    const bool spanX = local.x() >= left && local.x() <= right;
    const bool spanY = local.y() >= top && local.y() <= bottom;

    // Corners (two-axis) take priority over the sides.
    if (nearLeft && nearTop) {
        return QStringLiteral("top_left");
    }
    if (nearRight && nearTop) {
        return QStringLiteral("top_right");
    }
    if (nearLeft && nearBottom) {
        return QStringLiteral("bottom_left");
    }
    if (nearRight && nearBottom) {
        return QStringLiteral("bottom_right");
    }
    // Sides (single-axis).
    if (nearLeft && spanY) {
        return QStringLiteral("left");
    }
    if (nearRight && spanY) {
        return QStringLiteral("right");
    }
    if (nearTop && spanX) {
        return QStringLiteral("top");
    }
    if (nearBottom && spanX) {
        return QStringLiteral("bottom");
    }
    return {};
}

bool ProjectCanvas::rotateZoneAt(const QPointF &point, const SelectionBox &box) const
{
    // Rotate is the outer-anchor affordance: strictly outside the box, in the diagonal region
    // past a corner. Move owns the interior and Scale owns the edges and corner anchors
    // (resolved by transformHandleAt before this is consulted), so anything reaching here near
    // an edge is already past the scale band  Ewe only claim past the corner. Reach stays in
    // screen pixels; the inside/outward tests run in the box's (possibly rotated) local frame.
    if (!box.valid) {
        return false;
    }
    bool invertible = false;
    const QTransform toScreen = boxToScreen(box);
    const QTransform toLocal = toScreen.inverted(&invertible);
    if (!invertible) {
        return false;
    }
    const QPointF local = toLocal.map(point);
    if (box.localRect.contains(local)) {
        return false;
    }
    const QPointF centerLocal = box.localRect.center();
    const QPointF cornersLocal[4] = {
        box.localRect.topLeft(), box.localRect.topRight(), box.localRect.bottomLeft(), box.localRect.bottomRight(),
    };
    for (const QPointF &cornerLocal : cornersLocal) {
        const double dist = QLineF(point, toScreen.map(cornerLocal)).length();
        if (dist > RotateCornerReach) {
            continue;
        }
        const bool outwardX = (cornerLocal.x() < centerLocal.x()) ? (local.x() < cornerLocal.x()) : (local.x() > cornerLocal.x());
        const bool outwardY = (cornerLocal.y() < centerLocal.y()) ? (local.y() < cornerLocal.y()) : (local.y() > cornerLocal.y());
        if (outwardX && outwardY) {
            return true;
        }
    }
    return false;
}

bool ProjectCanvas::selectionIsGroupLike() const
{
    if (state_ == nullptr) {
        return false;
    }
    // Counting the selection id sets is O(1) and avoids resolving every selected pointer each
    // paint (this runs inside drawOverlay, including during the selection-flash repaints).
    return state_->selectedLayerIds().size() + state_->selectedGuideLayerIds().size() > 1;
}

bool ProjectCanvas::pointInPolygon(const QPointF &point, const QPolygonF &polygon) const
{
    if (polygon.size() < 3) {
        return false;
    }
    bool signSet = false;
    double sign = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF a = polygon[i];
        const QPointF b = polygon[(i + 1) % polygon.size()];
        const double cross = (b.x() - a.x()) * (point.y() - a.y()) - (b.y() - a.y()) * (point.x() - a.x());
        if (std::abs(cross) < 1e-8) {
            continue;
        }
        const double current = cross > 0.0 ? 1.0 : -1.0;
        if (!signSet) {
            sign = current;
            signSet = true;
        } else if (current != sign) {
            return false;
        }
    }
    return true;
}

bool ProjectCanvas::movedPastClickThreshold(const QPointF &point) const
{
    const QPointF delta = point - dragStartScreen_;
    return delta.x() * delta.x() + delta.y() * delta.y() > ClickDragThreshold * ClickDragThreshold;
}

} // namespace gui
