#include "layer_tree_model.h"

#include "raster_decals.h"
#include "scene_view.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int ShapePreviewSize = 64;
constexpr int GroupPreviewSize = 64;
constexpr char LayerEntryMimeType[] = "application/x-fh6-layer-entries";

quint64 mixHash(quint64 seed, quint64 value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

quint64 hashString(const QString &value) {
    return static_cast<quint64>(qHash(value));
}

quint64 hashDouble(double value) {
    return static_cast<quint64>(std::llround(value * 1000.0));
}

quint64 transformSignature(const fh6::scene::Transform2D &transform) {
    quint64 seed = 0x5472616e73666f72ULL;
    seed = mixHash(seed, hashDouble(transform.x));
    seed = mixHash(seed, hashDouble(transform.y));
    seed = mixHash(seed, hashDouble(transform.scaleX));
    seed = mixHash(seed, hashDouble(transform.scaleY));
    seed = mixHash(seed, hashDouble(transform.rotation));
    seed = mixHash(seed, hashDouble(transform.skew));
    return seed;
}

void indexNode(const fh6::scene::Layer &node, ProjectLookup &lookup) {
    switch (node.kind()) {
    case fh6::scene::LayerKind::Shape:
        lookup.layers.insert(node.id, static_cast<const fh6::scene::Shape *>(&node));
        break;
    case fh6::scene::LayerKind::Guide:
        lookup.guides.insert(node.id, static_cast<const fh6::scene::GuideLayer *>(&node));
        break;
    case fh6::scene::LayerKind::Group: {
        const auto &group = static_cast<const fh6::scene::Group &>(node);
        lookup.groups.insert(node.id, &group);
        for (const auto &child : group.children) {
            indexNode(*child, lookup);
        }
        break;
    }
    }
}

ProjectLookup buildLookup(const fh6::Project &project) {
    ProjectLookup lookup;
    if (project.root) {
        for (const auto &child : project.root->children) {
            indexNode(*child, lookup);
        }
    }
    return lookup;
}

const fh6::scene::Layer *nodeForId(const ProjectLookup &lookup, const QString &id) {
    if (const auto *shape = lookup.layers.value(id, nullptr)) {
        return shape;
    }
    if (const auto *guide = lookup.guides.value(id, nullptr)) {
        return guide;
    }
    return lookup.groups.value(id, nullptr);
}

QStringList leafIdsForNode(const fh6::scene::Layer &node) {
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        return {node.id};
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return {};
    }
    QStringList ids;
    QSet<QString> seen;
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        for (const QString &id : leafIdsForNode(*child)) {
            if (!seen.contains(id)) {
                ids.push_back(id);
                seen.insert(id);
            }
        }
    }
    return ids;
}

QStringList guideIdsForNode(const fh6::scene::Layer &node) {
    if (node.kind() == fh6::scene::LayerKind::Guide) {
        return {node.id};
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return {};
    }
    QStringList ids;
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        ids += guideIdsForNode(*child);
    }
    return ids;
}

void collectVisualLeafOrder(const fh6::scene::Layer &node, QStringList &out) {
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        out.push_back(node.id);
    } else if (node.kind() == fh6::scene::LayerKind::Group) {
        const auto &group = static_cast<const fh6::scene::Group &>(node);
        for (int i = static_cast<int>(group.children.size()) - 1; i >= 0; --i) {
            collectVisualLeafOrder(*group.children[i], out);
        }
    }
}

QString positionLabel(const QHash<QString, int> &positions, const QStringList &leafIds) {
    int minPos = 0;
    int maxPos = 0;
    for (const QString &leafId : leafIds) {
        const int pos = positions.value(leafId, 0);
        if (pos <= 0) {
            continue;
        }
        minPos = minPos == 0 ? pos : std::min(minPos, pos);
        maxPos = std::max(maxPos, pos);
    }
    if (minPos <= 0) {
        return {};
    }
    return minPos == maxPos ? QStringLiteral("#%1").arg(minPos)
                            : QStringLiteral("#%1-%2").arg(minPos).arg(maxPos);
}

bool allShapeVisible(const fh6::scene::Layer &node) {
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        return node.visible;
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return true;
    }
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        if (!allShapeVisible(*child)) {
            return false;
        }
    }
    return true;
}

