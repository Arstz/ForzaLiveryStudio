#include "binary_io.h"
#include "cgroup_codec.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "project_codec.h"
#include "wgs_save_reader.h"
#include "wgs_save_writer.h"

#include <QtCore>

#include <functional>
#include <memory>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

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

QByteArray guidBytes(const QUuid &id) {
    QByteArray bytes;
    fls::detail::appendLeU32(bytes, id.data1);
    fls::detail::appendLeU16(bytes, id.data2);
    fls::detail::appendLeU16(bytes, id.data3);
    bytes.append(reinterpret_cast<const char *>(id.data4), 8);
    return bytes;
}

fls::Project makeProject(bool livery, int car = 0) {
    fls::Project project;
    project.name = QStringLiteral("WGS replacement test");
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
    for (int index = 0; index < 3; ++index) {
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

QMap<QString, QByteArray> replacement(const fls::WgsExportTarget &target, const QString &staging) {
    auto project = makeProject(target.asset.kind == fls::WgsAssetKind::Livery, target.carId);
    auto header = fls::defaultDraftHeader(project.name, "WGS tests", target.carId);
    header.formatVersion = 4;
    header.guid = fls::parseHeader(target.headerBytes).guid;
    header.creatorTag = target.creatorTag;
    project.headerMetadata = header;
    if (project.isLivery) {
        fls::exportGameLivery(project, staging, fls::ExportGameVersion::Horizon5);
    } else {
        fls::exportGameGroup(project, staging, fls::ExportGameVersion::Horizon5);
    }
    const QString payload = project.isLivery ? QStringLiteral("C_livery") : QStringLiteral("C_group");
    return {{payload.toLower(), readFile(QDir(staging).filePath(payload))},
            {QStringLiteral("header"), readFile(QDir(staging).filePath(QStringLiteral("header")))}};
}

struct Fixture {
    QString profile;
    QString descriptor;
    QString index;
    int headerTimeOffset = 0;
    int entryTimeOffset = 0;
};

Fixture makeFixture(const QString &root, bool livery, int generation = 1) {
    Fixture fixture;
    fixture.profile = QDir(root).filePath(QStringLiteral("000901FEBE15706D_0000000000000000000000007900C3C7"));
    fixture.index = QDir(fixture.profile).filePath("containers.index");
    const QUuid id = QUuid::createUuid();
    const QString folder = QDir(fixture.profile).filePath(id.toString(QUuid::Id128).toUpper());
    fixture.descriptor = QDir(folder).filePath(QStringLiteral("container.%1").arg(generation));
    const QString pgs = QDir(root).filePath("initial");
    auto project = makeProject(livery, livery ? 1069 : 0);
    project.headerMetadata = fls::defaultDraftHeader(project.name, "Original", project.carId);
    if (livery) {
        fls::exportGameLivery(project, pgs, fls::ExportGameVersion::Horizon5);
    } else {
        fls::exportGameGroup(project, pgs, fls::ExportGameVersion::Horizon5);
    }
    QByteArray descriptor;
    fls::detail::appendLeU32(descriptor, 4);
    fls::detail::appendLeU32(descriptor, 3);
    quint64 size = 0;
    for (const QString &name : {livery ? QStringLiteral("C_livery") : QStringLiteral("C_group"),
                                QStringLiteral("header"), QStringLiteral("thumb.png")}) {
        QByteArray data = name == "thumb.png" ? QByteArray::fromHex("89504e470d0a1a0a01020304") : readFile(QDir(pgs).filePath(name));
        QByteArray encodedName;
        for (QChar character : name) {
            fls::detail::appendLeU16(encodedName, character.unicode());
        }
        encodedName.resize(128, '\0');
        const QUuid blobId = QUuid::createUuid();
        const QUuid previousId = QUuid::createUuid();
        descriptor.append(encodedName);
        descriptor.append(guidBytes(previousId));
        descriptor.append(guidBytes(blobId));
        writeFile(QDir(folder).filePath(blobId.toString(QUuid::Id128).toUpper()), data);
        size += data.size();
    }
    writeFile(fixture.descriptor, descriptor);
    QByteArray index;
    fls::detail::appendLeU32(index, 14);
    fls::detail::appendLeU32(index, 1);
    fls::detail::appendLeU32(index, 17);
    appendString(index, "Microsoft.624F8B84B80_8wekyb3d8bbwe!Forzahorizon5");
    fixture.headerTimeOffset = index.size();
    appendU64(index, 1);
    fls::detail::appendLeU32(index, 3);
    appendString(index, "b329481a-9cff-4b60-924b-41554f4c35f2");
    appendU64(index, 2147483648);
    const QString name = livery ? "Livery_1069_20261013000000" : "LayerGroup_0000_20261013000000";
    appendString(index, name);
    appendString(index, name);
    appendString(index, "\"0x012345abcdef\"");
    index.append(static_cast<char>(generation));
    fls::detail::appendLeU32(index, 1);
    index.append(guidBytes(id));
    fixture.entryTimeOffset = index.size();
    appendU64(index, 2);
    appendU64(index, 0x11223344);
    appendU64(index, size);
    writeFile(fixture.index, index);
    return fixture;
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

void mustReject(const std::function<void()> &action, const QString &message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, message);
}

void testSynthetic(const QString &root) {
    for (const bool livery : {false, true}) {
        const QString caseRoot = QDir(root).filePath(livery ? "livery" : "group");
        const auto fixture = makeFixture(caseRoot, livery, 3);
        const auto target = fls::inspectWgsExportTarget(fixture.descriptor, fls::ExportGameVersion::Horizon5, livery, livery ? 1069 : 0);
        const auto before = snapshotFiles(fixture.profile);
        const auto blobs = replacement(target, QDir(caseRoot).filePath("replacement"));
        mustReject([&] { fls::inspectWgsExportTarget(fixture.descriptor, fls::ExportGameVersion::Motorsport2023, livery); }, "game mismatch");
        mustReject([&] { fls::inspectWgsExportTarget(fixture.descriptor, fls::ExportGameVersion::Horizon5, !livery); }, "type mismatch");
        if (livery) {
            mustReject([&] { fls::inspectWgsExportTarget(fixture.descriptor, fls::ExportGameVersion::Horizon5, true, 123); }, "car mismatch");
        }
        writeFile(QDir(QFileInfo(fixture.descriptor).absolutePath()).filePath("container.1"), readFile(fixture.descriptor));
        mustReject([&] { fls::inspectWgsExportTarget(QDir(QFileInfo(fixture.descriptor).absolutePath()).filePath("container.1"),
            fls::ExportGameVersion::Horizon5, livery, livery ? 1069 : 0); }, "stale generation");
        auto invalid = blobs;
        invalid.insert("profiledata", "unexpected");
        mustReject([&] { fls::replaceWgsContainer(target, invalid); }, "unregistered blob");
        for (auto file = before.cbegin(); file != before.cend(); ++file) {
            require(readFile(file.key()) == file.value(), "rejection changed data");
        }
        const QString backup = fls::replaceWgsContainer(target, blobs);
        const QDir profile(fixture.profile);
        for (auto file = before.cbegin(); file != before.cend(); ++file) {
            require(readFile(QDir(backup).filePath(profile.relativeFilePath(file.key()))) == file.value(), "backup bytes");
        }
        require(readFile(fixture.descriptor) == before.value(fixture.descriptor), "descriptor GUID preservation");
        const QByteArray updated = readFile(fixture.index);
        const QByteArray original = before.value(fixture.index);
        require(updated.size() == original.size(), "index layout changed");
        for (int position = 0; position < updated.size(); ++position) {
            const bool field = (position >= fixture.headerTimeOffset && position < fixture.headerTimeOffset + 8)
                || (position >= fixture.entryTimeOffset && position < fixture.entryTimeOffset + 8)
                || (position >= fixture.entryTimeOffset + 16 && position < fixture.entryTimeOffset + 24);
            require(field || updated[position] == original[position], "opaque index field changed");
        }
        const auto save = fls::readWgsSave(fixture.profile);
        require(save.containerCount == 1 && save.assets.size() == 1, "registration changed");
        mustReject([&] { fls::replaceWgsContainer(target, blobs); }, "changed snapshot accepted");
    }
}

void testRollback(const QString &root) {
#ifdef Q_OS_WIN
    const auto fixture = makeFixture(QDir(root).filePath("rollback"), false);
    const auto target = fls::inspectWgsExportTarget(fixture.descriptor, fls::ExportGameVersion::Horizon5, false);
    const auto before = snapshotFiles(fixture.profile);
    const auto blobs = replacement(target, QDir(root).filePath("rollback-staging"));
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(fixture.index.utf16()), GENERIC_READ,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "index lock");
    bool failed = false;
    try {
        fls::replaceWgsContainer(target, blobs);
    } catch (const std::exception &error) {
        failed = QString::fromUtf8(error.what()).contains("rolled back");
    }
    CloseHandle(handle);
    require(failed, "index commit failure did not roll back");
    for (auto file = before.cbegin(); file != before.cend(); ++file) {
        require(readFile(file.key()) == file.value(), "rollback did not restore file");
    }
#else
    Q_UNUSED(root);
#endif
}

void copyTree(const QString &source, const QString &destination) {
    const QDir origin(source);
    QDirIterator iterator(source, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        writeFile(QDir(destination).filePath(origin.relativeFilePath(path)), readFile(path));
    }
}

void testSample(const QString &sample, const QString &root) {
    QString profile;
    fls::WgsSave originalSave;
    for (const QFileInfo &info : QDir(sample).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (fls::isWgsSaveDirectory(info.absoluteFilePath())) {
            const auto save = fls::readWgsSave(info.absoluteFilePath());
            if (save.containerCount > originalSave.containerCount) {
                originalSave = save;
                profile = info.absoluteFilePath();
            }
        }
    }
    require(!profile.isEmpty(), "active sample profile");
    const auto sourceBefore = snapshotFiles(profile);
    const QString copied = QDir(root).filePath(QDir(profile).dirName());
    copyTree(profile, copied);
    const auto copyBefore = snapshotFiles(copied);
    QSet<QString> affected;
    int containers = 0;
    for (const auto &asset : fls::readWgsSave(copied).assets) {
        const bool livery = asset.kind == fls::WgsAssetKind::Livery;
        if (!asset.containerName.startsWith(livery ? "Livery_" : "LayerGroup_")) {
            continue;
        }
        const QString folder = QFileInfo(asset.payloadPath).absolutePath();
        const QStringList names = QDir(folder).entryList({"container.*"}, QDir::Files);
        const int carId = livery ? asset.containerName.section('_', 1, 1).toInt() : 0;
        const auto target = fls::inspectWgsExportTarget(QDir(folder).filePath(names.front()), fls::ExportGameVersion::Horizon5, livery, carId);
        const auto blobs = replacement(target, QDir(root).filePath(QStringLiteral("staging-%1").arg(containers)));
        const QString backup = fls::replaceWgsContainer(target, blobs);
        for (auto blob = blobs.cbegin(); blob != blobs.cend(); ++blob) {
            const QString path = target.blobPaths.value(blob.key());
            require(readFile(path) == blob.value(), "sample replacement bytes");
            require(readFile(QDir(backup).filePath(QDir(copied).relativeFilePath(path))) == copyBefore.value(path), "sample backup bytes");
            affected.insert(path);
        }
        affected.insert(target.indexPath);
        if (livery) {
            require(fls::importCLiveryData(readFile(target.asset.payloadPath), readFile(target.asset.headerPath), "replacement").carId == carId, "sample livery import");
        } else {
            fls::importCGroupNestedData(readFile(target.asset.payloadPath), readFile(target.asset.headerPath), "replacement");
        }
        ++containers;
    }
    const auto finalSave = fls::readWgsSave(copied);
    require(finalSave.containerCount == originalSave.containerCount && finalSave.assets.size() == originalSave.assets.size(), "sample registration count");
    for (auto file = copyBefore.cbegin(); file != copyBefore.cend(); ++file) {
        require(affected.contains(file.key()) || readFile(file.key()) == file.value(), "unrelated sample file changed");
    }
    require(snapshotFiles(profile) == sourceBefore, "original sample changed");
    qInfo() << "WGS sample replacements:" << containers << "registered containers:" << finalSave.containerCount;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    try {
        const QString parent = QDir::current().filePath("tests");
        require(QDir().mkpath(parent), "temporary parent");
        QTemporaryDir temporary(QDir(parent).filePath("wgs-writer-XXXXXX"));
        require(temporary.isValid(), "temporary directory");
        testSynthetic(temporary.path());
        testRollback(temporary.path());
        if (application.arguments().size() == 3 && application.arguments()[1] == "--sample") {
            testSample(application.arguments()[2], temporary.path());
        }
        qInfo() << "WGS writer tests passed";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << "WGS writer test failed:" << error.what();
        return 1;
    }
}
