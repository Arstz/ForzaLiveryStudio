#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

// Shared helpers (flatEntry*, EffectiveSelection, collectGuideIds, handle axes,
// handle-box geometry constants, buildTransformTargetIds, normalizeRotation) live in
// project_canvas_internal.h so every ProjectCanvas translation unit reuses one definition.
using namespace pc_detail;

namespace {

template <typename Start>
QTransform entryStartTransform(const Start &start)
{
    QTransform transform;
    transform.translate(start.x, start.y);
    transform.rotate(start.rotation);
    transform.shear(start.skew, 0.0);
    transform.scale(start.scaleX, start.scaleY);
    return transform;
}

QTransform parentWorldTransform(const fh6::scene::Layer &node)
{
    const fh6::scene::Layer *parent = node.parent();
    return parent != nullptr ? sceneWorldTransform(*parent) : QTransform();
}

template <typename Start>
QTransform localResultForWorldTransform(const fh6::scene::Layer &node,
                                        const Start &start,
                                        const QTransform &worldTransform)
{
    const QTransform startLocal = entryStartTransform(start);
    const QTransform parentWorld = parentWorldTransform(node);
    bool invertible = false;
    const QTransform parentWorldInverse = parentWorld.inverted(&invertible);
    if (!invertible) {
        return startLocal;
    }
    return startLocal * parentWorld * worldTransform * parentWorldInverse;
}

double snapRotation(double degrees, Qt::KeyboardModifiers modifiers)
{
    if (modifiers & Qt::ShiftModifier) {
        return std::round(degrees / 15.0) * 15.0;
    }
    return degrees;
}

QPointF constrainDelta(QPointF delta, Qt::KeyboardModifiers modifiers)
{
    if (!(modifiers & Qt::ShiftModifier)) {
        return delta;
    }
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
        return {delta.x(), 0.0};
    }
    return {0.0, delta.y()};
}

QString formatHintNumber(double value, int decimals = 2)
{
    if (std::abs(value) < 0.005) {
        value = 0.0;
    }
    return QString::number(value, 'f', decimals);
}

// Decomposed affine result shared by the Shape/Guide scale-drag loops. ok is false
// when the X axis collapsed (degenerate); skew falls back to fallbackSkew then.
struct ScaleDecomposition {
    bool ok = false;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    double skew = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};

ScaleDecomposition decomposeScaleResult(const QTransform &result, double fallbackSkew)
{
    const double a = result.m11();
    const double b = result.m12();
    const double c = result.m21();
    const double d = result.m22();
    const double scaleXLen = std::hypot(a, b);
    if (scaleXLen < 1e-9) {
        return {};
    }
    const double det = a * d - b * c;
    ScaleDecomposition out;
    out.ok = true;
    out.x = result.dx();
    out.y = result.dy();
    out.rotation = normalizeRotation(std::atan2(b, a) * 180.0 / kPi);
    out.skew = std::abs(det) > 1e-9 ? (a * c + b * d) / det : fallbackSkew;
    out.scaleX = std::clamp(scaleXLen, -100.0, 100.0);
    out.scaleY = std::clamp(det / scaleXLen, -100.0, 100.0);
    return out;
}

// Write a decomposed affine back to a scene leaf's editor fields. Guides carry no skew, so
// only Shape leaves receive it.
template <typename Item>
void assignDecomposition(Item *item, const ScaleDecomposition &dec)
{
    item->x = dec.x;
    item->y = dec.y;
    item->rotation = dec.rotation;
    item->scaleX = dec.scaleX;
    item->scaleY = dec.scaleY;
    if constexpr (std::is_same_v<Item, fh6::scene::Shape>) {
        item->skew = dec.skew;
    }
}

} // namespace


