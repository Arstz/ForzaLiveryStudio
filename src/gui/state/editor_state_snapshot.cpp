// Undo/redo machinery split out of editor_state.cpp: edit/transform command
// lifecycle, the undo/redo stacks, snapshot capture/apply, snapshot equality,
// and refresh classification for history commands.

#include "editor_state.h"

#include "editor_state_internal.h"
#include "perf_utils.h"

#include <array>

namespace gui {

using namespace detail;

namespace {

bool colorsEqual(const std::array<quint8, 4> &a, const std::array<quint8, 4> &b)
{
    return a == b;
}

bool layersEqual(const fh6::ShapeLayer &a, const fh6::ShapeLayer &b)
{
    return commonEntryFieldsEqual(a, b)
        && a.shapeId == b.shapeId
        && a.skew == b.skew
        && colorsEqual(a.color, b.color)
        && a.mask == b.mask
        && a.sourceShape == b.sourceShape
        && a.absOffset == b.absOffset
        && a.marker == b.marker
        && a.flags == b.flags;
}

bool groupsEqual(const fh6::LayerGroup &a, const fh6::LayerGroup &b)
{
    return a.id == b.id
        && a.name == b.name
        && a.childIds == b.childIds
        && a.locked == b.locked
        && a.sourceAbsPos == b.sourceAbsPos
        && a.pendingTransformMarker == b.pendingTransformMarker
        && a.inlineTransformMarker == b.inlineTransformMarker
        && a.effectiveTransformMarker == b.effectiveTransformMarker
        && a.headerControlBytes == b.headerControlBytes
        && a.flags == b.flags
        && a.sourceParentId == b.sourceParentId
        && a.sourcePreviousSiblingId == b.sourcePreviousSiblingId
        && a.sourcePreviousGroupDepth == b.sourcePreviousGroupDepth
        && a.sourceChildIds == b.sourceChildIds
        && a.isLiverySection == b.isLiverySection
        && a.liverySectionSlot == b.liverySectionSlot;
}

} // namespace

void EditorState::beginProjectEdit()
{
    if (!hasProject_) {
        pendingEdit_.reset();
        return;
    }
    ProjectEditCommand command;
    command.before = captureSnapshot();
    command.undoSelection = {selectedLayerIds_, selectedGuideLayerIds_};
    command.redoSelection = {selectedLayerIds_, selectedGuideLayerIds_};
    pendingEdit_ = command;
}

void EditorState::commitProjectEdit()
{
    ScopedPerf perf("EditorState::commitProjectEdit");
    if (!pendingEdit_.has_value()) {
        return;
    }
    pendingEdit_->after = captureSnapshot();
    pendingEdit_->redoSelection = {selectedLayerIds_, selectedGuideLayerIds_};
    if (!snapshotsEqual(pendingEdit_->before, pendingEdit_->after)) {
        pendingEdit_->refresh = classifySnapshotRefresh(pendingEdit_->before, pendingEdit_->after);
        undoStack_.push_back(*pendingEdit_);
        redoStack_.clear();
        setModified(true);
        Q_EMIT historyChanged();
    }
    pendingEdit_.reset();
}

void EditorState::cancelProjectEdit()
{
    if (!pendingEdit_.has_value()) {
        return;
    }
    applySnapshot(pendingEdit_->before);
    const ProjectSelectionSnapshot undoSelection = pendingEdit_->undoSelection;
    pendingEdit_.reset();
    setSelectionIds(undoSelection.layerIds, undoSelection.guideLayerIds);
    noteProjectStructureChanged();
}

void EditorState::beginTransformCommand()
{
    beginProjectEdit();
}

void EditorState::commitTransformCommand()
{
    commitProjectEdit();
}

void EditorState::cancelTransformCommand()
{
    cancelProjectEdit();
}

void EditorState::undo()
{
    if (undoStack_.isEmpty()) {
        return;
    }
    const ProjectEditCommand command = undoStack_.takeLast();
    applySnapshot(command.before);
    redoStack_.push_back(command);
    setModified(true);
    if (command.refresh == ProjectEditRefresh::Structure) {
        selectedLayerIds_ = existingLayerIds(command.undoSelection.layerIds);
        selectedGuideLayerIds_ = existingGuideLayerIds(command.undoSelection.guideLayerIds);
    } else {
        setSelectionIds(command.undoSelection.layerIds, command.undoSelection.guideLayerIds);
    }
    refreshAfterHistoryCommand(command.refresh);
    Q_EMIT historyChanged();
}

void EditorState::redo()
{
    if (redoStack_.isEmpty()) {
        return;
    }
    const ProjectEditCommand command = redoStack_.takeLast();
    applySnapshot(command.after);
    undoStack_.push_back(command);
    setModified(true);
    if (command.refresh == ProjectEditRefresh::Structure) {
        selectedLayerIds_ = existingLayerIds(command.redoSelection.layerIds);
        selectedGuideLayerIds_ = existingGuideLayerIds(command.redoSelection.guideLayerIds);
    } else {
        setSelectionIds(command.redoSelection.layerIds, command.redoSelection.guideLayerIds);
    }
    refreshAfterHistoryCommand(command.refresh);
    Q_EMIT historyChanged();
}

void EditorState::applySnapshot(const ProjectEditSnapshot &snapshot)
{
    project_.layers = snapshot.layers;
    project_.guideLayers = snapshot.guideLayers;
    project_.groups = snapshot.groups;
    project_.rootChildIds = snapshot.rootChildIds;
    project_.colorSwatches = snapshot.colorSwatches;
    invalidateProjectIndexCache();
}

ProjectEditSnapshot EditorState::captureSnapshot() const
{
    ScopedPerf perf("EditorState::captureSnapshot");
    // Cheap copy-on-write copy: snapshots stay shared with project_ until the
    // next edit mutates a container. Mutations made through QVector iteration
    // detach automatically; callers that edit through cached raw pointers (the
    // property panel) must detach the project first so they cannot rewrite this
    // shared "before" snapshot. See PropertyPanel::detachSelectionForEdit().
    return {project_.layers, project_.guideLayers, project_.groups, project_.rootChildIds, project_.colorSwatches};
}

bool EditorState::snapshotsEqual(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const
{
    ScopedPerf perf("EditorState::snapshotsEqual");
    if (a.rootChildIds != b.rootChildIds
        || a.colorSwatches != b.colorSwatches
        || a.layers.size() != b.layers.size()
        || a.guideLayers.size() != b.guideLayers.size()
        || a.groups.size() != b.groups.size()) {
        return false;
    }
    for (int i = 0; i < a.layers.size(); ++i) {
        if (!layersEqual(a.layers[i], b.layers[i])) {
            return false;
        }
    }
    for (int i = 0; i < a.guideLayers.size(); ++i) {
        if (!guideLayersEqual(a.guideLayers[i], b.guideLayers[i])) {
            return false;
        }
    }
    for (int i = 0; i < a.groups.size(); ++i) {
        if (!groupsEqual(a.groups[i], b.groups[i])) {
            return false;
        }
    }
    return true;
}

ProjectEditRefresh EditorState::classifySnapshotRefresh(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const
{
    ScopedPerf perf("EditorState::classifySnapshotRefresh");
    if (a.rootChildIds != b.rootChildIds
        || a.groups.size() != b.groups.size()
        || a.layers.size() != b.layers.size()
        || a.guideLayers.size() != b.guideLayers.size()) {
        return ProjectEditRefresh::Structure;
    }
    for (int i = 0; i < a.guideLayers.size(); ++i) {
        const fh6::GuideLayer &before = a.guideLayers[i];
        const fh6::GuideLayer &after = b.guideLayers[i];
        if (before.id != after.id
            || before.name != after.name
            || before.sourcePath != after.sourcePath
            || before.imageBytes != after.imageBytes
            || before.pixelBytes != after.pixelBytes
            || before.imageFormat != after.imageFormat
            || before.width != after.width
            || before.height != after.height
            || before.visible != after.visible
            || before.locked != after.locked) {
            return ProjectEditRefresh::Structure;
        }
    }
    for (int i = 0; i < a.groups.size(); ++i) {
        if (!groupsEqual(a.groups[i], b.groups[i])) {
            return ProjectEditRefresh::Structure;
        }
    }
    for (int i = 0; i < a.layers.size(); ++i) {
        const fh6::ShapeLayer &before = a.layers[i];
        const fh6::ShapeLayer &after = b.layers[i];
        if (before.id != after.id
            || before.name != after.name
            || before.visible != after.visible
            || before.locked != after.locked
            || before.mask != after.mask
            || before.sourceShape != after.sourceShape
            || before.absOffset != after.absOffset
            || before.marker != after.marker
            || before.flags != after.flags) {
            return ProjectEditRefresh::Structure;
        }
    }
    for (int i = 0; i < a.layers.size(); ++i) {
        const fh6::ShapeLayer &before = a.layers[i];
        const fh6::ShapeLayer &after = b.layers[i];
        if (before.shapeId != after.shapeId || !colorsEqual(before.color, after.color)) {
            return ProjectEditRefresh::Previews;
        }
    }
    for (int i = 0; i < a.guideLayers.size(); ++i) {
        const fh6::GuideLayer &before = a.guideLayers[i];
        const fh6::GuideLayer &after = b.guideLayers[i];
        if (before.x != after.x
            || before.y != after.y
            || before.scaleX != after.scaleX
            || before.scaleY != after.scaleY
            || before.rotation != after.rotation
            || before.opacity != after.opacity) {
            return ProjectEditRefresh::GeometryOnly;
        }
    }
    return ProjectEditRefresh::GeometryOnly;
}

void EditorState::refreshAfterHistoryCommand(ProjectEditRefresh refresh)
{
    switch (refresh) {
    case ProjectEditRefresh::GeometryOnly:
        noteProjectGeometryChanged(false);
        return;
    case ProjectEditRefresh::Previews:
        noteProjectGeometryChanged(true);
        return;
    case ProjectEditRefresh::Structure:
        noteProjectStructureChanged();
        return;
    }
}

} // namespace gui
