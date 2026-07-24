#include "fh6_core.h"
#include "fm_codec.h"
#include "header_codec.h"
#include "vinyl_decoder.h"
#include "livery_codec.h"
#include "cgroup_codec.h"
#include "layer.h"
#include "matrix_math.h"
#include "project_codec.h"
#include "shape_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <cstdio>

using namespace fh6;

namespace {

struct SectionMap {
    const char *folder;
    int slot;
};
const SectionMap kSections[] = {
    {"front", 0}, {"back", 1}, {"top", 2},
    {"left", 3}, {"right", 4}, {"spoiler", 5},
    {"frontWindow", 6},
    {"backWindow", 7},
    {"topWindow", 8},
    {"leftWindow", 9}, {"rightWindow", 10},
};

struct ShapeView {
    const scene::Shape *shape = nullptr;
    scene::Transform2D world;
};

void collectLeaves(const scene::Layer &node, QVector<ShapeView> &out)
{
    if (node.kind() == scene::LayerKind::Shape) {
        const auto &shape = static_cast<const scene::Shape &>(node);
        if (!shape.raster) {
            out.push_back({&shape, decomposeTransform2D(shape.worldMatrix())});
        }
        return;
    }
    if (node.kind() == scene::LayerKind::Group) {
        for (const auto &child : static_cast<const scene::Group &>(node).children) {
            collectLeaves(*child, out);
        }
    }
}

QVector<ShapeView> sectionLeaves(const Project &livery, int slot)
{
    QVector<ShapeView> out;
    if (!livery.root) {
        return out;
    }
    for (const auto &child : livery.root->children) {
        if (child->kind() == scene::LayerKind::Group) {
            const auto &group = static_cast<const scene::Group &>(*child);
            if (group.isLiverySection && group.liverySectionSlot == slot) {
                collectLeaves(group, out);
                break;
            }
        }
    }
    return out;
}

QVector<ShapeView> truthLeaves(const Project &truth)
{
    QVector<ShapeView> out;
    if (!truth.root) {
        return out;
    }
    for (const auto &child : truth.root->children) {
        collectLeaves(*child, out);
    }
    return out;
}

bool shapesMatch(const ShapeView &a, const ShapeView &b)
{
    if (a.shape == nullptr || b.shape == nullptr) return false;
    const double posTol = 0.5;
    const double scaleTol = 0.01;
    const double rotTol = 0.5;
    if (a.shape->shapeId != b.shape->shapeId) return false;
    if (std::abs(a.world.x - b.world.x) > posTol) return false;
    if (std::abs(a.world.y - b.world.y) > posTol) return false;
    if (std::abs(std::abs(a.world.scaleX) - std::abs(b.world.scaleX)) > scaleTol + 0.001 * std::abs(b.world.scaleX)) return false;
    if (std::abs(std::abs(a.world.scaleY) - std::abs(b.world.scaleY)) > scaleTol + 0.001 * std::abs(b.world.scaleY)) return false;
    double dr = std::fmod(std::abs(a.world.rotation - b.world.rotation), 360.0);
    if (dr > 180.0) dr = 360.0 - dr;
    if (dr > rotTol) return false;
    if (a.shape->color != b.shape->color) return false;
    return true;
}

QString colStr(const std::array<quint8, 4> &c)
{
    return QStringLiteral("%1,%2,%3,%4").arg(c[0]).arg(c[1]).arg(c[2]).arg(c[3]);
}

void dumpTree(const scene::Layer &node, int depth, int maxDepth, int maxChildren)
{
    QString ind(depth * 2, QLatin1Char(' '));
    if (node.kind() == scene::LayerKind::Shape) {
        const auto &s = static_cast<const scene::Shape &>(node);
        const scene::Transform2D world = decomposeTransform2D(s.worldMatrix());
        std::printf("%sshape id=%u x=%.2f y=%.2f sx=%.3f sy=%.3f rot=%.1f mask=%d\n",
                    ind.toLatin1().constData(), s.shapeId, world.x, world.y,
                    world.scaleX, world.scaleY, world.rotation, s.mask ? 1 : 0);
        return;
    }
    if (node.kind() != scene::LayerKind::Group) return;
    const auto &g = static_cast<const scene::Group &>(node);
    std::printf("%sGROUP %s '%s' flags=%d children=%d ptm=%s inl=%s\n",
                ind.toLatin1().constData(), g.id.toLatin1().constData(),
                g.name.toLatin1().constData(), g.flags,
                static_cast<int>(g.children.size()),
                g.pendingTransformMarker.toHex().constData(),
                g.inlineTransformMarker.toHex().constData());
    if (depth >= maxDepth) return;
    int shown = 0;
    for (const auto &child : g.children) {
        if (shown++ >= maxChildren) {
            std::printf("%s  ... (%d more)\n", ind.toLatin1().constData(),
                        static_cast<int>(g.children.size()) - maxChildren);
            break;
        }
        dumpTree(*child, depth + 1, maxDepth, maxChildren);
    }
}

void collectVisualLeafIds(const scene::Layer &node, QStringList &out)
{
    if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(node.id);
        return;
    }
    if (node.kind() != scene::LayerKind::Group) {
        return;
    }
    const auto &group = static_cast<const scene::Group &>(node);
    for (int i = static_cast<int>(group.children.size()) - 1; i >= 0; --i) {
        collectVisualLeafIds(*group.children[i], out);
    }
}

void collectSourceLeafIds(const scene::Layer &node, QStringList &out)
{
    if (node.kind() == scene::LayerKind::Shape) {
        out.push_back(node.id);
        return;
    }
    if (node.kind() != scene::LayerKind::Group) {
        return;
    }
    const auto &group = static_cast<const scene::Group &>(node);
    for (const auto &child : group.children) {
        collectSourceLeafIds(*child, out);
    }
}

QPair<int, int> leafRange(const QStringList &leaves, const QHash<QString, int> &positions)
{
    int minPos = 0;
    int maxPos = 0;
    for (const QString &leafId : leaves) {
        const int pos = positions.value(leafId, 0);
        if (pos <= 0) {
            continue;
        }
        minPos = minPos == 0 ? pos : std::min(minPos, pos);
        maxPos = std::max(maxPos, pos);
    }
    return {minPos, maxPos};
}

void dumpGroupRanges(const scene::Layer &node,
                     const QHash<QString, int> &visualPositions,
                     const QHash<QString, int> &sourcePositions,
                     int depth)
{
    if (node.kind() != scene::LayerKind::Group) {
        return;
    }
    const auto &group = static_cast<const scene::Group &>(node);
    QStringList visualLeaves;
    collectVisualLeafIds(node, visualLeaves);
    QStringList sourceLeaves;
    collectSourceLeafIds(node, sourceLeaves);
    const auto visualRange = leafRange(visualLeaves, visualPositions);
    const auto sourceRange = leafRange(sourceLeaves, sourcePositions);
    QString ind(depth * 2, QLatin1Char(' '));
    std::printf("%sGROUP %s '%s' visual=%d-%d source=%d-%d leaves=%d children=%d srcAbs=%d flags=%d ptm=%s inl=%s\n",
                ind.toLatin1().constData(),
                group.id.toLatin1().constData(),
                group.name.toLatin1().constData(),
                visualRange.first, visualRange.second,
                sourceRange.first, sourceRange.second,
                static_cast<int>(visualLeaves.size()),
                static_cast<int>(group.children.size()),
                group.sourceAbsPos, group.flags,
                group.pendingTransformMarker.toHex().constData(),
                group.inlineTransformMarker.toHex().constData());
    for (const auto &child : group.children) {
        dumpGroupRanges(*child, visualPositions, sourcePositions, depth + 1);
    }
}

int sceneShapeCount(const Project &p)
{
    return truthLeaves(p).size();
}

struct RawStats {
    int shapes = 0;
    int logos = 0;
    int masks = 0;
    int skipped = 0;
    int groups = 0;
    int incompleteGroups = 0;
};

void collectRawStats(const VinylGroup &node, RawStats &stats)
{
    stats.skipped += node.skippedChildren;
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            ++stats.shapes;
            const VinylShape &shape = std::get<VinylShape>(item.value);
            if (shape.isLogo) {
                ++stats.logos;
            }
            if (shape.isMask) {
                ++stats.masks;
            }
            continue;
        }
        const VinylGroup &group = *std::get<VinylGroupPtr>(item.value);
        ++stats.groups;
        if (group.expectedChildren && group.totalChildren() != *group.expectedChildren) {
            ++stats.incompleteGroups;
        }
        collectRawStats(group, stats);
    }
}