// Tab flips the selection, cycling through the four scale-sign states in the order
// (+x +y) -> (+x -y) -> (-x -y) -> (-x +y) -> (+x +y). Each press mirrors the whole
// selection about its combined centre, so a multi-shape group mirrors as a unit
// (and a single shape flips in place).
void ProjectCanvas::cycleFlipSelection()
{
    if (state_ == nullptr || project_ == nullptr) {
        return;
    }
    QVector<fh6::scene::Shape *> layers = selectedLayers();
    QVector<fh6::scene::GuideLayer *> guides = selectedGuideLayers();
    if (layers.isEmpty() && guides.isEmpty()) {
        return;
    }

    // Derive the current state from a representative item, then advance one step.
    // flipV mirrors x positions and is represented by negative scaleY after
    // the rotation complement; flipH mirrors y positions and is represented by
    // negative scaleX. This matches decomposing a world-axis reflection back
    // into the editor's rotate-then-shear-then-scale layer fields.
    // Toggling either axis also complements the rotation and negates the skew:
    // with the rotate-then-scale convention chosen here, the rotation complement
    // alone does not absorb the shear, so the shear sign must be flipped too or
    // skewed shapes reflect incorrectly. (In a group the per-flip skew error
    // cancels every other press, so the offset only shows up on odd presses.)
    const double repScaleX = layers.isEmpty() ? guides.front()->scaleX : layers.front()->scaleX;
    const double repScaleY = layers.isEmpty() ? guides.front()->scaleY : layers.front()->scaleY;
    const int currentState = (repScaleY < 0 ? 1 : 0) + (repScaleX < 0 ? 2 : 0);
    const QVector<int> cycle = {0, 1, 3, 2};
    const int cycleIndex = cycle.contains(currentState) ? cycle.indexOf(currentState) : 0;
    const int nextState = cycle[(cycleIndex + 1) % cycle.size()];
    const bool toggleVertical = (repScaleY < 0) != ((nextState & 1) != 0);
    const bool toggleHorizontal = (repScaleX < 0) != ((nextState & 2) != 0);
    if (!toggleVertical && !toggleHorizontal) {
        return;
    }

    const QPointF center = selectedWorldBounds().center();
    const auto complementRotation = [](double rotation) {
        return normalizeRotation(180.0 - rotation);
    };

    layers = selectedLayers();
    guides = selectedGuideLayers();
    state_->beginTransformCommand(buildTransformTargetIds({}, layers, guides));

    for (fh6::scene::Shape *layer : layers) {
        if (toggleVertical) {
            layer->x = 2.0 * center.x() - layer->x;
            layer->scaleY = -layer->scaleY;
            layer->skew = -layer->skew;
            layer->rotation = complementRotation(layer->rotation);
        }
        if (toggleHorizontal) {
            layer->y = 2.0 * center.y() - layer->y;
            layer->scaleX = -layer->scaleX;
            layer->skew = -layer->skew;
            layer->rotation = complementRotation(layer->rotation);
        }
    }
    for (fh6::scene::GuideLayer *guide : guides) {
        if (toggleVertical) {
            guide->x = 2.0 * center.x() - guide->x;
            guide->scaleY = -guide->scaleY;
            guide->rotation = complementRotation(guide->rotation);
        }
        if (toggleHorizontal) {
            guide->y = 2.0 * center.y() - guide->y;
            guide->scaleX = -guide->scaleX;
            guide->rotation = complementRotation(guide->rotation);
        }
    }

    state_->commitTransformCommand();
    state_->noteProjectGeometryChanged(false, state_->selectedTransformTargetIds());
    invalidateSceneCache();
    update();
}

void ProjectCanvas::collectDragGroups(QVector<QString> &groupIds,
                                      QHash<QString, QTransform> &startFrames,
                                      QSet<QString> &groupedLayerIds,
                                      QSet<QString> &groupedGuideIds) const
{
    if (state_ == nullptr) {
        return;
    }
    for (const QString &id : state_->fullySelectedTopGroupIds()) {
        if (state_->entryHasLockedLayer(id)) {
            continue;
        }
        groupIds.push_back(id);
        startFrames.insert(id, state_->groupLocalFrame(id));
        for (const QString &leafId : state_->leafLayerIdsForEntry(id)) {
            groupedLayerIds.insert(leafId);
        }
        if (const fh6::scene::Group *group = state_->groupForId(id)) {
            collectGuideIds(*group, groupedGuideIds);
        }
    }
}

void ProjectCanvas::captureDragStarts()
{
    dragStarts_.clear();
    dragGuideStarts_.clear();
    // Capture the frames of whole groups being transformed so a box drag carries the
    // group's own transform in the scene tree. Descendant leaves/guides covered by
    // those group roots are excluded below so the transform is applied exactly once.
    dragGroupIds_.clear();
    dragGroupStartFrames_.clear();
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    collectDragGroups(dragGroupIds_, dragGroupStartFrames_, groupedLayerIds, groupedGuideIds);
    dragLayers_.clear();
    dragGuides_.clear();
    for (fh6::scene::Shape *layer : selectedLayers()) {
        if (!groupedLayerIds.contains(layer->id)) {
            dragLayers_.push_back(layer);
            dragStarts_.insert(layer->id, {layer->x, layer->y, layer->scaleX, layer->scaleY, layer->rotation, layer->skew});
        }
    }
    for (fh6::scene::GuideLayer *guide : selectedGuideLayers()) {
        if (!groupedGuideIds.contains(guide->id)) {
            dragGuides_.push_back(guide);
            dragGuideStarts_.insert(guide->id, {guide->x, guide->y, guide->scaleX, guide->scaleY, guide->rotation});
        }
    }
}

QVector<QString> ProjectCanvas::dragTransformTargetIds() const
{
    return buildTransformTargetIds(dragGroupIds_, dragLayers_, dragGuides_);
}

