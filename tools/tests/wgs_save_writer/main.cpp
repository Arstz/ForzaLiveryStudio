#include "binary_io.h"
#include "cgroup_codec.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "project_codec.h"
#include "wgs_save_reader.h"
#include "wgs_save_writer.h"

#include <QtCore>
#include <QImage>

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

using Game = fls::ExportGameVersion;

struct ProfileFixture {
    Game game;
    const char *serviceId;
    const char *package;
    quint32 headerVersion;
    quint32 liveryVersion;
    int assetIdSize;
};

constexpr const char *kFh5ServiceId = "0000000000000000000000007900C3C7";
constexpr const char *kFh5Package = "Microsoft.624F8B84B80_8wekyb3d8bbwe!Forzahorizon5";
constexpr ProfileFixture kProfiles[] = {
    {Game::Horizon3, "1902010095754C2B99163D664CE1DFAB", "Microsoft.OpusPG_8wekyb3d8bbwe!OpusReleaseFinal", 1, 6, 8},
    {Game::Horizon4, "000000000000000000000000765B6743", "Microsoft.SunriseBaseGame_8wekyb3d8bbwe!SunriseReleaseFinal", 4, 17, 16},
    {Game::Motorsport7, "000000000000000000000000772F15B8", "Microsoft.ApolloBaseGame_8wekyb3d8bbwe!forzamotorsport7", 2, 7, 16},
    {Game::Horizon5, kFh5ServiceId, kFh5Package, 6, 18, 16},
    {Game::Motorsport2023, kFh5ServiceId, "Microsoft.ForzaMotorsport_8wekyb3d8bbwe!ReleaseFinal", 6, 18, 16},
};

void require(bool condition, const QString &message) {
    if (!condition) {
        throw std::runtime_error(message.toStdString());
    }
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read " + path);
    return file.readAll();
}

void writeFile(const QString &path, const QByteArray &data) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "parent directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "write " + path);
}

void appendU64(QByteArray &bytes, quint64 value) {
    fls::detail::appendLeU32(bytes, static_cast<quint32>(value));
    fls::detail::appendLeU32(bytes, static_cast<quint32>(value >> 32));
}

void appendString(QByteArray &bytes, const QString &value) {
    fls::detail::appendLeU32(bytes, static_cast<quint32>(value.size()));
    for (QChar character : value) {
        fls::detail::appendLeU16(bytes, character.unicode());
    }
}

fls::Project makeProject(bool livery, int car, bool paintOnly) {
    fls::Project project;
    project.name = QStringLiteral("WGS append test");
    project.isLivery = livery;
    project.carId = car;
    project.root = std::make_unique<fls::scene::Group>();
    fls::scene::Group *parent = project.root.get();
    if (livery) {
        auto section = std::make_unique<fls::scene::Group>();
        section->id = "section";
        section->isLiverySection = true;
        section->liverySectionSlot = 2;
        parent = section.get();
        project.root->append(std::move(section));
    }
    for (int index = 0; index < (paintOnly ? 0 : 3); ++index) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->id = QString::number(index);
        shape->setVectorShape(101);
        shape->x = index * 10.0;
        shape->scaleX = 0.25;
        shape->scaleY = 0.25;
        parent->append(std::move(shape));
    }
    return project;
}

QMap<QString, QByteArray> encodedArtwork(fls::Project project, const fls::WgsExportTarget &target,
                                        const QString &staging) {
    auto &header = *project.headerMetadata;
    header.formatVersion = target.versions.header;
    header.guid = target.assetGuid;
    header.creatorTag = target.creatorTag;
    const Game game = target.encodingGame();
    if (project.isLivery) {
        fls::exportGameLivery(project, staging, game);
    } else {
        fls::exportGameGroup(project, staging, game);
    }
    const QString payload = project.isLivery ? QStringLiteral("C_livery") : QStringLiteral("C_group");
    return {{payload.toLower(), readFile(QDir(staging).filePath(payload))},
            {QStringLiteral("header"), readFile(QDir(staging).filePath(QStringLiteral("header")))}};
}