void printIncompleteGroups(const VinylGroup &node)
{
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            continue;
        }
        const VinylGroup &group = *std::get<VinylGroupPtr>(item.value);
        if (group.expectedChildren && group.totalChildren() != *group.expectedChildren) {
            std::printf("    incomplete abs=%d expected=%d actual=%d source=%s marker=%s\n",
                        group.absPos, *group.expectedChildren, group.totalChildren(),
                        group.source.toLatin1().constData(),
                        group.effectiveTransformMarker.toHex().constData());
        }
        printIncompleteGroups(group);
    }
}

int sceneMaskCount(const Project &project)
{
    int count = 0;
    for (const ShapeView &view : truthLeaves(project)) {
        if (view.shape != nullptr && view.shape->mask) {
            ++count;
        }
    }
    return count;
}

bool nudgeFirstBuiltInShape(scene::Layer &node)
{
    if (node.kind() == scene::LayerKind::Shape) {
        auto &shape = static_cast<scene::Shape &>(node);
        if (!shape.raster && detail::isKnownShapeId(shape.shapeId)) {
            shape.x += 10.0;
            return true;
        }
        return false;
    }
    if (node.kind() != scene::LayerKind::Group) {
        return false;
    }
    for (const auto &child : static_cast<scene::Group &>(node).children) {
        if (nudgeFirstBuiltInShape(*child)) {
            return true;
        }
    }
    return false;
}

bool layerHasBuiltInShape(const scene::Layer &node)
{
    if (node.kind() == scene::LayerKind::Shape) {
        const auto &shape = static_cast<const scene::Shape &>(node);
        return !shape.raster && detail::isKnownShapeId(shape.shapeId);
    }
    if (node.kind() != scene::LayerKind::Group) {
        return false;
    }
    for (const auto &child : static_cast<const scene::Group &>(node).children) {
        if (layerHasBuiltInShape(*child)) {
            return true;
        }
    }
    return false;
}

bool rotateFirstArtworkGroup(scene::Layer &node)
{
    if (node.kind() != scene::LayerKind::Group) {
        return false;
    }

    auto &group = static_cast<scene::Group &>(node);
    if (!group.isLiverySection && layerHasBuiltInShape(group)) {
        group.rotation += 10.0;
        return true;
    }
    for (const auto &child : group.children) {
        if (rotateFirstArtworkGroup(*child)) {
            return true;
        }
    }
    return false;
}

int rootEntryCount(const Project &p)
{
    return p.root ? static_cast<int>(p.root->children.size()) : 0;
}

scene::Group *syntheticSection(Project &project, int slot)
{
    if (!project.root) {
        return nullptr;
    }
    for (const auto &child : project.root->children) {
        if (child->kind() != scene::LayerKind::Group) {
            continue;
        }
        auto *group = static_cast<scene::Group *>(child.get());
        if (group->isLiverySection && group->liverySectionSlot == slot) {
            return group;
        }
    }
    return nullptr;
}

void clearSyntheticArtwork(Project &project)
{
    if (!project.root) {
        return;
    }
    for (const auto &child : project.root->children) {
        if (child->kind() != scene::LayerKind::Group) {
            continue;
        }
        auto &group = static_cast<scene::Group &>(*child);
        if (group.isLiverySection) {
            group.children.clear();
            group.sourceChildren.clear();
        }
    }
}

quint16 syntheticShapeId(int index)
{
    static constexpr quint16 ids[] = {
        101, 102, 122, 127, 136, 138, 139, 606, 1536, 1909, 1912, 1926, 2026, 2103, 2104, 2118, 2133,
    };
    return ids[index % static_cast<int>(std::size(ids))];
}

std::array<quint8, 4> syntheticColor(int index)
{
    static constexpr std::array<quint8, 4> colors[] = {
        {36, 88, 240, 255}, {220, 48, 34, 255}, {42, 210, 112, 255},
        {238, 190, 28, 255}, {180, 55, 224, 220}, {245, 245, 245, 180},
    };
    return colors[index % static_cast<int>(std::size(colors))];
}

std::unique_ptr<scene::Shape> syntheticShape(int &idCounter, int index,
                                              double x, double y, bool mask = false)
{
    auto shape = std::make_unique<scene::Shape>();
    shape->id = QStringLiteral("synthetic-shape-%1").arg(++idCounter, 6, 10, QLatin1Char('0'));
    shape->name = QStringLiteral("Synthetic %1").arg(idCounter);
    shape->setVectorShape(syntheticShapeId(index));
    shape->x = x;
    shape->y = y;
    shape->scaleX = (index % 5 == 0 ? -1.0 : 1.0) * (0.10 + 0.017 * (index % 11));
    shape->scaleY = (index % 7 == 0 ? -1.0 : 1.0) * (0.08 + 0.013 * (index % 13));
    shape->rotation = std::fmod(index * 37.0 + 11.0, 360.0);
    shape->skew = (index % 4 - 1.5) * 0.08;
    shape->mask = mask;
    shape->color = mask ? std::array<quint8, 4>{0, 0, 0, 0} : syntheticColor(index);
    return shape;
}

std::unique_ptr<scene::Group> syntheticGroup(int &idCounter, const QString &name,
                                              int index, double x = 0.0, double y = 0.0)
{
    auto group = std::make_unique<scene::Group>();
    group->id = QStringLiteral("synthetic-group-%1").arg(++idCounter, 6, 10, QLatin1Char('0'));
    group->name = name;
    group->x = x;
    group->y = y;
    group->scaleX = index % 4 == 0 ? -1.0 : 1.0;
    group->scaleY = 0.85 + 0.05 * (index % 5);
    group->rotation = std::fmod(index * 19.0, 360.0);
    group->skew = (index % 3 - 1.0) * 0.04;
    return group;
}

void appendFlatCluster(scene::Group &section, int &idCounter, int seed, int count,
                       double x, double y)
{
    auto outer = syntheticGroup(idCounter, QStringLiteral("Flat cluster %1").arg(seed), seed, x, y);
    for (int branch = 0; branch < 3; ++branch) {
        auto inner = syntheticGroup(idCounter, QStringLiteral("Flat branch %1").arg(branch),
                                    seed + branch + 1, branch * 17.0 - 17.0, branch * 9.0 - 9.0);
        for (int i = branch; i < count; i += 3) {
            inner->append(syntheticShape(idCounter, seed + i,
                                         (i % 9) * 13.0 - 52.0,
                                         (i / 9) * 15.0 - 30.0));
        }
        outer->append(std::move(inner));
    }
    section.append(std::move(outer));
}

std::unique_ptr<scene::Group> maskedPair(int &idCounter, int seed, double x, double y)
{
    auto group = syntheticGroup(idCounter, QStringLiteral("Masked pair %1").arg(seed), seed, x, y);
    group->append(syntheticShape(idCounter, seed, -8.0, -5.0, true));
    group->append(syntheticShape(idCounter, seed + 1, 9.0, 6.0, true));
    return group;
}

std::unique_ptr<scene::Group> deepMaskedBranch(int &idCounter, int seed, int depth)
{
    auto group = syntheticGroup(idCounter, QStringLiteral("Mask depth %1").arg(depth),
                                seed + depth, 6.0 * depth, -4.0 * depth);
    group->append(syntheticShape(idCounter, seed + depth, -10.0, depth * 3.0, true));
    if (depth > 1) {
        group->append(deepMaskedBranch(idCounter, seed + 7, depth - 1));
    } else {
        group->append(syntheticShape(idCounter, seed + 1, 11.0, -3.0, true));
    }
    return group;
}

Project syntheticBase(const QString &sourceFolder)
{
    Project project = importCLivery(sourceFolder);
    clearSyntheticArtwork(project);
    return project;
}

void buildSparseFlatSlots(Project &project)
{
    int ids = 0;
    const int sectionSlots[] = {0, 2, 5, 8, 10};
    const int counts[] = {13, 9, 17, 5, 33};
    for (int i = 0; i < 5; ++i) {
        if (scene::Group *section = syntheticSection(project, sectionSlots[i])) {
            appendFlatCluster(*section, ids, 30 + i * 40, counts[i], i * 23.0 - 45.0, i * -11.0);
        }
    }
}

