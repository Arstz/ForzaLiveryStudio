#include "binary_io.h"
#include "cgroup_codec.h"
#include "editor_state.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "matrix_math.h"
#include "project_codec.h"
#include "vinyl_decoder.h"

#include <QtCore>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace {

struct ArtworkShape {
    fls::Matrix3 matrix;
    std::array<quint8, 4> color;
    quint16 shapeId;
    quint32 rasterId;
    int section;
    bool raster;
    bool mask;
    bool visible;
};

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("could not read %1").arg(path));
    return file.readAll();
}

void collectArtwork(const fls::scene::Layer &layer, QVector<ArtworkShape> &shapes,
                    int section = -1, bool mask = false) {
    if (layer.kind() == fls::scene::LayerKind::Shape) {
        const auto &shape = static_cast<const fls::scene::Shape &>(layer);
        shapes.append({shape.worldMatrix(), shape.color, shape.shapeId, shape.rasterId,
                       section, shape.raster, mask || shape.mask, shape.visible});
    } else if (layer.kind() == fls::scene::LayerKind::Group) {
        const auto &group = static_cast<const fls::scene::Group &>(layer);
        if (group.isLiverySection) {
            section = group.liverySectionSlot;
        }
        mask = mask || (group.flags & 0x40) != 0;
        for (const auto &child : group.children) {
            collectArtwork(*child, shapes, section, mask);
        }
    }
}

void compareArtwork(const fls::Project &expected, const fls::Project &actual,
                    const QString &stage, bool discardWindows = false, bool centered = false) {
    QVector<ArtworkShape> before;
    QVector<ArtworkShape> after;
    collectArtwork(*expected.root, before);
    collectArtwork(*actual.root, after);
    if (discardWindows) {
        before.erase(std::remove_if(before.begin(), before.end(), [](const auto &shape) {
            return shape.section >= 6;
        }), before.end());
    }
    require(expected.isLivery == actual.isLivery, stage + " project kind");
    require(before.size() == after.size(), QStringLiteral("%1 shape count: %2 expected, %3 imported")
        .arg(stage).arg(before.size()).arg(after.size()));
    std::array<int, 11> sectionCounts{};
    for (int index = 0; index < before.size(); ++index) {
        const auto &source = before[index];
        const auto &destination = after[index];
        const QString label = QStringLiteral("%1 shape %2 section %3").arg(stage).arg(index).arg(source.section);
        require(source.section == destination.section, label + " section assignment");
        require(source.shapeId == destination.shapeId && source.raster == destination.raster
            && source.rasterId == destination.rasterId, label + " identifier");
        require(source.color == destination.color, label + " color");
        require(source.visible == destination.visible, label + " authored visibility");
        require(source.mask == destination.mask, QStringLiteral("%1 mask: %2 expected, %3 imported")
            .arg(label).arg(source.mask).arg(destination.mask));
        if (destination.section >= 0) {
            ++sectionCounts[destination.section];
        }
        for (int row = 0; row < 2; ++row) {
            for (int column = 0; column < 3; ++column) {
                double value = source.matrix.m[row][column];
                double converted = destination.matrix.m[row][column];
                if (centered && column == 2) {
                    value -= before.front().matrix.m[row][column];
                    converted -= after.front().matrix.m[row][column];
                }
                const double tolerance = 0.001 + std::abs(value) * 0.00001;
                require(std::abs(value - converted) <= tolerance,
                    QStringLiteral("%1 transform [%2,%3]: %4 expected, %5 imported")
                        .arg(label).arg(row).arg(column).arg(value, 0, 'g', 12).arg(converted, 0, 'g', 12));
            }
        }
    }
    if (actual.isLivery) {
        require(expected.carId == actual.carId, stage + " car identifier");
        qInfo().noquote() << stage << "sections:" << sectionCounts;
    } else {
        qInfo().noquote() << stage << "shapes:" << after.size();
    }
}