QString ProjectCanvas::transformSelectionSignature() const
{
    if (state_ == nullptr) {
        return {};
    }
    QStringList parts;
    for (const QString &id : state_->selectedLayerIds()) {
        parts.push_back(QStringLiteral("l:%1").arg(id));
    }
    for (const QString &id : state_->selectedGuideLayerIds()) {
        parts.push_back(QStringLiteral("g:%1").arg(id));
    }
    parts.sort();
    return parts.join(QLatin1Char('|'));
}

QSizeF ProjectCanvas::transformBoxVisualExtents(const SelectionBox &box) const
{
    if (!box.valid || box.localRect.isEmpty()) {
        return {};
    }
    const QPointF origin = box.localToWorld.map(QPointF(0.0, 0.0));
    const double axisX = QLineF(origin, box.localToWorld.map(QPointF(1.0, 0.0))).length();
    const double axisY = QLineF(origin, box.localToWorld.map(QPointF(0.0, 1.0))).length();
    return QSizeF(std::abs(box.localRect.width()) * std::max(axisX, 1e-9),
                  std::abs(box.localRect.height()) * std::max(axisY, 1e-9));
}

void ProjectCanvas::captureScaleHintReference()
{
    const QString signature = transformSelectionSignature();
    const QSizeF startExtents = transformBoxVisualExtents(dragStartBox_);
    const bool reset = !scaleHintBaseValid_
        || scaleHintSelectionSignature_ != signature
        || scaleHintBaseExtents_.width() <= 1e-9
        || scaleHintBaseExtents_.height() <= 1e-9;
    if (reset) {
        scaleHintSelectionSignature_ = signature;
        scaleHintBaseExtents_ = startExtents;
        scaleHintBaseValid_ = startExtents.width() > 1e-9 && startExtents.height() > 1e-9;
    }
    scaleHintStartScaleX_ = scaleHintBaseValid_ ? startExtents.width() / scaleHintBaseExtents_.width() : 1.0;
    scaleHintStartScaleY_ = scaleHintBaseValid_ ? startExtents.height() / scaleHintBaseExtents_.height() : 1.0;
}

void ProjectCanvas::applyWorldTransformToDragItems(const QTransform &worldTransform)
{
    const auto applyItem = [&](auto *item, const EntryStart &start) {
        const ScaleDecomposition dec = decomposeScaleResult(localResultForWorldTransform(*item, start, worldTransform), start.skew);
        if (dec.ok) {
            assignDecomposition(item, dec);
        }
    };
    for (fh6::scene::Shape *layer : dragLayers_) {
        applyItem(layer, dragStarts_.value(layer->id));
    }
    for (fh6::scene::GuideLayer *guide : dragGuides_) {
        applyItem(guide, dragGuideStarts_.value(guide->id));
    }
    if (!dragGroupStartFrames_.isEmpty()) {
        state_->setGroupFramesFromStart(dragGroupStartFrames_, worldTransform);
    }
}

void ProjectCanvas::applyMoveDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers)
{
    updateCursorForPoint(screenPoint);
    const QPointF delta = constrainDelta(screenToWorld(screenPoint) - dragStartWorld_, modifiers);
    applyWorldTransformToDragItems(QTransform::fromTranslate(delta.x(), delta.y()));
    setCursorHint(screenPoint, {QStringLiteral("X: %1").arg(formatHintNumber(delta.x())),
                                QStringLiteral("Y: %1").arg(formatHintNumber(delta.y()))});
    requestLiveSceneUpdate();
}

bool ProjectCanvas::nudgeSelection(const QPointF &delta)
{
    if (state_ == nullptr || project_ == nullptr || delta.isNull()) {
        return false;
    }
    if (selectedLayers().isEmpty() && selectedGuideLayers().isEmpty()) {
        return false;
    }

    // Same group/loose partition as a drag: whole groups move via their frame, loose leaves
    // (excluding those nested under a moved group) decompose the world move per item.
    QVector<QString> groupIds;
    QHash<QString, QTransform> groupStartFrames;
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    collectDragGroups(groupIds, groupStartFrames, groupedLayerIds, groupedGuideIds);

    QVector<fh6::scene::Shape *> layers;
    QVector<fh6::scene::GuideLayer *> guides;
    for (fh6::scene::Shape *layer : selectedLayers()) {
        if (!groupedLayerIds.contains(layer->id)) {
            layers.push_back(layer);
        }
    }
    for (fh6::scene::GuideLayer *guide : selectedGuideLayers()) {
        if (!groupedGuideIds.contains(guide->id)) {
            guides.push_back(guide);
        }
    }
    if (layers.isEmpty() && guides.isEmpty() && groupStartFrames.isEmpty()) {
        return false;
    }

    state_->beginTransformCommand(buildTransformTargetIds(groupIds, layers, guides));

    const QTransform worldMove = QTransform::fromTranslate(delta.x(), delta.y());
    for (fh6::scene::Shape *layer : layers) {
        const EntryStart start{layer->x, layer->y, layer->scaleX, layer->scaleY, layer->rotation, layer->skew};
        const ScaleDecomposition dec = decomposeScaleResult(localResultForWorldTransform(*layer, start, worldMove), start.skew);
        if (dec.ok) {
            assignDecomposition(layer, dec);
        }
    }
    for (fh6::scene::GuideLayer *guide : guides) {
        const EntryStart start{guide->x, guide->y, guide->scaleX, guide->scaleY, guide->rotation, 0.0};
        const ScaleDecomposition dec = decomposeScaleResult(localResultForWorldTransform(*guide, start, worldMove), start.skew);
        if (dec.ok) {
            assignDecomposition(guide, dec);
        }
    }
    if (!groupStartFrames.isEmpty()) {
        state_->setGroupFramesFromStart(groupStartFrames, worldMove);
    }

    state_->commitTransformCommand();
    state_->noteProjectGeometryChanged(false, state_->selectedTransformTargetIds());
    invalidateSceneCache();
    invalidateSelectionCache();
    update();
    return true;
}

