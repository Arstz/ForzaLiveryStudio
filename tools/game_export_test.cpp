#include "binary_io.h"
#include "cgroup_codec.h"
#include "fm_codec.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "livery_codec.h"
#include "project_codec.h"
#include "vinyl_decoder.h"
#include "vinyl_flattener.h"
#include "wgs_save_reader.h"

#include <QtCore>

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace {

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("read failed: %1").arg(path));
    return file.readAll();
}

std::unique_ptr<fls::scene::Shape> makeShape(int index) {
    auto shape = std::make_unique<fls::scene::Shape>();
    shape->id = QString::number(index);
    shape->setVectorShape(101);
    shape->x = index * 3.0;
    shape->y = index * -2.0;
    shape->scaleX = 0.25;
    shape->scaleY = 0.5;
    shape->rotation = index * 0.25;
    shape->color = {20, 40, 80, 255};
    return shape;
}

fls::Project makeLivery(int sectionCount, bool nested = false) {
    fls::Project project;
    project.isLivery = true;
    project.name = QStringLiteral("Native export");
    project.carId = 1069;
    project.root = std::make_unique<fls::scene::Group>();
    project.headerMetadata = fls::defaultDraftHeader(project.name, QStringLiteral("Exporter"), project.carId);
    for (int slot = 0; slot < sectionCount; ++slot) {
        auto section = std::make_unique<fls::scene::Group>();
        section->isLiverySection = true;
        section->liverySectionSlot = slot;
        section->name = QString::number(slot);
        if (nested) {
            auto group = std::make_unique<fls::scene::Group>();
            group->x = 12.0;
            group->y = -8.0;
            auto first = makeShape(slot * 2);
            first->mask = true;
            group->append(std::move(first));
            group->append(makeShape(slot * 2 + 1));
            section->append(std::move(group));
        } else {
            section->append(makeShape(slot));
        }
        project.root->append(std::move(section));
    }
    return project;
}

int shapeCount(const fls::scene::Layer &layer) {
    if (layer.kind() == fls::scene::LayerKind::Shape) {
        return 1;
    }
    int count = 0;
    if (layer.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
            count += shapeCount(*child);
        }
    }
    return count;
}

void collectShapes(const fls::scene::Layer &layer, QVector<const fls::scene::Shape *> &shapes) {
    if (layer.kind() == fls::scene::LayerKind::Shape) {
        shapes.append(static_cast<const fls::scene::Shape *>(&layer));
    } else if (layer.kind() == fls::scene::LayerKind::Group) {
        for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
            collectShapes(*child, shapes);
        }
    }
}

void compareArtwork(const fls::Project &source, const fls::Project &imported, const QString &name,
                    bool centered = false) {
    QVector<const fls::scene::Shape *> before;
    QVector<const fls::scene::Shape *> after;
    collectShapes(*source.root, before);
    collectShapes(*imported.root, after);
    if (before.size() != after.size()) {
        for (int index = 0; index < std::min(before.size(), after.size()); ++index) {
            const auto expected = before[index]->worldMatrix();
            const auto actual = after[index]->worldMatrix();
            if (before[index]->shapeId != after[index]->shapeId
                || before[index]->color != after[index]->color
                || std::abs(expected.m[0][0] - actual.m[0][0]) > 0.03) {
                qInfo() << "First divergence" << index << before[index]->shapeId << after[index]->shapeId
                    << before[index]->x << before[index]->y << before[index]->rotation
                    << before[index]->scaleX << before[index]->scaleY << before[index]->skew;
                break;
            }
        }
        for (int index = 0; index < before.size(); ++index) {
            const auto *shape = before[index];
            if (std::abs(shape->scaleX) >= 200 || std::abs(shape->scaleY) >= 5000
                || std::abs(shape->x) >= 50000 || std::abs(shape->y) >= 50000
                || std::abs(shape->scaleX) <= 1e-6 || std::abs(shape->scaleY) <= 1e-6) {
                qInfo() << "Shape outside decoder range" << index << shape->shapeId
                    << shape->x << shape->y << shape->scaleX << shape->scaleY;
            }
        }
    }
    require(before.size() == after.size(), QStringLiteral("%1 artwork count %2/%3")
        .arg(name).arg(before.size()).arg(after.size()));
    for (int index = 0; index < before.size(); ++index) {
        require(before[index]->shapeId == after[index]->shapeId
            && before[index]->rasterId == after[index]->rasterId,
            QStringLiteral("%1 shape %2 identifier %3/%4 vs %5/%6")
                .arg(name).arg(index).arg(before[index]->shapeId).arg(before[index]->rasterId)
                .arg(after[index]->shapeId).arg(after[index]->rasterId));
        auto expected = before[index]->worldMatrix();
        auto actual = after[index]->worldMatrix();
        if (centered) {
            for (int row = 0; row < 2; ++row) {
                expected.m[row][2] -= before.front()->worldMatrix().m[row][2];
                actual.m[row][2] -= after.front()->worldMatrix().m[row][2];
            }
        }
        require(before[index]->color == after[index]->color, name + " shape color");
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                require(std::abs(expected.m[row][column] - actual.m[row][column]) < 0.03,
                    QStringLiteral("%1 shape %2 transform %3/%4: %5 vs %6")
                        .arg(name).arg(index).arg(row).arg(column)
                        .arg(expected.m[row][column]).arg(actual.m[row][column]));
            }
        }
    }
}

QByteArray paintBlock(const QByteArray &raw) {
    const int artwork = raw.indexOf(QByteArray("gyvl", 4));
    const int stats = artwork + static_cast<int>(fls::detail::readLeU32(raw, artwork - 4));
    const bool tagged = raw.mid(stats, 4) == QByteArray("yrvl", 4);
    const int paint = stats + (tagged ? 36 : 28);
    const int end = tagged ? raw.indexOf(QByteArray("yrvl", 4), paint) : raw.size();
    return raw.mid(paint, end - paint);
}

void testEditedLivery(const fls::Project &source, const QString &output, fls::ExportGameVersion game) {
    auto edited = source;
    QVector<const fls::scene::Shape *> shapes;
    collectShapes(*edited.root, shapes);
    if (shapes.isEmpty()) {
        return;
    }
    auto *shape = const_cast<fls::scene::Shape *>(shapes.front());
    shape->x += 0.25;
    shape->y -= 1.5;
    shape->rotation += 12.0;
    shape->scaleX *= 1.05;
    shape->skew += 0.03;
    shape->color[0] ^= 8;
    fls::exportGameLivery(edited, output, game);
    const auto imported = game == fls::ExportGameVersion::Motorsport2023
        ? fls::importFM2023Asset(output) : fls::importCLivery(output);
    compareArtwork(edited, imported, source.name + " edited");
}