fls::Project saveAndReload(const fls::Project &project, const QString &path) {
    const QByteArray bytes = fls::encodeProjectDocument(project);
    require(bytes.startsWith(QByteArray::fromHex("1f8b")), "packed project gzip header");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "could not write project");
    require(file.write(bytes) == bytes.size(), "could not write complete project");
    file.close();
    auto reloaded = fls::decodeProjectDocument(readFile(path));
    compareArtwork(project, reloaded, QFileInfo(path).fileName());
    return reloaded;
}

void checkFh5Format(const QString &folder, bool livery) {
    const auto header = fls::parseHeader(readFile(QDir(folder).filePath("header")));
    require(header.formatVersion == 6, "FH5 export must write a version-6 header");
    const QByteArray raw = fls::inflateContainer(readFile(QDir(folder).filePath(livery ? "C_livery" : "C_group")));
    if (livery) {
        require(fls::detail::readLeU32(raw, 29) == 18, "FH5 export must write livery information version 18");
    } else {
        require(static_cast<quint8>(raw.at(12)) == 3, "FH5 export must write generation-3 group records");
    }
}

void checkSectionIsolation(gui::EditorState &editor) {
    const QJsonObject before = fls::projectToJson(*editor.project());
    const QString previousSection = editor.activeSectionId_;
    const auto totalEntries = editor.renderEntries().size();
    for (const auto &child : editor.project()->root->children) {
        if (child->kind() != fls::scene::LayerKind::Group
            || !static_cast<const fls::scene::Group &>(*child).isLiverySection) {
            continue;
        }
        editor.setActiveSectionId(child->id);
        const auto &active = editor.activeRenderEntries();
        const auto expected = std::count_if(editor.renderEntries().cbegin(), editor.renderEntries().cend(),
            [&](const auto &entry) { return entry.sectionGroupId == child->id; });
        require(active.size() == expected, "active canvas section entry count");
        require(std::all_of(active.cbegin(), active.cend(), [&](const auto &entry) {
            return entry.sectionGroupId == child->id;
        }), "canvas contains another section");
        require(editor.renderEntries().size() == totalEntries, "section selection changed document entries");
        require(fls::projectToJson(*editor.project()) == before, "section selection changed saved artwork");
    }
    editor.setActiveSectionId(previousSection);
}

void roundTrip(const fls::Project &fh6, const QString &output, const QString &name) {
    gui::EditorState editor;
    editor.setProject(fh6);
    checkSectionIsolation(editor);
    require(QDir().mkpath(output), "round-trip output directory");
    const auto saved = saveAndReload(*editor.project(), QDir(output).filePath("fh6.3so"));
    editor.setProject(saved);
    checkSectionIsolation(editor);
    if (saved.isLivery && !saved.root->children.empty()) {
        editor.setActiveSectionId(saved.root->children.front()->id);
    }
    const QString fh5Folder = QDir(output).filePath("fh5");
    if (saved.isLivery) {
        fls::exportGameLivery(*editor.project(), fh5Folder, fls::ExportGameVersion::Horizon5);
    } else {
        fls::exportGameGroup(*editor.project(), fh5Folder, fls::ExportGameVersion::Horizon5);
    }
    checkFh5Format(fh5Folder, saved.isLivery);
    const auto fh5 = saved.isLivery ? fls::importCLivery(fh5Folder) : fls::importCGroupNested(fh5Folder);
    editor.setProject(fh5);
    checkSectionIsolation(editor);
    const auto reloaded = saveAndReload(*editor.project(), QDir(output).filePath("fh5.3so"));
    compareArtwork(fh6, fh5, name + " FH5 import", saved.isLivery, !saved.isLivery);
    compareArtwork(fh6, reloaded, name + " second project import", saved.isLivery, !saved.isLivery);
}

