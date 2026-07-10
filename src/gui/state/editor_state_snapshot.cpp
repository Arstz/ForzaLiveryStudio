#include "editor_state.h"

#include "perf_utils.h"

#include <utility>

namespace gui {
namespace {

bool visualEqual(const fh6::scene::Shape &a, const fh6::scene::Shape &b)
{
    if (a.isRaster() != b.isRaster()) {
        return false;
    }
    if (a.isRaster()) {
        return a.rasterId == b.rasterId
            && a.rasterWidth == b.rasterWidth
            && a.rasterHeight == b.rasterHeight;
    }
    return a.shapeId == b.shapeId;
}

bool transformEqual(const fh6::scene::Transform2D &a, const fh6::scene::Transform2D &b)
{
    return a.x == b.x
        && a.y == b.y
        && a.scaleX == b.scaleX
        && a.scaleY == b.scaleY
        && a.rotation == b.rotation
        && a.skew == b.skew;
}

bool nodeEqual(const fh6::scene::Layer &a, const fh6::scene::Layer &b)
{
    if (a.kind() != b.kind()
        || a.id != b.id
        || a.name != b.name
        || !transformEqual(a.transform, b.transform)
        || a.opacity != b.opacity
        || a.visible != b.visible
        || a.locked != b.locked) {
        return false;
    }
    switch (a.kind()) {
    case fh6::scene::LayerKind::Shape: {
        const auto &sa = static_cast<const fh6::scene::Shape &>(a);
        const auto &sb = static_cast<const fh6::scene::Shape &>(b);
        return visualEqual(sa, sb)
            && sa.color == sb.color
            && sa.mask == sb.mask
            && sa.sourceShape == sb.sourceShape
            && sa.absOffset == sb.absOffset
            && sa.marker == sb.marker
            && sa.flags == sb.flags
            && sa.sourceLogoId == sb.sourceLogoId;
    }
    case fh6::scene::LayerKind::Guide: {
        const auto &ga = static_cast<const fh6::scene::GuideLayer &>(a);
        const auto &gb = static_cast<const fh6::scene::GuideLayer &>(b);
        if (ga.sourcePath != gb.sourcePath || static_cast<bool>(ga.image) != static_cast<bool>(gb.image)) {
            return false;
        }
        if (!ga.image) {
            return true;
        }
        return ga.image->width == gb.image->width
            && ga.image->height == gb.image->height
            && ga.image->pixels == gb.image->pixels
            && ga.image->encoded == gb.image->encoded
            && ga.image->format == gb.image->format;
    }
    case fh6::scene::LayerKind::Group: {
        const auto &ga = static_cast<const fh6::scene::Group &>(a);
        const auto &gb = static_cast<const fh6::scene::Group &>(b);
        if (ga.isLiverySection != gb.isLiverySection
            || ga.liverySectionSlot != gb.liverySectionSlot
            || ga.sourceAbsPos != gb.sourceAbsPos
            || ga.pendingTransformMarker != gb.pendingTransformMarker
            || ga.inlineTransformMarker != gb.inlineTransformMarker
            || ga.effectiveTransformMarker != gb.effectiveTransformMarker
            || ga.headerControlBytes != gb.headerControlBytes
            || ga.flags != gb.flags
            || ga.sourceParentId != gb.sourceParentId
            || ga.sourcePreviousSiblingId != gb.sourcePreviousSiblingId
            || ga.sourcePreviousGroupDepth != gb.sourcePreviousGroupDepth
            || ga.sourceChildren != gb.sourceChildren
            || ga.children.size() != gb.children.size()) {
            return false;
        }
        for (int i = 0; i < static_cast<int>(ga.children.size()); ++i) {
            if (!nodeEqual(*ga.children[i], *gb.children[i])) {
                return false;
            }
        }
        return true;
    }
    }
    return false;
}

bool structureEqual(const fh6::scene::Layer &a, const fh6::scene::Layer &b)
{
    if (a.kind() != b.kind() || a.id != b.id || a.name != b.name || a.visible != b.visible || a.locked != b.locked) {
        return false;
    }
    if (a.kind() == fh6::scene::LayerKind::Group) {
        const auto &ga = static_cast<const fh6::scene::Group &>(a);
        const auto &gb = static_cast<const fh6::scene::Group &>(b);
        if (ga.children.size() != gb.children.size()
            || ga.isLiverySection != gb.isLiverySection
            || ga.liverySectionSlot != gb.liverySectionSlot) {
            return false;
        }
        for (int i = 0; i < static_cast<int>(ga.children.size()); ++i) {
            if (!structureEqual(*ga.children[i], *gb.children[i])) {
                return false;
            }
        }
    }
    if (a.kind() == fh6::scene::LayerKind::Shape) {
        const auto &sa = static_cast<const fh6::scene::Shape &>(a);
        const auto &sb = static_cast<const fh6::scene::Shape &>(b);
        return visualEqual(sa, sb) && sa.mask == sb.mask;
    }
    return true;
}

bool colorOnlyPreviewChange(const fh6::scene::Layer &a, const fh6::scene::Layer &b)
{
    if (a.kind() == fh6::scene::LayerKind::Shape) {
        const auto &sa = static_cast<const fh6::scene::Shape &>(a);
        const auto &sb = static_cast<const fh6::scene::Shape &>(b);
        return sa.color != sb.color || sa.shapeId != sb.shapeId || sa.rasterId != sb.rasterId;
    }
    if (a.kind() == fh6::scene::LayerKind::Group) {
        const auto &ga = static_cast<const fh6::scene::Group &>(a);
        const auto &gb = static_cast<const fh6::scene::Group &>(b);
        for (int i = 0; i < static_cast<int>(ga.children.size()); ++i) {
            if (colorOnlyPreviewChange(*ga.children[i], *gb.children[i])) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void EditorState::beginProjectEdit()
{
    if (!hasProject_) {
        pendingEdit_.reset();
        return;
    }
    ProjectEditCommand command;
    command.kind = ProjectEditCommandKind::Snapshot;
    command.before = captureSnapshot();
    command.undoSelection = {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
    command.redoSelection = {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
    pendingEdit_ = command;
}

void EditorState::commitProjectEdit()
{
    ScopedPerf perf("EditorState::commitProjectEdit");
    if (!pendingEdit_.has_value()) {
        return;
    }
    if (pendingEdit_->kind != ProjectEditCommandKind::Snapshot) {
        commitTransformCommand();
        return;
    }
    pendingEdit_->after = captureSnapshot();
    pendingEdit_->redoSelection = {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
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
    if (pendingEdit_->kind != ProjectEditCommandKind::Snapshot) {
        cancelTransformCommand();
        return;
    }
    applySnapshot(pendingEdit_->before);
    const ProjectSelectionSnapshot undoSelection = pendingEdit_->undoSelection;
    pendingEdit_.reset();
    setSelectionFromEntries(undoSelection.layerIds, undoSelection.guideLayerIds, undoSelection.entryIds);
    noteProjectStructureChanged();
}

void EditorState::beginTransformCommand()
{
    beginTransformCommand(selectedTransformTargetIds());
}

void EditorState::beginTransformCommand(const QVector<QString> &targetIds)
{
    if (!hasProject_) {
        pendingEdit_.reset();
        return;
    }
    ProjectEditCommand command;
    command.kind = ProjectEditCommandKind::Transform;
    command.refresh = ProjectEditRefresh::GeometryOnly;
    command.undoSelection = {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
    command.redoSelection = {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
    QSet<QString> seen;
    command.transforms.reserve(targetIds.size());
    for (const QString &id : targetIds) {
        if (id.isEmpty() || seen.contains(id)) {
            continue;
        }
        fh6::scene::Layer *node = sceneNode(id);
        if (node == nullptr) {
            continue;
        }
        command.transforms.push_back({id, node->transform, node->transform});
        seen.insert(id);
    }
    pendingEdit_ = command;
}

void EditorState::commitTransformCommand()
{
    ScopedPerf perf("EditorState::commitTransformCommand");
    if (!pendingEdit_.has_value()) {
        return;
    }
    if (pendingEdit_->kind != ProjectEditCommandKind::Transform) {
        commitProjectEdit();
        return;
    }
    QVector<ProjectTransformEdit> changed;
    changed.reserve(pendingEdit_->transforms.size());
    for (ProjectTransformEdit edit : pendingEdit_->transforms) {
        fh6::scene::Layer *node = sceneNode(edit.nodeId);
        if (node == nullptr) {
            pendingEdit_.reset();
            noteProjectStructureChanged();
            return;
        }
        edit.after = node->transform;
        if (!transformEqual(edit.before, edit.after)) {
            changed.push_back(edit);
        }
    }
    pendingEdit_->transforms = std::move(changed);
    pendingEdit_->redoSelection = {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
    if (!pendingEdit_->transforms.isEmpty()) {
        undoStack_.push_back(*pendingEdit_);
        redoStack_.clear();
        setModified(true);
        Q_EMIT historyChanged();
    }
    pendingEdit_.reset();
}

void EditorState::cancelTransformCommand()
{
    if (!pendingEdit_.has_value()) {
        return;
    }
    if (pendingEdit_->kind != ProjectEditCommandKind::Transform) {
        cancelProjectEdit();
        return;
    }
    const ProjectSelectionSnapshot undoSelection = pendingEdit_->undoSelection;
    applyTransformEdits(pendingEdit_->transforms, false);
    pendingEdit_.reset();
    setSelectionFromEntries(undoSelection.layerIds, undoSelection.guideLayerIds, undoSelection.entryIds);
    noteProjectGeometryChanged(false);
}

void EditorState::undo()
{
    if (undoStack_.isEmpty()) {
        return;
    }
    const QSet<QString> previousLayerIds = selectedLayerIds_;
    const QSet<QString> previousGuideIds = selectedGuideLayerIds_;
    const QVector<QString> previousEntryIds = selectedEntryIds_;
    const ProjectEditCommand command = undoStack_.takeLast();
    if (command.kind == ProjectEditCommandKind::Transform) {
        applyTransformEdits(command.transforms, false);
    } else {
        applySnapshot(command.before);
    }
    redoStack_.push_back(command);
    setModified(true);
    selectedLayerIds_ = existingLayerIds(command.undoSelection.layerIds);
    selectedGuideLayerIds_ = existingGuideLayerIds(command.undoSelection.guideLayerIds);
    selectedEntryIds_ = normalizeEntrySelection(command.undoSelection.entryIds);
    refreshAfterHistoryCommand(command.refresh);
    if (selectedLayerIds_ != previousLayerIds || selectedGuideLayerIds_ != previousGuideIds
        || selectedEntryIds_ != previousEntryIds) {
        Q_EMIT selectionChanged();
    }
    Q_EMIT historyChanged();
}

void EditorState::redo()
{
    if (redoStack_.isEmpty()) {
        return;
    }
    const QSet<QString> previousLayerIds = selectedLayerIds_;
    const QSet<QString> previousGuideIds = selectedGuideLayerIds_;
    const QVector<QString> previousEntryIds = selectedEntryIds_;
    const ProjectEditCommand command = redoStack_.takeLast();
    if (command.kind == ProjectEditCommandKind::Transform) {
        applyTransformEdits(command.transforms, true);
    } else {
        applySnapshot(command.after);
    }
    undoStack_.push_back(command);
    setModified(true);
    selectedLayerIds_ = existingLayerIds(command.redoSelection.layerIds);
    selectedGuideLayerIds_ = existingGuideLayerIds(command.redoSelection.guideLayerIds);
    selectedEntryIds_ = normalizeEntrySelection(command.redoSelection.entryIds);
    refreshAfterHistoryCommand(command.refresh);
    if (selectedLayerIds_ != previousLayerIds || selectedGuideLayerIds_ != previousGuideIds
        || selectedEntryIds_ != previousEntryIds) {
        Q_EMIT selectionChanged();
    }
    Q_EMIT historyChanged();
}

void EditorState::applySnapshot(const ProjectEditSnapshot &snapshot)
{
    project_ = snapshot.project;
    invalidateProjectIndexCache();
}

ProjectEditSnapshot EditorState::captureSnapshot() const
{
    ScopedPerf perf("EditorState::captureSnapshot");
    return {project_};
}

bool EditorState::snapshotsEqual(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const
{
    ScopedPerf perf("EditorState::snapshotsEqual");
    if (a.project.colorSwatches != b.project.colorSwatches
        || static_cast<bool>(a.project.root) != static_cast<bool>(b.project.root)) {
        return false;
    }
    if (!a.project.root) {
        return true;
    }
    return nodeEqual(*a.project.root, *b.project.root);
}

ProjectEditRefresh EditorState::classifySnapshotRefresh(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const
{
    ScopedPerf perf("EditorState::classifySnapshotRefresh");
    if (!a.project.root || !b.project.root || !structureEqual(*a.project.root, *b.project.root)) {
        return ProjectEditRefresh::Structure;
    }
    if (colorOnlyPreviewChange(*a.project.root, *b.project.root)) {
        return ProjectEditRefresh::Previews;
    }
    return ProjectEditRefresh::GeometryOnly;
}

void EditorState::applyTransformEdits(const QVector<ProjectTransformEdit> &edits, bool useAfter)
{
    for (const ProjectTransformEdit &edit : edits) {
        if (fh6::scene::Layer *node = sceneNode(edit.nodeId)) {
            node->transform = useAfter ? edit.after : edit.before;
        }
    }
    invalidateSceneTree();
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
