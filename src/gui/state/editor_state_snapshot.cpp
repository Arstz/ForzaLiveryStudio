#include "editor_state.h"

#include "perf_utils.h"

#include <utility>

namespace gui {
namespace {

bool visualEqual(const fh6::scene::Shape &a, const fh6::scene::Shape &b) {
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

bool transformEqual(const fh6::scene::Transform2D &a, const fh6::scene::Transform2D &b) {
    return a.x == b.x
        && a.y == b.y
        && a.scaleX == b.scaleX
        && a.scaleY == b.scaleY
        && a.rotation == b.rotation
        && a.skew == b.skew;
}

bool rasterContentEqual(const fh6::scene::RasterContainer *a,
                        const fh6::scene::RasterContainer *b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }

    return a->width == b->width
        && a->height == b->height
        && a->pixels == b->pixels
        && a->encoded == b->encoded
        && a->format == b->format;
}

bool nodeEqual(const fh6::scene::Layer &a, const fh6::scene::Layer &b) {
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
        if (ga.sourcePath != gb.sourcePath
            || ga.preprocessColorCount != gb.preprocessColorCount
            || !rasterContentEqual(ga.image.get(), gb.image.get())) {
            return false;
        }

        return true;
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

bool structureEqual(const fh6::scene::Layer &a, const fh6::scene::Layer &b) {
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

bool previewChange(const fh6::scene::Layer &a, const fh6::scene::Layer &b) {
    if (a.kind() == fh6::scene::LayerKind::Shape) {
        const auto &sa = static_cast<const fh6::scene::Shape &>(a);
        const auto &sb = static_cast<const fh6::scene::Shape &>(b);
        return sa.color != sb.color || sa.shapeId != sb.shapeId || sa.rasterId != sb.rasterId;
    }
    if (a.kind() == fh6::scene::LayerKind::Guide) {
        const auto &ga = static_cast<const fh6::scene::GuideLayer &>(a);
        const auto &gb = static_cast<const fh6::scene::GuideLayer &>(b);
        return ga.preprocessColorCount != gb.preprocessColorCount
            || !rasterContentEqual(ga.image.get(), gb.image.get());
    }
    if (a.kind() == fh6::scene::LayerKind::Group) {
        const auto &ga = static_cast<const fh6::scene::Group &>(a);
        const auto &gb = static_cast<const fh6::scene::Group &>(b);
        for (int i = 0; i < static_cast<int>(ga.children.size()); ++i) {
            if (previewChange(*ga.children[i], *gb.children[i])) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

void EditorState::beginProjectEdit() {
    if (!hasProject_) {
        pendingEdit_.reset();
        return;
    }
    ProjectEditCommand command;
    command.kind = ProjectEditCommandKind::Snapshot;
    command.before = captureSnapshot();
    command.undoSelection = captureSelection();
    command.redoSelection = command.undoSelection;
    pendingEdit_ = command;
}

void EditorState::commitProjectEdit() {
    ScopedPerf perf("EditorState::commitProjectEdit");
    if (!pendingEdit_.has_value()) {
        return;
    }
    if (pendingEdit_->kind != ProjectEditCommandKind::Snapshot) {
        commitTransformCommand();
        return;
    }
    pendingEdit_->after = captureSnapshot();
    pendingEdit_->redoSelection = captureSelection();
    if (!snapshotsEqual(pendingEdit_->before, pendingEdit_->after)) {
        pendingEdit_->refresh = classifySnapshotRefresh(pendingEdit_->before, pendingEdit_->after);
        undoStack_.push_back(*pendingEdit_);
        redoStack_.clear();
        setModified(true);
        Q_EMIT historyChanged();
    }
    pendingEdit_.reset();
}

void EditorState::cancelProjectEdit() {
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

void EditorState::beginTransformCommand() {
    beginTransformCommand(selectedTransformTargetIds());
}

void EditorState::beginTransformCommand(const QVector<QString> &targetIds) {
    if (!hasProject_) {
        pendingEdit_.reset();
        return;
    }
    ProjectEditCommand command;
    command.kind = ProjectEditCommandKind::Transform;
    command.refresh = ProjectEditRefresh::GeometryOnly;
    command.undoSelection = captureSelection();
    command.redoSelection = command.undoSelection;
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

void EditorState::commitTransformCommand() {
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
    pendingEdit_->redoSelection = captureSelection();
    if (!pendingEdit_->transforms.isEmpty()) {
        undoStack_.push_back(*pendingEdit_);
        redoStack_.clear();
        setModified(true);
        Q_EMIT historyChanged();
    }
    pendingEdit_.reset();
}

void EditorState::cancelTransformCommand() {
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

void EditorState::undo() {
    applyHistoryCommand(HistoryDirection::Undo);
}

void EditorState::redo() {
    applyHistoryCommand(HistoryDirection::Redo);
}

void EditorState::applySnapshot(const ProjectEditSnapshot &snapshot) {
    project_ = snapshot.project;
    invalidateProjectIndexCache();
}

ProjectEditSnapshot EditorState::captureSnapshot() const {
    ScopedPerf perf("EditorState::captureSnapshot");
    return {project_};
}

ProjectSelectionSnapshot EditorState::captureSelection() const {
    return {selectedLayerIds_, selectedGuideLayerIds_, selectedEntryIds_};
}

bool EditorState::snapshotsEqual(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const {
    ScopedPerf perf("EditorState::snapshotsEqual");
    if (a.project.colorSwatches != b.project.colorSwatches
        || a.project.horizontalGuidelines != b.project.horizontalGuidelines
        || a.project.verticalGuidelines != b.project.verticalGuidelines
        || static_cast<bool>(a.project.root) != static_cast<bool>(b.project.root)) {
        return false;
    }
    if (!a.project.root) {
        return true;
    }
    return nodeEqual(*a.project.root, *b.project.root);
}

ProjectEditRefresh EditorState::classifySnapshotRefresh(const ProjectEditSnapshot &a, const ProjectEditSnapshot &b) const {
    ScopedPerf perf("EditorState::classifySnapshotRefresh");
    if (!a.project.root || !b.project.root || !structureEqual(*a.project.root, *b.project.root)) {
        return ProjectEditRefresh::Structure;
    }
    if (previewChange(*a.project.root, *b.project.root)) {
        return ProjectEditRefresh::Previews;
    }
    return ProjectEditRefresh::GeometryOnly;
}

void EditorState::applyTransformEdits(const QVector<ProjectTransformEdit> &edits, bool useAfter) {
    for (const ProjectTransformEdit &edit : edits) {
        if (fh6::scene::Layer *node = sceneNode(edit.nodeId)) {
            node->transform = useAfter ? edit.after : edit.before;
        }
    }
    invalidateSceneTree();
}

void EditorState::applyHistoryCommand(HistoryDirection direction) {
    const bool isRedo = direction == HistoryDirection::Redo;
    QVector<ProjectEditCommand> &source = isRedo ? redoStack_ : undoStack_;
    QVector<ProjectEditCommand> &destination = isRedo ? undoStack_ : redoStack_;
    if (source.isEmpty()) {
        return;
    }
    const ProjectSelectionSnapshot previousSelection = captureSelection();
    const ProjectEditCommand command = source.takeLast();
    if (command.kind == ProjectEditCommandKind::Transform) {
        applyTransformEdits(command.transforms, isRedo);
    } else {
        applySnapshot(isRedo ? command.after : command.before);
    }
    destination.push_back(command);
    setModified(true);
    restoreSelection(isRedo ? command.redoSelection : command.undoSelection);
    refreshAfterHistoryCommand(command.refresh);
    const ProjectSelectionSnapshot restoredSelection = captureSelection();
    if (restoredSelection.layerIds != previousSelection.layerIds
        || restoredSelection.guideLayerIds != previousSelection.guideLayerIds
        || restoredSelection.entryIds != previousSelection.entryIds) {
        Q_EMIT selectionChanged();
    }
    Q_EMIT historyChanged();
}

void EditorState::restoreSelection(const ProjectSelectionSnapshot &selection) {
    selectedLayerIds_ = existingLayerIds(selection.layerIds);
    selectedGuideLayerIds_ = existingGuideLayerIds(selection.guideLayerIds);
    selectedEntryIds_ = normalizeEntrySelection(selection.entryIds);
}

void EditorState::refreshAfterHistoryCommand(ProjectEditRefresh refresh) {
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
