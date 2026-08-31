#include "wgs_save_reader.h"

#include "header_codec.h"
#include "layer.h"
#include "livery_codec.h"
#include "project_codec.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

struct ContainerFixture {
    QString name;
    QByteArray guid;
    QVector<QPair<QString, QByteArray>> files;
};

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void appendU8(QByteArray &bytes, quint8 value) {
    bytes.append(static_cast<char>(value));
}

void appendU16(QByteArray &bytes, quint16 value) {
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
}

void appendU32(QByteArray &bytes, quint32 value) {
    appendU16(bytes, static_cast<quint16>(value & 0xffff));
    appendU16(bytes, static_cast<quint16>(value >> 16));
}

void appendU64(QByteArray &bytes, quint64 value) {
    appendU32(bytes, static_cast<quint32>(value & 0xffffffffu));
    appendU32(bytes, static_cast<quint32>(value >> 32));
}

void appendString(QByteArray &bytes, const QString &value) {
    appendU32(bytes, static_cast<quint32>(value.size()));
    for (QChar character : value) {
        appendU16(bytes, character.unicode());
    }
}

void testHeaderNamePrefix() {
    QByteArray header;
    QString paddedName = QStringLiteral("Legacy Name");
    paddedName.append(QString(3, QChar::Null));
    appendU32(header, 4);
    appendString(header, paddedName);
    appendString(header, QStringLiteral("Readable description"));

    require(fls::parseHeaderName(header) == QStringLiteral("Legacy Name"),
            "legacy header name mismatch");
    require(fls::parseHeaderDescription(header) == QStringLiteral("Readable description"),
            "legacy header description mismatch");
}

QByteArray guidBytes(quint8 base) {
    QByteArray bytes;
    for (int index = 0; index < 16; ++index) {
        bytes.append(static_cast<char>(base + index));
    }
    return bytes;
}

QString guidName(const QByteArray &bytes) {
    const auto u16 = [&bytes](int offset) {
        return static_cast<quint16>(static_cast<uchar>(bytes[offset]))
            | (static_cast<quint16>(static_cast<uchar>(bytes[offset + 1])) << 8);
    };
    const auto u32 = [&bytes](int offset) {
        return static_cast<quint32>(static_cast<uchar>(bytes[offset]))
            | (static_cast<quint32>(static_cast<uchar>(bytes[offset + 1])) << 8)
            | (static_cast<quint32>(static_cast<uchar>(bytes[offset + 2])) << 16)
            | (static_cast<quint32>(static_cast<uchar>(bytes[offset + 3])) << 24);
    };
    QString result = QStringLiteral("%1%2%3")
                         .arg(u32(0), 8, 16, QLatin1Char('0'))
                         .arg(u16(4), 4, 16, QLatin1Char('0'))
                         .arg(u16(6), 4, 16, QLatin1Char('0'));
    for (int index = 8; index < bytes.size(); ++index) {
        result += QStringLiteral("%1").arg(
            static_cast<quint8>(bytes[index]), 2, 16, QLatin1Char('0'));
    }
    return result.toUpper();
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "could not create fixture file");
    require(file.write(bytes) == bytes.size(), "could not write fixture file");
}

void writeDescriptor(const QDir &root, const ContainerFixture &container) {
    const QString directoryPath = root.filePath(guidName(container.guid));
    require(QDir().mkpath(directoryPath), "could not create fixture container");
    QByteArray descriptor;
    appendU32(descriptor, 4);
    appendU32(descriptor, static_cast<quint32>(container.files.size()));
    for (const auto &[fileId, payloadGuid] : container.files) {
        QByteArray encodedId;
        for (QChar character : fileId) {
            appendU16(encodedId, character.unicode());
        }
        encodedId.resize(128, '\0');
        descriptor.append(encodedId);
        descriptor.append(QByteArray(16, '\0'));
        descriptor.append(payloadGuid);
        writeFile(QDir(directoryPath).filePath(guidName(payloadGuid)), fileId.toUtf8());
    }
    writeFile(QDir(directoryPath).filePath(QStringLiteral("container.1")), descriptor);
}