namespace {

// A movable align/distribute unit: a whole group (moved via its frame) or a loose
// shape/guide leaf (moved by decomposing a world translation into its local fields).
struct MoveUnit {
    QString groupId;
    fh6::scene::Shape *layer = nullptr;
    fh6::scene::GuideLayer *guide = nullptr;
};

} // namespace

bool ProjectCanvas::applyAlignDistribute(const std::function<QVector<QPointF>(const QVector<QRectF> &)> &computeDeltas,
                                         bool descendSoleGroup)
{
    if (state_ == nullptr || project_ == nullptr) {
        return false;
    }
    if (selectedLayers().isEmpty() && selectedGuideLayers().isEmpty()) {
        return false;
    }

    // Units are moved either via a group's frame (whole group) or by decomposing a world
    // translation into a loose leaf's local fields. The add* helpers keep the parallel
    // partition lists (used for the transform command) in step with units/bounds.
    QVector<QString> groupIds;
    QVector<fh6::scene::Shape *> looseLayers;
    QVector<fh6::scene::GuideLayer *> looseGuides;
    QVector<MoveUnit> units;
    QVector<QRectF> bounds;

    const auto addGroupUnit = [&](const QString &id) {
        fh6::scene::Layer *node = state_->sceneNode(id);
        if (node == nullptr || node->kind() != fh6::scene::LayerKind::Group) {
            return;
        }
        const auto &group = static_cast<const fh6::scene::Group &>(*node);
        BoundsAccumulator acc;
        for (const fh6::scene::Shape *shape : sceneShapeLeaves(group)) {
            acc.add(sceneWorldTransform(*shape), flatEntryVisualRect(*shape, geometry_));
        }
        for (const fh6::scene::GuideLayer *guide : sceneGuideLeaves(group)) {
            acc.add(sceneWorldTransform(*guide), flatEntryRect(*guide));
        }
        if (!acc.hasBounds()) {
            return;
        }
        groupIds.push_back(id);
        units.push_back({id, nullptr, nullptr});
        bounds.push_back(acc.bounds());
    };
    const auto addLayerUnit = [&](fh6::scene::Shape *layer) {
        looseLayers.push_back(layer);
        units.push_back({QString(), layer, nullptr});
        bounds.push_back(sceneWorldTransform(*layer).mapRect(flatEntryVisualRect(*layer, geometry_)));
    };
    const auto addGuideUnit = [&](fh6::scene::GuideLayer *guide) {
        looseGuides.push_back(guide);
        units.push_back({QString(), nullptr, guide});
        bounds.push_back(sceneWorldTransform(*guide).mapRect(flatEntryRect(*guide)));
    };

    // Same group/loose partition as a drag: a fully-selected group is one unit, loose
    // leaves not covered by a moved group are their own units.
    QVector<QString> selGroupIds;
    QHash<QString, QTransform> groupStartFrames;
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    collectDragGroups(selGroupIds, groupStartFrames, groupedLayerIds, groupedGuideIds);
    for (const QString &groupId : selGroupIds) {
        addGroupUnit(groupId);
    }
    for (fh6::scene::Shape *layer : selectedLayers()) {
        if (!groupedLayerIds.contains(layer->id)) {
            addLayerUnit(layer);
        }
    }
    for (fh6::scene::GuideLayer *guide : selectedGuideLayers()) {
        if (!groupedGuideIds.contains(guide->id)) {
            addGuideUnit(guide);
        }
    }

    // A lone selected group operates on its direct children instead (e.g. distribute the
    // members of a group without ungrouping it first).
    if (descendSoleGroup && units.size() == 1 && !units.front().groupId.isEmpty()) {
        fh6::scene::Layer *node = state_->sceneNode(units.front().groupId);
        if (node != nullptr && node->kind() == fh6::scene::LayerKind::Group) {
            const auto &group = static_cast<fh6::scene::Group &>(*node);
            groupIds.clear();
            looseLayers.clear();
            looseGuides.clear();
            units.clear();
            bounds.clear();
            for (const auto &child : group.children) {
                switch (child->kind()) {
                case fh6::scene::LayerKind::Group:
                    addGroupUnit(child->id);
                    break;
                case fh6::scene::LayerKind::Shape:
                    addLayerUnit(static_cast<fh6::scene::Shape *>(child.get()));
                    break;
                case fh6::scene::LayerKind::Guide:
                    addGuideUnit(static_cast<fh6::scene::GuideLayer *>(child.get()));
                    break;
                }
            }
        }
    }
    if (units.size() < 2) {
        return false;
    }

    const QVector<QPointF> deltas = computeDeltas(bounds);
    if (deltas.size() != units.size()) {
        return false;
    }
    bool anyMove = false;
    for (const QPointF &delta : deltas) {
        if (!delta.isNull()) {
            anyMove = true;
            break;
        }
    }
    if (!anyMove) {
        return false;
    }

    state_->beginTransformCommand(buildTransformTargetIds(groupIds, looseLayers, looseGuides));
    for (int i = 0; i < units.size(); ++i) {
        const QPointF delta = deltas[i];
        if (delta.isNull()) {
            continue;
        }
        const QTransform worldMove = QTransform::fromTranslate(delta.x(), delta.y());
        const MoveUnit &unit = units[i];
        if (!unit.groupId.isEmpty()) {
            state_->transformGroupFrames({unit.groupId}, worldMove);
        } else if (unit.layer != nullptr) {
            const EntryStart start{unit.layer->x, unit.layer->y, unit.layer->scaleX, unit.layer->scaleY, unit.layer->rotation, unit.layer->skew};
            const ScaleDecomposition dec = decomposeScaleResult(localResultForWorldTransform(*unit.layer, start, worldMove), start.skew);
            if (dec.ok) {
                assignDecomposition(unit.layer, dec);
            }
        } else if (unit.guide != nullptr) {
            const EntryStart start{unit.guide->x, unit.guide->y, unit.guide->scaleX, unit.guide->scaleY, unit.guide->rotation, 0.0};
            const ScaleDecomposition dec = decomposeScaleResult(localResultForWorldTransform(*unit.guide, start, worldMove), start.skew);
            if (dec.ok) {
                assignDecomposition(unit.guide, dec);
            }
        }
    }
    state_->commitTransformCommand();
    state_->noteProjectGeometryChanged(false, state_->selectedTransformTargetIds());
    invalidateSceneCache();
    invalidateSelectionCache();
    update();
    return true;
}

