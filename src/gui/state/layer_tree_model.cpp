#include "layer_tree_model.h"

#include <QBrush>
#include <QDataStream>
#include <QIODevice>
#include <QImageReader>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QTransform>

#include <algorithm>
#include <cstring>

namespace gui {
namespace {

constexpr int ShapePreviewSize = 64;
constexpr int GroupPreviewSize = 64;
constexpr double PreviewPadding = 4.0;
constexpr int CheckerSize = 8;
constexpr char LayerEntryMimeType[] = "application/x-fh6-layer-entries";

enum class ThumbnailMode {
    Normal,
    MaskSilhouette,
};

ProjectLookup buildLookup(const fh6::Project &project)
{
    ProjectLookup lookup;
    lookup.layers.reserve(project.layers.size());
    lookup.guides.reserve(project.guideLayers.size());
    lookup.groups.reserve(project.groups.size());
    for (const fh6::ShapeLayer &layer : project.layers) {
        lookup.layers.insert(layer.id, &layer);
    }
    for (const fh6::GuideLayer &guide : project.guideLayers) {
        lookup.guides.insert(guide.id, &guide);
    }
    for (const fh6::LayerGroup &group : project.groups) {
        lookup.groups.insert(group.id, &group);
    }
    return lookup;
}

const fh6::LayerGroup *groupForId(const ProjectLookup &lookup, const QString &id)
{
    return lookup.groups.value(id, nullptr);
}

const fh6::ShapeLayer *layerForId(const ProjectLookup &lookup, const QString &id)
{
    return lookup.layers.value(id, nullptr);
}

const fh6::GuideLayer *guideForId(const ProjectLookup &lookup, const QString &id)
{
    return lookup.guides.value(id, nullptr);
}

QStringList leafIdsForId(const ProjectLookup &lookup, const QString &id)
{
    if (lookup.layers.contains(id)) {
        return {id};
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr) {
        return {};
    }
    QStringList ids;
    QSet<QString> seen;
    for (const QString &childId : group->childIds) {
        for (const QString &leafId : leafIdsForId(lookup, childId)) {
            if (!seen.contains(leafId)) {
                seen.insert(leafId);
                ids.push_back(leafId);
            }
        }
    }
    return ids;
}

// Append leaf ids in the same top-to-bottom order the tree lays them out (each sibling list is
// reversed so the foreground entry is on top). A group's leaves stay contiguous, so a covered
// range like "#3-7" is always a true span.
void collectVisualLeafOrder(const ProjectLookup &lookup, const QString &id, QStringList &out)
{
    if (lookup.layers.contains(id)) {
        out.push_back(id);
        return;
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr) {
        return;
    }
    for (int i = group->childIds.size() - 1; i >= 0; --i) {
        collectVisualLeafOrder(lookup, group->childIds[i], out);
    }
}

// "#3" for a single position, "#3-7" for a span; empty when no leaf carries a position.
QString positionLabel(const QHash<QString, int> &positions, const QStringList &leafIds)
{
    int minPos = 0;
    int maxPos = 0;
    for (const QString &leafId : leafIds) {
        const int pos = positions.value(leafId, 0);
        if (pos <= 0) {
            continue;
        }
        if (minPos == 0 || pos < minPos) {
            minPos = pos;
        }
        if (pos > maxPos) {
            maxPos = pos;
        }
    }
    if (minPos <= 0) {
        return {};
    }
    return minPos == maxPos ? QStringLiteral("#%1").arg(minPos)
                            : QStringLiteral("#%1-%2").arg(minPos).arg(maxPos);
}

bool entryHasLockedDescendant(const ProjectLookup &lookup, const QString &id)
{
    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        return layer->locked;
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr) {
        return false;
    }
    if (group->locked) {
        return true;
    }
    for (const QString &childId : group->childIds) {
        if (entryHasLockedDescendant(lookup, childId)) {
            return true;
        }
    }
    return false;
}

bool allLeafVisible(const ProjectLookup &lookup, const QString &id)
{
    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        return layer->visible;
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr || group->childIds.isEmpty()) {
        return true;
    }
    for (const QString &childId : group->childIds) {
        if (!allLeafVisible(lookup, childId)) {
            return false;
        }
    }
    return true;
}

bool allLeafMask(const ProjectLookup &lookup, const QString &id)
{
    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        return layer->mask;
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr || group->childIds.isEmpty()) {
        return false;
    }
    for (const QString &childId : group->childIds) {
        if (!allLeafMask(lookup, childId)) {
            return false;
        }
    }
    return true;
}