void writeIndex(const QDir &root, const QVector<ContainerFixture> &containers) {
    QByteArray index;
    appendU32(index, 14);
    appendU32(index, static_cast<quint32>(containers.size()));
    appendU32(index, 0);
    appendString(index, QStringLiteral("Microsoft.Test_8wekyb3d8bbwe!Test"));
    appendU64(index, 0);
    appendU32(index, 0);
    appendString(index, QStringLiteral("00000000-0000-0000-0000-000000000000"));
    appendU64(index, 0);
    for (const ContainerFixture &container : containers) {
        appendString(index, container.name);
        appendString(index, container.name);
        appendString(index, container.name);
        appendU8(index, 1);
        appendU32(index, 0);
        index.append(container.guid);
        appendU64(index, 0);
        appendU64(index, 0);
        appendU64(index, 0);
    }
    writeFile(root.filePath(QStringLiteral("containers.index")), index);
}

void testSyntheticSave() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "could not create temporary directory");
    const QDir root(temporary.path());
    const ContainerFixture livery{
        QStringLiteral("Livery_0123_20260812120000"),
        guidBytes(0x10),
        {
            {QStringLiteral("C_livery"), guidBytes(0x30)},
            {QStringLiteral("header"), guidBytes(0x50)},
            {QStringLiteral("bigThumb.png"), guidBytes(0x70)},
        },
    };
    const ContainerFixture group{
        QStringLiteral("LayerGroup_20260812120100"),
        guidBytes(0x90),
        {
            {QStringLiteral("C_group"), guidBytes(0xb0)},
            {QStringLiteral("header"), guidBytes(0xd0)},
        },
    };
    writeDescriptor(root, livery);
    writeDescriptor(root, group);
    writeIndex(root, {livery, group});
    require(QDir().mkpath(root.filePath(QStringLiteral("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"))),
            "could not create orphan container");

    require(fls::isWgsSaveDirectory(root.path()), "WGS root was not recognized");
    const fls::WgsSave save = fls::readWgsSave(root.path());
    require(save.containerCount == 2, "active container count mismatch");
    require(save.assets.size() == 2, "asset count mismatch");
    const auto liveryAsset = std::find_if(save.assets.cbegin(), save.assets.cend(), [](const fls::WgsAsset &asset) {
        return asset.kind == fls::WgsAssetKind::Livery;
    });
    const auto groupAsset = std::find_if(save.assets.cbegin(), save.assets.cend(), [](const fls::WgsAsset &asset) {
        return asset.kind == fls::WgsAssetKind::Group;
    });
    require(liveryAsset != save.assets.cend(), "livery asset missing");
    require(groupAsset != save.assets.cend(), "group asset missing");
    require(liveryAsset->containerName == livery.name, "livery name mismatch");
    require(QFileInfo(liveryAsset->payloadPath).fileName() == guidName(livery.files[0].second),
            "livery payload GUID mismatch");
    require(!liveryAsset->headerPath.isEmpty(), "livery header missing");
    require(!liveryAsset->thumbnailPath.isEmpty(), "livery thumbnail missing");
    require(groupAsset->containerName == group.name, "group name mismatch");
    require(QFileInfo(groupAsset->payloadPath).fileName() == guidName(group.files[0].second),
            "group payload GUID mismatch");
}

void testGroupExportOwnershipMetadata() {
    QTemporaryDir temporary;
    QByteArray expectedCreatorTag;
    constexpr quint64 kProfileId = 0x0102030405060708;

    require(temporary.isValid(), "could not create temporary directory");
    appendU64(expectedCreatorTag, kProfileId);

    fls::Project project;
    project.name = QStringLiteral("Ownership metadata");
    project.headerMetadata = fls::defaultDraftHeader(project.name, QStringLiteral("Creator"));
    for (int index = 0; index < 2; ++index) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->id = QStringLiteral("shape-%1").arg(index);
        shape->setVectorShape(static_cast<quint16>(index + 1));
        project.root->append(std::move(shape));
    }

    const QString profileDirectory = QStringLiteral("u_%1_ABCDEF").arg(kProfileId);
    const QString outputFolder = QDir(temporary.path()).filePath(
        profileDirectory + QStringLiteral("/current/ContainersRoot/LayerGroup_Test"));
    fls::exportNestedProjectFolder(project, outputFolder, project.name);

    QFile headerFile(QDir(outputFolder).filePath(QStringLiteral("header")));
    require(headerFile.open(QIODevice::ReadOnly), "group export header was not created");
    const fls::HeaderMetadata metadata = fls::parseHeader(headerFile.readAll());
    require(metadata.creatorTag == expectedCreatorTag, "group export creator identity mismatch");
    require(metadata.typeValue == 2, "group export shape count mismatch");
}