void populateFrontMaskTransitions(scene::Group &front, int &ids)
{
    front.append(syntheticShape(ids, 10, -105.0, -48.0));
    auto nineMasks = syntheticGroup(ids, QStringLiteral("Nine masks"), 11, -55.0, -12.0);
    for (int i = 0; i < 9; ++i) {
        nineMasks->append(syntheticShape(ids, 20 + i, (i % 3) * 15.0, (i / 3) * 13.0, true));
    }
    front.append(std::move(nineMasks));
    front.append(syntheticShape(ids, 31, -8.0, 42.0));

    auto mixed = syntheticGroup(ids, QStringLiteral("Mixed mask sandwich"), 32, 45.0, -8.0);
    mixed->append(syntheticShape(ids, 33, -27.0, 15.0));
    mixed->append(maskedPair(ids, 34, 2.0, -7.0));
    mixed->append(syntheticShape(ids, 36, 31.0, -13.0));
    front.append(std::move(mixed));
    front.append(maskedPair(ids, 37, 92.0, 25.0));
    front.append(syntheticShape(ids, 39, 126.0, -37.0));
}

void populateTopMaskTransitions(scene::Group &top, int &ids)
{
    top.append(syntheticShape(ids, 41, -70.0, -22.0));
    top.append(maskedPair(ids, 42, 0.0, 0.0));
    top.append(syntheticShape(ids, 44, 74.0, 27.0, true));
}

void buildAlternatingMaskTransitions(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    scene::Group *top = syntheticSection(project, 2);
    if (!front || !top) {
        throw std::runtime_error("scaffold is missing livery sections");
    }
    populateFrontMaskTransitions(*front, ids);
    populateTopMaskTransitions(*top, ids);
}

void buildFrontMaskTransitions(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    if (!front) {
        throw std::runtime_error("scaffold is missing the front section");
    }
    populateFrontMaskTransitions(*front, ids);
}

void buildTopMaskTransitions(Project &project)
{
    int ids = 0;
    scene::Group *top = syntheticSection(project, 2);
    if (!top) {
        throw std::runtime_error("scaffold is missing the top section");
    }
    populateTopMaskTransitions(*top, ids);
}

void buildSingleMaskGroup(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    if (!front) {
        throw std::runtime_error("scaffold is missing the front section");
    }
    front->append(syntheticShape(ids, 50, -95.0, -34.0));
    auto masks = syntheticGroup(ids, QStringLiteral("Nine-mask boundary group"), 51, -8.0, 4.0);
    for (int i = 0; i < 9; ++i) {
        masks->append(syntheticShape(ids, 60 + i,
                                     (i % 3) * 18.0 - 18.0,
                                     (i / 3) * 16.0 - 16.0, true));
    }
    front->append(std::move(masks));
    front->append(syntheticShape(ids, 70, 96.0, 37.0));
}

void buildNestedMixedMaskGroup(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    if (!front) {
        throw std::runtime_error("scaffold is missing the front section");
    }
    front->append(syntheticShape(ids, 80, -104.0, -27.0));
    auto mixed = syntheticGroup(ids, QStringLiteral("Nested mixed group"), 81, 5.0, -6.0);
    mixed->append(syntheticShape(ids, 82, -38.0, 16.0));
    mixed->append(maskedPair(ids, 83, 0.0, -3.0));
    mixed->append(syntheticShape(ids, 85, 41.0, -14.0));
    front->append(std::move(mixed));
    front->append(syntheticShape(ids, 86, 109.0, 32.0));
}

void buildDirectTrailingMasks(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    if (!front) {
        throw std::runtime_error("scaffold is missing the front section");
    }
    for (int i = 0; i < 13; ++i) {
        front->append(syntheticShape(ids, 90 + i,
                                     (i % 5) * 38.0 - 76.0,
                                     (i / 5) * 34.0 - 36.0,
                                     i % 3 == 1 || i == 12));
    }
}

void buildGroupShapeAlternation(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    if (!front) {
        throw std::runtime_error("scaffold is missing the front section");
    }
    front->append(maskedPair(ids, 110, -103.0, -18.0));
    front->append(syntheticShape(ids, 112, -68.0, 31.0));
    front->append(maskedPair(ids, 113, -24.0, -9.0));
    front->append(syntheticShape(ids, 115, 12.0, 38.0));
    auto mixed = syntheticGroup(ids, QStringLiteral("Alternating mixed group"), 116, 58.0, -11.0);
    mixed->append(syntheticShape(ids, 117, -24.0, 15.0));
    mixed->append(maskedPair(ids, 118, 3.0, -6.0));
    mixed->append(syntheticShape(ids, 120, 29.0, 12.0));
    front->append(std::move(mixed));
    front->append(syntheticShape(ids, 121, 112.0, 34.0, true));
}

void buildTwoMaskSections(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    scene::Group *top = syntheticSection(project, 2);
    if (!front || !top) {
        throw std::runtime_error("scaffold is missing livery sections");
    }
    front->append(syntheticShape(ids, 130, -92.0, -29.0));
    front->append(maskedPair(ids, 131, -5.0, 7.0));
    front->append(syntheticShape(ids, 133, 91.0, 33.0));
    top->append(syntheticShape(ids, 134, -73.0, -25.0));
    top->append(maskedPair(ids, 135, 2.0, -4.0));
    top->append(syntheticShape(ids, 137, 76.0, 29.0, true));
}

void buildBitmapBoundaries(Project &project)
{
    int ids = 0;
    scene::Group *top = syntheticSection(project, 2);
    if (!top) {
        throw std::runtime_error("scaffold is missing the top section");
    }
    const int groupPositions[] = {0, 7, 8, 15, 16};
    for (int i = 0; i < 17; ++i) {
        const bool isGroup = std::find(std::begin(groupPositions), std::end(groupPositions), i)
            != std::end(groupPositions);
        if (!isGroup) {
            top->append(syntheticShape(ids, 100 + i,
                                       (i % 6) * 31.0 - 80.0,
                                       (i / 6) * 29.0 - 45.0,
                                       i == 9));
            continue;
        }
        if (i == 8) {
            auto large = syntheticGroup(ids, QStringLiteral("Sixty-five direct children"), 120, 0.0, 5.0);
            const int nestedPositions[] = {7, 8, 15, 16, 31, 32, 63, 64};
            for (int child = 0; child < 65; ++child) {
                const bool nested = std::find(std::begin(nestedPositions), std::end(nestedPositions), child)
                    != std::end(nestedPositions);
                if (nested) {
                    large->append(maskedPair(ids, 200 + child,
                                             (child % 13) * 12.0 - 72.0,
                                             (child / 13) * 14.0 - 28.0));
                } else {
                    large->append(syntheticShape(ids, 200 + child,
                                                 (child % 13) * 12.0 - 72.0,
                                                 (child / 13) * 14.0 - 28.0,
                                                 child == 33));
                }
            }
            top->append(std::move(large));
        } else {
            top->append(maskedPair(ids, 300 + i, i * 8.0 - 65.0, i * -3.0));
        }
    }
}

void buildSeventeenChildSection(Project &project)
{
    int ids = 0;
    scene::Group *top = syntheticSection(project, 2);
    if (!top) {
        throw std::runtime_error("scaffold is missing the top section");
    }
    const int groupPositions[] = {0, 7, 8, 15, 16};
    for (int i = 0; i < 17; ++i) {
        const bool isGroup = std::find(std::begin(groupPositions), std::end(groupPositions), i)
            != std::end(groupPositions);
        if (isGroup) {
            top->append(maskedPair(ids, 400 + i, i * 9.0 - 72.0, i * -3.0));
        } else {
            top->append(syntheticShape(ids, 420 + i,
                                       (i % 6) * 31.0 - 80.0,
                                       (i / 6) * 29.0 - 45.0,
                                       i == 9));
        }
    }
}

void buildSeventeenDirectShapes(Project &project)
{
    int ids = 0;
    scene::Group *top = syntheticSection(project, 2);
    if (!top) {
        throw std::runtime_error("scaffold is missing the top section");
    }
    for (int i = 0; i < 17; ++i) {
        top->append(syntheticShape(ids, 460 + i,
                                   (i % 6) * 34.0 - 85.0,
                                   (i / 6) * 31.0 - 42.0,
                                   i == 5 || i == 16));
    }
}