QString makeProfile(const QString &root, const QString &serviceId, const QString &package) {
    const QString profile = QDir(root).filePath(QStringLiteral("000901FEBE15706D_") + serviceId);
    QByteArray index;
    fls::detail::appendLeU32(index, 14);
    fls::detail::appendLeU32(index, 0);
    fls::detail::appendLeU32(index, 17);
    appendString(index, package);
    appendU64(index, 1);
    fls::detail::appendLeU32(index, 3);
    appendString(index, "b329481a-9cff-4b60-924b-41554f4c35f2");
    appendU64(index, 2147483648);
    writeFile(QDir(profile).filePath("containers.index"), index);
    return profile;
}

QMap<QString, QByteArray> snapshotFiles(const QString &directory) {
    QMap<QString, QByteArray> files;
    QDirIterator iterator(directory, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        if (!path.contains(".fls-wgs-backups")) {
            files.insert(path, readFile(path));
        }
    }
    return files;
}

void mustReject(const std::function<void()> &action, const QString &message, const QString &expectedError = {}) {
    try {
        action();
    } catch (const std::exception &error) {
        const QString actualError = QString::fromUtf8(error.what());
        require(actualError.startsWith(expectedError), message + ": " + actualError);
        return;
    }
    require(false, message);
}

void checkProfileDetection(const QString &root) {
    const struct {
        const char *titleId;
        const char *gameName;
        Game destination;
    } cases[] = {
        {"A72E0E3DCE314AE089B8741C625AB197", "Forza Motorsport 5", Game::Horizon5},
        {"788F0100A4BC46C3B19BD1001A96545B", "Forza Horizon 2", Game::Horizon5},
        {"417D0100B23041CF975D3EAA64F9397E", "Forza Motorsport 6", Game::Horizon5},
        {"1902010095754C2B99163D664CE1DFAB", "Forza Horizon 3", Game::Horizon5},
        {"000000000000000000000000772F15B8", "Forza Motorsport 7", Game::Horizon5},
        {"000000000000000000000000765B6743", "Forza Horizon 4", Game::Horizon5},
        {kFh5ServiceId, "Forza Horizon 5", Game::Horizon4},
    };
    for (const auto &fixture : cases) {
        const QString profile = makeProfile(root, QString::fromLatin1(fixture.titleId).toLower(), kFh5Package);
        mustReject([&] {
            fls::inspectWgsAppendTarget(QDir(profile).filePath("containers.index"),
                                        fixture.destination, fls::WgsAssetKind::Group);
        }, "title detected from the profile folder",
            QStringLiteral("Selected a container for %1,").arg(QLatin1String(fixture.gameName)));
    }
}

void copyTree(const QString &source, const QString &destination) {
    const QDir origin(source);
    QDirIterator iterator(source, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        writeFile(QDir(destination).filePath(origin.relativeFilePath(path)), readFile(path));
    }
}

void checkHorizonHeader(const fls::HeaderMetadata &header, const QString &containerName, Game game, bool livery) {
    const QString timestamp = containerName.section('_', -1);
    const QDate date = QDate::fromString(timestamp.left(8), "yyyyMMdd");
    const QTime time = QTime::fromString(timestamp.mid(8), "HHmmss");
    require(date.isValid() && time.isValid() && header.year == date.year()
        && header.month == date.month() && header.day == 0, "native WGS header date");
    require(fls::detail::readLeU16(header.fieldBlock, 0) == date.dayOfWeek() % 7
        && fls::detail::readLeU16(header.fieldBlock, 2) == date.day()
        && fls::detail::readLeU16(header.fieldBlock, 4) == time.hour()
        && fls::detail::readLeU16(header.fieldBlock, 6) == time.minute()
        && fls::detail::readLeU16(header.fieldBlock, 8) == time.second()
        && fls::detail::readLeU16(header.fieldBlock, 10) < 1000, "native WGS header time");
    if (!livery) return;
    if (game == Game::Horizon3) {
        QByteArray tail(2, '\0');
        fls::detail::appendLeU32(tail, header.carId);
        tail.append(QByteArray(8, '\xff'));
        require(header.trailing == tail, "authored FH3 livery header tail");
    } else if (game == Game::Horizon4) {
        require(header.trailing == QByteArray(18, '\0'), "authored FH4 livery header tail");
    } else {
        require(header.trailing == QByteArray(header.typeValue == 0 ? 16 : 14, '\0'),
            "authored FH5 livery header tail");
    }
}

