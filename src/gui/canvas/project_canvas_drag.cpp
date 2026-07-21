#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

using namespace pc_detail;

namespace {

template <typename Start>
QTransform entryStartTransform(const Start &start) {
    QTransform transform;
    transform.translate(start.x, start.y);
    transform.rotate(start.rotation);
    transform.shear(start.skew, 0.0);
    transform.scale(start.scaleX, start.scaleY);
    return transform;
}

QTransform parentWorldTransform(const fh6::scene::Layer &node) {
    const fh6::scene::Layer *parent = node.parent();
    return parent != nullptr ? sceneWorldTransform(*parent) : QTransform();
}

template <typename Start>
QTransform localResultForWorldTransform(const fh6::scene::Layer &node,
                                        const Start &start,
                                        const QTransform &worldTransform) {
    const QTransform startLocal = entryStartTransform(start);
    const QTransform parentWorld = parentWorldTransform(node);
    bool invertible = false;
    const QTransform parentWorldInverse = parentWorld.inverted(&invertible);
    if (!invertible) {
        return startLocal;
    }
    return startLocal * parentWorld * worldTransform * parentWorldInverse;
}

double snapRotation(double degrees, Qt::KeyboardModifiers modifiers) {
    if (modifiers & Qt::ShiftModifier) {
        return std::round(degrees / 15.0) * 15.0;
    }
    return degrees;
}

QPointF constrainDelta(QPointF delta, Qt::KeyboardModifiers modifiers) {
    if (!(modifiers & Qt::ShiftModifier)) {
        return delta;
    }
    if (std::abs(delta.x()) >= std::abs(delta.y())) {
        return {delta.x(), 0.0};
    }
    return {0.0, delta.y()};
}

QString formatHintNumber(double value, int decimals = 2) {
    if (std::abs(value) < 0.005) {
        value = 0.0;
    }
    return QString::number(value, 'f', decimals);
}

struct ScaleDecomposition {
    bool ok = false;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    double skew = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};

ScaleDecomposition decomposeScaleResult(const QTransform &result, double fallbackSkew) {
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

template <typename Item>
void assignDecomposition(Item *item, const ScaleDecomposition &dec) {
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


void ProjectCanvas::cycleFlipSelection() {
    if (state_ == nullptr || project_ == nullptr) {
        return;
    }
    const QVector<fh6::scene::Shape *> selectedShapeLeaves = selectedLayers();
    const QVector<fh6::scene::GuideLayer *> selectedGuideLeaves = selectedGuideLayers();
    if (selectedShapeLeaves.isEmpty() && selectedGuideLeaves.isEmpty()) {
        return;
    }

    QVector<QString> groupIds;
    QHash<QString, QTransform> groupStartFrames;
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    collectDragGroups(groupIds, groupStartFrames, groupedLayerIds, groupedGuideIds);

    QVector<fh6::scene::Shape *> layers;
    QVector<fh6::scene::GuideLayer *> guides;
    for (fh6::scene::Shape *layer : selectedShapeLeaves) {
        if (!groupedLayerIds.contains(layer->id)) {
            layers.push_back(layer);
        }
    }
    for (fh6::scene::GuideLayer *guide : selectedGuideLeaves) {
        if (!groupedGuideIds.contains(guide->id)) {
            guides.push_back(guide);
        }
    }
    if (!groupIds.isEmpty() && layers.isEmpty() && guides.isEmpty()) {
        groupIds.clear();
        layers = selectedShapeLeaves;
        guides = selectedGuideLeaves;
    }

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
    state_->beginTransformCommand(buildTransformTargetIds(groupIds, layers, guides));

    QTransform worldFlip;
    worldFlip.translate(center.x(), center.y());
    worldFlip.scale(toggleVertical ? -1.0 : 1.0, toggleHorizontal ? -1.0 : 1.0);
    worldFlip.translate(-center.x(), -center.y());
    state_->setGroupFramesFromStart(groupStartFrames, worldFlip);

    const auto applyFlip = [&worldFlip, toggleHorizontal](auto *item) {
        using Item = std::remove_pointer_t<decltype(item)>;
        const double skew = [] (const Item *entry) {
            if constexpr (std::is_same_v<Item, fh6::scene::Shape>) {
                return entry->skew;
            }
            return 0.0;
        }(item);
        const EntryStart start = {item->x, item->y, item->scaleX, item->scaleY,
                                  item->rotation, skew};
        ScaleDecomposition decomposition = decomposeScaleResult(
            localResultForWorldTransform(*item, start, worldFlip), start.skew);
        if (decomposition.ok) {
            const bool scaleXNegative = (start.scaleX < 0.0) != toggleHorizontal;
            if ((decomposition.scaleX < 0.0) != scaleXNegative) {
                decomposition.scaleX = -decomposition.scaleX;
                decomposition.scaleY = -decomposition.scaleY;
                decomposition.rotation = normalizeRotation(decomposition.rotation + 180.0);
            }
            assignDecomposition(item, decomposition);
        }
    };
    for (fh6::scene::Shape *layer : layers) {
        applyFlip(layer);
    }
    for (fh6::scene::GuideLayer *guide : guides) {
        applyFlip(guide);
    }

    state_->commitTransformCommand();
    state_->noteProjectGeometryChanged(false, state_->selectedTransformTargetIds());
    invalidateSceneCache();
    update();
}

void ProjectCanvas::collectDragGroups(QVector<QString> &groupIds,
                                      QHash<QString, QTransform> &startFrames,
                                      QSet<QString> &groupedLayerIds,
                                      QSet<QString> &groupedGuideIds) const {
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

void ProjectCanvas::captureDragStarts() {
    drag_.starts.clear();
    drag_.guideStarts.clear();
    drag_.groupIds.clear();
    drag_.groupStartFrames.clear();
    QSet<QString> groupedLayerIds;
    QSet<QString> groupedGuideIds;
    collectDragGroups(drag_.groupIds, drag_.groupStartFrames, groupedLayerIds, groupedGuideIds);
    drag_.layers.clear();
    drag_.guides.clear();
    for (fh6::scene::Shape *layer : selectedLayers()) {
        if (!groupedLayerIds.contains(layer->id)) {
            drag_.layers.push_back(layer);
            drag_.starts.insert(layer->id, {layer->x, layer->y, layer->scaleX, layer->scaleY, layer->rotation, layer->skew});
        }
    }
    for (fh6::scene::GuideLayer *guide : selectedGuideLayers()) {
        if (!groupedGuideIds.contains(guide->id)) {
            drag_.guides.push_back(guide);
            drag_.guideStarts.insert(guide->id, {guide->x, guide->y, guide->scaleX, guide->scaleY, guide->rotation});
        }
    }
}

QVector<QString> ProjectCanvas::dragTransformTargetIds() const {
    return buildTransformTargetIds(drag_.groupIds, drag_.layers, drag_.guides);
}

QString ProjectCanvas::transformSelectionSignature() const {
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

QSizeF ProjectCanvas::transformBoxVisualExtents(const SelectionBox &box) const {
    if (!box.valid || box.localRect.isEmpty()) {
        return {};
    }
    const QPointF origin = box.localToWorld.map(QPointF(0.0, 0.0));
    const double axisX = QLineF(origin, box.localToWorld.map(QPointF(1.0, 0.0))).length();
    const double axisY = QLineF(origin, box.localToWorld.map(QPointF(0.0, 1.0))).length();
    return QSizeF(std::abs(box.localRect.width()) * std::max(axisX, 1e-9),
                  std::abs(box.localRect.height()) * std::max(axisY, 1e-9));
}

void ProjectCanvas::captureScaleHintReference() {
    const QString signature = transformSelectionSignature();
    const QSizeF startExtents = transformBoxVisualExtents(drag_.startBox);
    const bool reset = !drag_.scaleHintBaseValid
        || drag_.scaleHintSelectionSignature != signature
        || drag_.scaleHintBaseExtents.width() <= 1e-9
        || drag_.scaleHintBaseExtents.height() <= 1e-9;
    if (reset) {
        drag_.scaleHintSelectionSignature = signature;
        drag_.scaleHintBaseExtents = startExtents;
        drag_.scaleHintBaseValid = startExtents.width() > 1e-9 && startExtents.height() > 1e-9;
    }
    drag_.scaleHintStartScaleX = drag_.scaleHintBaseValid ? startExtents.width() / drag_.scaleHintBaseExtents.width() : 1.0;
    drag_.scaleHintStartScaleY = drag_.scaleHintBaseValid ? startExtents.height() / drag_.scaleHintBaseExtents.height() : 1.0;
}

void ProjectCanvas::applyWorldTransformToDragItems(const QTransform &worldTransform) {
    const auto applyItem = [&](auto *item, const EntryStart &start) {
        const ScaleDecomposition dec = decomposeScaleResult(localResultForWorldTransform(*item, start, worldTransform), start.skew);
        if (dec.ok) {
            assignDecomposition(item, dec);
        }
    };
    for (fh6::scene::Shape *layer : drag_.layers) {
        applyItem(layer, drag_.starts.value(layer->id));
    }
    for (fh6::scene::GuideLayer *guide : drag_.guides) {
        applyItem(guide, drag_.guideStarts.value(guide->id));
    }
    if (!drag_.groupStartFrames.isEmpty()) {
        state_->setGroupFramesFromStart(drag_.groupStartFrames, worldTransform);
    }
}

void ProjectCanvas::applyMoveDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers) {
    updateCursorForPoint(screenPoint);
    const QPointF delta = constrainDelta(screenToWorld(screenPoint) - drag_.startWorld, modifiers);
    applyWorldTransformToDragItems(QTransform::fromTranslate(delta.x(), delta.y()));
    setCursorHint(screenPoint, {QStringLiteral("X: %1").arg(formatHintNumber(delta.x())),
                                QStringLiteral("Y: %1").arg(formatHintNumber(delta.y()))});
    requestLiveSceneUpdate();
}

bool ProjectCanvas::nudgeSelection(const QPointF &delta) {
    if (state_ == nullptr || project_ == nullptr || delta.isNull()) {
        return false;
    }
    if (selectedLayers().isEmpty() && selectedGuideLayers().isEmpty()) {
        return false;
    }

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

struct MoveUnit {
    QString groupId;
    fh6::scene::Shape *layer = nullptr;
    fh6::scene::GuideLayer *guide = nullptr;
};

} // namespace

bool ProjectCanvas::applyAlignDistribute(const std::function<QVector<QPointF>(const QVector<QRectF> &)> &computeDeltas,
                                         bool descendSoleGroup) {
    if (state_ == nullptr || project_ == nullptr) {
        return false;
    }
    if (selectedLayers().isEmpty() && selectedGuideLayers().isEmpty()) {
        return false;
    }

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

bool ProjectCanvas::alignSelection(AlignEdge edge) {
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
                dy = whole.center().y() - b.center().y();
                break;
            }
            deltas.push_back(QPointF(dx, dy));
        }
        return deltas;
    });
}

bool ProjectCanvas::distributeSelection(DistributeAxis axis) {
    return applyAlignDistribute([axis](const QVector<QRectF> &bounds) {
        const int n = bounds.size();
        QVector<QPointF> deltas(n, QPointF(0.0, 0.0));
        if (n < 3) {
            return deltas;
        }
        const bool horizontal = axis == DistributeAxis::Horizontal;
        QVector<int> order(n);
        for (int i = 0; i < n; ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return horizontal ? bounds[a].center().x() < bounds[b].center().x()
                              : bounds[a].center().y() < bounds[b].center().y();
        });
        const auto start = [&](int index) { return horizontal ? bounds[index].left() : bounds[index].top(); };
        const auto end = [&](int index) { return horizontal ? bounds[index].right() : bounds[index].bottom(); };
        const auto extent = [&](int index) { return horizontal ? bounds[index].width() : bounds[index].height(); };
        double totalExtent = 0.0;
        for (int index : order) {
            totalExtent += extent(index);
        }
        const double gap = (end(order.back()) - start(order.front()) - totalExtent) / static_cast<double>(n - 1);
        double nextStart = end(order.front()) + gap;
        for (int rank = 1; rank < n - 1; ++rank) {
            const int idx = order[rank];
            const double delta = nextStart - start(idx);
            if (horizontal) {
                deltas[idx].setX(delta);
            } else {
                deltas[idx].setY(delta);
            }
            nextStart += extent(idx) + gap;
        }
        return deltas;
    }, /*descendSoleGroup=*/true);
}