void appendSixtyFiveChildGroup(Project &project, int mode)
{
    int ids = 0;
    scene::Group *top = syntheticSection(project, 2);
    if (!top) {
        throw std::runtime_error("scaffold is missing the top section");
    }
    top->append(syntheticShape(ids, 500 + mode, -112.0, -47.0));
    auto large = syntheticGroup(ids, QStringLiteral("Sixty-five child mode %1").arg(mode),
                                510 + mode, 0.0, 3.0);
    const int nestedPositions[] = {7, 8, 15, 16, 31, 32, 63, 64};
    for (int child = 0; child < 65; ++child) {
        const bool nested = std::find(std::begin(nestedPositions), std::end(nestedPositions), child)
            != std::end(nestedPositions);
        if (mode == 2 && nested) {
            large->append(maskedPair(ids, 600 + child,
                                     (child % 13) * 12.0 - 72.0,
                                     (child / 13) * 14.0 - 28.0));
        } else {
            large->append(syntheticShape(ids, 600 + child,
                                         (child % 13) * 12.0 - 72.0,
                                         (child / 13) * 14.0 - 28.0,
                                         mode == 1 || (mode == 0 && child == 64)));
        }
    }
    top->append(std::move(large));
    top->append(syntheticShape(ids, 700 + mode, 114.0, 49.0, mode == 2));
}

void buildSixtyFiveDirectShapes(Project &project)
{
    appendSixtyFiveChildGroup(project, 0);
}

void buildSixtyFiveMaskGroup(Project &project)
{
    appendSixtyFiveChildGroup(project, 1);
}

void buildSixtyFiveNestedBitmap(Project &project)
{
    appendSixtyFiveChildGroup(project, 2);
}

void buildDeepNestedMasks(Project &project)
{
    int ids = 0;
    scene::Group *left = syntheticSection(project, 3);
    scene::Group *right = syntheticSection(project, 4);
    if (!left || !right) {
        throw std::runtime_error("scaffold is missing side sections");
    }
    left->append(syntheticShape(ids, 400, -115.0, 38.0));
    auto mixed = syntheticGroup(ids, QStringLiteral("Mixed outer branch"), 401, -25.0, -10.0);
    mixed->append(syntheticShape(ids, 402, -22.0, 24.0));
    mixed->append(maskedPair(ids, 403, 17.0, -8.0));
    left->append(std::move(mixed));
    left->append(deepMaskedBranch(ids, 420, 7));

    right->append(syntheticShape(ids, 500, -76.0, -28.0));
    right->append(maskedPair(ids, 501, 0.0, 3.0));
    right->append(syntheticShape(ids, 503, 78.0, 31.0, true));
}

void populateMixedSection(scene::Group &section, int &ids, int slot)
{
    if (slot % 2 == 0 && slot != 10) {
        appendFlatCluster(section, ids, 600 + slot * 20, 7 + slot,
                          slot * 9.0 - 45.0, slot * -4.0);
        return;
    }
    section.append(syntheticShape(ids, 700 + slot, -75.0, -31.0));
    section.append(maskedPair(ids, 710 + slot, -20.0, 14.0));
    auto mixed = syntheticGroup(ids, QStringLiteral("Section mixed %1").arg(slot),
                                720 + slot, 38.0, -9.0);
    mixed->append(syntheticShape(ids, 730 + slot, -16.0, 12.0));
    mixed->append(maskedPair(ids, 740 + slot, 13.0, -5.0));
    mixed->append(syntheticShape(ids, 750 + slot, 29.0, 17.0));
    section.append(std::move(mixed));
    if (slot == 10) {
        section.append(deepMaskedBranch(ids, 780, 4));
    } else {
        section.append(syntheticShape(ids, 760 + slot, 88.0, 35.0, slot % 3 == 0));
    }
}

void buildMixedSectionRange(Project &project, int first, int last)
{
    int ids = 0;
    for (int slot = first; slot <= last; ++slot) {
        scene::Group *section = syntheticSection(project, slot);
        if (!section) {
            throw std::runtime_error("scaffold is missing livery sections");
        }
        populateMixedSection(*section, ids, slot);
    }
}

void buildAllSectionsMixed(Project &project)
{
    buildMixedSectionRange(project, 0, 10);
}

void buildMixedSectionsZeroToTwo(Project &project)
{
    buildMixedSectionRange(project, 0, 2);
}

void buildMixedSectionsZeroToFive(Project &project)
{
    buildMixedSectionRange(project, 0, 5);
}

void buildMixedSectionsZeroToNine(Project &project)
{
    buildMixedSectionRange(project, 0, 9);
}

void buildMixedSectionsThreeToFive(Project &project)
{
    buildMixedSectionRange(project, 3, 5);
}

void buildMixedSectionsSixToTen(Project &project)
{
    buildMixedSectionRange(project, 6, 10);
}

void moveSectionTerminalShapeIntoGroup(Project &project, int slot)
{
    scene::Group *section = syntheticSection(project, slot);
    if (section == nullptr || section->children.size() < 2
        || section->children.back()->kind() != scene::LayerKind::Shape
        || section->children[section->children.size() - 2]->kind() != scene::LayerKind::Group) {
        throw std::runtime_error("synthetic section does not have a group-shape terminal pair");
    }
    std::unique_ptr<scene::Layer> terminal = section->takeAt(
        static_cast<int>(section->children.size()) - 1);
    auto &group = static_cast<scene::Group &>(*section->children.back());
    group.append(std::move(terminal));
}

void buildMixedSectionsFiveToSix(Project &project)
{
    buildMixedSectionRange(project, 5, 6);
}

void buildMixedSectionsFiveToSixGroupTerminal(Project &project)
{
    buildMixedSectionRange(project, 5, 6);
    moveSectionTerminalShapeIntoGroup(project, 5);
}

void buildAllSectionsGroupTerminal(Project &project)
{
    buildAllSectionsMixed(project);
    moveSectionTerminalShapeIntoGroup(project, 5);
}

void appendLargeFlatSection(scene::Group &section, int &ids, int seed, int count)
{
    auto group = syntheticGroup(ids, QStringLiteral("Large flat %1").arg(count), seed);
    for (int i = 0; i < count; ++i) {
        group->append(syntheticShape(ids, seed + i,
                                     (i % 47) * 5.5 - 126.5,
                                     (i / 47) * 4.25 - 92.0));
    }
    section.append(std::move(group));
}

void buildLargeStructuredCounts(Project &project)
{
    int ids = 0;
    scene::Group *front = syntheticSection(project, 0);
    scene::Group *back = syntheticSection(project, 1);
    scene::Group *left = syntheticSection(project, 3);
    scene::Group *rightWindow = syntheticSection(project, 10);
    if (!front || !back || !left || !rightWindow) {
        throw std::runtime_error("scaffold is missing livery sections");
    }
    appendLargeFlatSection(*front, ids, 900, 257);
    appendLargeFlatSection(*back, ids, 1200, 513);

    left->append(syntheticShape(ids, 1800, -135.0, -72.0));
    auto structured = syntheticGroup(ids, QStringLiteral("One-thousand-twenty-five children"), 1801);
    for (int i = 0; i < 1025; ++i) {
        structured->append(syntheticShape(ids, 1900 + i,
                                          (i % 53) * 4.8 - 125.0,
                                          (i / 53) * 7.2 - 68.0,
                                          i == 512));
    }
    left->append(std::move(structured));
    left->append(syntheticShape(ids, 3000, 137.0, 75.0));

    appendLargeFlatSection(*rightWindow, ids, 3200, 2049);
}

struct SyntheticCase {
    const char *folder;
    const char *purpose;
    void (*build)(Project &);
};