bool allShapeMask(const fh6::scene::Layer &node) {
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        return static_cast<const fh6::scene::Shape &>(node).mask;
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return false;
    }
    const auto &group = static_cast<const fh6::scene::Group &>(node);
    if (group.children.empty()) {
        return false;
    }
    for (const auto &child : group.children) {
        if (!allShapeMask(*child)) {
            return false;
        }
    }
    return true;
}

QTransform previewNodeTransform(const fh6::scene::Layer &node) {
    return sceneLocalTransform(node);
}

QRectF previewShapeLocalRect(const fh6::scene::Shape &shape, const ShapeGeometryStore &geometry) {
    if (shape.raster) {
        return sceneLocalRect(QSizeF(shape.rasterWidth, shape.rasterHeight));
    }
    return geometry.shapeInkBounds(shape.shapeId);
}

QRectF previewNodeBounds(const fh6::scene::Layer &node,
                        const ShapeGeometryStore &geometry,
                        const QTransform &parentWorld,
                        bool includeNodeTransform) {
    const QTransform world = includeNodeTransform ? (previewNodeTransform(node) * parentWorld) : parentWorld;
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        return world.mapRect(previewShapeLocalRect(static_cast<const fh6::scene::Shape &>(node), geometry));
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return {};
    }
    QRectF bounds;
    bool hasBounds = false;
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        const QRectF childBounds = previewNodeBounds(*child, geometry, world, true);
        if (!childBounds.isValid() || childBounds.isEmpty()) {
            continue;
        }
        bounds = hasBounds ? bounds.united(childBounds) : childBounds;
        hasBounds = true;
    }
    return bounds;
}

QColor shapePreviewColor(const fh6::scene::Shape &shape, double alphaScale = 1.0) {
    return QColor(shape.color[2],
                  shape.color[1],
                  shape.color[0],
                  std::clamp(static_cast<int>(std::round(shape.color[3] * shape.opacity * alphaScale)), 0, 255));
}

void rasterizePreviewTriangle(QImage &image,
                              const QColor &ink,
                              const QPointF &p0,
                              const QPointF &p1,
                              const QPointF &p2,
                              double alpha0,
                              double alpha1,
                              double alpha2) {
    if (image.isNull()) {
        return;
    }
    const int left = std::clamp(static_cast<int>(std::floor(std::min({p0.x(), p1.x(), p2.x()}))), 0, image.width() - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(std::max({p0.x(), p1.x(), p2.x()}))), 0, image.width() - 1);
    const int top = std::clamp(static_cast<int>(std::floor(std::min({p0.y(), p1.y(), p2.y()}))), 0, image.height() - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(std::max({p0.y(), p1.y(), p2.y()}))), 0, image.height() - 1);
    const double denominator = (p1.y() - p2.y()) * (p0.x() - p2.x())
        + (p2.x() - p1.x()) * (p0.y() - p2.y());
    if (std::abs(denominator) < 1e-8) {
        return;
    }

    for (int y = top; y <= bottom; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = left; x <= right; ++x) {
            const double sampleX = x + 0.5;
            const double sampleY = y + 0.5;
            const double w0 = ((p1.y() - p2.y()) * (sampleX - p2.x())
                               + (p2.x() - p1.x()) * (sampleY - p2.y()))
                / denominator;
            const double w1 = ((p2.y() - p0.y()) * (sampleX - p2.x())
                               + (p0.x() - p2.x()) * (sampleY - p2.y()))
                / denominator;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4
                || w0 > 1.0001 || w1 > 1.0001 || w2 > 1.0001) {
                continue;
            }

            const double vertexAlpha = std::clamp(w0 * alpha0 + w1 * alpha1 + w2 * alpha2, 0.0, 1.0);
            const int alpha = std::clamp(static_cast<int>(std::round(ink.alpha() * vertexAlpha)), 0, 255);
            if (alpha > qAlpha(line[x])) {
                line[x] = qPremultiply(qRgba(ink.red(), ink.green(), ink.blue(), alpha));
            }
        }
    }
}

