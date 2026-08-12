// Ported from Fr33dan's GPSaveConverter WGS reader (GPL-3.0): https://github.com/Fr33dan/GPSaveConverter

#include "wgs_save_reader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fls {
namespace {

constexpr quint32 kIndexVersion = 14;
constexpr quint32 kDescriptorVersion = 4;
constexpr qsizetype kDescriptorHeaderSize = 8;
constexpr qsizetype kDescriptorEntrySize = 160;
constexpr qsizetype kFileIdSize = 128;
constexpr qsizetype kCurrentGuidOffset = 144;
constexpr qsizetype kGuidSize = 16;
constexpr quint32 kMaximumContainerCount = 1000000;

class Cursor {
public:
    explicit Cursor(QByteArray bytes)
        : bytes_(std::move(bytes)) {}

    QByteArray readBytes(qsizetype count) {
        require(count);
        const QByteArray result = bytes_.mid(offset_, count);
        offset_ += count;

        return result;
    }

    quint8 readU8() {
        require(1);

        return static_cast<quint8>(bytes_.at(offset_++));
    }

    quint16 readU16() {
        require(2);
        const quint16 value = static_cast<quint16>(static_cast<uchar>(bytes_.at(offset_)))
            | (static_cast<quint16>(static_cast<uchar>(bytes_.at(offset_ + 1))) << 8);
        offset_ += 2;

        return value;
    }

    quint32 readU32() {
        require(4);
        const quint32 value = static_cast<quint32>(static_cast<uchar>(bytes_.at(offset_)))
            | (static_cast<quint32>(static_cast<uchar>(bytes_.at(offset_ + 1))) << 8)
            | (static_cast<quint32>(static_cast<uchar>(bytes_.at(offset_ + 2))) << 16)
            | (static_cast<quint32>(static_cast<uchar>(bytes_.at(offset_ + 3))) << 24);
        offset_ += 4;

        return value;
    }

    quint64 readU64() {
        const quint64 low = readU32();
        const quint64 high = readU32();

        return low | (high << 32);
    }

    QString readString() {
        const quint32 length = readU32();
        if (length > static_cast<quint32>(remaining() / 2)) {
            throw std::runtime_error("WGS string extends beyond its container");
        }
        QString result;
        result.reserve(static_cast<qsizetype>(length));
        for (quint32 index = 0; index < length; ++index) {
            result.append(QChar(readU16()));
        }

        return result;
    }

    void skip(qsizetype count) {
        require(count);
        offset_ += count;
    }

private:
    void require(qsizetype count) const {
        if (count < 0 || count > remaining()) {
            throw std::runtime_error("WGS data is truncated");
        }
    }

    qsizetype remaining() const {
        return bytes_.size() - offset_;
    }

    QByteArray bytes_;
    qsizetype offset_ = 0;
};

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open WGS file: " + path).toStdString());
    }

    return file.readAll();
}

quint16 readLeU16(const QByteArray &bytes, qsizetype offset) {
    return static_cast<quint16>(static_cast<uchar>(bytes.at(offset)))
        | (static_cast<quint16>(static_cast<uchar>(bytes.at(offset + 1))) << 8);
}

quint32 readLeU32(const QByteArray &bytes, qsizetype offset) {
    return static_cast<quint32>(static_cast<uchar>(bytes.at(offset)))
        | (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 1))) << 8)
        | (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 2))) << 16)
        | (static_cast<quint32>(static_cast<uchar>(bytes.at(offset + 3))) << 24);
}

QString guidPathName(const QByteArray &bytes) {
    if (bytes.size() != kGuidSize) {
        throw std::runtime_error("WGS GUID has an invalid size");
    }
    QString result = QStringLiteral("%1%2%3")
                         .arg(readLeU32(bytes, 0), 8, 16, QLatin1Char('0'))
                         .arg(readLeU16(bytes, 4), 4, 16, QLatin1Char('0'))
                         .arg(readLeU16(bytes, 6), 4, 16, QLatin1Char('0'));
    for (qsizetype index = 8; index < bytes.size(); ++index) {
        result += QStringLiteral("%1").arg(
            static_cast<quint8>(bytes.at(index)), 2, 16, QLatin1Char('0'));
    }

    return result.toUpper();
}

QString fixedUtf16String(const QByteArray &bytes) {
    QString result;
    result.reserve(bytes.size() / 2);
    for (qsizetype offset = 0; offset + 1 < bytes.size(); offset += 2) {
        const quint16 codeUnit = readLeU16(bytes, offset);
        if (codeUnit == 0) {
            break;
        }
        result.append(QChar(codeUnit));
    }

    return result;
}