void ProjectCanvas::captureScaleReference() {
    const QRectF &lr = drag_.startBox.localRect;
    captureScaleHintReference();

    QPointF handleLocal = lr.center();
    QPointF anchorLocal = lr.center();
    handleAnchorLocalPoints(drag_.activeHandle, lr, &handleLocal, &anchorLocal);

    drag_.scaleHandleLocal = handleLocal;
    drag_.scaleAnchorLocal = anchorLocal;
    drag_.scaleCenterLocal = lr.center();
    const QTransform &M = drag_.startBox.localToWorld;
    drag_.scaleHandleStartWorld = M.map(handleLocal);
    drag_.scaleAnchorWorld = M.map(anchorLocal);
    drag_.scaleCenterWorld = M.map(lr.center());
}

void ProjectCanvas::applyScaleDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers) {
    updateCursorForPoint(screenPoint);
    if (!drag_.startBox.valid || drag_.startBox.localRect.isEmpty()) {
        return;
    }

    const HandleAxes axes = handleAxes(drag_.activeHandle);

    const QTransform &M = drag_.startBox.localToWorld;
    bool invertible = false;
    const QTransform Minv = M.inverted(&invertible);
    if (!invertible) {
        return;
    }

    const QPointF anchorLocal = (modifiers & Qt::AltModifier) ? drag_.scaleCenterLocal : drag_.scaleAnchorLocal;

    const QPointF pressLocal = Minv.map(drag_.startWorld);
    const QPointF grabOffsetLocal = pressLocal - drag_.scaleHandleLocal;
    const QPointF cursorLocal = Minv.map(screenToWorld(screenPoint));
    const QPointF currentLocal = cursorLocal - grabOffsetLocal;

    const auto axisScale = [](double current, double start, double anchor) {
        const double span = start - anchor;
        if (std::abs(span) < 1e-6) {
            return 1.0;
        }
        return (current - anchor) / span;
    };

    double sx = (axes.left || axes.right) ? axisScale(currentLocal.x(), drag_.scaleHandleLocal.x(), anchorLocal.x()) : 1.0;
    double sy = (axes.top || axes.bottom) ? axisScale(currentLocal.y(), drag_.scaleHandleLocal.y(), anchorLocal.y()) : 1.0;

    const bool uniform = modifiers & Qt::ShiftModifier;
    if (uniform) {
        const QPointF anchorToHandle = drag_.scaleHandleLocal - anchorLocal;
        const QPointF anchorToCurrent = currentLocal - anchorLocal;
        const double denom = anchorToHandle.x() * anchorToHandle.x() + anchorToHandle.y() * anchorToHandle.y();
        const double s = denom > 1e-9
            ? (anchorToCurrent.x() * anchorToHandle.x() + anchorToCurrent.y() * anchorToHandle.y()) / denom
            : 1.0;
        sx = s;
        sy = s;
    }

    QTransform localScale;
    localScale.translate(anchorLocal.x(), anchorLocal.y());
    localScale.scale(sx, sy);
    localScale.translate(-anchorLocal.x(), -anchorLocal.y());

    const bool relativeSingle = options_.transformRelativeMode
        && drag_.groupStartFrames.isEmpty()
        && (drag_.layers.size() + drag_.guides.size() == 1);
    if (relativeSingle) {
        applyDragTransform(localScale, /*preMultiply=*/true);
    } else {
        applyDragTransform(Minv * localScale * M, /*preMultiply=*/false);
    }
    const double hintScaleX = drag_.scaleHintStartScaleX * sx;
    const double hintScaleY = drag_.scaleHintStartScaleY * sy;
    if (uniform) {
        setCursorHint(screenPoint, {QStringLiteral("Scale X/Y: %1x").arg(formatHintNumber(hintScaleX, 3))});
    } else {
        setCursorHint(screenPoint, {QStringLiteral("Scale X: %1x").arg(formatHintNumber(hintScaleX, 3)),
                                    QStringLiteral("Scale Y: %1x").arg(formatHintNumber(hintScaleY, 3))});
    }
    requestLiveSceneUpdate();
}