std::unique_ptr<fls::scene::Shape> makeShape(int index, bool mask = false) {
    auto shape = std::make_unique<fls::scene::Shape>();
    shape->id = QString::number(index);
    shape->setVectorShape(101 + index % 2);
    shape->x = index * 0.25;
    shape->y = index * -0.125;
    shape->scaleX = index % 3 == 0 ? -0.25 : 0.5;
    shape->scaleY = 0.75;
    shape->rotation = index % 360;
    shape->skew = 0.125;
    shape->color = mask ? std::array<quint8, 4>{128, 128, 128, 255}
                        : std::array<quint8, 4>{20, 40, 80, 255};
    shape->mask = mask;
    return shape;
}

fls::Project generatedProject(int count, bool livery) {
    fls::Project project;
    project.name = "Project round trip";
    project.isLivery = livery;
    project.carId = livery ? 1069 : 0;
    project.root = std::make_unique<fls::scene::Group>();
    project.headerMetadata = fls::defaultDraftHeader(project.name, "Roundtrip", project.carId);
    for (int slot = 0; slot < (livery ? 11 : 1); ++slot) {
        auto group = std::make_unique<fls::scene::Group>();
        group->id = QStringLiteral("section-%1").arg(slot);
        group->isLiverySection = livery;
        group->liverySectionSlot = livery ? slot : -1;
        group->x = 7.0;
        group->y = -11.0;
        group->rotation = 17.0;
        group->scaleX = -1.25;
        group->scaleY = 0.8;
        for (int index = 0; index < count; ++index) {
            group->append(makeShape(index + slot * count, index % 5 == 0));
        }
        project.root->append(std::move(group));
    }
    return project;
}

void testGenerated(const QString &output) {
    for (const bool livery : {false, true}) {
        for (const int count : {2, 9, 256}) {
            const QString name = QStringLiteral("%1-%2").arg(livery ? "livery" : "group").arg(count);
            const QString folder = QDir(output).filePath(name);
            const QString source = QDir(folder).filePath("fh6");
            const auto generated = generatedProject(count, livery);
            if (livery) {
                fls::exportGameLivery(generated, source, fls::ExportGameVersion::Horizon6);
            } else {
                fls::exportGameGroup(generated, source, fls::ExportGameVersion::Horizon6);
            }
            const auto imported = livery ? fls::importCLivery(source) : fls::importCGroupNested(source);
            compareArtwork(generated, imported, name + " FH6 import", false, !livery);
            roundTrip(imported, folder, name);
        }
    }
}

void testVisibilityMigration() {
    auto isolated = generatedProject(2, true);
    for (const auto &child : isolated.root->children) {
        auto &section = static_cast<fls::scene::Group &>(*child);
        for (const auto &shape : section.children) {
            shape->visible = section.liverySectionSlot == 0;
        }
    }
    QJsonObject legacy = fls::projectToJson(isolated);
    legacy.insert("version", 3);
    const auto restored = fls::projectFromJson(legacy);
    QVector<ArtworkShape> shapes;
    collectArtwork(*restored.root, shapes);
    require(std::all_of(shapes.cbegin(), shapes.cend(), [](const auto &shape) { return shape.visible; }),
        "legacy section isolation was not restored");
    const auto current = fls::decodeProjectDocument(fls::encodeProjectDocument(isolated));
    compareArtwork(isolated, current, "authored hidden sections");

    auto &first = static_cast<fls::scene::Group &>(*isolated.root->children.front());
    first.children.front()->visible = false;
    legacy = fls::projectToJson(isolated);
    legacy.insert("version", 3);
    compareArtwork(isolated, fls::projectFromJson(legacy), "legacy mixed visibility");
    gui::EditorState editor;
    editor.setProject(current);
    checkSectionIsolation(editor);
    editor.setActiveSectionId(editor.project()->root->children.front()->id);
    const auto entry = editor.activeRenderEntries().front();
    auto *shape = static_cast<fls::scene::Shape *>(editor.sceneNode(entry.nodeId));
    shape->x += 10.0;
    editor.noteTransformLiveChanged({shape->id});
    require(std::abs(editor.activeRenderEntries().front().worldTransform.dx() - entry.worldTransform.dx()) > 1.0,
        "active render cache retained an old transform");
}