void testLiveries(const QString &root) {
    const std::array<fls::ExportGameVersion, 9> games = {
        fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Horizon5,
        fls::ExportGameVersion::Motorsport2023, fls::ExportGameVersion::Horizon2,
        fls::ExportGameVersion::Horizon3, fls::ExportGameVersion::Motorsport5,
        fls::ExportGameVersion::Motorsport6, fls::ExportGameVersion::Motorsport7,
        fls::ExportGameVersion::Horizon4};
    const std::array<quint32, 9> versions = {7, 6, 9, 0, 1, 0, 0, 2, 4};
    const std::array<int, 9> envelopes = {51, 50, 66, 49, 50, 45, 49, 50, 50};
    const std::array<bool, 9> supportsMasks = {true, true, true, false, false, false, false, false, true};
    for (int gameIndex = 0; gameIndex < static_cast<int>(games.size()); ++gameIndex) {
        const auto game = games[gameIndex];
        require(fls::exportGameSupportsMasks(game) == supportsMasks[gameIndex], "game mask support");
        const int sections = gameIndex == 0 ? 11 : 6;
        for (int nested = 0; nested < 2; ++nested) {
            auto project = makeLivery(sections, nested != 0);
            auto expected = project;
            if (nested && !fls::exportGameSupportsMasks(game)) {
                for (auto &section : expected.root->children) {
                    auto &group = static_cast<fls::scene::Group &>(
                        *static_cast<fls::scene::Group &>(*section).children.front());
                    group.takeAt(0);
                }
            }
            const int expectedCount = shapeCount(*expected.root);
            const QString folder = QDir(root).filePath(QStringLiteral("livery-%1-%2").arg(gameIndex).arg(nested));
            fls::exportGameLivery(project, folder, game);
            const auto header = fls::parseHeader(readFile(QDir(folder).filePath(QStringLiteral("header"))));
            require(header.formatVersion == versions[gameIndex], "header version");
            require(header.carId == 1069 && header.typeValue == static_cast<quint32>(expectedCount), "header counts/car");
            require(header.creatorName == QStringLiteral("Exporter"), "creator");
            const auto imported = gameIndex == 2 ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
            require(shapeCount(*imported.root) == expectedCount, QStringLiteral("livery shape count %1/%2").arg(gameIndex).arg(nested));
            compareArtwork(expected, imported, QStringLiteral("generated %1/%2").arg(gameIndex).arg(nested));
            require(fls::exportMaskedLayerCount(project) == (nested ? sections : 0), "livery source masks preserved");
            const auto raw = fls::encodeGameLiveryPayload(project, game);
            require(fls::detail::readLeU32(raw, 8) == 0, "unlocked payload");
            require(raw.indexOf(QByteArray("gyvl", 4)) == envelopes[gameIndex], "root envelope");
            if (gameIndex != 0) {
                auto reexport = imported;
                const auto rebuilt = fls::encodeGameLiveryPayload(reexport, game);
                require(paintBlock(raw) == paintBlock(rebuilt), "paint preservation");
            }
        }
    }
}

void testThinShearedArtwork(const QString &root) {
    for (const auto game : {fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Horizon5,
            fls::ExportGameVersion::Motorsport2023, fls::ExportGameVersion::Horizon2,
            fls::ExportGameVersion::Horizon3, fls::ExportGameVersion::Motorsport5,
            fls::ExportGameVersion::Motorsport6, fls::ExportGameVersion::Motorsport7,
            fls::ExportGameVersion::Horizon4}) {
        auto project = makeLivery(6);
        auto &section = static_cast<fls::scene::Group &>(*project.root->children[4]);
        auto &shape = static_cast<fls::scene::Shape &>(*section.children.front());
        shape.rotation = 232.0435489560202;
        shape.scaleX = 0.0032935513482179007;
        shape.scaleY = 3.357689673452735e-7;
        shape.skew = 127335.5570428808;
        const QString folder = QDir(root).filePath(QStringLiteral("thin-shear-%1").arg(static_cast<int>(game)));
        fls::exportGameLivery(project, folder, game);
        const auto imported = game == fls::ExportGameVersion::Motorsport2023
            ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
        compareArtwork(project, imported, "thin sheared artwork");
        QVector<const fls::scene::Shape *> after;
        collectShapes(*imported.root, after);
        const auto expected = shape.worldMatrix();
        const auto actual = after[4]->worldMatrix();
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 3; ++column) {
                require(std::abs(expected.m[row][column] - actual.m[row][column]) < 1e-6,
                    "thin sheared artwork affine basis");
            }
        }
    }
}

void testGroups(const QString &root) {
    fls::Project project;
    project.name = QStringLiteral("Native group");
    project.root = std::make_unique<fls::scene::Group>();
    auto group = std::make_unique<fls::scene::Group>();
    auto first = makeShape(0);
    first->mask = true;
    group->append(std::move(first));
    group->append(makeShape(1));
    project.root->append(std::move(group));
    project.root->append(makeShape(2));
    for (const auto game : {fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023,
            fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
            fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6}) {
        const QString folder = QDir(root).filePath(QStringLiteral("group-%1").arg(static_cast<int>(game)));
        fls::exportGameGroup(project, folder, game);
        const auto imported = game == fls::ExportGameVersion::Motorsport2023
            ? fls::importFM2023Asset(folder) : fls::importCGroupNested(folder);
        auto expected = project;
        if (!fls::exportGameSupportsMasks(game)) {
            static_cast<fls::scene::Group &>(*expected.root->children.front()).takeAt(0);
        }
        const int expectedCount = shapeCount(*expected.root);
        require(shapeCount(*imported.root) == expectedCount, "group shape count");
        compareArtwork(expected, imported, "generated group", true);
        const auto header = fls::parseHeader(readFile(QDir(folder).filePath(QStringLiteral("header"))));
        require(header.typeValue == expectedCount && header.carId == 0, "group header counts");
        require(fls::exportMaskedLayerCount(project) == 1 && shapeCount(*project.root) == 3, "group source masks preserved");
        require(!QFile::exists(QDir(folder).filePath(QStringLiteral("container.1"))), "no WGS descriptor");
    }
}