void generateSyntheticLiveries(const QString &sourceFolder, const QString &outputRoot)
{
    const SyntheticCase cases[] = {
        {"Synthetic_01_SparseFlatSlots", "Flat transformed groups in non-contiguous sections, including the final slot.", buildSparseFlatSlots},
        {"Synthetic_02_AlternatingMaskTransitions", "Alternating shapes, mask groups, mixed groups, and terminal masks.", buildAlternatingMaskTransitions},
        {"Synthetic_02A_FrontTransitionsOnly", "The complete Front section from test 2 without Top artwork.", buildFrontMaskTransitions},
        {"Synthetic_02B_TopTransitionsOnly", "The complete Top section from test 2 without Front artwork.", buildTopMaskTransitions},
        {"Synthetic_02C_SingleMaskGroup", "A nine-child mask group between ordinary direct shapes.", buildSingleMaskGroup},
        {"Synthetic_02D_NestedMixedGroup", "A nested mask pair between ordinary children of a mixed group.", buildNestedMixedMaskGroup},
        {"Synthetic_02E_DirectTrailingMasks", "Thirteen direct shapes with alternating trailing mask state and a terminal mask.", buildDirectTrailingMasks},
        {"Synthetic_02F_GroupShapeAlternation", "Repeated mask-group and shape transitions with nested mixed artwork.", buildGroupShapeAlternation},
        {"Synthetic_02G_TwoMaskSections", "Reduced structured mask groups in two non-adjacent sections.", buildTwoMaskSections},
        {"Synthetic_03_BitmapBoundaries", "Structured child bitmaps crossing 8, 16, and 64 direct-child boundaries.", buildBitmapBoundaries},
        {"Synthetic_03A_SeventeenChildSection", "A 17-child section with group bits on both bitmap boundaries.", buildSeventeenChildSection},
        {"Synthetic_03B_SeventeenDirectShapes", "A 17-child section bitmap containing only direct shapes.", buildSeventeenDirectShapes},
        {"Synthetic_03C_SixtyFiveDirectShapes", "A normal group with 65 direct shapes and terminal mask state.", buildSixtyFiveDirectShapes},
        {"Synthetic_03D_SixtyFiveMaskGroup", "A mask group containing 65 direct shapes.", buildSixtyFiveMaskGroup},
        {"Synthetic_03E_SixtyFiveNestedBitmap", "A 65-child mixed group with nested group bits at byte boundaries.", buildSixtyFiveNestedBitmap},
        {"Synthetic_04_DeepNestedMasks", "Seven-deep terminal mask nesting followed by artwork in a later section.", buildDeepNestedMasks},
        {"Synthetic_05_AllSectionsMixed", "Every section populated with alternating flat and structured artwork.", buildAllSectionsMixed},
        {"Synthetic_05A_Sections0To2", "The first three sections from test 5.", buildMixedSectionsZeroToTwo},
        {"Synthetic_05B_Sections0To5", "The first six sections from test 5.", buildMixedSectionsZeroToFive},
        {"Synthetic_05C_Sections0To9", "The first ten sections from test 5.", buildMixedSectionsZeroToNine},
        {"Synthetic_05D_Sections3To5", "The middle three sections from test 5.", buildMixedSectionsThreeToFive},
        {"Synthetic_05E_Sections6To10", "The final five sections from test 5.", buildMixedSectionsSixToTen},
        {"Synthetic_05F_Sections5To6", "The exact Spoiler-to-FrontWindow boundary from test 5.", buildMixedSectionsFiveToSix},
        {"Synthetic_05G_Sections5To6_GroupTerminal", "The same boundary with the final Spoiler shape inside its preceding group.", buildMixedSectionsFiveToSixGroupTerminal},
        {"Synthetic_05H_AllSections_GroupTerminal", "Test 5 with the final Spoiler shape inside its preceding group.", buildAllSectionsGroupTerminal},
        {"Synthetic_06_LargeStructuredCounts", "Large flat counts plus a structured group with 1025 direct children.", buildLargeStructuredCounts},
    };

    if (!QDir().mkpath(outputRoot)) {
        throw std::runtime_error("could not create synthetic output root");
    }
    QString manifest = QStringLiteral(
        "Synthetic livery encoder cases\n"
        "Scaffold: %1\n\n"
        "Try in numeric order. Stop at the first folder the game does not recognize or decodes incorrectly.\n\n")
        .arg(QDir::toNativeSeparators(QFileInfo(sourceFolder).absoluteFilePath()));

    for (const SyntheticCase &testCase : cases) {
        Project project = syntheticBase(sourceFolder);
        project.name = QString::fromLatin1(testCase.folder);
        testCase.build(project);
        QStringList requestedCounts;
        for (int slot = 0; slot < 11; ++slot) {
            requestedCounts.push_back(QString::number(sectionLeaves(project, slot).size()));
        }
        const QString outputFolder = QDir(outputRoot).filePath(QString::fromLatin1(testCase.folder));
        exportCLivery(project, outputFolder);

        const Project decoded = importCLivery(outputFolder);
        QStringList decodedCounts;
        for (int slot = 0; slot < 11; ++slot) {
            decodedCounts.push_back(QString::number(sectionLeaves(decoded, slot).size()));
        }
        manifest += QStringLiteral("%1\n  %2\n  requested=[%3]\n  decoded=[%4] leaves=%5 masks=%6%7\n\n")
            .arg(QString::fromLatin1(testCase.folder), QString::fromLatin1(testCase.purpose))
            .arg(requestedCounts.join(QLatin1Char(',')))
            .arg(decodedCounts.join(QLatin1Char(',')))
            .arg(sceneShapeCount(decoded))
            .arg(sceneMaskCount(decoded))
            .arg(requestedCounts == decodedCounts
                     ? QString()
                     : QStringLiteral(" ROUNDTRIP_COUNT_MISMATCH"));
        std::printf("WROTE %-42s leaves=%d masks=%d requested=%s decoded=%s%s\n",
                    testCase.folder, sceneShapeCount(decoded), sceneMaskCount(decoded),
                    requestedCounts.join(QLatin1Char(',')).toLatin1().constData(),
                    decodedCounts.join(QLatin1Char(',')).toLatin1().constData(),
                    requestedCounts == decodedCounts ? "" : " MISMATCH");
    }

    QFile manifestFile(QDir(outputRoot).filePath(QStringLiteral("TEST_ORDER.txt")));
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || manifestFile.write(manifest.toUtf8()) != manifest.toUtf8().size()) {
        throw std::runtime_error("could not write synthetic test manifest");
    }
}

void printShape(const char *tag, const ShapeView *s)
{
    if (!s || s->shape == nullptr) {
        std::printf("      %s (none)\n", tag);
        return;
    }
    std::printf("      %s @%d id=%u x=%.2f y=%.2f sx=%.3f sy=%.3f rot=%.1f col=%s\n",
                tag, s->shape->absOffset, s->shape->shapeId, s->world.x, s->world.y,
                s->world.scaleX, s->world.scaleY, s->world.rotation,
                colStr(s->shape->color).toLatin1().constData());
}

void dumpRaw(const VinylGroup &node, int depth, int maxDepth, int maxChildren)
{
    QString ind(depth * 2, QLatin1Char(' '));
    if (node.nodeType == QStringLiteral("root")) {
        std::printf("%sROOT @%d px=%.2f py=%.2f sx=%.3f sy=%.3f rot=%.1f children=%d\n",
                    ind.toLatin1().constData(), node.absPos, node.px, node.py,
                    node.sx, node.sy, node.rot, static_cast<int>(node.items.size()));
    } else {
        std::printf("%sGROUP @%d px=%.2f py=%.2f sx=%.3f sy=%.3f rot=%.1f exp=%d flags=%d "
                    "src=%s ptm=%s inl=%s eff=%s\n",
                    ind.toLatin1().constData(), node.absPos, node.px, node.py, node.sx,
                    node.sy, node.rot, node.expectedChildren ? *node.expectedChildren : -1,
                    node.flags, node.source.toLatin1().constData(),
                    node.pendingTransformMarker.toHex().constData(),
                    node.inlineTransformMarker.toHex().constData(),
                    node.effectiveTransformMarker.toHex().constData());
    }
    if (depth >= maxDepth) return;
    int shown = 0;
    for (const VinylItem &item : node.items) {
        if (shown++ >= maxChildren) {
            std::printf("%s  ... (%d more)\n", ind.toLatin1().constData(),
                        static_cast<int>(node.items.size()) - maxChildren);
            break;
        }
        if (item.isShape()) {
            const VinylShape &s = std::get<VinylShape>(item.value);
            std::printf("%s  shape @%d id=%u px=%.2f py=%.2f sx=%.3f sy=%.3f rot=%.1f mk=%s\n",
                        ind.toLatin1().constData(), s.absPos, s.shapeId, s.posX, s.posY,
                        s.scaleX, s.scaleY, s.rotation, s.marker.toHex().constData());
        } else {
            dumpRaw(*std::get<VinylGroupPtr>(item.value), depth + 1, maxDepth, maxChildren);
        }
    }
}

void dumpRawGroups(const VinylGroup &node, int depth)
{
    for (const VinylItem &item : node.items) {
        if (item.isShape()) {
            continue;
        }
        const VinylGroup &group = *std::get<VinylGroupPtr>(item.value);
        std::printf("%*sGROUP @%d px=%.6f py=%.6f sx=%.6f sy=%.6f rot=%.6f exp=%d flags=%d ptm=%s inl=%s\n",
                    depth * 2, "", group.absPos, group.px, group.py, group.sx, group.sy,
                    group.rot, group.expectedChildren ? *group.expectedChildren : -1,
                    group.flags, group.pendingTransformMarker.toHex().constData(),
                    group.inlineTransformMarker.toHex().constData());
        dumpRawGroups(group, depth + 1);
    }
}

