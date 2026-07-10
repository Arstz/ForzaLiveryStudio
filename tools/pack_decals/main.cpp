#include "binary_io.h"
#include "swatchbin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cstdio>

namespace {

struct DecalImage {
    quint32 id = 0;
    quint32 width = 0;
    quint32 height = 0;
    QByteArray rgba;
};

void appendBytes(QByteArray &out, const char *bytes, int size)
{
    out.append(bytes, size);
}

bool parseDecalId(const QString &fileName, quint32 *id)
{
    static const QRegularExpression re(QStringLiteral("^decal(\\d+)\\.swatchbin$"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(fileName);
    if (!match.hasMatch()) {
        return false;
    }
    bool ok = false;
    const uint value = match.captured(1).toUInt(&ok);
    if (!ok) {
        return false;
    }
    *id = value;
    return true;
}

QByteArray buildPack(const QVector<DecalImage> &images)
{
    constexpr quint32 kVersion = 1;
    constexpr quint32 kEntrySize = 24;
    constexpr quint32 kHeaderSize = 16;
    const quint32 count = static_cast<quint32>(images.size());
    quint32 dataOffset = kHeaderSize + count * kEntrySize;

    QByteArray out;
    out.reserve(static_cast<int>(dataOffset));
    appendBytes(out, "FH6RAST1", 8);
    fh6::detail::appendLeU32(out, kVersion);
    fh6::detail::appendLeU32(out, count);

    QVector<quint32> offsets;
    offsets.reserve(images.size());
    for (const DecalImage &image : images) {
        offsets.push_back(dataOffset);
        dataOffset += static_cast<quint32>(image.rgba.size());
    }

    for (int i = 0; i < images.size(); ++i) {
        const DecalImage &image = images[i];
        fh6::detail::appendLeU32(out, image.id);
        fh6::detail::appendLeU32(out, image.width);
        fh6::detail::appendLeU32(out, image.height);
        fh6::detail::appendLeU32(out, offsets[i]);
        fh6::detail::appendLeU32(out, static_cast<quint32>(image.rgba.size()));
        fh6::detail::appendLeU32(out, 0); // RGBA8, top-left origin
    }

    for (const DecalImage &image : images) {
        out.append(image.rgba);
    }
    return out;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() != 3) {
        std::fprintf(stderr, "usage: fh6_pack_decals <textures-folder> <output-bin>\n");
        return 2;
    }

    const QDir inputDir(args[1]);
    if (!inputDir.exists()) {
        std::fprintf(stderr, "input folder does not exist: %s\n", qPrintable(args[1]));
        return 1;
    }

    QVector<QFileInfo> files;
    for (const QFileInfo &info : inputDir.entryInfoList(QStringList{QStringLiteral("decal*.swatchbin")},
                                                        QDir::Files, QDir::Name)) {
        quint32 id = 0;
        if (parseDecalId(info.fileName(), &id)) {
            files.push_back(info);
        }
    }
    std::sort(files.begin(), files.end(), [](const QFileInfo &a, const QFileInfo &b) {
        quint32 aid = 0;
        quint32 bid = 0;
        parseDecalId(a.fileName(), &aid);
        parseDecalId(b.fileName(), &bid);
        return aid < bid;
    });

    QVector<DecalImage> images;
    images.reserve(files.size());
    for (const QFileInfo &info : files) {
        quint32 id = 0;
        parseDecalId(info.fileName(), &id);
        QString error;
        const fh6::SwatchImage image = fh6::loadSwatchImage(info.absoluteFilePath(), &error);
        if (!image.valid()) {
            std::fprintf(stderr, "failed to decode %s: %s\n",
                         qPrintable(info.fileName()),
                         qPrintable(error.isEmpty() ? QStringLiteral("unknown error") : error));
            return 1;
        }
        DecalImage decal;
        decal.id = id;
        decal.width = static_cast<quint32>(image.width);
        decal.height = static_cast<quint32>(image.height);
        decal.rgba = QByteArray(reinterpret_cast<const char *>(image.rgba.data()),
                                static_cast<int>(image.rgba.size()));
        images.push_back(std::move(decal));
    }

    const QByteArray pack = buildPack(images);
    QFileInfo outputInfo(args[2]);
    QDir().mkpath(outputInfo.absolutePath());
    QFile output(args[2]);
    if (!output.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "cannot open output: %s\n", qPrintable(args[2]));
        return 1;
    }
    if (output.write(pack) != pack.size()) {
        std::fprintf(stderr, "failed to write output: %s\n", qPrintable(args[2]));
        return 1;
    }

    std::printf("packed %lld decals into %s (%lld bytes)\n",
                static_cast<long long>(images.size()),
                qPrintable(args[2]),
                static_cast<long long>(pack.size()));
    return 0;
}