QHash<QString, QString> readDescriptor(const QString &directoryPath, quint8 version) {
    const QString descriptorPath = QDir(directoryPath).filePath(
        QStringLiteral("container.%1").arg(version));
    const QByteArray descriptor = readFile(descriptorPath);
    if (descriptor.size() < kDescriptorHeaderSize
        || readLeU32(descriptor, 0) != kDescriptorVersion) {
        throw std::runtime_error(("unsupported WGS descriptor: " + descriptorPath).toStdString());
    }
    const quint32 fileCount = readLeU32(descriptor, 4);
    const quint64 requiredSize = static_cast<quint64>(kDescriptorHeaderSize)
        + static_cast<quint64>(fileCount) * static_cast<quint64>(kDescriptorEntrySize);
    if (requiredSize != static_cast<quint64>(descriptor.size())) {
        throw std::runtime_error(("invalid WGS descriptor size: " + descriptorPath).toStdString());
    }

    QHash<QString, QString> files;
    for (quint32 index = 0; index < fileCount; ++index) {
        const qsizetype offset = kDescriptorHeaderSize
            + static_cast<qsizetype>(index) * kDescriptorEntrySize;
        const QString fileId = fixedUtf16String(descriptor.mid(offset, kFileIdSize));
        const QString payloadName = guidPathName(
            descriptor.mid(offset + kCurrentGuidOffset, kGuidSize));
        const QString payloadPath = QDir(directoryPath).filePath(payloadName);
        if (fileId.isEmpty() || !QFileInfo(payloadPath).isFile()) {
            throw std::runtime_error(("WGS descriptor references a missing payload: "
                                      + descriptorPath).toStdString());
        }
        files.insert(fileId.toLower(), payloadPath);
    }

    return files;
}

QString firstFile(const QHash<QString, QString> &files, const QStringList &ids) {
    for (const QString &id : ids) {
        const auto found = files.constFind(id.toLower());
        if (found != files.cend()) {
            return found.value();
        }
    }

    return {};
}

} // namespace

bool isWgsSaveDirectory(const QString &path) {
    const QFileInfo info(path);
    return info.isDir()
        && QFileInfo(QDir(info.absoluteFilePath()).filePath(
                         QStringLiteral("containers.index"))).isFile();
}

WgsSave readWgsSave(const QString &path) {
    const QFileInfo rootInfo(path);
    if (!rootInfo.isDir()) {
        throw std::runtime_error("WGS save root is not a directory");
    }
    const QDir root(rootInfo.absoluteFilePath());
    Cursor index(readFile(root.filePath(QStringLiteral("containers.index"))));
    if (index.readU32() != kIndexVersion) {
        throw std::runtime_error("unsupported WGS container index version");
    }
    const quint32 containerCount = index.readU32();
    if (containerCount > kMaximumContainerCount) {
        throw std::runtime_error("WGS container count is unreasonable");
    }
    index.readU32();

    WgsSave save;
    save.packageId = index.readString();
    save.containerCount = static_cast<int>(containerCount);
    index.readU64();
    index.readU32();
    index.readString();
    index.readU64();
    save.assets.reserve(save.containerCount);

    for (quint32 indexNumber = 0; indexNumber < containerCount; ++indexNumber) {
        const QString containerName = index.readString();
        index.readString();
        index.readString();
        const quint8 version = index.readU8();
        index.readU32();
        const QString directoryName = guidPathName(index.readBytes(kGuidSize));
        index.readU64();
        index.readU64();
        index.readU64();

        const QString directoryPath = root.filePath(directoryName);
        const QHash<QString, QString> files = readDescriptor(directoryPath, version);
        const QString liveryPath = firstFile(files, {QStringLiteral("C_livery")});
        const QString groupPath = firstFile(files, {QStringLiteral("C_group")});
        if (liveryPath.isEmpty() && groupPath.isEmpty()) {
            continue;
        }

        WgsAsset asset;
        asset.containerName = containerName;
        asset.payloadPath = !liveryPath.isEmpty() ? liveryPath : groupPath;
        asset.headerPath = firstFile(files, {QStringLiteral("header")});
        asset.thumbnailPath = firstFile(files, {
            QStringLiteral("bigThumb.png"),
            QStringLiteral("thumb.png"),
            QStringLiteral("smallThumb.png"),
            QStringLiteral("bigThumb.webp"),
            QStringLiteral("thumb.webp"),
            QStringLiteral("thumbnail"),
        });
        asset.kind = !liveryPath.isEmpty() ? WgsAssetKind::Livery : WgsAssetKind::Group;
        save.assets.push_back(std::move(asset));
    }

    std::sort(save.assets.begin(), save.assets.end(), [](const WgsAsset &left, const WgsAsset &right) {
        return left.containerName.compare(right.containerName, Qt::CaseInsensitive) < 0;
    });

    return save;
}

} // namespace fls