bool ProjectCanvas::alignSelection(AlignEdge edge)
{
    return applyAlignDistribute([edge](const QVector<QRectF> &bounds) {
        // World +Y is up on the canvas, so a QRectF's numeric bottom() (max Y) is the
        // visual top edge and top() (min Y) is the visual bottom edge.
        QRectF whole = bounds.front();
        for (const QRectF &b : bounds) {
            whole = whole.united(b);
        }
        QVector<QPointF> deltas;
        deltas.reserve(bounds.size());
        for (const QRectF &b : bounds) {
            double dx = 0.0;
            double dy = 0.0;
            switch (edge) {
            case AlignEdge::Left:
                dx = whole.left() - b.left();
                break;
            case AlignEdge::Right:
                dx = whole.right() - b.right();
                break;
            case AlignEdge::HCenter:
                dx = whole.center().x() - b.center().x();
                break;
            case AlignEdge::Top:
                dy = whole.bottom() - b.bottom();
                break;
            case AlignEdge::Bottom:
                dy = whole.top() - b.top();
                break;
            case AlignEdge::VCenter:
            case AlignEdge::Center:
                // "Centre" aligns on the Y axis only (a vertical centre).
                dy = whole.center().y() - b.center().y();
                break;
            }
            deltas.push_back(QPointF(dx, dy));
        }
        return deltas;
    });
}