void testAppend(const QString &profile, const QString &staging, Game game, bool livery, bool paintOnly = false) {
    const QDir directory(profile);
    const QString index = directory.filePath("containers.index");
    const int carId = livery ? 1069 : 0;
    const auto kind = livery ? fls::WgsAssetKind::Livery : fls::WgsAssetKind::Group;
    const QString expectedPrefix = livery ? "Livery_1069_" : "LayerGroup_0000_";
    const QString payloadKey = livery ? "c_livery" : "c_group";
    const auto before = snapshotFiles(profile);
    const auto originalSave = fls::readWgsSave(profile);
    const auto target = fls::inspectWgsAppendTarget(index, game, kind, carId);
    auto project = makeProject(livery, carId, paintOnly);
    project.headerMetadata = fls::defaultDraftHeader(project.name, "WGS tests", carId);
    const auto blobs = encodedArtwork(project, target, staging);
    mustReject([&] { fls::appendWgsContainer(target, blobs); }, "missing thumbnails");
    require(snapshotFiles(profile) == before, "invalid append changed the profile");
    QImage thumbnail(QSize(32, 64), QImage::Format_RGBA8888);
    thumbnail.fill(Qt::blue);

#ifdef Q_OS_WIN
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(index.utf16()), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "index lock");
    bool failed = false;
    try {
        fls::exportWgsAsset(target, project, thumbnail, {}, {});
    } catch (const std::exception &error) {
        failed = QString::fromUtf8(error.what()).contains("Backup:");
    }
    CloseHandle(handle);
    require(failed, "locked index did not reject append at commit");
    require(snapshotFiles(profile) == before, "failed append left files in the profile");
#endif

    const auto added = fls::exportWgsAsset(target, project, thumbnail, {}, {});
    const auto save = fls::readWgsSave(profile);
    require(save.containerCount == originalSave.containerCount + 1, "append registration count");
    require(save.packageId == originalSave.packageId, "append changed the index package");
    require(added.containerName.startsWith(expectedPrefix), "append registration name");
    QByteArray registration;
    appendString(registration, added.containerName);
    appendString(registration, added.containerName);
    appendString(registration, QString());
    registration.append(char(1));
    fls::detail::appendLeU32(registration, 5);
    require(readFile(index).contains(registration), "append cloud state and empty tag");
    require(readFile(QDir(added.backupPath).filePath("containers.index")) == before.value(index), "append backup");
    for (auto file = before.cbegin(); file != before.cend(); ++file) {
        require(file.key() == index || readFile(file.key()) == file.value(), "append changed an existing blob or descriptor");
    }
    const auto asset = std::find_if(save.assets.cbegin(), save.assets.cend(),
        [&](const fls::WgsAsset &candidate) { return candidate.containerName == added.containerName; });
    require(asset != save.assets.cend() && asset->kind == kind, "appended asset missing");
    const QByteArray header = readFile(asset->headerPath);
    const QByteArray payload = readFile(asset->payloadPath);
    const QDir container(QFileInfo(asset->payloadPath).absolutePath());
    const QByteArray descriptor = readFile(container.filePath("container.1"));
    require(descriptor.size() == (livery ? 648 : 488), "append descriptor size");
    for (int offset = 136; offset < descriptor.size(); offset += 160) {
        require(descriptor.mid(offset, 16) == QByteArray(16, '\0'), "append cloud blob ID");
    }
    require(payload == blobs.value(payloadKey), "append artwork bytes");
    QList<QSize> thumbnailSizes;
    const QByteArray pngSignature = QByteArray::fromHex("89504e470d0a1a0a");
    for (const QString &name : container.entryList(QDir::Files)) {
        const QByteArray data = readFile(container.filePath(name));
        if (data.startsWith(pngSignature)) {
            thumbnailSizes.append(QImage::fromData(data, "PNG").size());
        }
    }
    require(QImage(asset->thumbnailPath).size() == target.thumbnailSize()
            && thumbnailSizes.size() == (livery ? 2 : 1)
            && (!livery || thumbnailSizes.contains(target.smallLiveryThumbnailSize)), "registered WGS thumbnail sizes");
    const auto metadata = fls::parseHeader(header);
    if (game == Game::Horizon3 || game == Game::Horizon4 || game == Game::Horizon5) {
        checkHorizonHeader(metadata, added.containerName, game, livery);
    } else {
        require(header == blobs.value("header"), "append header bytes");
    }
    if (livery) {
        require(fls::importCLiveryData(payload, header, "append").carId == carId, "appended livery import");
        if (paintOnly) {
            const QByteArray raw = fls::inflateContainer(payload);
            const qsizetype offset = raw.indexOf("gyvl");
            const QByteArray emptySlot = QByteArray::fromHex("00000000000000000000803f0000000000000000000000");
            const QByteArray artwork = QByteArray::fromHex("6779766c00000000000000000000803f0000000000")
                + QByteArray(6, '\0') + emptySlot.repeated(5);
            require(metadata.typeValue == 0 && offset >= 4 && fls::detail::readLeU32(raw, offset - 4) == artwork.size()
                    && raw.mid(offset, artwork.size()) == artwork, "native FH5 paint-only artwork");
        }
    } else {
        const auto group = fls::importCGroupNestedData(payload, header, "append");
        require(group.root && group.root->children.size() == 3, "appended group import");
        if (game == Game::Horizon5 || game == Game::Motorsport2023) {
            require(metadata.sectionPrefix == QByteArray(1, '\x01') + QByteArray(27, '\0')
                    && metadata.trailing.isEmpty(), "authored FH5 group header layout");
        }
    }
    mustReject([&] { fls::exportWgsAsset(target, project, thumbnail, {}, {}); }, "stale append target");
}

