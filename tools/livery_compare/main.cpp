// livery_compare — ground-truth diff harness for the C_livery decoder.
//
// Decodes a full C_livery (importCLivery) and, per section, diffs its composed
// leaf shapes against the in-game per-section C_group exports decoded with the
// conventional nested decoder (importCGroupNested), which is the ground truth
// for group decoding. Prints per-section match statistics and the first few
// mismatches so the transform-composition gaps (open item D) can be measured.
//
// Usage:
//   fh6_livery_compare <liveryFolder> <sectionsFolder> [section] [--verbose]
//
//   <liveryFolder>   folder containing a C_livery (e.g. LiveryResearch/liveryFull)
//   <sectionsFolder> folder of per-section exports (e.g. LiveryResearch/sections)
//                    with subfolders front/back/top/left/right/spoiler each
//                    holding a C_group.
//   [section]        optional: limit to one section name.
//   --verbose        dump more mismatches / the section group tree.

#include "fh6_core.h"
#include "vinyl_decoder.h"
#include "livery_codec.h"
#include "cgroup_codec.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <cstdio>

using namespace fh6;

namespace {

// The 6 exportable sections and their storage-order slot indices.
struct SectionMap {
    const char *folder;  // subfolder name under <sectionsFolder>
    int slot;            // buildLiverySections storage-order slot
};
// Covers the folder-naming of both ground-truth sets (sections/ and sections1/).
// Folders whose C_group is absent are skipped, so extra entries are harmless.
const SectionMap kSections[] = {
    {"front", 0}, {"back", 1}, {"top", 2},
    {"left", 3}, {"right", 4}, {"spoiler", 5},
    {"frontWindow", 6},   // FrontWindshield
    {"backWindow", 7},    // BackWindshield
    {"topWindow", 8},     // TopWindow
    {"leftWindow", 9}, {"rightWindow", 10},
};

// Collect the leaf shapes (composed ShapeLayers) under a group subtree in
// depth-first child order, matching importGroup's traversal.
void collectLeaves(const Project &project,
                   const QHash<QString, int> &shapeById,
                   const QHash<QString, int> &groupById,
                   const QString &groupId,
                   QVector<const ShapeLayer *> &out)
{
    auto git = groupById.constFind(groupId);
    if (git == groupById.constEnd()) {
        return;
    }
    const LayerGroup &group = project.groups[*git];
    for (const QString &childId : group.childIds) {
        auto sit = shapeById.constFind(childId);
        if (sit != shapeById.constEnd()) {
            out.push_back(&project.layers[*sit]);
        } else {
            collectLeaves(project, shapeById, groupById, childId, out);
        }
    }
}

QVector<const ShapeLayer *> sectionLeaves(const Project &livery, int slot)
{
    QHash<QString, int> shapeById;
    for (int i = 0; i < livery.layers.size(); ++i) {
        shapeById.insert(livery.layers[i].id, i);
    }
    QHash<QString, int> groupById;
    for (int i = 0; i < livery.groups.size(); ++i) {
        groupById.insert(livery.groups[i].id, i);
    }
    QString sectionId;
    for (const LayerGroup &g : livery.groups) {
        if (g.isLiverySection && g.liverySectionSlot == slot) {
            sectionId = g.id;
            break;
        }
    }
    QVector<const ShapeLayer *> out;
    if (!sectionId.isEmpty()) {
        collectLeaves(livery, shapeById, groupById, sectionId, out);
    }
    return out;
}

QVector<const ShapeLayer *> truthLeaves(const Project &truth)
{
    QVector<const ShapeLayer *> out;
    out.reserve(truth.layers.size());
    for (const ShapeLayer &l : truth.layers) {
        out.push_back(&l);
    }
    return out;
}

bool shapesMatch(const ShapeLayer &a, const ShapeLayer &b)
{
    const double posTol = 0.5;   // composed px
    const double scaleTol = 0.01; // relative-ish
    const double rotTol = 0.5;    // degrees
    if (a.shapeId != b.shapeId) return false;
    if (std::abs(a.x - b.x) > posTol) return false;
    if (std::abs(a.y - b.y) > posTol) return false;
    if (std::abs(std::abs(a.scaleX) - std::abs(b.scaleX)) > scaleTol + 0.001 * std::abs(b.scaleX)) return false;
    if (std::abs(std::abs(a.scaleY) - std::abs(b.scaleY)) > scaleTol + 0.001 * std::abs(b.scaleY)) return false;
    double dr = std::fmod(std::abs(a.rotation - b.rotation), 360.0);
    if (dr > 180.0) dr = 360.0 - dr;
    if (dr > rotTol) return false;
    if (a.color != b.color) return false;
    return true;
}

QString colStr(const std::array<quint8, 4> &c)
{
    return QStringLiteral("%1,%2,%3,%4").arg(c[0]).arg(c[1]).arg(c[2]).arg(c[3]);
}

// Dump a project's group tree rooted at the given group id (or the whole root
// when groupId is empty), depth-limited, showing local group transforms and
// child composition. Used to compare livery-section structure vs. ground truth.
void dumpTree(const Project &project,
              const QHash<QString, int> &shapeById,
              const QHash<QString, int> &groupById,
              const QString &groupId, int depth, int maxDepth, int maxChildren)
{
    QString ind(depth * 2, QLatin1Char(' '));
    auto git = groupById.constFind(groupId);
    if (git == groupById.constEnd()) {
        return;
    }
    const LayerGroup &g = project.groups[*git];
    std::printf("%sGROUP %s '%s' flags=%d children=%d ptm=%s inl=%s\n",
                ind.toLatin1().constData(), g.id.toLatin1().constData(),
                g.name.toLatin1().constData(), g.flags,
                static_cast<int>(g.childIds.size()),
                g.pendingTransformMarker.toHex().constData(),
                g.inlineTransformMarker.toHex().constData());
    if (depth >= maxDepth) {
        return;
    }
    int shown = 0;
    for (const QString &childId : g.childIds) {
        if (shown++ >= maxChildren) {
            std::printf("%s  ... (%d more)\n", ind.toLatin1().constData(),
                        static_cast<int>(g.childIds.size()) - maxChildren);
            break;
        }
        auto sit = shapeById.constFind(childId);
        if (sit != shapeById.constEnd()) {
            const ShapeLayer &s = project.layers[*sit];
            std::printf("%s  shape id=%u x=%.2f y=%.2f sx=%.3f sy=%.3f rot=%.1f\n",
                        ind.toLatin1().constData(), s.shapeId, s.x, s.y,
                        s.scaleX, s.scaleY, s.rotation);
        } else {
            dumpTree(project, shapeById, groupById, childId, depth + 1, maxDepth, maxChildren);
        }
    }
}

void buildMaps(const Project &p, QHash<QString, int> &shapeById, QHash<QString, int> &groupById)
{
    for (int i = 0; i < p.layers.size(); ++i) shapeById.insert(p.layers[i].id, i);
    for (int i = 0; i < p.groups.size(); ++i) groupById.insert(p.groups[i].id, i);
}

void printShape(const char *tag, const ShapeLayer *s)
{
    if (!s) {
        std::printf("      %s (none)\n", tag);
        return;
    }
    std::printf("      %s @%d id=%u x=%.2f y=%.2f sx=%.3f sy=%.3f rot=%.1f col=%s\n",
                tag, s->absOffset, s->shapeId, s->x, s->y, s->scaleX, s->scaleY, s->rotation,
                colStr(s->color).toLatin1().constData());
}

// Recursively dump a decoder VinylGroup tree with local transforms, absolute
// byte offsets and marker bytes -- the definitive view of where the livery and
// ground-truth group structures diverge.
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

// Collect, in pre-order, every group that carries a real transform (non-identity
// or a marker), skipping synthetic/identity wrappers. On a section that decodes
// 100%, the livery and truth lists correspond 1:1, so zipping them reveals the
// exact livery<->standalone marker mapping.
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
                    dumpRaw(sec.subtree, 0, verbose ? 40 : 4, verbose ? 500 : 14);
                    break;
                }
            }
            const QString truthDir = QDir(sectionsFolder).filePath(QString::fromLatin1(sm.folder));
            if (QFileInfo(QDir(truthDir).filePath(QStringLiteral("C_group"))).exists()) {
                const QByteArray payload = readCGroupPayload(truthDir);
                const LayerData ld = getLayerData(payload);
                const VinylGroup root = buildTree(ld.data, payload);
                std::printf("----- %s: TRUTH root tree (layerStart=%d) -----\n", sm.folder, ld.start);
                dumpRaw(root, 0, verbose ? 40 : 4, verbose ? 500 : 14);
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

    std::printf("Livery %s: %d leaf shapes, %d sections\n",
                livery.name.toLatin1().constData(),
                static_cast<int>(livery.layers.size()),
                static_cast<int>(livery.rootChildIds.size()));

    if (treeMode) {
        QHash<QString, int> lShape, lGroup;
        buildMaps(livery, lShape, lGroup);
        for (const SectionMap &sm : kSections) {
            if (!onlySection.isEmpty() && QString::fromLatin1(sm.folder).compare(onlySection, Qt::CaseInsensitive) != 0) continue;
            QString sectionId;
            for (const LayerGroup &g : livery.groups) {
                if (g.isLiverySection && g.liverySectionSlot == sm.slot) { sectionId = g.id; break; }
            }
            std::printf("\n===== %s: LIVERY tree =====\n", sm.folder);
            dumpTree(livery, lShape, lGroup, sectionId, 0, verbose ? 6 : 3, verbose ? 40 : 12);

            const QString truthDir = QDir(sectionsFolder).filePath(QString::fromLatin1(sm.folder));
            if (QFileInfo(QDir(truthDir).filePath(QStringLiteral("C_group"))).exists()) {
                Project truth = importCGroupNested(truthDir);
                QHash<QString, int> tShape, tGroup;
                buildMaps(truth, tShape, tGroup);
                std::printf("----- %s: TRUTH tree (root children=%d) -----\n",
                            sm.folder, static_cast<int>(truth.rootChildIds.size()));
                for (const QString &rid : truth.rootChildIds) {
                    auto sit = tShape.constFind(rid);
                    if (sit != tShape.constEnd()) {
                        const ShapeLayer &s = truth.layers[*sit];
                        std::printf("  shape id=%u x=%.2f y=%.2f sx=%.3f sy=%.3f rot=%.1f\n",
                                    s.shapeId, s.x, s.y, s.scaleX, s.scaleY, s.rotation);
                    } else {
                        dumpTree(truth, tShape, tGroup, rid, 1, verbose ? 6 : 3, verbose ? 40 : 12);
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
        const QVector<const ShapeLayer *> got = sectionLeaves(livery, sm.slot);
        const QVector<const ShapeLayer *> want = truthLeaves(truth);
        const int n = std::min(got.size(), want.size());
        int matched = 0;
        int firstMismatch = -1;
        for (int i = 0; i < n; ++i) {
            if (shapesMatch(*got[i], *want[i])) {
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
            if (!shapesMatch(*got[i], *want[i])) {
                std::printf("    mismatch @%d:\n", i);
                printShape("livery", got[i]);
                printShape("truth ", want[i]);
                ++dumped;
            }
        }
        (void)firstMismatch;
    }

    const double gpct = grandTruth == 0 ? 100.0 : 100.0 * grandMatch / grandTruth;
    std::printf("TOTAL matched %d / %d (%.1f%%)\n", grandMatch, grandTruth, gpct);
    return 0;
}