bool ProjectCanvas::distributeSelection(DistributeAxis axis)
{
    return applyAlignDistribute([axis](const QVector<QRectF> &bounds) {
        const int n = bounds.size();
        QVector<QPointF> deltas(n, QPointF(0.0, 0.0));
        // Distributing spacing needs at least three units (the two extremes stay put).
        if (n < 3) {
            return deltas;
        }
        const bool horizontal = axis == DistributeAxis::Horizontal;
        // Order units by centre along the axis; evenly interpolate the interior centres
        // between the two extreme units' centres.
        QVector<int> order(n);
        for (int i = 0; i < n; ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return horizontal ? bounds[a].center().x() < bounds[b].center().x()
                              : bounds[a].center().y() < bounds[b].center().y();
        });
        const double first = horizontal ? bounds[order.front()].center().x() : bounds[order.front()].center().y();
        const double last = horizontal ? bounds[order.back()].center().x() : bounds[order.back()].center().y();
        const double step = (last - first) / static_cast<double>(n - 1);
        for (int rank = 1; rank < n - 1; ++rank) {
            const int idx = order[rank];
            const double target = first + step * rank;
            const double current = horizontal ? bounds[idx].center().x() : bounds[idx].center().y();
            if (horizontal) {
                deltas[idx].setX(target - current);
            } else {
                deltas[idx].setY(target - current);
            }
        }
        return deltas;
    }, /*descendSoleGroup=*/true);
}

void ProjectCanvas::captureScaleReference()
{
    const QRectF &lr = dragStartBox_.localRect;
    captureScaleHintReference();

    // The grabbed handle (named side/corner) and the anchor (opposite side/corner) are resolved
    // once in the box's local frame, then mapped to world via the drag-start frame so they
    // survive any pan/zoom during the drag. In Absolute mode localToWorld is identity, so the
    // local coords equal world coords and this matches the legacy behaviour.
    QPointF handleLocal = lr.center();
    QPointF anchorLocal = lr.center();
    handleAnchorLocalPoints(activeHandle_, lr, &handleLocal, &anchorLocal);

    scaleHandleLocal_ = handleLocal;
    scaleAnchorLocal_ = anchorLocal;
    scaleCenterLocal_ = lr.center();
    const QTransform &M = dragStartBox_.localToWorld;
    scaleHandleStartWorld_ = M.map(handleLocal);
    scaleAnchorWorld_ = M.map(anchorLocal);
    scaleCenterWorld_ = M.map(lr.center());
}

void ProjectCanvas::applyScaleDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers)
{
    updateCursorForPoint(screenPoint);
    if (!dragStartBox_.valid || dragStartBox_.localRect.isEmpty()) {
        return;
    }

    const HandleAxes axes = handleAxes(activeHandle_);

    // Everything is computed in the drag-start box's local frame so scaling runs along the
    // box's own axes. In Absolute mode the frame is the identity, so local == world and this
    // reduces exactly to the legacy world-axis behaviour.
    const QTransform &M = dragStartBox_.localToWorld;
    bool invertible = false;
    const QTransform Minv = M.inverted(&invertible);
    if (!invertible) {
        return;
    }

    // Alt anchors the scale at the box centre so it grows/shrinks in place (centre fixed)
    // rather than pinning the opposite side/corner. Resolved per-move so Alt can be toggled
    // mid-drag.
    const QPointF anchorLocal = (modifiers & Qt::AltModifier) ? scaleCenterLocal_ : scaleAnchorLocal_;

    // Track the grabbed handle, not the raw press point: preserving the initial grab offset
    // (in local coords) keeps the handle exactly under the cursor with no error accumulating
    // over distance, and holds under pan/zoom mid-drag.
    const QPointF pressLocal = Minv.map(dragStartWorld_);
    const QPointF grabOffsetLocal = pressLocal - scaleHandleLocal_;
    const QPointF cursorLocal = Minv.map(screenToWorld(screenPoint));
    const QPointF currentLocal = cursorLocal - grabOffsetLocal;

    const auto axisScale = [](double current, double start, double anchor) {
        const double span = start - anchor;
        if (std::abs(span) < 1e-6) {
            return 1.0;
        }
        return (current - anchor) / span;
    };

    double sx = (axes.left || axes.right) ? axisScale(currentLocal.x(), scaleHandleLocal_.x(), anchorLocal.x()) : 1.0;
    double sy = (axes.top || axes.bottom) ? axisScale(currentLocal.y(), scaleHandleLocal_.y(), anchorLocal.y()) : 1.0;

    // Groups and single shapes scale the same way: edge handles drive one axis, corners track
    // the cursor on both axes freely, and Shift locks the aspect ratio. For a multi-selection
    // the non-uniform factors run in the group's oriented frame (worldScale below), so each
    // child keeps its relative position and its form round-trips through the per-shape skew
    // field - the same math the image_transformer app applies to a whole image.
    const bool uniform = modifiers & Qt::ShiftModifier;
    if (uniform) {
        // Project the cursor onto the anchor->handle diagonal for a single continuous
        // factor. Picking the dominant axis instead snaps when the axes cross (e.g. while
        // scaling down past 1x); projection is smooth and rotation-independent.
        const QPointF anchorToHandle = scaleHandleLocal_ - anchorLocal;
        const QPointF anchorToCurrent = currentLocal - anchorLocal;
        const double denom = anchorToHandle.x() * anchorToHandle.x() + anchorToHandle.y() * anchorToHandle.y();
        const double s = denom > 1e-9
            ? (anchorToCurrent.x() * anchorToHandle.x() + anchorToCurrent.y() * anchorToHandle.y()) / denom
            : 1.0;
        sx = s;
        sy = s;
    }

    // Build the scale about the anchor in the box's local frame.
    QTransform localScale;
    localScale.translate(anchorLocal.x(), anchorLocal.y());
    localScale.scale(sx, sy);
    localScale.translate(-anchorLocal.x(), -anchorLocal.y());

    // Relative single-item mode applies the scale on the shape's pre-image side
    // (result = localScale * start), which only changes scaleX/scaleY and the position while
    // preserving rotation/skew - the pure scale the Relative mode exists for. All other cases
    // (Absolute, or Relative multi) scale about the anchor in the box frame on the world side
    // (result = start * (Minv * localScale * M)); for Absolute that frame is the identity, so
    // this is byte-for-byte the legacy path.
    const bool relativeSingle = transformRelativeMode_
        && dragGroupStartFrames_.isEmpty()
        && (dragLayers_.size() + dragGuides_.size() == 1);
    if (relativeSingle) {
        applyDragTransform(localScale, /*preMultiply=*/true);
    } else {
        applyDragTransform(Minv * localScale * M, /*preMultiply=*/false);
    }
    const double hintScaleX = scaleHintStartScaleX_ * sx;
    const double hintScaleY = scaleHintStartScaleY_ * sy;
    if (uniform) {
        setCursorHint(screenPoint, {QStringLiteral("Scale X/Y: %1x").arg(formatHintNumber(hintScaleX, 3))});
    } else {
        setCursorHint(screenPoint, {QStringLiteral("Scale X: %1x").arg(formatHintNumber(hintScaleX, 3)),
                                    QStringLiteral("Scale Y: %1x").arg(formatHintNumber(hintScaleY, 3))});
    }
    requestLiveSceneUpdate();
}