void testDiscardedMasks(const QString &root) {
    using Game = fls::ExportGameVersion;
    for (const auto game : {Game::Horizon2, Game::Horizon3, Game::Motorsport5, Game::Motorsport6,
            Game::Motorsport7}) {
        for (bool livery : {false, true}) {
            auto project = makeLivery(6, true);
            if (!livery) {
                project.root = std::unique_ptr<fls::scene::Group>(
                    static_cast<fls::scene::Group *>(project.root->takeAt(0).release()));
                project.root->isLiverySection = false;
                project.isLivery = false;
                project.carId = 0;
            }
            auto &section = livery
                ? static_cast<fls::scene::Group &>(*project.root->children.front()) : *project.root;
            auto inherited = std::make_unique<fls::scene::Group>();
            inherited->flags = 64;
            inherited->append(makeShape(40));
            auto descendant = std::make_unique<fls::scene::Group>();
            descendant->append(makeShape(41));
            inherited->append(std::move(descendant));
            section.append(std::move(inherited));
            auto emptyAfterDiscard = std::make_unique<fls::scene::Group>();
            auto mask = makeShape(42);
            mask->mask = true;
            emptyAfterDiscard->append(std::move(mask));
            emptyAfterDiscard->append(std::make_unique<fls::scene::GuideLayer>());
            section.append(std::move(emptyAfterDiscard));
            auto hidden = std::make_unique<fls::scene::Group>();
            hidden->visible = false;
            auto hiddenMask = makeShape(43);
            hiddenMask->mask = true;
            hidden->append(std::move(hiddenMask));
            section.append(std::move(hidden));
            require(fls::exportMaskedLayerCount(project) == (livery ? 9 : 4), "visible inherited mask count");
            const QString folder = QDir(root).filePath(QString("discard-masks-%1-%2").arg(int(game)).arg(livery));
            if (livery) {
                fls::exportGameLivery(project, folder, game);
            } else {
                fls::exportGameGroup(project, folder, game);
            }
            const auto imported = livery ? fls::importCLivery(folder) : fls::importCGroupNested(folder);
            const int expectedCount = livery ? 6 : 1;
            require(shapeCount(*imported.root) == expectedCount, "masked subtrees discarded");
            require(fls::exportMaskedLayerCount(imported) == 0, "export contains no masks");
            const auto header = fls::parseHeader(readFile(QDir(folder).filePath("header")));
            require(header.typeValue == expectedCount, "discarded masks excluded from header count");
            require(fls::exportMaskedLayerCount(project) == (livery ? 9 : 4), "source inherited masks preserved");
            if (livery) {
                for (auto &child : project.root->children) {
                    static_cast<fls::scene::Group &>(*child).flags = 64;
                }
                fls::exportGameLivery(project, folder, game);
                require(shapeCount(*fls::importCLivery(folder).root) == 0, "mask-only livery exports empty sections");
            }
        }
    }
}

void testBoundsAndBlocks(const QString &root) {
    for (const auto game : {fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023,
            fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
            fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6}) {
        const int sections = 6;
        for (int populated = -1; populated < sections; ++populated) {
            auto sparse = makeLivery(sections);
            for (int slot = 0; slot < sections; ++slot) {
                if (slot != populated) {
                    static_cast<fls::scene::Group &>(*sparse.root->children[slot]).children.clear();
                }
            }
            const QString folder = QDir(root).filePath(QStringLiteral("sparse-%1-%2").arg(sections).arg(populated));
            fls::exportGameLivery(sparse, folder, game);
            const auto imported = game == fls::ExportGameVersion::Motorsport2023
                ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
            require(shapeCount(*imported.root) == (populated < 0 ? 0 : 1), "sparse section framing");
        }
    }
    auto unsupported = makeLivery(11);
    const auto stripped = fls::parseInflatedLiveryPayload(
        fls::encodeGameLiveryPayload(unsupported, fls::ExportGameVersion::Horizon5));
    require(stripped.sectionCounts.size() == 6
        && std::accumulate(stripped.sectionCounts.begin(), stripped.sectionCounts.end(), 0) == 6,
        "window artwork discarded and spoiler retained");
    auto painted = makeLivery(6);
    painted.liveryPaint.setDefaultCarColorBgra({20, 40, 80, 255});
    require(paintBlock(fls::encodeGameLiveryPayload(painted, fls::ExportGameVersion::Horizon5))
        == paintBlock(fls::encodeGameLiveryPayload(makeLivery(6), fls::ExportGameVersion::Horizon5)),
        "unmatched paint replaced with native fallback");
    for (const auto game : {fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
            fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6,
            fls::ExportGameVersion::Motorsport7}) {
        auto scaled = makeLivery(6);
        auto &section = static_cast<fls::scene::Group &>(*scaled.root->children[2]);
        static_cast<fls::scene::Shape &>(*section.children.front()).scaleX = 150.0;
        section.append(makeShape(12));
        const QString scaledFolder = QDir(root).filePath(QStringLiteral("scaled-%1").arg(static_cast<int>(game)));
        fls::exportGameLivery(scaled, scaledFolder, game);
        compareArtwork(scaled, fls::importCLivery(scaledFolder), "section scale across siblings");
    }
    auto large = makeLivery(6);
    auto &top = static_cast<fls::scene::Group &>(*large.root->children[2]);
    for (int index = 1; index < 1500; ++index) {
        top.append(makeShape(index));
    }
    const QString folder = QDir(root).filePath(QStringLiteral("blocks"));
    fls::exportGameLivery(large, folder, fls::ExportGameVersion::Motorsport2023);
    const QByteArray data = readFile(QDir(folder).filePath(QStringLiteral("data")));
    QByteArray inflated;
    int blocks = 0;
    for (int offset = 0; offset < data.size(); ++blocks) {
        const int compressed = static_cast<int>(fls::detail::readLeU32(data, offset));
        require(fls::detail::readLeU32(data, offset + 4) <= 32768, "FM block limit");
        inflated.append(fls::inflateFirstContainer(data.mid(offset, compressed + 8)));
        offset += compressed + 8;
    }
    require(blocks > 1 && inflated == fls::encodeGameLiveryPayload(large, fls::ExportGameVersion::Motorsport2023), "block concatenation");
    require(shapeCount(*fls::importFM2023Asset(folder).root) == 1505, "multi-block import");
}

int skippedShapes(const fls::VinylGroup &group) {
    int count = group.skippedChildren;
    for (const auto &item : group.items) {
        if (!item.isShape()) {
            count += skippedShapes(*std::get<fls::VinylGroupPtr>(item.value));
        }
    }
    return count;
}