void collectTransformGroups(const VinylGroup &node, QVector<const VinylGroup *> &out)
{
    for (const VinylItem &item : node.items) {
        if (!item.isShape()) {
            const VinylGroup &g = *std::get<VinylGroupPtr>(item.value);
            const bool ident = std::abs(g.px) < 1e-6 && std::abs(g.py) < 1e-6
                && std::abs(g.sx - 1.0) < 1e-6 && std::abs(g.rot) < 1e-6;
            const bool hasMarker = !g.pendingTransformMarker.isEmpty()
                || !g.inlineTransformMarker.isEmpty();
            if (!ident || hasMarker) {
                out.push_back(&g);
            }
            collectTransformGroups(g, out);
        }
    }
}

int rawTerminalGroupDepth(const VinylGroup &group)
{
    if (group.items.isEmpty() || group.items.back().isShape()) {
        return 1;
    }
    return 1 + rawTerminalGroupDepth(*std::get<VinylGroupPtr>(group.items.back().value));
}

void printRawGroupShapeTransitions(const VinylGroup &group, const QByteArray &body, int slot)
{
    for (int i = 0; i < group.items.size(); ++i) {
        const VinylItem &item = group.items[i];
        if (!item.isShape()) {
            printRawGroupShapeTransitions(*std::get<VinylGroupPtr>(item.value), body, slot);
        }
        if (i == 0 || !item.isShape() || group.items[i - 1].isShape()) {
            continue;
        }
        const VinylGroup &previous = *std::get<VinylGroupPtr>(group.items[i - 1].value);
        const VinylShape &shape = std::get<VinylShape>(item.value);
        const int prefixStart = std::max(0, shape.absPos - 8);
        std::printf("slot=%d parent=%d previous=%d depth=%d flags=%d shape=%d marker=%s before=%s\n",
                    slot, group.absPos, previous.absPos, rawTerminalGroupDepth(previous),
                    previous.flags, shape.absPos, shape.marker.toHex().constData(),
                    body.mid(prefixStart, shape.absPos - prefixStart).toHex(' ').constData());
    }
}

const VinylShape *terminalRawShape(const VinylGroup &group, int &groupDepth)
{
    if (group.items.isEmpty()) {
        return nullptr;
    }
    const VinylItem *item = &group.items.back();
    while (!item->isShape()) {
        const VinylGroup &child = *std::get<VinylGroupPtr>(item->value);
        ++groupDepth;
        if (child.items.isEmpty()) {
            return nullptr;
        }
        item = &child.items.back();
    }
    return &std::get<VinylShape>(item->value);
}