void ProjectCanvas::applyDragTransform(const QTransform &transform, bool preMultiply)
{
    const auto apply = [&](auto *item, const EntryStart &start) {
        const QTransform result = preMultiply ? (transform * entryStartTransform(start))
                                              : localResultForWorldTransform(*item, start, transform);
        const ScaleDecomposition dec = decomposeScaleResult(result, start.skew);
        if (dec.ok) {
            assignDecomposition(item, dec);
        }
    };
    for (fh6::scene::Shape *layer : dragLayers_) {
        apply(layer, dragStarts_.value(layer->id));
    }
    for (fh6::scene::GuideLayer *guide : dragGuides_) {
        apply(guide, dragGuideStarts_.value(guide->id));
    }
    // The box gestures (scale/skew) pass a world-space transform with preMultiply=false;
    // accumulate the same transform into each dragged group's own frame. The relative
    // single-item path (preMultiply=true) never has group selections.
    if (!preMultiply && !dragGroupStartFrames_.isEmpty()) {
        state_->setGroupFramesFromStart(dragGroupStartFrames_, transform);
    }
}

void ProjectCanvas::applySkewDrag(const QPointF &screenPoint)
{
    updateCursorForPoint(screenPoint);

    // Multi-selection: shear the whole group as a unit in the drag-start box's local frame, so
    // every child keeps its relative position and its induced shear/rotation round-trips through
    // the per-shape fields - the same box-frame treatment the group scale gesture uses. A lone
    // shape keeps the direct width-normalised skew below.
    if (dragLayers_.size() + dragGuides_.size() > 1 || !dragGroupStartFrames_.isEmpty()) {
        if (!dragStartBox_.valid || dragStartBox_.localRect.isEmpty()) {
            return;
        }
        const QTransform &M = dragStartBox_.localToWorld;
        bool invertible = false;
        const QTransform Minv = M.inverted(&invertible);
        if (!invertible) {
            return;
        }
        const QRectF &lr = dragStartBox_.localRect;
        const QPointF centerLocal = lr.center();
        const double pressLocalX = Minv.map(dragStartWorld_).x();
        const double cursorLocalX = Minv.map(screenToWorld(screenPoint)).x();
        const double deltaLocalX = cursorLocalX - pressLocalX;
        // The skew handle sits at the top edge. Shearing about the box centre by
        // k = deltaLocalX / halfHeight makes the top edge track the cursor's horizontal motion
        // in the same rotational sense as a single shape's skew (Qt shear maps x' = x + k*(y-cy),
        // and the top is -halfHeight from the centre).
        const double halfHeight = lr.height() * 0.5;
        const double k = halfHeight > 1e-6 ? deltaLocalX / halfHeight : 0.0;
        QTransform localSkew;
        localSkew.translate(centerLocal.x(), centerLocal.y());
        localSkew.shear(k, 0.0);
        localSkew.translate(-centerLocal.x(), -centerLocal.y());
        applyDragTransform(Minv * localSkew * M, /*preMultiply=*/false);
        setCursorHint(screenPoint, {QStringLiteral("Skew: %1").arg(formatHintNumber(k, 3))});
        requestLiveSceneUpdate();
        return;
    }

    const QPointF current = screenToWorld(screenPoint);
    // dragStartWorld_ is the world point captured at press; using it (instead of
    // re-mapping the stale start screen point) keeps skew stable across pan/zoom.
    const double delta = current.x() - dragStartWorld_.x();
    for (fh6::scene::Shape *layer : dragLayers_) {
        const EntryStart startState = dragStarts_.value(layer->id);
        const QSizeF size = flatEntrySize(*layer, geometry_.shapeSize(layer->shapeId));
        layer->skew = startState.skew + delta / std::max(size.width(), 1.0);
    }
    setCursorHint(screenPoint, {QStringLiteral("Skew: %1").arg(formatHintNumber(delta, 2))});
    requestLiveSceneUpdate();
}