void testAppendSample(const QString &sample, const QString &root, fls::ExportGameVersion game) {
    QString original;
    for (const QString &name : QDir(sample).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (name.endsWith("7900C3C7", Qt::CaseInsensitive)) {
            original = QDir(sample).absoluteFilePath(name);
            break;
        }
    }
    require(!original.isEmpty(), "FH5 service profile sample");
    const auto before = snapshotFiles(original);
    const QString copied = QDir(root).filePath(QDir(original).dirName());
    copyTree(original, copied);
    testAppend(copied, QDir(root).filePath("sample-livery-staging"), game, true);
    testAppend(copied, QDir(root).filePath("sample-group-staging"), game, false);
    require(snapshotFiles(original) == before, "original import sample changed");
    qInfo() << "WGS livery and group append verified against" << sample;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    try {
        const QString parent = QDir::current().filePath("tests");
        require(QDir().mkpath(parent), "temporary parent");
        QTemporaryDir temporary(QDir(parent).filePath("wgs-writer-XXXXXX"));
        require(temporary.isValid(), "temporary directory");
        checkProfileDetection(QDir(temporary.path()).filePath("profile-detection"));
        for (const auto &fixture : kProfiles) {
            const QDir appendRoot(QDir(temporary.path()).filePath(fls::exportGameCode(fixture.game)));
            const QString profile = makeProfile(appendRoot.absolutePath(), fixture.serviceId, fixture.package);
            const QString index = QDir(profile).filePath("containers.index");
            const auto target = fls::inspectWgsAppendTarget(index, fixture.game, fls::WgsAssetKind::Group);
            require(target.versions.header == fixture.headerVersion && target.versions.livery == fixture.liveryVersion
                    && target.assetGuid.size() == fixture.assetIdSize, "native WGS artwork profile");
            if (fixture.game != Game::Horizon5) {
                const QString nested = makeProfile(appendRoot.filePath("fh5-package/wgs"),
                                                   fixture.serviceId, kFh5Package);
                testAppend(nested, appendRoot.filePath("staged-nested-group"), fixture.game, false);
            }
            testAppend(profile, appendRoot.filePath("staged-livery"), fixture.game, true);
            testAppend(profile, appendRoot.filePath("staged-group"), fixture.game, false);
            if (fixture.game == Game::Horizon5) {
                testAppend(profile, appendRoot.filePath("staged-paint-only"), fixture.game, true, true);
            }
        }
        if (application.arguments().size() == 3 && application.arguments()[1] == "--sample") {
            testAppendSample(application.arguments()[2], QDir(temporary.path()).filePath("native-append"),
                             fls::ExportGameVersion::Horizon5);
        }
        if (application.arguments().size() == 3 && application.arguments()[1] == "--import-sample") {
            testAppendSample(application.arguments()[2], temporary.path(), fls::ExportGameVersion::Motorsport2023);
        }
        qInfo() << "WGS writer tests passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << "WGS writer test failed:" << error.what();
        return 1;
    }
}