QTransform layerTransform(const fh6::ShapeLayer &layer)
{
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

QRectF layerBounds(const fh6::ShapeLayer &layer, const ShapeGeometryStore &geometry)
{
    const QSizeF size = geometry.shapeSize(layer.shapeId);
    const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
    return layerTransform(layer).mapRect(local);
}

QRectF entryBounds(const ProjectLookup &lookup, const ShapeGeometryStore &geometry, const QString &id)
{
    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        return layerBounds(*layer, geometry);
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr) {
        return {};
    }
    QRectF bounds;
    bool hasBounds = false;
    for (const QString &childId : group->childIds) {
        const QRectF childBounds = entryBounds(lookup, geometry, childId);
        if (!childBounds.isValid() || childBounds.isEmpty()) {
            continue;
        }
        bounds = hasBounds ? bounds.united(childBounds) : childBounds;
        hasBounds = true;
    }
    return bounds;
}

QColor layerColor(const fh6::ShapeLayer &layer, double alphaScale)
{
    return QColor(layer.color[2],
                  layer.color[1],
                  layer.color[0],
                  std::clamp(static_cast<int>(std::round(layer.color[3] * alphaScale)), 0, 255));
}

QColor thumbnailLayerColor(const fh6::ShapeLayer &layer, double alphaScale, ThumbnailMode mode)
{
    if (mode == ThumbnailMode::MaskSilhouette) {
        return QColor(35, 35, 35, std::clamp(static_cast<int>(std::round(layer.color[3] * alphaScale)), 0, 255));
    }
    return layerColor(layer, alphaScale);
}

QImage checkerboardImage(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    painter.fillRect(image.rect(), QColor(238, 238, 238));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(198, 198, 198));
    for (int y = 0; y < image.height(); y += CheckerSize) {
        for (int x = ((y / CheckerSize) % 2) * CheckerSize; x < image.width(); x += CheckerSize * 2) {
            painter.drawRect(QRect(x, y, CheckerSize, CheckerSize));
        }
    }
    return image;
}

QImage compositeOnCheckerboard(const QImage &content)
{
    QImage image = checkerboardImage(content.size());
    QPainter painter(&image);
    painter.drawImage(0, 0, content);
    return image;
}

void blendPreviewPixel(QImage &image, int x, int y, const fh6::ShapeLayer &layer, double alphaScale, ThumbnailMode mode)
{
    const double sourceAlpha = std::clamp((layer.color[3] / 255.0) * alphaScale, 0.0, 1.0);
    if (sourceAlpha <= 0.0) {
        return;
    }

    QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
    const QRgb destination = line[x];
    const double keep = 1.0 - sourceAlpha;
    if (layer.mask && mode == ThumbnailMode::Normal) {
        line[x] = qRgba(std::clamp(static_cast<int>(std::round(qRed(destination) * keep)), 0, 255),
                        std::clamp(static_cast<int>(std::round(qGreen(destination) * keep)), 0, 255),
                        std::clamp(static_cast<int>(std::round(qBlue(destination) * keep)), 0, 255),
                        std::clamp(static_cast<int>(std::round(qAlpha(destination) * keep)), 0, 255));
        return;
    }

    const QColor color = thumbnailLayerColor(layer, 1.0, mode);
    const double sourceRed = color.red() * sourceAlpha;
    const double sourceGreen = color.green() * sourceAlpha;
    const double sourceBlue = color.blue() * sourceAlpha;
    line[x] = qRgba(std::clamp(static_cast<int>(std::round(sourceRed + qRed(destination) * keep)), 0, 255),
                    std::clamp(static_cast<int>(std::round(sourceGreen + qGreen(destination) * keep)), 0, 255),
                    std::clamp(static_cast<int>(std::round(sourceBlue + qBlue(destination) * keep)), 0, 255),
                    std::clamp(static_cast<int>(std::round(sourceAlpha * 255.0 + qAlpha(destination) * keep)), 0, 255));
}

void rasterizePreviewTriangle(
    QImage &image,
    const QPointF &p0,
    const QPointF &p1,
    const QPointF &p2,
    double alpha0,
    double alpha1,
    double alpha2,
    const fh6::ShapeLayer &layer,
    ThumbnailMode mode)
{
    const double minX = std::floor(std::min({p0.x(), p1.x(), p2.x()}));
    const double maxX = std::ceil(std::max({p0.x(), p1.x(), p2.x()}));
    const double minY = std::floor(std::min({p0.y(), p1.y(), p2.y()}));
    const double maxY = std::ceil(std::max({p0.y(), p1.y(), p2.y()}));
    const int left = std::clamp(static_cast<int>(minX), 0, image.width() - 1);
    const int right = std::clamp(static_cast<int>(maxX), 0, image.width() - 1);
    const int top = std::clamp(static_cast<int>(minY), 0, image.height() - 1);
    const int bottom = std::clamp(static_cast<int>(maxY), 0, image.height() - 1);
    const double denominator = (p1.y() - p2.y()) * (p0.x() - p2.x()) + (p2.x() - p1.x()) * (p0.y() - p2.y());
    if (std::abs(denominator) < 1e-8) {
        return;
    }

    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const double sampleX = x + 0.5;
            const double sampleY = y + 0.5;
            const double w0 = ((p1.y() - p2.y()) * (sampleX - p2.x()) + (p2.x() - p1.x()) * (sampleY - p2.y())) / denominator;
            const double w1 = ((p2.y() - p0.y()) * (sampleX - p2.x()) + (p0.x() - p2.x()) * (sampleY - p2.y())) / denominator;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4) {
                continue;
            }
            blendPreviewPixel(image, x, y, layer, w0 * alpha0 + w1 * alpha1 + w2 * alpha2, mode);
        }
    }
}