void paintPreviewShape(QImage &image,
                       const fh6::scene::Shape &shape,
                       const ShapeGeometryStore &geometry,
                       const QTransform &worldToPreview,
                       const QTransform &parentWorld,
                       bool includeNodeTransform) {
    if (!shape.visible || shape.opacity <= 0.0) {
        return;
    }
    const QTransform localToPreview = (includeNodeTransform ? (previewNodeTransform(shape) * parentWorld) : parentWorld) * worldToPreview;
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(shape.mask ? QPainter::CompositionMode_DestinationOut
                                           : QPainter::CompositionMode_SourceOver);

    if (shape.raster) {
        const fh6::RasterDecal decal = fh6::sharedRasterDecals().decal(shape.rasterId);
        if (decal.valid()) {
            QImage decalImage(reinterpret_cast<const uchar *>(decal.rgba.constData()),
                              decal.width,
                              decal.height,
                              decal.width * 4,
                              QImage::Format_RGBA8888);
            decalImage = decalImage.mirrored(false, true);
            painter.save();
            painter.setTransform(localToPreview, true);
            painter.setOpacity(std::clamp(shape.opacity * (shape.color[3] / 255.0), 0.0, 1.0));
            painter.drawImage(sceneLocalRect(QSizeF(decal.width, decal.height)), decalImage);
            painter.restore();
            return;
        }
    }

    const ShapeGeometry *shapeGeometry = shape.raster ? nullptr : geometry.shape(shape.shapeId);
    if (shapeGeometry != nullptr && !shapeGeometry->triangles.isEmpty()) {
        QImage shapeImage(image.size(), QImage::Format_ARGB32_Premultiplied);
        shapeImage.fill(Qt::transparent);
        const QColor ink = shapePreviewColor(shape);
        for (const ShapeTriangle &triangle : shapeGeometry->triangles) {
            rasterizePreviewTriangle(shapeImage,
                                     ink,
                                     localToPreview.map(triangle.p0),
                                     localToPreview.map(triangle.p1),
                                     localToPreview.map(triangle.p2),
                                     triangle.alpha0,
                                     triangle.alpha1,
                                     triangle.alpha2);
        }
        painter.drawImage(QPoint(), shapeImage);
        return;
    }

    const QRectF fallback = localToPreview.mapRect(previewShapeLocalRect(shape, geometry));
    painter.setBrush(shapePreviewColor(shape, 0.75));
    painter.drawRect(fallback);
}

void paintPreviewNode(QImage &image,
                      const fh6::scene::Layer &node,
                      const ShapeGeometryStore &geometry,
                      const QTransform &worldToPreview,
                      const QTransform &parentWorld,
                      bool includeNodeTransform) {
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        paintPreviewShape(image,
                          static_cast<const fh6::scene::Shape &>(node),
                          geometry,
                          worldToPreview,
                          parentWorld,
                          includeNodeTransform);
        return;
    }
    if (node.kind() != fh6::scene::LayerKind::Group || !node.visible) {
        return;
    }
    const QTransform world = includeNodeTransform ? (previewNodeTransform(node) * parentWorld) : parentWorld;
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        paintPreviewNode(image, *child, geometry, worldToPreview, world, true);
    }
}

QIcon shapeIcon(const fh6::scene::Layer &node,
                const ShapeGeometryStore &geometry,
                bool includeRootTransform) {
    QImage image(ShapePreviewSize, ShapePreviewSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(42, 42, 42));
    QRectF bounds = previewNodeBounds(node, geometry, QTransform(), includeRootTransform);
    if (!bounds.isValid() || bounds.isEmpty()) {
        bounds = QRectF(-32, -32, 64, 64);
    }
    QTransform worldToPreview;
    worldToPreview.translate(ShapePreviewSize / 2.0, ShapePreviewSize / 2.0);
    const double scale = std::min((ShapePreviewSize - 10.0) / std::max(1.0, bounds.width()),
                                  (ShapePreviewSize - 10.0) / std::max(1.0, bounds.height()));
    worldToPreview.scale(scale, -scale);
    worldToPreview.translate(-bounds.center().x(), -bounds.center().y());
    paintPreviewNode(image, node, geometry, worldToPreview, QTransform(), includeRootTransform);
    return QIcon(QPixmap::fromImage(image));
}

