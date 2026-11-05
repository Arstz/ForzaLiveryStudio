#include "binary_io.h"
#include "cgroup_codec.h"
#include "game_export.h"
#include "layer.h"
#include "livery_codec.h"
#include "material_hashes.h"
#include "paint_database.h"
#include "positional_paint.h"
#include "project_codec.h"

#include <QtCore>

#include <array>
#include <stdexcept>

namespace {

using namespace fls::material_hashes::binding;
using Color = std::array<quint8, 4>;
constexpr Color kCyan = {255, 255, 0, 255};
constexpr Color kMagenta = {255, 0, 255, 255};
constexpr Color kBlue = {255, 0, 0, 255};
constexpr Color kGreen = {0, 255, 0, 255};
constexpr Color kYellow = {0, 255, 255, 255};

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

QByteArray read(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read " + path);
    return file.readAll();
}

void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "write " + path);
    require(file.write(bytes) == bytes.size(), "write size " + path);
}

fls::Project authoredProject() {
    fls::Project project;
    project.isLivery = true;
    project.carId = 368;
    project.name = "Legacy paint validation";
    project.liveryPaint.game = "FH5";
    project.liveryPaint.setDefaultCarColorBgra(kCyan);
    for (quint64 hash : {kHoodPaint, kHoodSecondaryPaint, kHoodTertiaryPaint}) {
        project.liveryPaint.setColorBgra(hash, false, kMagenta);
    }
    for (quint64 hash : {kMirrorPaint, kMirrorSecondaryPaint, kMirrorTertiaryPaint}) {
        project.liveryPaint.setColorBgra(hash, false, kBlue);
    }
    project.liveryPaint.setColorBgra(kRims, false, kGreen);
    project.liveryPaint.setColorBgra(kBrakeCaliper, false, kYellow);
    project.liveryPaint.setColorBgra(kWindowGlass, false, {0, 0, 0, 255});
    return project;
}

void verifyColor(const fls::LiveryPaintState &state, quint64 hash, Color color) {
    const fls::LiveryPaintMaterial *material = state.find(hash);
    require(material && material->primary.enabled && material->primary.bgra == color,
        QStringLiteral("paint color %1").arg(hash, 16, 16, QLatin1Char('0')));
}

void testAuthored() {
    const std::array<fls::ExportGameVersion, 8> games = {
        fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
        fls::ExportGameVersion::Horizon4, fls::ExportGameVersion::Horizon5,
        fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6,
        fls::ExportGameVersion::Motorsport7, fls::ExportGameVersion::Motorsport2023,
    };
    for (const auto game : games) {
        const auto project = authoredProject();
        const QByteArray raw = fls::encodeGameLiveryPayload(project, game);
        const auto payload = fls::parseInflatedLiveryPayload(raw);
        require(payload.carId == 368 && payload.paint.materials.size() == 15, "authored paint metadata");
        verifyColor(payload.paint, kBodyPaint, kCyan);
        verifyColor(payload.paint, kHoodPaint, kMagenta);
        verifyColor(payload.paint, kMirrorPaint, kBlue);
        verifyColor(payload.paint, kBrakeCaliper, kYellow);
        verifyColor(payload.paint, kRims, kGreen);
        require(payload.paint.find(kBodyPaint)->finish == 1, "authored default gloss");
        require(payload.paint.find(kWindowGlass)->finish == 0, "authored window finish");
        const auto reloaded = fls::decodeProjectDocument(fls::encodeProjectDocument(project));
        require(reloaded.liveryPaint == project.liveryPaint, "authored project persistence");
        const QByteArray block = fls::positionalPaintBlock(raw);
        require(fls::encodePositionalPaint(fls::importCLiveryData(
            fls::deflateContainer(raw), {}, "authored"), (block.size() - 3) / 40,
            payload.paint.game) == block, "authored native paint roundtrip");
        auto nativeProject = fls::importCLiveryData(fls::deflateContainer(raw), {}, "authored");
        const QByteArray hashed = fls::encodeCLiveryPayload(nativeProject);
        verifyColor(fls::parseInflatedLiveryPayload(hashed).paint, kBodyPaint, kCyan);
    }
    require(fls::matchingPaintFinish(4, "FH5", "FH6") == 4, "metallic finish compatibility");
    require(fls::matchingPaintFinish(72, "FH5", "FH6") == -1, "Spectraflame cannot become glitter");
    bool rejected = false;
    try {
        auto project = authoredProject();
        project.liveryPaint.ensure(kBodyPaint).finish = 72;
        fls::encodeGameLiveryPayload(project, fls::ExportGameVersion::Horizon4);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "unsupported destination finish rejected");
    auto splitWheels = authoredProject();
    for (quint64 hash : kFrontWheelPaint) {
        splitWheels.liveryPaint.setColorBgra(hash, false, kBlue);
    }
    rejected = false;
    try {
        fls::encodeGameLiveryPayload(splitWheels, fls::ExportGameVersion::Horizon5);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "independent axle paint cannot silently become shared paint");
}