void testLiveryExportOwnershipMetadata() {
    QTemporaryDir temporary;
    QByteArray expectedCreatorTag;
    constexpr quint64 kProfileId = 0x0102030405060708;

    require(temporary.isValid(), "could not create temporary directory");
    appendU64(expectedCreatorTag, kProfileId);

    fls::Project project;
    project.name = QStringLiteral("Livery ownership metadata");
    project.carId = 1229;
    project.isLivery = true;
    project.headerMetadata = fls::defaultDraftHeader(
        project.name, QStringLiteral("Creator"), static_cast<quint32>(project.carId));
    project.headerMetadata->creatorTag = QByteArray(8, '\x7f');

    const QString profileDirectory = QStringLiteral("u_%1_ABCDEF").arg(kProfileId);
    const QString outputFolder = QDir(temporary.path()).filePath(
        profileDirectory + QStringLiteral("/current/ContainersRoot/Livery_Test"));
    fls::exportCLivery(project, outputFolder);

    QFile headerFile(QDir(outputFolder).filePath(QStringLiteral("header")));
    require(headerFile.open(QIODevice::ReadOnly), "livery export header was not created");
    const fls::HeaderMetadata metadata = fls::parseHeader(headerFile.readAll());
    require(metadata.creatorTag == expectedCreatorTag,
            "livery export header creator identity mismatch");

    const fls::LiveryPayload payload = fls::readLiveryPayload(outputFolder);
    const int metadataOffset = payload.raw.indexOf(QByteArray("yrvl", 4));
    require(metadataOffset >= 0, "livery export metadata record was not created");
    require(payload.raw.mid(metadataOffset + 8, 8) == expectedCreatorTag,
            "livery export payload creator identity mismatch");
}

QByteArray readFixtureFile(const QString &path) {
    if (path.isEmpty()) {
        return {};
    }
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not open save payload");
    return file.readAll();
}

void testImportedSave(const QString &path) {
    const fls::WgsSave save = fls::readWgsSave(path);
    require(save.containerCount > 0, "save has no active containers");
    require(!save.assets.isEmpty(), "save has no livery or group assets");
    bool importedGroup = false;
    bool importedLivery = false;
    for (const fls::WgsAsset &asset : save.assets) {
        if ((asset.kind == fls::WgsAssetKind::Group && importedGroup)
            || (asset.kind == fls::WgsAssetKind::Livery && importedLivery)) {
            continue;
        }
        try {
            const QByteArray payload = readFixtureFile(asset.payloadPath);
            const QByteArray header = readFixtureFile(asset.headerPath);
            const fls::Project project = asset.kind == fls::WgsAssetKind::Livery
                ? fls::importCLiveryData(payload, header, asset.containerName)
                : fls::importCGroupNestedData(payload, header, asset.containerName);
            if (asset.kind == fls::WgsAssetKind::Livery) {
                importedLivery = project.isLivery;
            } else {
                importedGroup = !project.isLivery;
            }
        } catch (const std::exception &) {
        }
        if (importedGroup && importedLivery) {
            break;
        }
    }
    require(importedLivery, "could not directly import a livery from the save");
    require(importedGroup, "could not directly import a group from the save");
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication application(argc, argv);
    try {
        testHeaderNamePrefix();
        testSyntheticSave();
        testGroupExportOwnershipMetadata();
        testLiveryExportOwnershipMetadata();
        if (application.arguments().size() > 1) {
            testImportedSave(application.arguments().at(1));
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