void testFixtures(const QString &fixtures, const QString &output) {
    QDirIterator files(fixtures, QStringList{"C_livery", "C_group", "*.3so"}, QDir::Files, QDirIterator::Subdirectories);
    int tested = 0;
    while (files.hasNext()) {
        const QString path = files.next();
        const QFileInfo file(path);
        const QString folder = file.absolutePath();
        if (file.suffix() == "3so") {
            const auto project = fls::decodeProjectDocument(readFile(path));
            roundTrip(project, QDir(output).filePath(QString::number(tested)),
                QDir(fixtures).relativeFilePath(path));
            ++tested;
            continue;
        }
        if (fls::parseHeader(readFile(QDir(folder).filePath("header"))).formatVersion != 7) {
            continue;
        }
        const auto project = file.fileName() == "C_livery" ? fls::importCLivery(folder) : fls::importCGroupNested(folder);
        const QString name = QDir(fixtures).relativeFilePath(folder);
        roundTrip(project, QDir(output).filePath(QString::number(tested)), name);
        ++tested;
        if (project.isLivery) {
            for (const auto &child : project.root->children) {
                if (child->kind() != fls::scene::LayerKind::Group) {
                    continue;
                }
                const auto &section = static_cast<const fls::scene::Group &>(*child);
                if (section.liverySectionSlot != 0 && section.liverySectionSlot != 2) {
                    continue;
                }
                QVector<ArtworkShape> shapes;
                collectArtwork(section, shapes);
                fls::Project group;
                group.name = name + QStringLiteral(" section %1 group").arg(section.liverySectionSlot);
                group.root = std::make_unique<fls::scene::Group>();
                group.headerMetadata = fls::defaultDraftHeader(group.name, "Roundtrip");
                for (int index = 0; index < shapes.size(); ++index) {
                    const auto &source = shapes[index];
                    require(!source.raster, "vector group fixture");
                    auto shape = makeShape(index, source.mask);
                    shape->setVectorShape(source.shapeId);
                    shape->color = source.color;
                    shape->transform = fls::decomposeTransform2D(source.matrix);
                    group.root->append(std::move(shape));
                }
                const QString groupOutput = QDir(output).filePath(QString::number(tested));
                const QString fh6Folder = QDir(groupOutput).filePath("fh6");
                fls::exportGameGroup(group, fh6Folder, fls::ExportGameVersion::Horizon6);
                const auto imported = fls::importCGroupNested(fh6Folder);
                compareArtwork(group, imported, group.name + " FH6 import", false, true);
                roundTrip(imported, groupOutput, group.name);
                ++tested;
            }
        }
    }
    require(tested > 0, "no FH6 round-trip fixtures found");
    qInfo() << "FH6 fixture conversions passed:" << tested;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir output(QDir::temp().filePath("fls-roundtrip-XXXXXX"));
        require(output.isValid(), "temporary directory");
        if (qEnvironmentVariableIsSet("FLS_KEEP_TEST_OUTPUT")) {
            output.setAutoRemove(false);
            qInfo() << "Retained output:" << output.path();
        }
        if (app.arguments().size() == 2) {
            const QString input = app.arguments()[1];
            if (QFileInfo(input).isFile()) {
                roundTrip(fls::decodeProjectDocument(readFile(input)), output.path(), QFileInfo(input).fileName());
            } else {
                testFixtures(input, output.path());
            }
        } else {
            testVisibilityMigration();
            testGenerated(output.path());
        }
        qInfo() << "Project conversion tests passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << "Project conversion test failed:" << error.what();
        return 1;
    }
}