void testPreservation(const QString &root) {
    int checked = 0;
    const QStringList directories = QDir(root).entryList({"PTG*"}, QDir::Dirs);
    for (const QString &directory : directories) {
        QDirIterator files(QDir(root).filePath(directory), {"C_livery"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto payload = fls::readLiveryPayload(files.next());
            fls::Project project;
            project.liverySource = payload.raw;
            project.liveryPaint = payload.paint;
            const QByteArray block = fls::positionalPaintBlock(payload.raw);
            require(!block.isEmpty() && payload.paint.materials.size() == 15, "native paint decoded");
            require(fls::encodePositionalPaint(project, (block.size() - 3) / 40,
                payload.paint.game) == block, "PTG native bytes preserved");
            const auto reloaded = fls::decodeProjectDocument(fls::encodeProjectDocument(project));
            require(reloaded.liveryPaint == project.liveryPaint, "PTG paint project persistence");
            ++checked;
        }
    }
    qInfo() << "Preserved native paint blocks:" << checked;
}

void testOpaqueFields() {
    auto project = authoredProject();
    auto block = fls::encodePositionalPaint(project, 34, "FH5");
    const int body = 1;
    const int unknown = 1 + 29 * 40;
    block[body + 12] = '\x01';
    block.replace(body + 13, 4, QByteArray::fromHex("0000003f"));
    block.replace(body + 35, 5, QByteArray::fromHex("01deadbeef"));
    block.replace(unknown + 12, 15, QByteArray(15, '\x5a'));
    project.liveryPaint = fls::decodePositionalPaint(block, "FH5");
    project.liveryPaint.setColorBgra(kBodyPaint, false, kBlue);
    const QByteArray changed = fls::encodePositionalPaint(project, 34, "FH5");
    require(changed.mid(body + 12, 15) == block.mid(body + 12, 15), "opaque scalar bytes preserved");
    require(changed.mid(body + 35, 5) == block.mid(body + 35, 5), "opaque record tail preserved");
    require(changed.mid(unknown, 40) == block.mid(unknown, 40), "unknown slot preserved");
    auto malformed = block;
    malformed[1] = '\x02';
    bool rejected = false;
    try {
        fls::decodePositionalPaint(malformed, "FH5");
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "malformed record rejected");
}

void testColorModes() {
    auto project = authoredProject();
    project.liveryPaint.setColorBgra(kBodyPaint, true, kMagenta);
    project.liveryPaint.ensure(kBodyPaint).finish = 51;
    auto paint = fls::parseInflatedLiveryPayload(
        fls::encodeGameLiveryPayload(project, fls::ExportGameVersion::Horizon5)).paint;
    const auto *body = paint.find(kBodyPaint);
    require(body && body->secondary.enabled && body->secondary.bgra == kMagenta
        && body->finish == 51, "two-tone color and finish decoded");
    auto &spoiler = project.liveryPaint.ensure(kSpoilerPaint);
    spoiler.primary = {true, {0, 0, 255, 255}};
    spoiler.manufacturerSelector = 0;
    spoiler.finish = 0;
    paint = fls::parseInflatedLiveryPayload(
        fls::encodeGameLiveryPayload(project, fls::ExportGameVersion::Horizon5)).paint;
    const auto *factory = paint.find(kSpoilerPaint);
    require(factory && factory->primary.enabled && factory->primary.bgra == spoiler.primary.bgra
        && factory->manufacturerSelector == 0 && factory->finish == 0, "factory selection retains stale custom bytes");
    paint = fls::parseInflatedLiveryPayload(fls::encodeCLiveryPayload(project)).paint;
    factory = paint.find(kSpoilerPaint);
    require(factory && factory->manufacturerSelector == 0 && factory->finish == 0,
        "factory selection survives hashed export");
    project.liveryPaint.ensure(kBodyPaint).manufacturerSelector = 0;
    require(!project.liveryPaint.defaultCarColorBgra(), "factory selection takes precedence over stored body color");
}

void prepare(const QString &source, const QString &output) {
    auto original = fls::importCLivery(source);
    require(original.carId == 368, "controlled sample car");
    require(original.liveryPaint.find(kSpoilerPaint)->manufacturerSelector == 0, "manufacturer spoiler selector");
    const QString control = QDir(output).filePath("roundtrip");
    const QString edited = QDir(output).filePath("edited");
    fls::exportGameLivery(original, control, fls::ExportGameVersion::Horizon5);
    require(fls::positionalPaintBlock(fls::readLiveryPayload(control).raw)
        == original.liveryPaint.positionalSource, "controlled native paint byte preservation");
    write(QDir(control).filePath("project.3so"), fls::encodeProjectDocument(original));
    auto changed = authoredProject();
    changed.liverySource = original.liverySource;
    changed.sourceHeader = original.sourceHeader;
    changed.headerMetadata = original.headerMetadata;
    changed.liveryPaint.positionalSource = original.liveryPaint.positionalSource;
    for (quint64 hash : {kSpoilerPaint, kWingSecondaryPaint, kWingTertiaryPaint}) {
        changed.liveryPaint.ensure(hash) = *original.liveryPaint.find(hash);
    }
    for (quint64 hash : {kBodyPaint, kBodySecondaryPaint, kBodyTertiaryPaint}) {
        changed.liveryPaint.ensure(hash).finish = 3;
    }
    for (quint64 hash : {kHoodPaint, kHoodSecondaryPaint, kHoodTertiaryPaint}) {
        changed.liveryPaint.ensure(hash).finish = 2;
    }
    fls::exportGameLivery(changed, edited, fls::ExportGameVersion::Horizon5);
    const auto reimported = fls::importCLivery(edited);
    verifyColor(reimported.liveryPaint, kBodyPaint, kCyan);
    verifyColor(reimported.liveryPaint, kHoodPaint, kMagenta);
    verifyColor(reimported.liveryPaint, kMirrorPaint, kBlue);
    verifyColor(reimported.liveryPaint, kRims, kGreen);
    verifyColor(reimported.liveryPaint, kBrakeCaliper, kYellow);
    require(reimported.liveryPaint.find(kSpoilerPaint)->manufacturerSelector == 0, "edited factory spoiler retained");
    const QByteArray paint = reimported.liveryPaint.positionalSource;
    for (int slot : {30, 31, 32, 33}) {
        require(paint.mid(1 + slot * 40, 40) == paint.mid(1 + 6 * 40, 40), "edited whole-wheel channels");
    }
    write(QDir(edited).filePath("project.3so"), fls::encodeProjectDocument(changed));
    auto fresh = changed;
    fresh.liverySource.clear();
    fresh.liveryPaint.positionalSource.clear();
    const QString authored = QDir(output).filePath("authored");
    fls::exportGameLivery(fresh, authored, fls::ExportGameVersion::Horizon5);
    verifyColor(fls::readLiveryPayload(authored).paint, kBodyPaint, kCyan);
    write(QDir(authored).filePath("project.3so"), fls::encodeProjectDocument(fresh));
    const QJsonObject expected = {
        {"car_id", 368}, {"roundtrip", "Body red, hood blue, mirrors green, rims yellow, brakes magenta, glass black, spoiler manufacturer color 0 (black)."},
        {"edited", "Body cyan matte, hood magenta semigloss, mirrors blue gloss, rims green gloss, brakes yellow gloss, glass black, spoiler manufacturer color 0 (black)."},
        {"authored", "Same appearance as edited; paint and artwork generated without the native livery source."},
    };
    write(QDir(output).filePath("expected.json"), QJsonDocument(expected).toJson());
    qInfo().noquote() << "Prepared FH5 visual checks:" << output;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        testAuthored();
        testOpaqueFields();
        testColorModes();
        testPreservation(QDir(QStringLiteral(FLS_SOURCE_DIR)).filePath("tests"));
        if (app.arguments().size() == 4 && app.arguments()[1] == "--prepare") {
            prepare(app.arguments()[2], app.arguments()[3]);
        }
        qInfo() << "Positional paint checks passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
}