void ProjectCanvas::applyDragTransform(const QTransform &transform, bool preMultiply) {
    const auto apply = [&](auto *item, const EntryStart &start) {
        const QTransform result = preMultiply ? (transform * entryStartTransform(start))
                                              : localResultForWorldTransform(*item, start, transform);
        const ScaleDecomposition dec = decomposeScaleResult(result, start.skew);
        if (dec.ok) {
            assignDecomposition(item, dec);
        }
    };
    for (fh6::scene::Shape *layer : drag_.layers) {
        apply(layer, drag_.starts.value(layer->id));
    }
    for (fh6::scene::GuideLayer *guide : drag_.guides) {
        apply(guide, drag_.guideStarts.value(guide->id));
    }
    if (!preMultiply && !drag_.groupStartFrames.isEmpty()) {
        state_->setGroupFramesFromStart(drag_.groupStartFrames, transform);
    }
}

void ProjectCanvas::applySkewDrag(const QPointF &screenPoint) {
    updateCursorForPoint(screenPoint);

    if (drag_.layers.size() + drag_.guides.size() > 1 || !drag_.groupStartFrames.isEmpty()) {
        if (!drag_.startBox.valid || drag_.startBox.localRect.isEmpty()) {
            return;
        }
        const QTransform &M = drag_.startBox.localToWorld;
        bool invertible = false;
        const QTransform Minv = M.inverted(&invertible);
        if (!invertible) {
            return;
        }
        const QRectF &lr = drag_.startBox.localRect;
        const QPointF centerLocal = lr.center();
        const double pressLocalX = Minv.map(drag_.startWorld).x();
        const double cursorLocalX = Minv.map(screenToWorld(screenPoint)).x();
        const double deltaLocalX = cursorLocalX - pressLocalX;
        // Qt shear uses x' = x + k(y - cy).
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
    const double delta = current.x() - drag_.startWorld.x();
    for (fh6::scene::Shape *layer : drag_.layers) {
        const EntryStart startState = drag_.starts.value(layer->id);
        const QSizeF size = flatEntrySize(*layer, geometry_.shapeSize(layer->shapeId));
        layer->skew = startState.skew + delta / std::max(size.width(), 1.0);
    }
    setCursorHint(screenPoint, {QStringLiteral("Skew: %1").arg(formatHintNumber(delta, 2))});
    requestLiveSceneUpdate();
}

void ProjectCanvas::applyRotateDrag(const QPointF &screenPoint, Qt::KeyboardModifiers modifiers) {
    updateCursorForPoint(screenPoint);
    const QPointF current = screenToWorld(screenPoint);
    const double angle = std::atan2(current.y() - drag_.rotateCenterWorld.y(), current.x() - drag_.rotateCenterWorld.x());
    const double deltaDegrees = snapRotation((angle - drag_.rotateStartAngle) * 180.0 / kPi, modifiers);
    QTransform worldRotate;
    worldRotate.translate(drag_.rotateCenterWorld.x(), drag_.rotateCenterWorld.y());
    worldRotate.rotate(deltaDegrees);
    worldRotate.translate(-drag_.rotateCenterWorld.x(), -drag_.rotateCenterWorld.y());
    applyWorldTransformToDragItems(worldRotate);
    double displayedRotation = normalizeRotation(deltaDegrees);
    if (!drag_.layers.isEmpty()) {
        displayedRotation = drag_.layers.front()->rotation;
    } else if (!drag_.guides.isEmpty()) {
        displayedRotation = drag_.guides.front()->rotation;
    }
    setCursorHint(screenPoint, {QStringLiteral("Rotation: %1 deg").arg(formatHintNumber(displayedRotation, 1))});
    requestLiveSceneUpdate();
}

bool ProjectCanvas::isTransformDrag() const {
    switch (drag_.mode) {
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

void ProjectCanvas::requestLiveSceneUpdate() {
    if (state_ != nullptr) {
        state_->noteTransformLiveChanged(dragTransformTargetIds());
    } else {
        invalidateSceneCache();
        update();
    }
}

void ProjectCanvas::resetDragState() {
    drag_.duplicated = false;
    drag_.usesProjectEdit = false;
    drag_.mode = DragMode::None;
    drag_.marqueeRect = {};
    clearCursorHint();
    drag_.activeHandle.clear();
    drag_.starts.clear();
    drag_.guideStarts.clear();
    drag_.layers.clear();
    drag_.guides.clear();
    drag_.groupIds.clear();
    drag_.groupStartFrames.clear();
    updateCursorForPoint(mapFromGlobal(QCursor::pos()));
    update();
}

void ProjectCanvas::finishDrag() {
    if (isTransformDrag() && state_ != nullptr) {
        if (drag_.usesProjectEdit) {
            state_->commitProjectEdit();
        } else {
            state_->commitTransformCommand();
        }
        if (drag_.duplicated) {
            state_->noteProjectStructureChanged();
        } else {
            state_->noteProjectGeometryChanged(false, dragTransformTargetIds());
        }
    }
    resetDragState();
}

void ProjectCanvas::cancelDrag() {
    if (isTransformDrag() && state_ != nullptr) {
        if (drag_.usesProjectEdit) {
            state_->cancelProjectEdit();
        } else {
            state_->cancelTransformCommand();
        }
    }
    resetDragState();
}

void ProjectCanvas::beginToolDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) {
    if (activeTool_ != nullptr) {
        activeTool_->beginDrag(screenPos, boxCenterWorld);
    }
}

void ProjectCanvas::beginRotateDrag(const QPointF &boxCenterWorld) {
    drag_.mode = DragMode::Rotate;
    drag_.rotateCenterWorld = boxCenterWorld;
    drag_.rotateStartAngle = std::atan2(drag_.startWorld.y() - drag_.rotateCenterWorld.y(),
                                   drag_.startWorld.x() - drag_.rotateCenterWorld.x());
}

} // namespace gui