QIcon guideIcon(const fh6::scene::GuideLayer &guide) {
    QImage image;
    if (guide.image) {
        if (!guide.image->pixels.isEmpty() && guide.image->width > 0 && guide.image->height > 0) {
            image = QImage(reinterpret_cast<const uchar *>(guide.image->pixels.constData()),
                           guide.image->width,
                           guide.image->height,
                           guide.image->width * 4,
                           QImage::Format_ARGB32_Premultiplied).copy();
        } else {
            image.loadFromData(guide.image->encoded, guide.image->format.toLatin1().constData());
        }
    }
    if (image.isNull()) {
        image = QImage(ShapePreviewSize, ShapePreviewSize, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(70, 70, 70, 180));
    } else {
        image = image.mirrored(false, true);
    }
    return QIcon(QPixmap::fromImage(image.scaled(ShapePreviewSize, ShapePreviewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

quint64 contentSignature(const fh6::scene::Layer &node, QHash<QString, quint64> &cache);

quint64 transformedContentSignature(const fh6::scene::Layer &node, QHash<QString, quint64> &cache) {
    return mixHash(contentSignature(node, cache), transformSignature(node.transform));
}

quint64 contentSignature(const fh6::scene::Layer &node, QHash<QString, quint64> &cache) {
    const QString cacheKey = QStringLiteral("%1|content").arg(node.id);
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd()) {
        return cached.value();
    }

    quint64 seed = mixHash(0x4c61796572507265ULL, static_cast<quint64>(node.kind()));
    seed = mixHash(seed, node.visible ? 1 : 0);
    seed = mixHash(seed, hashDouble(node.opacity));
    switch (node.kind()) {
    case fh6::scene::LayerKind::Shape: {
        const auto &shape = static_cast<const fh6::scene::Shape &>(node);
        seed = mixHash(seed, shape.raster ? 1 : 0);
        seed = mixHash(seed, shape.shapeId);
        seed = mixHash(seed, shape.rasterId);
        seed = mixHash(seed, shape.rasterWidth);
        seed = mixHash(seed, shape.rasterHeight);
        seed = mixHash(seed, shape.mask ? 1 : 0);
        for (quint8 channel : shape.color) {
            seed = mixHash(seed, channel);
        }
        break;
    }
    case fh6::scene::LayerKind::Guide: {
        const auto &guide = static_cast<const fh6::scene::GuideLayer &>(node);
        seed = mixHash(seed, guide.image ? guide.image->width : 0);
        seed = mixHash(seed, guide.image ? guide.image->height : 0);
        seed = mixHash(seed, guide.image ? hashString(guide.image->format) : 0);
        seed = mixHash(seed, guide.image ? static_cast<quint64>(qHash(guide.image->encoded)) : 0);
        break;
    }
    case fh6::scene::LayerKind::Group: {
        const auto &group = static_cast<const fh6::scene::Group &>(node);
        seed = mixHash(seed, static_cast<quint64>(group.children.size()));
        for (const auto &child : group.children) {
            seed = mixHash(seed, transformedContentSignature(*child, cache));
        }
        break;
    }
    }

    cache.insert(cacheKey, seed);
    return seed;
}

} // namespace

LayerTreeModel::LayerTreeModel(QObject *parent)
    : QStandardItemModel(parent) {
    setHorizontalHeaderLabels({QStringLiteral("Layer")});
    geometryLoaded_ = geometry_.loadDefault();
}

void LayerTreeModel::setEditorState(EditorState *state) {
    state_ = state;
}

void LayerTreeModel::setGeneratePreviewsWithTransformations(bool enabled) {
    if (generatePreviewsWithTransformations_ == enabled) {
        return;
    }
    generatePreviewsWithTransformations_ = enabled;
    previewCache_.clear();
    previewSignatureCache_.clear();
}

bool LayerTreeModel::generatePreviewsWithTransformations() const {
    return generatePreviewsWithTransformations_;
}

QStandardItem *LayerTreeModel::itemForId(const ProjectLookup &lookup, const QString &id, bool ancestorLocked) const {
    const fh6::scene::Layer *node = nodeForId(lookup, id);
    if (node == nullptr) {
        return new QStandardItem(id);
    }
    auto *item = new QStandardItem(node->name);
    item->setEditable(false);
    item->setData(node->id, EntryIdRole);
    item->setData(leafIdsForNode(*node), LeafIdsRole);
    item->setData(guideIdsForNode(*node), GuideIdsRole);
    const bool isGroup = node->kind() == fh6::scene::LayerKind::Group;
    const bool isGuide = node->kind() == fh6::scene::LayerKind::Guide;
    const bool effectiveLocked = ancestorLocked || node->locked;
    item->setData(isGroup, IsGroupRole);
    item->setData(isGuide, IsGuideRole);
    item->setData(isGuide ? node->visible : allShapeVisible(*node), VisibleRole);
    item->setData(!isGuide && allShapeMask(*node), MaskRole);
    item->setData(node->locked, OwnLockedRole);
    item->setData(effectiveLocked, EffectiveLockedRole);
    item->setData(positionLabel(leafPositions_, item->data(LeafIdsRole).toStringList()), PositionTextRole);
    if (isGuide) {
        item->setIcon(guideIcon(static_cast<const fh6::scene::GuideLayer &>(*node)));
    } else if (geometryLoaded_) {
        item->setIcon(previewIconForNode(*node));
    }
    item->setSizeHint(QSize((isGroup ? GroupPreviewSize : ShapePreviewSize) + 8, ShapePreviewSize + 8));
    if (!node->visible || effectiveLocked) {
        item->setForeground(QBrush(QColor(isGroup ? 150 : 130, isGroup ? 150 : 130, isGroup ? 150 : 130)));
    }
    if (isGroup) {
        const auto &group = static_cast<const fh6::scene::Group &>(*node);
        for (int i = static_cast<int>(group.children.size()) - 1; i >= 0; --i) {
            item->appendRow(itemForId(lookup, group.children[i]->id, effectiveLocked));
        }
    }
    return item;
}

void LayerTreeModel::setProject(const fh6::Project *project) {
    clearSectionCache();
    previewSignatureCache_.clear();
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Layer")});
    displayParentGroupId_.clear();
    if (project == nullptr || !project->root) {
        return;
    }
    const ProjectLookup lookup = buildLookup(*project);
    leafPositions_.clear();
    QStringList order;
    for (int i = static_cast<int>(project->root->children.size()) - 1; i >= 0; --i) {
        collectVisualLeafOrder(*project->root->children[i], order);
    }
    for (int i = 0; i < order.size(); ++i) {
        leafPositions_.insert(order[i], order.size() - i);
    }
    for (int i = static_cast<int>(project->root->children.size()) - 1; i >= 0; --i) {
        appendRow(itemForId(lookup, project->root->children[i]->id));
    }
}

void LayerTreeModel::setProjectSection(const fh6::Project *project, const QString &sectionGroupId) {
    previewSignatureCache_.clear();
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Layer")});
    displayParentGroupId_ = sectionGroupId;
    if (project == nullptr) {
        return;
    }
    const ProjectLookup lookup = buildLookup(*project);
    const fh6::scene::Group *section = lookup.groups.value(sectionGroupId, nullptr);
    if (section == nullptr) {
        return;
    }
    leafPositions_.clear();
    QStringList order;
    for (int i = static_cast<int>(section->children.size()) - 1; i >= 0; --i) {
        collectVisualLeafOrder(*section->children[i], order);
    }
    for (int i = 0; i < order.size(); ++i) {
        leafPositions_.insert(order[i], order.size() - i);
    }
    for (int i = static_cast<int>(section->children.size()) - 1; i >= 0; --i) {
        appendRow(itemForId(lookup, section->children[i]->id));
    }
}

void LayerTreeModel::clearSectionCache() {
    sectionRowsCache_.clear();
}

void LayerTreeModel::cacheDisplayedSectionRows() {}

Qt::ItemFlags LayerTreeModel::flags(const QModelIndex &index) const {
    Qt::ItemFlags itemFlags = QStandardItemModel::flags(index);
    itemFlags |= Qt::ItemIsDropEnabled;
    if (index.isValid()) {
        itemFlags |= Qt::ItemIsDragEnabled;
    }
    return itemFlags;
}

Qt::DropActions LayerTreeModel::supportedDragActions() const { return Qt::MoveAction; }
Qt::DropActions LayerTreeModel::supportedDropActions() const { return Qt::MoveAction; }
QStringList LayerTreeModel::mimeTypes() const { return {QString::fromLatin1(LayerEntryMimeType)}; }

QMimeData *LayerTreeModel::mimeData(const QModelIndexList &indexes) const {
    auto *mime = new QMimeData();
    QVector<QModelIndex> rows;
    QSet<QString> seen;
    for (const QModelIndex &index : indexes) {
        if (index.isValid() && index.column() == 0) {
            const QString id = index.data(EntryIdRole).toString();
            if (!id.isEmpty() && !seen.contains(id)) {
                rows.push_back(index);
                seen.insert(id);
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    QString parentGroupId;
    QVector<QString> entryIds;
    bool haveParent = false;
    for (const QModelIndex &index : rows) {
        const QString entryId = index.data(EntryIdRole).toString();
        const QString rowParent = state_ != nullptr ? state_->parentGroupForEntry(entryId)
                                                    : (index.parent().isValid() ? index.parent().data(EntryIdRole).toString() : displayParentGroupId_);
        if (!haveParent) {
            parentGroupId = rowParent;
            haveParent = true;
        } else if (parentGroupId != rowParent) {
            entryIds.clear();
            break;
        }
        entryIds.push_back(entryId);
    }
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << parentGroupId << entryIds;
    mime->setData(QString::fromLatin1(LayerEntryMimeType), payload);
    return mime;
}

bool LayerTreeModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) {
    if (state_ == nullptr || data == nullptr || action != Qt::MoveAction || column > 0) {
        return false;
    }
    QByteArray payload = data->data(QString::fromLatin1(LayerEntryMimeType));
    if (payload.isEmpty()) {
        return false;
    }
    QDataStream stream(payload);
    QString sourceParentGroupId;
    QVector<QString> entryIds;
    stream >> sourceParentGroupId >> entryIds;
    const QString targetParentGroupId = parent.isValid() ? parent.data(EntryIdRole).toString() : sourceParentGroupId;
    if (entryIds.isEmpty() || targetParentGroupId != sourceParentGroupId) {
        return false;
    }
    if (row < 0) {
        row = rowCount(parent);
    }
    QVector<QString> movedEntries = entryIds;
    std::reverse(movedEntries.begin(), movedEntries.end());
    const int siblingCount = rowCount(parent);
    const int clampedRow = std::clamp(row, 0, siblingCount);
    state_->beginProjectEdit();
    const bool changed = state_->reorderEntries(targetParentGroupId, movedEntries, siblingCount - clampedRow);
    if (changed) {
        const QSet<QString> preservedSelection = state_->selectedLayerIds();
        const QSet<QString> preservedGuideSelection = state_->selectedGuideLayerIds();
        state_->commitProjectEdit();
        QPointer<EditorState> state(state_);
        QMetaObject::invokeMethod(this, [state, preservedSelection, preservedGuideSelection]() {
            if (!state.isNull()) {
                state->noteProjectStructureChanged();
                state->setSelectionIds(preservedSelection, preservedGuideSelection);
            }
        }, Qt::QueuedConnection);
    } else {
        state_->cancelProjectEdit();
    }
    return changed;
}

void LayerTreeModel::refreshStateRoles(const fh6::Project *project) {
    if (project == nullptr || !project->root) {
        return;
    }
    const ProjectLookup lookup = buildLookup(*project);
    refreshStateRolesForParent(lookup, QModelIndex(), false);
}

void LayerTreeModel::refreshPreviews(const fh6::Project *project) {
    if (project == nullptr || !project->root) {
        return;
    }
    previewSignatureCache_.clear();
    const ProjectLookup lookup = buildLookup(*project);
    refreshPreviewsForParent(lookup, QModelIndex());
}

QIcon LayerTreeModel::previewIconForId(const ProjectLookup &lookup, const QString &id) const {
    const fh6::scene::Layer *node = nodeForId(lookup, id);
    return node != nullptr && geometryLoaded_ ? previewIconForNode(*node) : QIcon();
}

QIcon LayerTreeModel::previewIconForNode(const fh6::scene::Layer &node) const {
    const quint64 signature = generatePreviewsWithTransformations_
        ? transformedContentSignature(node, previewSignatureCache_)
        : contentSignature(node, previewSignatureCache_);
    const auto cached = previewCache_.constFind(signature);
    if (cached != previewCache_.constEnd()) {
        return cached.value();
    }
    const QIcon icon = shapeIcon(node, geometry_, generatePreviewsWithTransformations_);
    if (previewCache_.size() > 4096) {
        previewCache_.clear();
    }
    previewCache_.insert(signature, icon);
    return icon;
}

QImage renderProjectPreviewImage(const fh6::Project &project, const QSize &size) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    if (size.isEmpty() || !project.root) {
        return image;
    }

    ShapeGeometryStore geometry;
    if (geometry.loadDefault()) {
        QRectF bounds = previewNodeBounds(*project.root, geometry, QTransform(), false);
        if (bounds.isValid() && !bounds.isEmpty()) {
            QTransform worldToPreview;
            worldToPreview.translate(size.width() * 0.5, size.height() * 0.5);
            const double availableWidth = std::max(1.0, size.width() - 20.0);
            const double availableHeight = std::max(1.0, size.height() - 20.0);
            const double scale = std::min(availableWidth / std::max(1.0, bounds.width()),
                                          availableHeight / std::max(1.0, bounds.height()));
            worldToPreview.scale(scale, -scale);
            worldToPreview.translate(-bounds.center().x(), -bounds.center().y());
            paintPreviewNode(image, *project.root, geometry, worldToPreview, QTransform(), false);
            return image;
        }
    }

    QPainter painter(&image);
    painter.setPen(QColor(210, 210, 210));
    painter.drawText(image.rect(), Qt::AlignCenter, project.name);
    return image;
}

void LayerTreeModel::refreshStateRolesForParent(const ProjectLookup &lookup, const QModelIndex &parent, bool ancestorLocked) {
    const int rows = rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = this->index(row, 0, parent);
        QStandardItem *entry = itemFromIndex(index);
        if (entry == nullptr) {
            continue;
        }
        const fh6::scene::Layer *node = nodeForId(lookup, entry->data(EntryIdRole).toString());
        if (node == nullptr) {
            continue;
        }
        const bool isGroup = node->kind() == fh6::scene::LayerKind::Group;
        const bool isGuide = node->kind() == fh6::scene::LayerKind::Guide;
        const bool effectiveLocked = ancestorLocked || node->locked;
        entry->setText(node->name);
        entry->setData(isGroup, IsGroupRole);
        entry->setData(isGuide, IsGuideRole);
        entry->setData(isGuide ? node->visible : allShapeVisible(*node), VisibleRole);
        entry->setData(!isGuide && allShapeMask(*node), MaskRole);
        entry->setData(node->locked, OwnLockedRole);
        entry->setData(effectiveLocked, EffectiveLockedRole);
        if (!node->visible || effectiveLocked) {
            entry->setForeground(QBrush(QColor(isGroup ? 150 : 130, isGroup ? 150 : 130, isGroup ? 150 : 130)));
        } else {
            entry->setData(QVariant(), Qt::ForegroundRole);
        }
        refreshStateRolesForParent(lookup, index, effectiveLocked);
    }
}

void LayerTreeModel::refreshPreviewsForParent(const ProjectLookup &lookup, const QModelIndex &parent) {
    const int rows = rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = this->index(row, 0, parent);
        QStandardItem *entry = itemFromIndex(index);
        if (entry == nullptr) {
            continue;
        }
        const fh6::scene::Layer *node = nodeForId(lookup, entry->data(EntryIdRole).toString());
        if (node == nullptr) {
            continue;
        }
        const bool isGroup = node->kind() == fh6::scene::LayerKind::Group;
        if (node->kind() == fh6::scene::LayerKind::Guide) {
            entry->setIcon(guideIcon(static_cast<const fh6::scene::GuideLayer &>(*node)));
        } else if (geometryLoaded_) {
            entry->setIcon(previewIconForNode(*node));
        }
        entry->setSizeHint(QSize((isGroup ? GroupPreviewSize : ShapePreviewSize) + 8, ShapePreviewSize + 8));
        refreshPreviewsForParent(lookup, index);
    }
}

} // namespace gui