void testEarlySamples(const QString &sampleRoot, const QString &outputRoot) {
    const std::array<std::pair<QString, fls::ExportGameVersion>, 4> games = {{
        {"PTG Stuzib85 - Forza Horizon 2", fls::ExportGameVersion::Horizon2},
        {"PTG Stuzib85 - Forza Horizon 3", fls::ExportGameVersion::Horizon3},
        {"PTG Stuzib85 - Forza Motorsport 5", fls::ExportGameVersion::Motorsport5},
        {"PTG Stuzib85 - Forza Motorsport 6", fls::ExportGameVersion::Motorsport6}}};
    int headers = 0;
    int roundTrips = 0;
    int locked = 0;
    for (const auto &[name, game] : games) {
        for (const auto &folder : QDir(QDir(sampleRoot).filePath(name)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = folder.absoluteFilePath();
            const QByteArray header = readFile(QDir(path).filePath("header"));
            require(fls::buildHeader(fls::parseHeader(header)) == header,
                name + "/" + folder.fileName() + " header round trip");
            ++headers;
            const bool livery = QFile::exists(QDir(path).filePath("C_livery"));
            const auto raw = fls::inflateContainer(readFile(QDir(path).filePath(livery ? "C_livery" : "C_group")));
            if ((livery && fls::detail::readLeU32(raw, 8) == 1)
                || (!livery && static_cast<quint8>(raw.at(29)) == 33)) {
                ++locked;
                continue;
            }
            qInfo().noquote() << name << folder.fileName();
            const auto project = livery ? fls::importCLivery(path) : fls::importCGroupNested(path);
            require(project.headerMetadata.has_value(), "native metadata imported");
            if (livery) {
                const auto payload = fls::parseInflatedLiveryPayload(raw);
                require(payload.sectionCounts.size() == 6, "six early sections");
                const int expected = std::accumulate(payload.sectionCounts.begin(), payload.sectionCounts.end(), 0);
                int skipped = 0;
                for (const auto &section : fls::decodeLiverySections(payload)) {
                    skipped += skippedShapes(section.subtree);
                }
                if (shapeCount(*project.root) + skipped != expected) {
                    qInfo() << "Declared sections" << payload.sectionCounts;
                    for (const auto &section : project.root->children) {
                        qInfo() << section->name << shapeCount(*section);
                    }
                }
                require(shapeCount(*project.root) + skipped == expected,
                    QStringLiteral("%1/%2 complete native artwork %3/%4")
                        .arg(name).arg(folder.fileName()).arg(shapeCount(*project.root)).arg(expected));
            }
            const QString output = QDir(outputRoot).filePath(QStringLiteral("early-%1-%2").arg(static_cast<int>(game)).arg(folder.fileName()));
            if (livery) {
                fls::exportGameLivery(project, output, game);
            } else {
                fls::exportGameGroup(project, output, game);
            }
            const auto imported = livery ? fls::importCLivery(output) : fls::importCGroupNested(output);
            compareArtwork(project, imported, name + "/" + folder.fileName(), !livery);
            if (livery) {
                require(paintBlock(raw) == paintBlock(imported.liverySource), "native paint preserved");
                testEditedLivery(project, output + "-edited", game);
            }
            ++roundTrips;
        }
    }
    qInfo() << "Early sample headers:" << headers << "round trips:" << roundTrips << "locked:" << locked;
}

void testSamples(const QString &sampleRoot, const QString &outputRoot) {
    int headerCount = 0;
    int exportCount = 0;
    int locked = 0;
    for (const QFileInfo &folder : QDir(QDir(sampleRoot).filePath(QStringLiteral("fh5"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        fls::WgsAsset asset;
        try {
            asset = fls::readWgsContainer(folder.absoluteFilePath());
        } catch (const std::exception &) {
            continue;
        }
        if (!asset.headerPath.isEmpty()) {
            const QByteArray header = readFile(asset.headerPath);
            require(fls::buildHeader(fls::parseHeader(header)) == header, QStringLiteral("FH5 header round trip: %1").arg(folder.fileName()));
            ++headerCount;
        }
        const QByteArray wrapped = readFile(asset.payloadPath);
        const QByteArray raw = fls::inflateContainer(wrapped);
        if ((asset.kind == fls::WgsAssetKind::Livery && fls::detail::readLeU32(raw, 8) == 1)
            || (asset.kind == fls::WgsAssetKind::Group && static_cast<quint8>(raw.at(29)) == 33)) {
            ++locked;
            continue;
        }
        const QByteArray header = asset.headerPath.isEmpty() ? QByteArray() : readFile(asset.headerPath);
        const auto project = asset.kind == fls::WgsAssetKind::Livery
            ? fls::importCLiveryData(wrapped, header, folder.fileName())
            : fls::importCGroupNestedData(wrapped, header, folder.fileName());
        const QString output = QDir(outputRoot).filePath(folder.fileName());
        if (project.isLivery) {
            fls::exportGameLivery(project, output, fls::ExportGameVersion::Horizon5);
        } else {
            fls::exportGameGroup(project, output, fls::ExportGameVersion::Horizon5);
        }
        const auto imported = project.isLivery ? fls::importCLivery(output) : fls::importCGroupNested(output);
        require(shapeCount(*project.root) == shapeCount(*imported.root), QStringLiteral("FH5 sample geometry count: %1").arg(folder.fileName()));
        compareArtwork(project, imported, folder.fileName());
        if (project.isLivery) {
            require(paintBlock(raw) == paintBlock(imported.liverySource), "FH5 native paint preservation");
            testEditedLivery(project, output + QStringLiteral("-edited"), fls::ExportGameVersion::Horizon5);
        }
        ++exportCount;
    }
    for (const QFileInfo &folder : QDir(QDir(sampleRoot).filePath(QStringLiteral("fm23"))).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QByteArray header = readFile(QDir(folder.absoluteFilePath()).filePath(QStringLiteral("header")));
        require(fls::buildHeader(fls::parseHeader(header)) == header, QStringLiteral("FM23 header round trip: %1").arg(folder.fileName()));
        ++headerCount;
        const auto payload = fls::readFM2023LiveryPayload(folder.absoluteFilePath());
        if (fls::detail::readLeU32(payload.raw, 8) == 1) {
            ++locked;
            continue;
        }
        const auto project = fls::importFM2023Asset(folder.absoluteFilePath());
        const QString output = QDir(outputRoot).filePath(folder.fileName());
        fls::exportGameLivery(project, output, fls::ExportGameVersion::Motorsport2023);
        const auto imported = fls::importFM2023Asset(output);
        require(shapeCount(*project.root) == shapeCount(*imported.root), QStringLiteral("FM23 sample geometry count: %1").arg(folder.fileName()));
        compareArtwork(project, imported, folder.fileName());
        require(paintBlock(project.liverySource) == paintBlock(imported.liverySource),
            QStringLiteral("FM23 native paint preservation %1: version %2 sizes %3/%4")
                .arg(folder.fileName()).arg(fls::detail::readLeU32(project.liverySource, 45))
                .arg(paintBlock(project.liverySource).size()).arg(paintBlock(imported.liverySource).size()));
        testEditedLivery(project, output + QStringLiteral("-edited"), fls::ExportGameVersion::Motorsport2023);
        ++exportCount;
    }
    qInfo() << "Sample headers:" << headerCount << "export/import:" << exportCount << "locked skipped:" << locked;
}

fls::Project saveReload(const fls::Project &project, const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "project save");
    const auto bytes = fls::encodeProjectDocument(project);
    require(file.write(bytes) == bytes.size(), "project write");
    file.close();
    return fls::decodeProjectDocument(readFile(path));
}

QByteArray normalizedHeader(const QByteArray &bytes, bool discardTrailing = false) {
    auto header = fls::parseHeader(bytes);
    header.name.clear();
    header.description.clear();
    header.published = false;
    header.creatorName.clear();
    header.creatorTag.fill('\0');
    header.year = 0;
    header.month = 0;
    header.day = 0;
    header.fieldBlock.replace(0, 12, QByteArray(12, '\0'));
    header.guid.fill('\0');
    if (discardTrailing) {
        header.trailing.clear();
    }
    return fls::buildHeader(header);
}

void equalBytes(const QByteArray &expected, const QByteArray &actual, const QString &label) {
    if (expected == actual) {
        return;
    }
    int offset = 0;
    while (offset < std::min(expected.size(), actual.size()) && expected[offset] == actual[offset]) {
        ++offset;
    }
    require(false, QStringLiteral("%1 differs at %2, sizes %3/%4, expected %5 actual %6")
        .arg(label).arg(offset).arg(expected.size()).arg(actual.size())
        .arg(QString::fromLatin1(expected.mid(offset, 24).toHex()))
        .arg(QString::fromLatin1(actual.mid(offset, 24).toHex())));
}

QByteArray normalizedLivery(QByteArray bytes, bool fallback) {
    bytes.replace(33, 8, QByteArray(8, '\0'));
    if (!fallback) {
        return bytes;
    }
    const auto version = fls::detail::readLeU32(bytes, 29);
    int artwork = bytes.indexOf(QByteArray("gyvl", 4));
    if (version >= 5 && version <= 7) {
        const int entryOffset = version == 5 ? 41 : 42;
        const int count = fls::detail::readLeU32(bytes, entryOffset);
        require(entryOffset + 8 + count * 7 == artwork, "native entry table bounds");
        bytes.remove(entryOffset + 4, count * 7);
        bytes.replace(entryOffset, 4, QByteArray(4, '\0'));
        artwork = bytes.indexOf(QByteArray("gyvl", 4));
    }
    const int stats = artwork + fls::detail::readLeU32(bytes, artwork - 4);
    const int paint = stats + (version == 2 ? 28 : 36);
    const int end = version == 2 ? bytes.size() : bytes.indexOf(QByteArray("yrvl", 4), paint);
    require(end > paint && (end - paint - 3) % 40 == 0, "native paint bounds");
    for (int offset = paint + 1; offset < end - 2; offset += 40) {
        require(bytes.mid(offset, 2) == QByteArray("\x01\x01", 2), "native paint framing");
        bytes.replace(offset + 2, 10, QByteArray(10, '\0'));
        bytes.replace(offset + 27, 8, QByteArray(8, '\0'));
    }
    return bytes;
}

void testNativeReferences(const QString &referenceRoot, const QString &outputRoot) {
    using Game = fls::ExportGameVersion;
    const std::array<std::pair<QString, Game>, 7> games = {{
        {"1)FM5", Game::Motorsport5}, {"2)FH2", Game::Horizon2},
        {"3)FM6", Game::Motorsport6}, {"4)FH3", Game::Horizon3},
        {"5)FM7", Game::Motorsport7}, {"6)FH4", Game::Horizon4},
        {"7)FH5", Game::Horizon5}}};
    const std::array<quint32, 7> versions = {0, 0, 0, 1, 2, 4, 6};
    std::array<std::array<fls::Project, 2>, 7> projects;
    std::array<std::array<QByteArray, 2>, 7> payloads;
    std::array<std::array<QByteArray, 2>, 7> headers;
    for (int index = 0; index < games.size(); ++index) {
        std::array<bool, 2> found{};
        const auto &[name, game] = games[index];
        for (const auto &folder : QDir(QDir(referenceRoot).filePath(name)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = folder.absoluteFilePath();
            const bool livery = QFile::exists(QDir(path).filePath("C_livery"));
            const int kind = livery ? 1 : 0;
            require(!found[kind], name + " duplicate reference");
            found[kind] = true;
            const QString label = name + (livery ? " livery" : " group");
            const auto headerBytes = readFile(QDir(path).filePath("header"));
            const auto header = fls::parseHeader(headerBytes);
            equalBytes(headerBytes, fls::buildHeader(header), label + " header read/write");
            require(header.formatVersion == versions[index] && header.typeValue == (livery ? 5 : 2)
                && header.hasCarId == livery && header.carId == (livery ? 1668 : 0), label + " metadata");
            auto imported = livery ? fls::importCLivery(path) : fls::importCGroupNested(path);
            require(shapeCount(*imported.root) == (livery ? 5 : 2), label + " complete import");
            QVector<const fls::scene::Shape *> shapes;
            collectShapes(*imported.root, shapes);
            for (const auto *shape : shapes) {
                require(shape->shapeId == 101 && !shape->raster && !shape->mask
                    && shape->color == std::array<quint8, 4>{255, 255, 255, 255}, label + " reference shapes");
            }
            const auto raw = fls::inflateContainer(readFile(QDir(path).filePath(livery ? "C_livery" : "C_group")));
            if (livery) {
                const auto payload = fls::parseInflatedLiveryPayload(raw);
                require(payload.sectionCounts == QVector<int>({1, 1, 1, 1, 1, 0}), label + " declared sections");
                const auto sections = fls::decodeLiverySections(payload);
                require(sections.size() >= 6, label + " decoded sections");
                for (const auto &section : sections) {
                    const int expected = section.slot < 5 ? 1 : 0;
                    require(skippedShapes(section.subtree) == 0
                        && (expected == 0 || fls::validateTree(section.subtree).isEmpty()), label + " tree validity");
                    require(fls::flattenGroup(section.subtree).size() == expected, label + " section shapes");
                }
            } else {
                require(fls::validateTree(fls::VinylTreeDecoder{}.decodeGroup(raw)).isEmpty(), label + " tree validity");
            }
            projects[index][kind] = saveReload(imported, QDir(outputRoot).filePath(label + ".3so"));
            compareArtwork(imported, projects[index][kind], label + " saved project");
            headers[index][kind] = headerBytes;
            payloads[index][kind] = raw;
            qInfo().noquote() << "Read native reference:" << label;
        }
        require(found[0] && found[1], name + " missing reference");
    }
    for (int index = 0; index < games.size(); ++index) {
        const auto &[name, game] = games[index];
        for (int kind = 0; kind < 2; ++kind) {
            const bool livery = kind == 1;
            const QString label = name + (livery ? " livery" : " group");
            compareArtwork(projects.back()[kind], projects[index][kind], label + " consistency");
            for (const bool sameTitle : {true, false}) {
                const auto &source = sameTitle ? projects[index][kind] : projects.back()[kind];
                const QString output = QDir(outputRoot).filePath(label + (sameTitle ? " same-title" : " from-FH5"));
                if (livery) {
                    fls::exportGameLivery(source, output, game);
                } else {
                    fls::exportGameGroup(source, output, game);
                }
                const auto actual = fls::inflateContainer(readFile(QDir(output).filePath(livery ? "C_livery" : "C_group")));
                equalBytes(normalizedHeader(headers[index][kind]), normalizedHeader(readFile(QDir(output).filePath("header"))), label + " native header framing");
                equalBytes(livery ? normalizedLivery(payloads[index][kind], !sameTitle) : payloads[index][kind],
                    livery ? normalizedLivery(actual, !sameTitle) : actual, label + (sameTitle ? " same-title bytes" : " converted bytes"));
                const auto reimported = livery ? fls::importCLivery(output) : fls::importCGroupNested(output);
                compareArtwork(source, reimported, label + " second import");
                saveReload(reimported, output + ".3so");
                qInfo().noquote() << "Matched native encoding:" << output;
            }
        }
    }
}

int shapeSection(const fls::scene::Shape &shape) {
    for (auto *parent = shape.parent(); parent != nullptr; parent = parent->parent()) {
        if (parent->kind() == fls::scene::LayerKind::Group) {
            const auto &group = static_cast<const fls::scene::Group &>(*parent);
            if (group.isLiverySection) {
                return group.liverySectionSlot;
            }
        }
    }
    return -1;
}

bool effectiveMask(const fls::scene::Shape &shape) {
    if (shape.mask) {
        return true;
    }
    for (auto *parent = shape.parent(); parent != nullptr; parent = parent->parent()) {
        if (parent->kind() == fls::scene::LayerKind::Group
            && (static_cast<const fls::scene::Group &>(*parent).flags & 64) != 0) {
            return true;
        }
    }
    return false;
}

void compareComplexArtwork(const fls::Project &expected, const fls::Project &actual,
                           const QString &label) {
    compareArtwork(expected, actual, label, !expected.isLivery);
    QVector<const fls::scene::Shape *> before;
    QVector<const fls::scene::Shape *> after;
    collectShapes(*expected.root, before);
    collectShapes(*actual.root, after);
    for (int index = 0; index < before.size(); ++index) {
        require(shapeSection(*before[index]) == shapeSection(*after[index]), label + " section assignment");
        require(before[index]->visible == after[index]->visible, label + " visibility");
        require(effectiveMask(*before[index]) == effectiveMask(*after[index]),
            QStringLiteral("%1 shape %2 mask").arg(label).arg(index));
    }
}

void testHorizonLiveryShapeToGroupMasks(const QString &root) {
    for (const auto game : {fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Horizon4,
            fls::ExportGameVersion::Horizon5, fls::ExportGameVersion::Motorsport2023}) {
        for (const bool nested : {false, true}) {
            for (const bool masked : {false, true}) {
                auto project = makeLivery(6);
                auto &section = static_cast<fls::scene::Group &>(*project.root->children[2]);
                section.children.clear();
                auto container = std::make_unique<fls::scene::Group>();
                auto first = makeShape(0);
                first->color = {0, 0, 0, 255};
                first->mask = masked;
                auto following = std::make_unique<fls::scene::Group>();
                following->append(makeShape(1));
                following->append(makeShape(2));
                auto &parent = nested ? *container : section;
                parent.append(std::move(first));
                parent.append(std::move(following));
                if (nested) {
                    section.append(std::move(container));
                }
                const QString label = QStringLiteral("shape-to-group-mask-%1-%2-%3")
                    .arg(static_cast<int>(game)).arg(nested).arg(masked);
                const QString folder = QDir(root).filePath(label);
                fls::exportGameLivery(project, folder, game);
                const auto imported = game == fls::ExportGameVersion::Motorsport2023
                    ? fls::importFM2023Asset(folder) : fls::importCLivery(folder);
                compareComplexArtwork(project, imported, label);
            }
        }
    }
}

void testHorizonLiveryGroupBoundaries(const QString &root) {
    for (const auto game : {fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Horizon4,
            fls::ExportGameVersion::Horizon5}) {
        for (int depth = 1; depth <= 3; ++depth) {
            auto project = makeLivery(6);
            auto &section = static_cast<fls::scene::Group &>(*project.root->children[1]);
            section.children.clear();
            auto nested = std::make_unique<fls::scene::Group>();
            nested->append(makeShape(0));
            nested->append(makeShape(1));
            for (int level = 1; level < depth; ++level) {
                auto parent = std::make_unique<fls::scene::Group>();
                parent->append(makeShape(0));
                parent->append(std::move(nested));
                nested = std::move(parent);
            }
            auto outer = std::make_unique<fls::scene::Group>();
            outer->append(std::move(nested));
            outer->append(makeShape(2));
            section.append(std::move(outer));
            section.append(makeShape(3));
            const QString label = QStringLiteral("group-boundaries-%1-%2").arg(static_cast<int>(game)).arg(depth);
            const QString folder = QDir(root).filePath(label);
            qInfo().noquote() << "Checking" << label;
            saveReload(project, folder + ".source.3so");
            fls::exportGameLivery(project, folder, game);
            const auto imported = fls::importCLivery(folder);
            compareComplexArtwork(project, saveReload(imported, folder + ".3so"), label);
            const auto sections = fls::decodeLiverySections(fls::readLiveryPayload(folder));
            const auto shapes = fls::flattenGroup(sections[1].subtree);
            require(shapes.size() == depth + 3, label + " Back artwork count");
            for (int index = shapes.size() - 3; index + 1 < shapes.size(); ++index) {
                const int terminalEnd = shapes[index].absPos + shapes[index].marker.size() + 30;
                require(shapes[index + 1].absPos == terminalEnd,
                    label + " shape following a completed group must start at the terminal payload end");
            }
        }
    }
}

void testComplexReferences(const QString &referenceRoot, const QString &outputRoot) {
    using Game = fls::ExportGameVersion;
    const std::array<std::pair<QString, Game>, 7> games = {{
        {"1)FM5", Game::Motorsport5}, {"2)FH2", Game::Horizon2},
        {"3)FM6", Game::Motorsport6}, {"4)FH3", Game::Horizon3},
        {"5)FM7", Game::Motorsport7}, {"6)FH4", Game::Horizon4},
        {"7)FH5", Game::Horizon5}}};
    const QVector<int> sectionCounts{1, 751, 2606, 2772, 2772, 0};
    std::array<std::array<fls::Project, 2>, 7> projects;
    std::array<std::array<QByteArray, 2>, 7> payloads;
    std::array<std::array<QByteArray, 2>, 7> headers;
    QJsonArray results;
    QStringList failures;
    for (int index = 0; index < games.size(); ++index) {
        const auto &[name, game] = games[index];
        std::array<bool, 2> found{};
        for (const auto &folder : QDir(QDir(referenceRoot).filePath(name)).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = folder.absoluteFilePath();
            const bool livery = QFile::exists(QDir(path).filePath("C_livery"));
            const int kind = livery ? 1 : 0;
            require(!found[kind], name + " duplicate complex reference");
            found[kind] = true;
            const QString label = name + (livery ? " livery" : " group");
            const QByteArray raw = fls::inflateContainer(readFile(QDir(path).filePath(livery ? "C_livery" : "C_group")));
            const QByteArray headerBytes = readFile(QDir(path).filePath("header"));
            const auto header = fls::parseHeader(headerBytes);
            equalBytes(headerBytes, fls::buildHeader(header), label + " header round trip");
            require(header.typeValue == (livery ? 8902 : 1806)
                && header.carId == (livery ? 1668 : 0), label + " header count and car");
            if (livery) {
                const auto payload = fls::parseInflatedLiveryPayload(raw);
                require(payload.sectionCounts == sectionCounts, label + " declared sections");
                for (const auto &section : fls::decodeLiverySections(payload)) {
                    const int count = section.slot < sectionCounts.size() ? sectionCounts[section.slot] : 0;
                    require(skippedShapes(section.subtree) == 0
                        && fls::flattenGroup(section.subtree).size() == count
                        && (count == 0 || fls::validateTree(section.subtree).isEmpty()), label + " complete section tree");
                }
            } else {
                require(fls::validateTree(fls::VinylTreeDecoder{}.decodeGroup(raw)).isEmpty(), label + " complete group tree");
            }
            const auto imported = livery ? fls::importCLivery(path) : fls::importCGroupNested(path);
            require(shapeCount(*imported.root) == header.typeValue, label + " imported shapes");
            projects[index][kind] = saveReload(imported, QDir(outputRoot).filePath(label + ".3so"));
            compareComplexArtwork(imported, projects[index][kind], label + " saved project");
            payloads[index][kind] = raw;
            headers[index][kind] = headerBytes;
            qInfo().noquote() << "Read complex reference:" << label;
        }
        require(found[0] && found[1], name + " missing complex reference");
    }
    for (int target = 0; target < games.size(); ++target) {
        for (int kind = 0; kind < 2; ++kind) {
            const bool livery = kind == 1;
            const auto &[name, game] = games[target];
            const QString label = name + (livery ? " livery" : " group");
            compareComplexArtwork(projects.back()[kind], projects[target][kind], label + " reference consistency");
            for (int source = 0; source < games.size(); ++source) {
                const QString stage = games[source].first + " -> " + label;
                QJsonObject result{{"source", games[source].first}, {"target", name}, {"kind", livery ? "livery" : "group"}};
                try {
                    const QString output = QDir(outputRoot).filePath(label + " from-" + games[source].first);
                    const auto &project = projects[source][kind];
                    if (livery) {
                        fls::exportGameLivery(project, output, game);
                    } else {
                        fls::exportGameGroup(project, output, game);
                    }
                    const QByteArray raw = fls::inflateContainer(readFile(QDir(output).filePath(livery ? "C_livery" : "C_group")));
                    const auto imported = livery ? fls::importCLivery(output) : fls::importCGroupNested(output);
                    compareComplexArtwork(project, imported, stage + " second import");
                    compareComplexArtwork(projects[target][kind], imported, stage + " target artwork");
                    compareComplexArtwork(imported, saveReload(imported, output + ".3so"), stage + " second project");
                    equalBytes(normalizedHeader(headers[target][kind], true),
                        normalizedHeader(readFile(QDir(output).filePath("header")), true), stage + " header layout");
                    if (livery) {
                        require(fls::parseInflatedLiveryPayload(raw).sectionCounts == sectionCounts, stage + " exported sections");
                        if (source == target) {
                            equalBytes(paintBlock(payloads[source][kind]), paintBlock(raw), stage + " paint preservation");
                        }
                    }
                    result.insert("passed", true);
                    result.insert("native_payload_bytes_exact", raw == payloads[target][kind]);
                    if (livery) {
                        const auto expected = fls::parseInflatedLiveryPayload(payloads[target][kind]);
                        const auto actual = fls::parseInflatedLiveryPayload(raw);
                        result.insert("native_artwork_bytes_exact", expected.body == actual.body);
                        result.insert("native_artwork_size_matches", expected.body.size() == actual.body.size());
                        if (target < 5) {
                            require(expected.body.size() == actual.body.size(), stage + " native legacy artwork framing size");
                        }
                    } else {
                        require(raw.size() == payloads[target][kind].size(), stage + " native group framing size");
                    }
                    result.insert("expected_payload_bytes", payloads[target][kind].size());
                    result.insert("actual_payload_bytes", raw.size());
                    qInfo().noquote() << "Complex conversion passed:" << stage;
                } catch (const std::exception &error) {
                    const QString message = QString::fromUtf8(error.what());
                    result.insert("passed", false);
                    result.insert("error", message);
                    failures.append(stage + ": " + message);
                    qWarning().noquote() << "Complex conversion failed:" << stage << message;
                }
                results.append(result);
            }
        }
    }
    QFile report(QDir(outputRoot).filePath("report.json"));
    require(report.open(QIODevice::WriteOnly), "complex report");
    report.write(QJsonDocument(QJsonObject{{"conversions", results}}).toJson());
    require(failures.isEmpty(), failures.join('\n'));
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 4 && app.arguments()[1] == "--fh5-framing-reference") {
            const auto project = fls::decodeProjectDocument(readFile(app.arguments()[2]));
            const QString reference = app.arguments()[3];
            const QStringList payloads = QDir(reference).entryList(
                {QStringLiteral("C_livery"), QStringLiteral("*.c_livery")}, QDir::Files);
            require(payloads.size() == 1, "framing reference must contain one livery payload");
            const auto expected = fls::readLiveryPayload(QDir(reference).filePath(payloads.front()));
            QTemporaryDir output;
            require(output.isValid(), "framing reference output directory");
            const auto reloaded = saveReload(project, QDir(output.path()).filePath("source.3so"));
            fls::exportGameLivery(reloaded, output.path(), fls::ExportGameVersion::Horizon5);
            const auto actual = fls::readLiveryPayload(output.path());
            equalBytes(expected.body, actual.body, "FH5 in-game validated artwork framing");
            require(expected.sectionCounts == actual.sectionCounts, "FH5 framing reference section counters");
            auto supported = saveReload(project, QDir(output.path()).filePath("supported.3so"));
            for (int index = static_cast<int>(supported.root->children.size()) - 1; index >= 0; --index) {
                const auto &child = supported.root->children[index];
                if (child->kind() == fls::scene::LayerKind::Group) {
                    const auto &group = static_cast<const fls::scene::Group &>(*child);
                    if (group.isLiverySection && group.liverySectionSlot >= 6) {
                        supported.root->takeAt(index);
                    }
                }
            }
            compareComplexArtwork(supported, fls::importCLivery(output.path()), "FH5 framing reference artwork");
            qInfo() << "FH5 framing reference passed";
            return 0;
        }
        if (app.arguments().size() == 4 && (app.arguments()[1] == "--export-fh5"
                || app.arguments()[1] == "--compare-livery")) {
            const auto project = fls::decodeProjectDocument(readFile(app.arguments()[2]));
            const QString output = app.arguments()[3];
            if (app.arguments()[1] == "--export-fh5") {
                fls::exportGameLivery(project, output, fls::ExportGameVersion::Horizon5);
            }
            const auto imported = fls::importCLivery(output);
            compareComplexArtwork(project, imported, "diagnostic livery");
            compareComplexArtwork(project, saveReload(imported, output + ".imported.3so"),
                "diagnostic saved project");
            qInfo() << "Diagnostic livery artwork comparison passed";
            return 0;
        }
        if (app.arguments().size() >= 3 && app.arguments()[1] == "--complex-references") {
            QTemporaryDir temporary;
            const QString output = app.arguments().size() == 4 ? app.arguments()[3] : temporary.path();
            require(QDir().mkpath(output), "complex output directory");
            testComplexReferences(app.arguments()[2], output);
            return 0;
        }
        if (app.arguments().size() >= 3 && app.arguments()[1] == "--references") {
            QTemporaryDir temporary;
            const QString output = app.arguments().size() == 4 ? app.arguments()[3] : temporary.path();
            require(QDir().mkpath(output), "reference output directory");
            testNativeReferences(app.arguments()[2], output);
            return 0;
        }
        if (app.arguments().size() == 3 && app.arguments()[1] == "--inspect") {
            const auto payload = fls::readLiveryPayload(app.arguments()[2]);
            qInfo() << "Artwork body bytes" << payload.body.size();
            for (const auto &section : fls::decodeLiverySections(payload)) {
                const auto layers = fls::flattenGroup(section.subtree);
                const int expected = section.slot < payload.sectionCounts.size() ? payload.sectionCounts[section.slot] : 0;
                qInfo() << section.name << "offset" << section.absPos << "shapes" << layers.size()
                    << "declared" << expected << "skipped" << skippedShapes(section.subtree)
                    << "errors" << fls::validateTree(section.subtree);
                if (!layers.isEmpty()) {
                    const auto &last = layers.back();
                    qInfo() << "Last record" << last.absPos << "remaining bytes" << payload.body.size() - last.absPos
                        << "last identifier" << last.shapeId;
                }
                if (qEnvironmentVariableIsSet("FLS_DUMP_RECORDS")) {
                    for (const auto &layer : layers) {
                        qInfo() << "RECORD" << section.slot << layer.absPos << layer.shapeId;
                    }
                }
            }
            return 0;
        }
        const QString root = QDir::current().filePath(QStringLiteral("tests"));
        require(QDir().mkpath(root), "temporary parent");
        QTemporaryDir temporary(QDir(root).filePath(QStringLiteral("export-XXXXXX")));
        require(temporary.isValid(), "temporary directory");
        if (qEnvironmentVariableIsSet("FLS_KEEP_TEST_OUTPUT")) {
            temporary.setAutoRemove(false);
            qInfo() << "Retained output:" << temporary.path();
        }
        qInfo() << "Testing generated liveries";
        testLiveries(temporary.path());
        qInfo() << "Testing thin sheared artwork";
        testThinShearedArtwork(temporary.path());
        qInfo() << "Testing Horizon livery group boundaries";
        testHorizonLiveryGroupBoundaries(temporary.path());
        qInfo() << "Testing livery shape-to-group masks";
        testHorizonLiveryShapeToGroupMasks(temporary.path());
        qInfo() << "Testing generated groups";
        testGroups(temporary.path());
        qInfo() << "Testing discarded masks";
        testDiscardedMasks(temporary.path());
        qInfo() << "Testing sparse sections, bounds, and blocks";
        testBoundsAndBlocks(temporary.path());
        if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--samples")) {
            qInfo() << "Testing native samples";
            testEarlySamples(app.arguments()[2], temporary.path());
            testSamples(app.arguments()[2], temporary.path());
        }
        qInfo() << "Game export tests passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << "Game export test failed:" << error.what();
        return 1;
    }
}