void paintPreviewLayer(QImage &image, const fh6::ShapeLayer &layer, const ShapeGeometryStore &geometry, const QTransform &worldToPreview, ThumbnailMode mode)
{
    const ShapeGeometry *shape = geometry.shape(layer.shapeId);
    const QTransform localToWorld = layerTransform(layer);
    if (shape == nullptr || shape->triangles.isEmpty()) {
        const QSizeF size = geometry.shapeSize(layer.shapeId);
        const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
        QPolygonF polygon;
        polygon << worldToPreview.map(localToWorld.map(local.topLeft()))
                << worldToPreview.map(localToWorld.map(local.topRight()))
                << worldToPreview.map(localToWorld.map(local.bottomRight()))
                << worldToPreview.map(localToWorld.map(local.bottomLeft()));
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setCompositionMode(layer.mask && mode == ThumbnailMode::Normal ? QPainter::CompositionMode_DestinationOut : QPainter::CompositionMode_SourceOver);
        painter.setBrush(thumbnailLayerColor(layer, 0.75, mode));
        painter.drawPolygon(polygon);
        return;
    }

    for (const ShapeTriangle &triangle : shape->triangles) {
        rasterizePreviewTriangle(image,
                                 worldToPreview.map(localToWorld.map(triangle.p0)),
                                 worldToPreview.map(localToWorld.map(triangle.p1)),
                                 worldToPreview.map(localToWorld.map(triangle.p2)),
                                 triangle.alpha0,
                                 triangle.alpha1,
                                 triangle.alpha2,
                                 layer,
                                 mode);
    }
}

void paintPreviewEntry(QImage &image, const ProjectLookup &lookup, const ShapeGeometryStore &geometry, const QString &id, const QTransform &worldToPreview, ThumbnailMode mode = ThumbnailMode::Normal)
{
    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        if (layer->visible) {
            paintPreviewLayer(image, *layer, geometry, worldToPreview, mode);
        }
        return;
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr) {
        return;
    }
    for (const QString &childId : group->childIds) {
        paintPreviewEntry(image, lookup, geometry, childId, worldToPreview, mode);
    }
}

int previewSizeForId(const ProjectLookup &lookup, const QString &id)
{
    return lookup.groups.contains(id) ? GroupPreviewSize : ShapePreviewSize;
}