void ProjectCanvas::applyRotateDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers)
{
    updateCursorForPoint(screenPoint);
    const QPointF current = screenToWorld(screenPoint);
    const double angle = std::atan2(current.y() - rotateCenterWorld_.y(), current.x() - rotateCenterWorld_.x());
    const double deltaDegrees = snapRotation((angle - rotateStartAngle_) * 180.0 / kPi, modifiers);
    QTransform worldRotate;
    worldRotate.translate(rotateCenterWorld_.x(), rotateCenterWorld_.y());
    worldRotate.rotate(deltaDegrees);
    worldRotate.translate(-rotateCenterWorld_.x(), -rotateCenterWorld_.y());
    applyWorldTransformToDragItems(worldRotate);
    double displayedRotation = normalizeRotation(deltaDegrees);
    if (!dragLayers_.isEmpty()) {
        displayedRotation = dragLayers_.front()->rotation;
    } else if (!dragGuides_.isEmpty()) {
        displayedRotation = dragGuides_.front()->rotation;
    }
    setCursorHint(screenPoint, {QStringLiteral("Rotation: %1 deg").arg(formatHintNumber(displayedRotation, 1))});
    requestLiveSceneUpdate();
}

bool ProjectCanvas::isTransformDrag() const
{
    switch (dragMode_) {
    case DragMode::Move:
    case DragMode::TransformMove:
    case DragMode::Scale:
    case DragMode::Skew:
    case DragMode::Rotate:
        return true;
    case DragMode::None:
    case DragMode::Pan:
    case DragMode::Marquee:
        break;
    }
    return false;
}

void ProjectCanvas::requestLiveSceneUpdate()
{
    if (state_ != nullptr) {
        state_->noteTransformLiveChanged(dragTransformTargetIds());
    } else {
        invalidateSceneCache();
        update();
    }
}

void ProjectCanvas::resetDragState()
{
    dragDuplicated_ = false;
    dragUsesProjectEdit_ = false;
    dragMode_ = DragMode::None;
    marqueeRect_ = {};
    clearCursorHint();
    activeHandle_.clear();
    dragStarts_.clear();
    dragGuideStarts_.clear();
    dragLayers_.clear();
    dragGuides_.clear();
    dragGroupIds_.clear();
    dragGroupStartFrames_.clear();
    updateCursorForPoint(mapFromGlobal(QCursor::pos()));
    update();
}

void ProjectCanvas::finishDrag()
{
    if (isTransformDrag() && state_ != nullptr) {
        if (dragUsesProjectEdit_) {
            state_->commitProjectEdit();
        } else {
            state_->commitTransformCommand();
        }
        // An Alt-duplicate added layers/groups, so rebuild the tree to show the clones;
        // a plain transform only needs a geometry refresh.
        if (dragDuplicated_) {
            state_->noteProjectStructureChanged();
        } else {
            state_->noteProjectGeometryChanged(false, dragTransformTargetIds());
        }
    }
    resetDragState();
}

void ProjectCanvas::cancelDrag()
{
    if (isTransformDrag() && state_ != nullptr) {
        if (dragUsesProjectEdit_) {
            state_->cancelProjectEdit();
        } else {
            state_->cancelTransformCommand();
        }
    }
    resetDragState();
}

void ProjectCanvas::beginToolDrag(const QPointF &screenPos, const QPointF &boxCenterWorld)
{
    if (activeTool_ != nullptr) {
        activeTool_->beginDrag(screenPos, boxCenterWorld);
    }
}

void ProjectCanvas::beginRotateDrag(const QPointF &boxCenterWorld)
{
    dragMode_ = DragMode::Rotate;
    rotateCenterWorld_ = boxCenterWorld;
    rotateStartAngle_ = std::atan2(dragStartWorld_.y() - rotateCenterWorld_.y(),
                                   dragStartWorld_.x() - rotateCenterWorld_.x());
}

} // namespace gui