void printSectionTerminals(const QVector<LiverySection> &sections, const QByteArray &body)
{
    for (int i = 0; i < sections.size(); ++i) {
        const LiverySection &section = sections[i];
        int depth = 0;
        const VinylShape *shape = terminalRawShape(section.subtree, depth);
        if (shape == nullptr) {
            continue;
        }
        const int recordEnd = shape->absPos + shape->marker.size() + 30;
        const int next = i + 1 < sections.size() ? sections[i + 1].absPos : body.size();
        std::printf("slot=%d depth=%d shape=%d marker=%s recordEnd=%d next=%d gap=%d bytes=%s\n",
                    section.slot, depth, shape->absPos, shape->marker.toHex().constData(),
                    recordEnd, next, next - recordEnd,
                    body.mid(recordEnd, std::max(0, next - recordEnd)).toHex(' ').constData());
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    bool verbose = args.removeAll(QStringLiteral("--verbose")) > 0;
    bool treeMode = args.removeAll(QStringLiteral("--tree")) > 0;
    bool rawMode = args.removeAll(QStringLiteral("--raw")) > 0;
    bool rawGroupsMode = args.removeAll(QStringLiteral("--raw-groups")) > 0;
    bool transitionsMode = args.removeAll(QStringLiteral("--transitions")) > 0;
    bool sectionTerminalsMode = args.removeAll(QStringLiteral("--section-terminals")) > 0;
    bool markersMode = args.removeAll(QStringLiteral("--markers")) > 0;
    bool rangesMode = args.removeAll(QStringLiteral("--ranges")) > 0;
    bool paintMode = args.removeAll(QStringLiteral("--paint")) > 0;
    bool roundtripMode = args.removeAll(QStringLiteral("--roundtrip")) > 0;
    bool exportReencodedMode = args.removeAll(QStringLiteral("--export-reencoded")) > 0;
    bool allMode = args.removeAll(QStringLiteral("--all")) > 0;
    bool nudgeFirstShape = args.removeAll(QStringLiteral("--nudge-first-shape")) > 0;
    bool rotateFirstGroup = args.removeAll(QStringLiteral("--rotate-first-group")) > 0;
    const bool withoutSource = args.removeAll(QStringLiteral("--without-source")) > 0;
    const bool generateSyntheticMode = args.removeAll(QStringLiteral("--generate-synthetic")) > 0;
    const bool bodyRangeMode = args.removeAll(QStringLiteral("--body-range")) > 0;
    const auto takeOption = [&args](const QString &name) {
        const int index = args.indexOf(name);
        if (index < 0 || index + 1 >= args.size()) {
            return QString();
        }
        const QString value = args.takeAt(index + 1);
        args.removeAt(index);
        return value;
    };
    const QString targetCarOption = takeOption(QStringLiteral("--target-car"));
    const QString projectNameOption = takeOption(QStringLiteral("--project-name"));
    const QString creatorOption = takeOption(QStringLiteral("--creator"));

    if (generateSyntheticMode) {
        if (args.size() < 3) {
            std::fprintf(stderr, "usage: fh6_livery_compare --generate-synthetic <sourceFolder> <outputRoot>\n");
            return 2;
        }
        try {
            generateSyntheticLiveries(args[1], args[2]);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "synthetic generation failed: %s\n", e.what());
            return 1;
        }
        return 0;
    }

    if (bodyRangeMode) {
        if (args.size() < 4) {
            std::fprintf(stderr, "usage: fh6_livery_compare --body-range <liveryFolder> <start> <length>\n");
            return 2;
        }
        bool startOk = false;
        bool lengthOk = false;
        const int start = args[2].toInt(&startOk, 0);
        const int length = args[3].toInt(&lengthOk, 0);
        if (!startOk || !lengthOk || start < 0 || length < 0) {
            std::fprintf(stderr, "invalid body range\n");
            return 2;
        }
        const LiveryPayload payload = readLiveryPayload(args[1]);
        const int end = std::min(start + length, static_cast<int>(payload.body.size()));
        for (int pos = start; pos < end; pos += 16) {
            const QByteArray row = payload.body.mid(pos, std::min(16, end - pos));
            std::printf("%08x  %s\n", pos, row.toHex(' ').constData());
        }
        return 0;
    }

    const bool fmStatsMode = args.removeAll(QStringLiteral("--fm-stats")) > 0;
    if (fmStatsMode) {
        if (args.size() < 2) {
            std::fprintf(stderr, "usage: fh6_livery_compare --fm-stats <assetFolder>\n");
            return 2;
        }
        try {
            QFile dataFile(QDir(args[1]).filePath(QStringLiteral("data")));
            if (!dataFile.open(QIODevice::ReadOnly)) {
                throw std::runtime_error("could not open FM data file");
            }
            const QByteArray fileData = dataFile.readAll();
            if (isRawGyvl(fileData)) {
                const LayerData layerData = getLayerData(fileData);
                const VinylGroup legacyTree = buildTree(layerData.data, fileData);
                const VinylGroup tree = decodeFM2023RawGroup(fileData);
                RawStats legacyStats;
                RawStats stats;
                collectRawStats(legacyTree, legacyStats);
                collectRawStats(tree, stats);
                const Project project = importFM2023Asset(args[1]);
                std::printf("FM %s bytes=%d layerStart=%d rootExpected=%d legacyShapes=%d shapes=%d importedShapes=%d masks=%d/%d skipped=%d groups=%d incomplete=%d\n",
                            QFileInfo(args[1]).fileName().toLatin1().constData(),
                            static_cast<int>(fileData.size()), layerData.start,
                            tree.expectedChildren ? *tree.expectedChildren : -1,
                            legacyStats.shapes, stats.shapes, sceneShapeCount(project),
                            stats.masks, sceneMaskCount(project), stats.skipped,
                            stats.groups, stats.incompleteGroups);
                return 0;
            }
            const FM2023LiveryPayload payload = readFM2023LiveryPayload(args[1]);
            const QVector<LiverySection> sections = decodeFM2023LiverySections(payload);
            const Project project = importFM2023Asset(args[1]);
            std::printf("FM %s body=%d carId=%d vectors=%d masks=%d counts=",
                        QFileInfo(args[1]).fileName().toLatin1().constData(),
                        static_cast<int>(payload.gyvlBody.size()), payload.carId,
                        sceneShapeCount(project), sceneMaskCount(project));
            for (int count : payload.sectionCounts) {
                std::printf("%d,", count);
            }
            std::printf("\n");
            for (const LiverySection &section : sections) {
                RawStats stats;
                collectRawStats(section.subtree, stats);
                const int target = section.slot < payload.sectionCounts.size()
                    ? payload.sectionCounts[section.slot] : 0;
                std::printf("  %-18s target=%-5d shapes=%-5d logos=%-4d skipped=%-5d groups=%-4d incomplete=%-3d abs=%d\n",
                            section.name.toLatin1().constData(), target, stats.shapes,
                            stats.logos, stats.skipped, stats.groups, stats.incompleteGroups, section.absPos);
                printIncompleteGroups(section.subtree);
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "FM stats failed: %s\n", e.what());
            return 1;
        }
        return 0;
    }

    if (paintMode) {
        if (args.size() < 2) {
            std::fprintf(stderr, "usage: fh6_livery_compare --paint <liveryFolder>\n");
            return 2;
        }
        const LiveryPayload payload = readLiveryPayload(args[1]);
        std::printf("paint materials=%lld raw=%lld gyvl=%d body=%lld\n",
                    static_cast<long long>(payload.paint.materials.size()),
                    static_cast<long long>(payload.raw.size()), payload.gyvlOffset,
                    static_cast<long long>(payload.body.size()));
        for (const LiveryPaintMaterial &material : payload.paint.materials) {
            if (!material.primary.enabled && !material.secondary.enabled
                && material.manufacturerSelector == 0xffffffffu) {
                continue;
            }
            std::printf(
                "  %016llX primary=%d:%02X%02X%02X%02X secondary=%d:%02X%02X%02X%02X selector=%u finish=%u\n",
                static_cast<unsigned long long>(material.materialHash),
                material.primary.enabled ? 1 : 0,
                material.primary.bgra[0], material.primary.bgra[1],
                material.primary.bgra[2], material.primary.bgra[3],
                material.secondary.enabled ? 1 : 0,
                material.secondary.bgra[0], material.secondary.bgra[1],
                material.secondary.bgra[2], material.secondary.bgra[3],
                material.manufacturerSelector, material.finish);
        }
        return 0;
    }

    if (exportReencodedMode) {
        if (args.size() < 3) {
            std::fprintf(
                stderr,
                "usage: fh6_livery_compare --export-reencoded [--without-source] <liveryFolder> <outputFolder>\n");
            return 2;
        }
        const QString folder = args[1];
        const QString outputFolder = args[2];
        Project project;
        try {
            project = importCLivery(folder);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "import failed: %s\n", e.what());
            return 1;
        }
        if (!targetCarOption.isEmpty()) {
            bool ok = false;
            const int carId = targetCarOption.toInt(&ok);
            if (!ok || carId <= 0) {
                std::fprintf(stderr, "invalid target car\n");
                return 2;
            }
            project.carId = carId;
        }
        if (!projectNameOption.isEmpty()) {
            project.name = projectNameOption;
        }
        if (!creatorOption.isEmpty()) {
            if (!project.headerMetadata) {
                project.headerMetadata = defaultDraftHeader(
                    project.name, creatorOption, static_cast<quint32>(project.carId));
            }
            project.headerMetadata->creatorName = creatorOption;
        }
        if (withoutSource) {
            project.sourceFolder.clear();
            project.sourceHeader.clear();
            project.liverySource.clear();
        }
        if (nudgeFirstShape && (!project.root || !nudgeFirstBuiltInShape(*project.root))) {
            std::fprintf(stderr, "no built-in shape found to nudge\n");
            return 1;
        }
        if (rotateFirstGroup && (!project.root || !rotateFirstArtworkGroup(*project.root))) {
            std::fprintf(stderr, "no artwork group found to rotate\n");
            return 1;
        }

        try {
            exportCLivery(project, outputFolder);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "write failed: %s\n", e.what());
            return 1;
        }
        std::printf("WROTE %s carId=%d\n",
                    outputFolder.toLatin1().constData(),
                    project.carId);
        return 0;
    }

    if (roundtripMode) {
        if (args.size() < 2) {
            std::fprintf(stderr, "usage: fh6_livery_compare --roundtrip <liveryFolder>\n");
            return 2;
        }
        const QString folder = args[1];
        const LiveryPayload lp = readLiveryPayload(folder);
        const QByteArray origChunk = lp.raw.mid(lp.gyvlOffset, 0x15 + lp.body.size());
        Project project;
        try {
            project = importCLivery(folder);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "import failed: %s\n", e.what());
            return 1;
        }
        QByteArray re;
        try {
            re = buildLiveryGyvl(project);
        } catch (const std::exception &e) {
            std::printf("THROW %-28s buildLiveryGyvl: %s\n",
                        QFileInfo(folder).fileName().toLatin1().constData(), e.what());
            return 1;
        }
        if (re == origChunk) {
            std::printf("PASS  %-28s gyvl %d bytes\n",
                        QFileInfo(folder).fileName().toLatin1().constData(),
                        static_cast<int>(origChunk.size()));
            return 0;
        }
        const int m = std::min(re.size(), origChunk.size());
        int diff = m;
        for (int i = 0; i < m; ++i) {
            if (re[i] != origChunk[i]) { diff = i; break; }
        }
        std::printf("FAIL  %-28s orig=%d re=%d first-diff@%d\n",
                    QFileInfo(folder).fileName().toLatin1().constData(),
                    static_cast<int>(origChunk.size()), static_cast<int>(re.size()), diff);
        return 1;
    }

    if (transitionsMode) {
        if (args.size() < 2) {
            std::fprintf(stderr, "usage: fh6_livery_compare --transitions <liveryFolder>\n");
            return 2;
        }
        const LiveryPayload payload = readLiveryPayload(args[1]);
        const QVector<LiverySection> sections = buildLiverySections(payload.body, payload.sectionCounts);
        for (const LiverySection &section : sections) {
            printRawGroupShapeTransitions(section.subtree, payload.body, section.slot);
        }
        return 0;
    }


    if (sectionTerminalsMode) {
        if (args.size() < 2) {
            std::fprintf(stderr, "usage: fh6_livery_compare --section-terminals <liveryFolder>\n");
            return 2;
        }
        const LiveryPayload payload = readLiveryPayload(args[1]);
        const QVector<LiverySection> sections = buildLiverySections(payload.body, payload.sectionCounts);
        printSectionTerminals(sections, payload.body);
        return 0;
    }

    if (args.size() < 3) {
        std::fprintf(stderr,
            "usage: fh6_livery_compare <liveryFolder> <sectionsFolder> [section] [--verbose]\n");
        return 2;
    }
    const QString liveryFolder = args[1];
    const QString sectionsFolder = args[2];
    const QString onlySection = args.size() >= 4 ? args[3].toLower() : QString();

    if (markersMode) {
        const LiveryPayload lp = readLiveryPayload(liveryFolder);
        const QVector<LiverySection> sections = buildLiverySections(lp.body, lp.sectionCounts);
        for (const SectionMap &sm : kSections) {
            if (!onlySection.isEmpty()
                && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) continue;
            const QString truthDir = QDir(sectionsFolder).filePath(QString::fromLatin1(sm.folder));
            if (!QFileInfo(QDir(truthDir).filePath(QStringLiteral("C_group"))).exists()) continue;
            const VinylGroup *sub = nullptr;
            for (const LiverySection &sec : sections)
                if (sec.slot == sm.slot) { sub = &sec.subtree; break; }
            if (!sub) continue;
            const QByteArray payload = readCGroupPayload(truthDir);
            const LayerData ld = getLayerData(payload);
            const VinylGroup root = buildTree(ld.data, payload);
            QVector<const VinylGroup *> lg, tg;
            collectTransformGroups(*sub, lg);
            collectTransformGroups(root, tg);
            std::printf("\n===== %s: livery %d vs truth %d transform-groups %s\n",
                        sm.folder, static_cast<int>(lg.size()), static_cast<int>(tg.size()),
                        lg.size() == tg.size() ? "(aligned)" : "(MISALIGNED)");
            const int n = std::min(lg.size(), tg.size());
            for (int i = 0; i < n; ++i) {
                const bool sameXform = std::abs(lg[i]->px - tg[i]->px) < 0.5
                    && std::abs(lg[i]->sx - tg[i]->sx) < 0.01 && std::abs(lg[i]->rot - tg[i]->rot) < 0.5;
                std::printf("  %s  L[ptm=%-10s inl=%-6s]  T[ptm=%-10s inl=%-6s]  exp=%d\n",
                            sameXform ? " " : "X",
                            lg[i]->pendingTransformMarker.toHex().constData(),
                            lg[i]->inlineTransformMarker.toHex().constData(),
                            tg[i]->pendingTransformMarker.toHex().constData(),
                            tg[i]->inlineTransformMarker.toHex().constData(),
                            tg[i]->expectedChildren ? *tg[i]->expectedChildren : -1);
                if (verbose) {
                    std::printf("       L@%d p=(%.6f,%.6f) s=(%.6f,%.6f) r=%.6f flags=%d exp=%d ctrl=%s bits=%s"
                                "  T@%d p=(%.6f,%.6f) s=(%.6f,%.6f) r=%.6f flags=%d ctrl=%s bits=%s\n",
                                lg[i]->absPos, lg[i]->px, lg[i]->py, lg[i]->sx, lg[i]->sy,
                                lg[i]->rot, lg[i]->flags,
                                lg[i]->expectedChildren ? *lg[i]->expectedChildren : -1,
                                lg[i]->headerControlBytes.toHex().constData(),
                                lg[i]->childTypeBitmap.toHex().constData(),
                                tg[i]->absPos, tg[i]->px, tg[i]->py, tg[i]->sx, tg[i]->sy,
                                tg[i]->rot, tg[i]->flags,
                                tg[i]->headerControlBytes.toHex().constData(),
                                tg[i]->childTypeBitmap.toHex().constData());
                }
            }
        }
        return 0;
    }

    if (rawMode || rawGroupsMode) {
        const LiveryPayload lp = readLiveryPayload(liveryFolder);
        const QVector<LiverySection> sections = buildLiverySections(lp.body, lp.sectionCounts);
        for (const SectionMap &sm : kSections) {
            if (!onlySection.isEmpty() && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) continue;
            std::printf("\n===== %s (slot %d): LIVERY subtree =====\n", sm.folder, sm.slot);
            for (const LiverySection &sec : sections) {
                if (sec.slot == sm.slot) {
                    std::printf("section absPos=%d populated=%d\n", sec.absPos, sec.populated);
                    if (rawGroupsMode) {
                        dumpRawGroups(sec.subtree, 0);
                    } else {
                        dumpRaw(sec.subtree, 0, allMode ? 100000 : (verbose ? 40 : 4),
                                allMode ? 100000 : (verbose ? 500 : 14));
                    }
                    break;
                }
            }
            const QString truthDir = QDir(sectionsFolder).filePath(QString::fromLatin1(sm.folder));
            if (QFileInfo(QDir(truthDir).filePath(QStringLiteral("C_group"))).exists()) {
                const QByteArray payload = readCGroupPayload(truthDir);
                const LayerData ld = getLayerData(payload);
                const VinylGroup root = buildTree(ld.data, payload);
                std::printf("----- %s: TRUTH root tree (layerStart=%d) -----\n", sm.folder, ld.start);
                dumpRaw(root, 0, allMode ? 100000 : (verbose ? 40 : 4),
                        allMode ? 100000 : (verbose ? 500 : 14));
            }
        }
        return 0;
    }

    Project livery;
    try {
        livery = importCLivery(liveryFolder);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "failed to import C_livery: %s\n", e.what());
        return 1;
    }

    std::printf("Livery %s: %d leaf shapes, %d sections, carId=%d\n",
                livery.name.toLatin1().constData(),
                sceneShapeCount(livery),
                rootEntryCount(livery),
                livery.carId);

    if (rangesMode) {
        for (const SectionMap &sm : kSections) {
            if (!onlySection.isEmpty() && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) continue;
            if (!livery.root) {
                continue;
            }
            for (const auto &child : livery.root->children) {
                if (child->kind() != scene::LayerKind::Group) {
                    continue;
                }
                const auto &section = static_cast<const scene::Group &>(*child);
                if (!section.isLiverySection || section.liverySectionSlot != sm.slot) {
                    continue;
                }
                QStringList order;
                collectVisualLeafIds(section, order);
                QHash<QString, int> visualPositions;
                for (int i = 0; i < order.size(); ++i) {
                    visualPositions.insert(order[i], i + 1);
                }
                QStringList sourceOrder;
                collectSourceLeafIds(section, sourceOrder);
                QHash<QString, int> sourcePositions;
                for (int i = 0; i < sourceOrder.size(); ++i) {
                    sourcePositions.insert(sourceOrder[i], i + 1);
                }
                std::printf("\n===== %s: LIVERY ranges =====\n", sm.folder);
                dumpGroupRanges(section, visualPositions, sourcePositions, 0);
                break;
            }
        }
        return 0;
    }

    if (treeMode) {
        for (const SectionMap &sm : kSections) {
            if (!onlySection.isEmpty() && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) continue;
            std::printf("\n===== %s: LIVERY tree =====\n", sm.folder);
            if (livery.root) {
                for (const auto &child : livery.root->children) {
                    if (child->kind() == scene::LayerKind::Group) {
                        const auto &section = static_cast<const scene::Group &>(*child);
                        if (section.isLiverySection && section.liverySectionSlot == sm.slot) {
                            dumpTree(section, 0, allMode ? 100000 : (verbose ? 6 : 3),
                                     allMode ? 100000 : (verbose ? 40 : 12));
                            break;
                        }
                    }
                }
            }

            const QString truthDir = QDir(sectionsFolder).filePath(QString::fromLatin1(sm.folder));
            if (QFileInfo(QDir(truthDir).filePath(QStringLiteral("C_group"))).exists()) {
                Project truth = importCGroupNested(truthDir);
                std::printf("----- %s: TRUTH tree (root children=%d) -----\n",
                            sm.folder, rootEntryCount(truth));
                if (truth.root) {
                    for (const auto &child : truth.root->children) {
                        dumpTree(*child, 1, allMode ? 100000 : (verbose ? 6 : 3),
                                 allMode ? 100000 : (verbose ? 40 : 12));
                    }
                }
            }
        }
        return 0;
    }

    int grandTruth = 0, grandMatch = 0;
    for (const SectionMap &sm : kSections) {
        if (!onlySection.isEmpty() && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const QString truthDir = QDir(sectionsFolder).filePath(QString::fromLatin1(sm.folder));
        if (!QFileInfo(QDir(truthDir).filePath(QStringLiteral("C_group"))).exists()) {
            continue;
        }
        Project truth;
        try {
            truth = importCGroupNested(truthDir);
        } catch (const std::exception &e) {
            std::printf("  [%s] ground-truth decode failed: %s\n", sm.folder, e.what());
            continue;
        }
        const QVector<ShapeView> got = sectionLeaves(livery, sm.slot);
        const QVector<ShapeView> want = truthLeaves(truth);
        const int n = std::min(got.size(), want.size());
        int matched = 0;
        int firstMismatch = -1;
        for (int i = 0; i < n; ++i) {
            if (shapesMatch(got[i], want[i])) {
                ++matched;
            } else if (firstMismatch < 0) {
                firstMismatch = i;
            }
        }
        grandTruth += want.size();
        grandMatch += matched;
        const double pct = want.isEmpty() ? 100.0 : 100.0 * matched / want.size();
        std::printf("  [%-8s] livery=%d truth=%d matched=%d (%.1f%%)%s\n",
                    sm.folder, static_cast<int>(got.size()), static_cast<int>(want.size()),
                    matched, pct,
                    got.size() != want.size() ? "  <count mismatch>" : "");

        const int dumpCount = verbose ? 8 : 3;
        int dumped = 0;
        for (int i = 0; i < n && dumped < dumpCount; ++i) {
            if (!shapesMatch(got[i], want[i])) {
                std::printf("    mismatch @%d:\n", i);
                printShape("livery", &got[i]);
                printShape("truth ", &want[i]);
                ++dumped;
            }
        }
        (void)firstMismatch;
    }

    const double gpct = grandTruth == 0 ? 100.0 : 100.0 * grandMatch / grandTruth;
    std::printf("TOTAL matched %d / %d (%.1f%%)\n", grandMatch, grandTruth, gpct);
    return 0;
}
