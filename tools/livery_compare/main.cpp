#include "fh6_core.h"
#include "fm_codec.h"
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
        std::printf("%sshape id=%u x=%.2f y=%.2f sx=%.3f sy=%.3f rot=%.1f\n",
                    ind.toLatin1().constData(), s.shapeId, world.x, world.y,
                    world.scaleX, world.scaleY, world.rotation);
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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    bool verbose = args.removeAll(QStringLiteral("--verbose")) > 0;
    bool treeMode = args.removeAll(QStringLiteral("--tree")) > 0;
    bool rawMode = args.removeAll(QStringLiteral("--raw")) > 0;
    bool markersMode = args.removeAll(QStringLiteral("--markers")) > 0;
    bool rangesMode = args.removeAll(QStringLiteral("--ranges")) > 0;
    bool paintMode = args.removeAll(QStringLiteral("--paint")) > 0;
    bool roundtripMode = args.removeAll(QStringLiteral("--roundtrip")) > 0;
    bool exportReencodedMode = args.removeAll(QStringLiteral("--export-reencoded")) > 0;
    bool allMode = args.removeAll(QStringLiteral("--all")) > 0;
    bool nudgeFirstShape = args.removeAll(QStringLiteral("--nudge-first-shape")) > 0;
    bool rotateFirstGroup = args.removeAll(QStringLiteral("--rotate-first-group")) > 0;

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
            std::fprintf(stderr, "usage: fh6_livery_compare --export-reencoded <liveryFolder> <outputFolder>\n");
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
            }
        }
        return 0;
    }

    if (rawMode) {
        const LiveryPayload lp = readLiveryPayload(liveryFolder);
        const QVector<LiverySection> sections = buildLiverySections(lp.body, lp.sectionCounts);
        for (const SectionMap &sm : kSections) {
            if (!onlySection.isEmpty() && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) continue;
            std::printf("\n===== %s (slot %d): LIVERY subtree =====\n", sm.folder, sm.slot);
            for (const LiverySection &sec : sections) {
                if (sec.slot == sm.slot) {
                    std::printf("section absPos=%d populated=%d\n", sec.absPos, sec.populated);
                    dumpRaw(sec.subtree, 0, allMode ? 100000 : (verbose ? 40 : 4),
                            allMode ? 100000 : (verbose ? 500 : 14));
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