quint64 hashCombine(quint64 seed, quint64 value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

quint64 doubleBits(double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// A cheap signature over everything that affects an entry's rendered thumbnail (shape, colour,
// mask, visibility, transform, and - for groups - the ordered set of descendant signatures).
// It deliberately omits entry ids: two entries that look identical (e.g. a clone and its source)
// produce the same signature and therefore share a cached thumbnail. Recomputing this is far
// cheaper than rasterizing, so it both keys and gates the preview cache.
quint64 previewSignature(const ProjectLookup &lookup, const QString &id)
{
    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        quint64 h = 0xcbf29ce484222325ULL;
        h = hashCombine(h, layer->shapeId);
        h = hashCombine(h, (layer->visible ? 2ULL : 0ULL) | (layer->mask ? 1ULL : 0ULL));
        h = hashCombine(h, (quint64(layer->color[0]) << 24) | (quint64(layer->color[1]) << 16)
                               | (quint64(layer->color[2]) << 8) | quint64(layer->color[3]));
        h = hashCombine(h, doubleBits(layer->x));
        h = hashCombine(h, doubleBits(layer->y));
        h = hashCombine(h, doubleBits(layer->scaleX));
        h = hashCombine(h, doubleBits(layer->scaleY));
        h = hashCombine(h, doubleBits(layer->rotation));
        h = hashCombine(h, doubleBits(layer->skew));
        return h;
    }
    const fh6::LayerGroup *group = groupForId(lookup, id);
    if (group == nullptr) {
        return 0;
    }
    // Only the ordered child appearances matter for a group thumbnail, not the child ids, so a
    // cloned group hashes the same as its source.
    quint64 h = 0x100000001b3ULL;  // group marker seed
    for (const QString &childId : group->childIds) {
        h = hashCombine(h, previewSignature(lookup, childId));
    }
    return h;
}

QIcon guidePreviewIcon(const fh6::GuideLayer &guide)
{
    QImage image;
    if (!guide.pixelBytes.isEmpty() && guide.width > 0 && guide.height > 0) {
        image = QImage(reinterpret_cast<const uchar *>(guide.pixelBytes.constData()),
                       guide.width,
                       guide.height,
                       guide.width * 4,
                       QImage::Format_ARGB32_Premultiplied).copy();
    } else {
        image.loadFromData(guide.imageBytes, guide.imageFormat.toLatin1().constData());
    }
    if (image.isNull()) {
        QImage fallback(ShapePreviewSize, ShapePreviewSize, QImage::Format_ARGB32_Premultiplied);
        fallback.fill(QColor(70, 70, 70, 180));
        QPainter painter(&fallback);
        painter.setPen(QColor(220, 220, 220));
        painter.drawRect(fallback.rect().adjusted(2, 2, -3, -3));
        painter.drawLine(QPoint(10, 10), QPoint(ShapePreviewSize - 10, ShapePreviewSize - 10));
        painter.drawLine(QPoint(ShapePreviewSize - 10, 10), QPoint(10, ShapePreviewSize - 10));
        return QIcon(QPixmap::fromImage(fallback));
    }
    image = image.mirrored(false, true);
    return QIcon(QPixmap::fromImage(image.scaled(ShapePreviewSize, ShapePreviewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

} // namespace

LayerTreeModel::LayerTreeModel(QObject *parent)
    : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({QStringLiteral("Layer")});
    geometryLoaded_ = geometry_.loadDefault();
}

void LayerTreeModel::setEditorState(EditorState *state)
{
    state_ = state;
}

QStandardItem *LayerTreeModel::itemForId(const ProjectLookup &lookup, const QString &id, bool ancestorLocked) const
{
    if (const fh6::GuideLayer *guide = guideForId(lookup, id)) {
        auto *item = new QStandardItem(guide->name);
        item->setEditable(false);
        item->setIcon(guidePreviewIcon(*guide));
        item->setSizeHint(QSize(ShapePreviewSize + 8, ShapePreviewSize + 8));
        item->setData(guide->id, EntryIdRole);
        item->setData(QStringList{}, LeafIdsRole);
        item->setData(QStringList{guide->id}, GuideIdsRole);
        item->setData(false, IsGroupRole);
        item->setData(true, IsGuideRole);
        item->setData(guide->visible, VisibleRole);
        item->setData(false, MaskRole);
        item->setData(guide->locked, OwnLockedRole);
        item->setData(guide->locked, EffectiveLockedRole);
        if (!guide->visible || guide->locked) {
            item->setForeground(QBrush(QColor(130, 130, 130)));
        }
        return item;
    }

    if (const fh6::LayerGroup *group = groupForId(lookup, id)) {
        const bool effectivelyLocked = ancestorLocked || group->locked;
        const bool visible = allLeafVisible(lookup, group->id);
        const bool mask = allLeafMask(lookup, group->id);
        auto *item = new QStandardItem(group->name);
        item->setEditable(false);
        item->setIcon(previewIconForId(lookup, group->id));
        item->setSizeHint(QSize(GroupPreviewSize + 8, GroupPreviewSize + 8));
        item->setData(group->id, EntryIdRole);
        item->setData(leafIdsForId(lookup, group->id), LeafIdsRole);
        item->setData(QStringList{}, GuideIdsRole);
        item->setData(true, IsGroupRole);
        item->setData(false, IsGuideRole);
        item->setData(visible, VisibleRole);
        item->setData(mask, MaskRole);
        item->setData(group->locked, OwnLockedRole);
        item->setData(effectivelyLocked, EffectiveLockedRole);
        item->setData(positionLabel(leafPositions_, leafIdsForId(lookup, group->id)), PositionTextRole);
        if (!visible || effectivelyLocked) {
            item->setForeground(QBrush(QColor(150, 150, 150)));
        }
        // Display order is reversed (foreground on top): iterate children back-to-front
        // so the last-drawn child appears at the top of the group.
        for (int i = group->childIds.size() - 1; i >= 0; --i) {
            item->appendRow(itemForId(lookup, group->childIds[i], effectivelyLocked));
        }
        return item;
    }

    if (const fh6::ShapeLayer *layer = layerForId(lookup, id)) {
        const bool effectivelyLocked = ancestorLocked || layer->locked;
        auto *item = new QStandardItem(layer->name);
        item->setEditable(false);
        item->setIcon(previewIconForId(lookup, layer->id));
        item->setSizeHint(QSize(ShapePreviewSize + 8, ShapePreviewSize + 8));
        item->setData(layer->id, EntryIdRole);
        item->setData(QStringList{layer->id}, LeafIdsRole);
        item->setData(QStringList{}, GuideIdsRole);
        item->setData(false, IsGroupRole);
        item->setData(false, IsGuideRole);
        item->setData(layer->visible, VisibleRole);
        item->setData(layer->mask, MaskRole);
        item->setData(layer->locked, OwnLockedRole);
        item->setData(effectivelyLocked, EffectiveLockedRole);
        item->setData(positionLabel(leafPositions_, {layer->id}), PositionTextRole);
        if (!layer->visible || effectivelyLocked) {
            item->setForeground(QBrush(QColor(130, 130, 130)));
        }
        return item;
    }

    auto *missing = new QStandardItem(QStringLiteral("<missing %1>").arg(id));
    missing->setEditable(false);
    missing->setData(id, EntryIdRole);
    missing->setData(QStringList{}, LeafIdsRole);
    missing->setData(QStringList{}, GuideIdsRole);
    missing->setData(false, IsGroupRole);
    missing->setData(false, IsGuideRole);
    missing->setData(true, VisibleRole);
    missing->setData(false, MaskRole);
    missing->setData(false, OwnLockedRole);
    missing->setData(false, EffectiveLockedRole);
    return missing;
}

void LayerTreeModel::setProject(const fh6::Project *project)
{
    clearSectionCache();
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Layer")});
    displayParentGroupId_.clear();
    if (project == nullptr) {
        return;
    }

    const ProjectLookup lookup = buildLookup(*project);

    // Number the leaves top-to-bottom (position 1 = foreground) so rows can show their draw-order
    // position and groups their covered range.
    leafPositions_.clear();
    {
        QStringList order;
        if (!project->rootChildIds.isEmpty()) {
            for (int i = project->rootChildIds.size() - 1; i >= 0; --i) {
                collectVisualLeafOrder(lookup, project->rootChildIds[i], order);
            }
        } else {
            for (int li = project->layers.size() - 1; li >= 0; --li) {
                order.push_back(project->layers[li].id);
            }
        }
        for (int i = 0; i < order.size(); ++i) {
            leafPositions_.insert(order[i], order.size() - i);
        }
    }

    // The layer list shows the most-foreground (last-drawn) entry at the top and the
    // most-background entry at the bottom, so every sibling list is built back-to-front.
    if (!project->rootChildIds.isEmpty()) {
        for (int i = project->rootChildIds.size() - 1; i >= 0; --i) {
            appendRow(itemForId(lookup, project->rootChildIds[i]));
        }
    } else {
        for (int li = project->layers.size() - 1; li >= 0; --li) {
            const fh6::ShapeLayer &layer = project->layers[li];
            auto *item = new QStandardItem(layer.name);
            item->setEditable(false);
            item->setIcon(previewIconForId(lookup, layer.id));
            item->setSizeHint(QSize(ShapePreviewSize + 8, ShapePreviewSize + 8));
            item->setData(layer.id, EntryIdRole);
            item->setData(QStringList{layer.id}, LeafIdsRole);
            item->setData(QStringList{}, GuideIdsRole);
            item->setData(false, IsGroupRole);
            item->setData(false, IsGuideRole);
            item->setData(layer.visible, VisibleRole);
            item->setData(layer.mask, MaskRole);
            item->setData(layer.locked, OwnLockedRole);
            item->setData(layer.locked, EffectiveLockedRole);
            item->setData(positionLabel(leafPositions_, {layer.id}), PositionTextRole);
            if (!layer.visible || layer.locked) {
                item->setForeground(QBrush(QColor(130, 130, 130)));
            }
            appendRow(item);
        }
    }

    // Guides live at the very bottom of the tree because they are rendered behind all
    // shape layers (most background). Like the shapes above them, they are reversed so
    // the most-background guide sits at the very bottom.
    for (int gi = project->guideLayers.size() - 1; gi >= 0; --gi) {
        appendRow(itemForId(lookup, project->guideLayers[gi].id));
    }
}

void LayerTreeModel::setProjectSection(const fh6::Project *project, const QString &sectionGroupId)
{
    if (displayParentGroupId_ == sectionGroupId && rowCount() > 0) {
        return;
    }
    cacheDisplayedSectionRows();
    clear();
    setHorizontalHeaderLabels({QStringLiteral("Layer")});
    displayParentGroupId_ = sectionGroupId;
    if (project == nullptr) {
        return;
    }
    const auto cached = sectionRowsCache_.find(sectionGroupId);
    if (cached != sectionRowsCache_.end()) {
        QList<QList<QStandardItem *>> rows = cached.value();
        sectionRowsCache_.erase(cached);
        for (const QList<QStandardItem *> &row : rows) {
            appendRow(row);
        }
        return;
    }
    const ProjectLookup lookup = buildLookup(*project);
    const fh6::LayerGroup *section = groupForId(lookup, sectionGroupId);
    leafPositions_.clear();
    if (section != nullptr) {
        // Positions are relative to the displayed section, numbered top-to-bottom.
        QStringList order;
        for (int i = section->childIds.size() - 1; i >= 0; --i) {
            collectVisualLeafOrder(lookup, section->childIds[i], order);
        }
        for (int i = 0; i < order.size(); ++i) {
            leafPositions_.insert(order[i], order.size() - i);
        }
        // Reversed so the foreground entry is on top; see setProject().
        for (int i = section->childIds.size() - 1; i >= 0; --i) {
            appendRow(itemForId(lookup, section->childIds[i]));
        }
    }
    // Guides at the very bottom (drawn behind shapes), reversed; see setProject().
    for (int gi = project->guideLayers.size() - 1; gi >= 0; --gi) {
        appendRow(itemForId(lookup, project->guideLayers[gi].id));
    }
}

void LayerTreeModel::clearSectionCache()
{
    for (const QList<QList<QStandardItem *>> &rows : std::as_const(sectionRowsCache_)) {
        for (const QList<QStandardItem *> &row : rows) {
            qDeleteAll(row);
        }
    }
    sectionRowsCache_.clear();
}

void LayerTreeModel::cacheDisplayedSectionRows()
{
    if (displayParentGroupId_.isEmpty() || rowCount() == 0 || sectionRowsCache_.contains(displayParentGroupId_)) {
        return;
    }
    QList<QList<QStandardItem *>> rows;
    rows.reserve(rowCount());
    while (rowCount() > 0) {
        rows.push_back(takeRow(0));
    }
    sectionRowsCache_.insert(displayParentGroupId_, rows);
}

Qt::ItemFlags LayerTreeModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags itemFlags = QStandardItemModel::flags(index);
    if (index.isValid()) {
        itemFlags |= Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    } else {
        itemFlags |= Qt::ItemIsDropEnabled;
    }
    return itemFlags;
}

Qt::DropActions LayerTreeModel::supportedDragActions() const
{
    return Qt::MoveAction;
}

Qt::DropActions LayerTreeModel::supportedDropActions() const
{
    return Qt::MoveAction;
}

QStringList LayerTreeModel::mimeTypes() const
{
    return {QString::fromLatin1(LayerEntryMimeType)};
}

QMimeData *LayerTreeModel::mimeData(const QModelIndexList &indexes) const
{
    auto *mime = new QMimeData();
    QVector<QModelIndex> rows;
    QSet<QString> seenIds;
    for (const QModelIndex &index : indexes) {
        if (!index.isValid() || index.column() != 0) {
            continue;
        }
        const QString entryId = index.data(EntryIdRole).toString();
        if (entryId.isEmpty() || seenIds.contains(entryId)) {
            continue;
        }
        rows.push_back(index);
        seenIds.insert(entryId);
    }
    QVector<QModelIndex> normalizedRows;
    normalizedRows.reserve(rows.size());
    for (const QModelIndex &index : rows) {
        bool hasSelectedAncestor = false;
        for (QModelIndex ancestor = index.parent(); ancestor.isValid(); ancestor = ancestor.parent()) {
            const QString ancestorId = ancestor.data(EntryIdRole).toString();
            if (!ancestorId.isEmpty() && seenIds.contains(ancestorId)) {
                hasSelectedAncestor = true;
                break;
            }
        }
        if (!hasSelectedAncestor) {
            normalizedRows.push_back(index);
        }
    }
    rows = normalizedRows;
    std::sort(rows.begin(), rows.end(), [](const QModelIndex &a, const QModelIndex &b) {
        if (a.parent() == b.parent()) {
            return a.row() < b.row();
        }
        return a.data(EntryIdRole).toString() < b.data(EntryIdRole).toString();
    });

    QString parentGroupId;
    bool haveParent = false;
    QVector<QString> entryIds;
    for (const QModelIndex &index : rows) {
        const QString rowParent = index.parent().isValid()
            ? index.parent().data(EntryIdRole).toString()
            : displayParentGroupId_;
        if (!haveParent) {
            parentGroupId = rowParent;
            haveParent = true;
        } else if (parentGroupId != rowParent) {
            entryIds.clear();
            break;
        }
        entryIds.push_back(index.data(EntryIdRole).toString());
    }

    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << parentGroupId << entryIds;
    mime->setData(QString::fromLatin1(LayerEntryMimeType), payload);
    return mime;
}

bool LayerTreeModel::dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent)
{
    if (state_ == nullptr || data == nullptr || action != Qt::MoveAction || column > 0) {
        return false;
    }
    const QByteArray payload = data->data(QString::fromLatin1(LayerEntryMimeType));
    if (payload.isEmpty()) {
        return false;
    }

    QDataStream stream(payload);
    QString sourceParentGroupId;
    QVector<QString> entryIds;
    stream >> sourceParentGroupId >> entryIds;
    if (entryIds.isEmpty()) {
        return false;
    }

    const QString targetParentGroupId = parent.isValid()
        ? parent.data(EntryIdRole).toString()
        : displayParentGroupId_;
    if (targetParentGroupId != sourceParentGroupId) {
        return false;
    }
    if (row < 0) {
        row = rowCount(parent);
    }

    state_->beginProjectEdit();
    const bool guideDrop = std::all_of(entryIds.begin(), entryIds.end(), [this](const QString &id) {
        const QModelIndexList matches = match(index(0, 0), EntryIdRole, id, 1, Qt::MatchExactly | Qt::MatchRecursive);
        return !matches.isEmpty() && matches.front().data(IsGuideRole).toBool();
    });

    // The view is reversed (foreground on top), so a display insertion row maps to a data
    // index counted from the end of the sibling list (dataIndex = siblingCount - row). The
    // dragged entries are also reversed so a multi-row drag keeps its visual order.
    QVector<QString> movedEntries = entryIds;
    std::reverse(movedEntries.begin(), movedEntries.end());

    // Root-level guide rows sit at the bottom of the reversed list; count them so shape and
    // guide drops can locate their own region.
    const int totalRoot = rowCount(QModelIndex());
    int rootGuideCount = 0;
    for (int i = 0; i < totalRoot; ++i) {
        if (index(i, 0).data(IsGuideRole).toBool()) {
            ++rootGuideCount;
        }
    }

    bool changed = false;
    if (guideDrop && !parent.isValid()) {
        const int guideRegionStart = totalRoot - rootGuideCount;
        const int localRow = std::clamp(row - guideRegionStart, 0, rootGuideCount);
        changed = state_->reorderGuideLayers(movedEntries, rootGuideCount - localRow);
    } else {
        const int siblingCount = parent.isValid() ? rowCount(parent) : (totalRoot - rootGuideCount);
        const int clampedRow = std::clamp(row, 0, siblingCount);
        changed = state_->reorderEntries(targetParentGroupId, movedEntries, siblingCount - clampedRow);
    }
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

void LayerTreeModel::refreshStateRoles(const fh6::Project *project)
{
    if (project == nullptr) {
        return;
    }
    const ProjectLookup lookup = buildLookup(*project);
    refreshStateRolesForParent(lookup, QModelIndex(), false);
}

void LayerTreeModel::refreshPreviews(const fh6::Project *project)
{
    if (project == nullptr) {
        return;
    }
    const ProjectLookup lookup = buildLookup(*project);
    refreshPreviewsForParent(lookup, QModelIndex());
}

QIcon LayerTreeModel::previewIconForId(const ProjectLookup &lookup, const QString &id) const
{
    if (!geometryLoaded_) {
        return {};
    }
    const quint64 signature = previewSignature(lookup, id);
    const auto cached = previewCache_.constFind(signature);
    if (cached != previewCache_.constEnd()) {
        return cached.value();
    }
    const int previewSize = previewSizeForId(lookup, id);
    QRectF bounds = entryBounds(lookup, geometry_, id);
    if (!bounds.isValid() || bounds.isEmpty()) {
        bounds = QRectF(-64.0, -64.0, 128.0, 128.0);
    }

    QImage image(previewSize, previewSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double available = previewSize - PreviewPadding * 2.0;
    const double scale = std::min(available / std::max(bounds.width(), 1.0), available / std::max(bounds.height(), 1.0));
    QTransform worldToPreview;
    worldToPreview.translate(previewSize * 0.5, previewSize * 0.5);
    worldToPreview.scale(scale, -scale);
    worldToPreview.translate(-bounds.center().x(), -bounds.center().y());
    const fh6::ShapeLayer *layer = layerForId(lookup, id);
    const bool maskPreview = layer != nullptr ? layer->mask : allLeafMask(lookup, id);
    paintPreviewEntry(image, lookup, geometry_, id, worldToPreview,
                      maskPreview ? ThumbnailMode::MaskSilhouette : ThumbnailMode::Normal);
    image = compositeOnCheckerboard(image);

    // Bound memory across long editing sessions: distinct appearances are few in practice, but
    // live recolor/transform can mint many transient signatures, so clear when the cache grows
    // large. The current visible thumbnails are simply re-rasterized on the next refresh.
    constexpr int PreviewCacheCap = 4096;
    if (previewCache_.size() >= PreviewCacheCap) {
        previewCache_.clear();
    }
    QIcon icon(QPixmap::fromImage(image));
    previewCache_.insert(signature, icon);
    return icon;
}

QImage renderProjectPreviewImage(const fh6::Project &project, const QSize &size)
{
    if (size.isEmpty()) {
        return {};
    }

    ShapeGeometryStore geometry;
    if (!geometry.loadDefault()) {
        return {};
    }

    const ProjectLookup lookup = buildLookup(project);
    QRectF bounds;
    bool hasBounds = false;
    const auto addBoundsForEntry = [&](const QString &id) {
        const QRectF entry = entryBounds(lookup, geometry, id);
        if (!entry.isValid() || entry.isEmpty()) {
            return;
        }
        bounds = hasBounds ? bounds.united(entry) : entry;
        hasBounds = true;
    };

    if (!project.rootChildIds.isEmpty()) {
        for (const QString &id : project.rootChildIds) {
            addBoundsForEntry(id);
        }
    } else {
        for (const fh6::ShapeLayer &layer : project.layers) {
            addBoundsForEntry(layer.id);
        }
    }
    if (!hasBounds) {
        bounds = QRectF(-64.0, -64.0, 128.0, 128.0);
    }

    // Supersample: the manual triangle rasterizer point-samples one sample per pixel, so shape
    // edges alias badly at the export resolution. Render into a 4x buffer and let the smooth
    // downscale average those samples into antialiased edges (SSAA).
    constexpr int Supersample = 4;
    const QSize renderSize(size.width() * Supersample, size.height() * Supersample);

    QImage image(renderSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double available = std::max(1.0, static_cast<double>(std::min(renderSize.width(), renderSize.height())) - PreviewPadding * Supersample * 2.0);
    const double scale = std::min(available / std::max(bounds.width(), 1.0), available / std::max(bounds.height(), 1.0));
    QTransform worldToPreview;
    worldToPreview.translate(renderSize.width() * 0.5, renderSize.height() * 0.5);
    worldToPreview.scale(scale, -scale);
    worldToPreview.translate(-bounds.center().x(), -bounds.center().y());

    if (!project.rootChildIds.isEmpty()) {
        for (const QString &id : project.rootChildIds) {
            paintPreviewEntry(image, lookup, geometry, id, worldToPreview);
        }
    } else {
        for (const fh6::ShapeLayer &layer : project.layers) {
            paintPreviewEntry(image, lookup, geometry, layer.id, worldToPreview);
        }
    }
    return image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void LayerTreeModel::refreshStateRolesForParent(const ProjectLookup &lookup, const QModelIndex &parent, bool ancestorLocked)
{
    const int rows = rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = this->index(row, 0, parent);
        QStandardItem *entry = itemFromIndex(index);
        if (entry == nullptr) {
            continue;
        }
        const QString entryId = entry->data(EntryIdRole).toString();
        bool effectiveLocked = ancestorLocked;
        bool visible = true;
        bool mask = false;
        bool ownLocked = false;
        bool isGroup = false;
        bool isGuide = false;
        QString text = entry->text();
        if (const fh6::GuideLayer *guide = guideForId(lookup, entryId)) {
            isGuide = true;
            text = guide->name;
            visible = guide->visible;
            mask = false;
            ownLocked = guide->locked;
            effectiveLocked = guide->locked;
        } else if (const fh6::LayerGroup *group = groupForId(lookup, entryId)) {
            isGroup = true;
            text = group->name;
            visible = allLeafVisible(lookup, entryId);
            mask = allLeafMask(lookup, entryId);
            ownLocked = group->locked;
            effectiveLocked = ancestorLocked || group->locked;
        } else if (const fh6::ShapeLayer *layer = layerForId(lookup, entryId)) {
            text = layer->name;
            visible = layer->visible;
            mask = layer->mask;
            ownLocked = layer->locked;
            effectiveLocked = ancestorLocked || layer->locked;
        }
        entry->setText(text);
        entry->setData(visible, VisibleRole);
        entry->setData(mask, MaskRole);
        entry->setData(ownLocked, OwnLockedRole);
        entry->setData(effectiveLocked, EffectiveLockedRole);
        entry->setData(isGuide, IsGuideRole);
        // Match the build path (itemForId): dim with an explicit gray brush, but
        // otherwise clear the foreground role entirely so the view falls back to
        // the palette text color. Setting an empty QBrush() instead paints black,
        // which is invisible-to-wrong on the dark theme.
        if (!visible || effectiveLocked) {
            entry->setForeground(QBrush(QColor(isGroup ? 150 : 130, isGroup ? 150 : 130, isGroup ? 150 : 130)));
        } else {
            entry->setData(QVariant(), Qt::ForegroundRole);
        }
        refreshStateRolesForParent(lookup, index, effectiveLocked);
    }
}

void LayerTreeModel::refreshPreviewsForParent(const ProjectLookup &lookup, const QModelIndex &parent)
{
    const int rows = rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = this->index(row, 0, parent);
        QStandardItem *entry = itemFromIndex(index);
        if (entry == nullptr) {
            continue;
        }
        const QString entryId = entry->data(EntryIdRole).toString();
        const int previewSize = previewSizeForId(lookup, entryId);
        if (const fh6::GuideLayer *guide = guideForId(lookup, entryId)) {
            entry->setIcon(guidePreviewIcon(*guide));
        } else {
            entry->setIcon(previewIconForId(lookup, entryId));
        }
        entry->setSizeHint(QSize(previewSize + 8, previewSize + 8));
        refreshPreviewsForParent(lookup, index);
    }
}

} // namespace gui
