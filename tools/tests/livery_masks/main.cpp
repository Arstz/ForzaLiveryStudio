#include "livery_masks.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    require(file.write(bytes) == bytes.size(), "Could not write mask fixture");
}

QByteArray maskSwatch(int width, int height) {
    constexpr int metadata = 44;
    constexpr int header = metadata + 8;
    constexpr int pixels = header + 68;
    const int size = width * height;
    QByteArray bytes(pixels + size, '\0');
    const auto u16 = [&](int offset, quint16 value) { qToLittleEndian(value, bytes.data() + offset); };
    const auto u32 = [&](int offset, quint32 value) { qToLittleEndian(value, bytes.data() + offset); };
    u32(0, 0x47727562);
    bytes[4] = bytes[5] = 1;
    u32(16, 1);
    u32(20, 0x54584342);
    u16(26, 1);
    u32(28, metadata);
    u32(32, pixels);
    u32(36, size);
    u32(40, size);
    u32(metadata, 0x54584348);
    u16(metadata + 6, 8);
    u32(header + 24, width);
    u32(header + 28, height);
    u32(header + 56, 64);
    u32(header + 64, 19);
    bytes[pixels + 9 * width + 7] = static_cast<char>(255);

    return bytes;
}

void checkCanvas(int width, int height, int expectedWidth, int expectedHeight,
                 float expectedLeft, float expectedTop, float expectedOriginX, float expectedOriginY) {
    QTemporaryDir directory;
    QString error;
    require(directory.isValid(), "Could not create mask fixture directory");
    writeFile(QDir(directory.path()).filePath(QStringLiteral("Masks.xml")),
        "<LiveryMasks><Front valid=\"true\" left=\"-400\" right=\"400\" top=\"200\" bottom=\"-200\" "
        "xorigin=\"32\" yorigin=\"10\" xAxis=\"-x\" yAxis=\"+y\" /></LiveryMasks>");
    writeFile(QDir(directory.path()).filePath(QStringLiteral("front.swatchbin")), maskSwatch(width, height));
    const auto masks = fls::loadLiveryMasks(directory.path(), &error);
    const auto &side = masks.sides[0];
    require(masks.valid() && masks.loadedMasks == 1 && side.valid, qPrintable(error));
    require(side.mask.width == expectedWidth && side.mask.height == expectedHeight, "Mask canvas dimensions mismatch");
    require(side.left == expectedLeft && side.top == expectedTop, "Mask region translation mismatch");
    require(side.xOrigin == expectedOriginX && side.yOrigin == expectedOriginY, "Mask origin translation mismatch");
    require(side.right - side.left == 800 && side.top - side.bottom == 400, "Mask region was stretched");
    require(side.mask.at(7, 9) == 255 && side.mask.at(14, 9) == 0, "Mask pixels were resampled");
    require(std::count(side.mask.coverage.begin(), side.mask.coverage.end(), 255) == 1, "Mask coverage changed");
    require(side.mask.at(expectedWidth - 1, expectedHeight - 1) == 0, "Mask padding contains coverage");
}

void checkArchive(const QString &path) {
    QTemporaryDir directory;
    QString error;
    QStringList wanted;
    require(directory.isValid(), "Could not create archive mask directory");
    const auto entries = fls::listZipEntries(path, &error);
    require(!entries.isEmpty(), qPrintable(error));
    for (const QString &entry : entries) {
        if (entry.contains(QStringLiteral("LiveryMasks/"), Qt::CaseInsensitive)
            && (entry.endsWith(QStringLiteral(".swatchbin"), Qt::CaseInsensitive)
                || entry.endsWith(QStringLiteral("Masks.xml"), Qt::CaseInsensitive))) wanted.push_back(entry);
    }
    const auto files = fls::readZipEntries(path, wanted, &error);
    require(files.size() == wanted.size(), qPrintable(error));
    for (auto file = files.cbegin(); file != files.cend(); ++file) {
        const QString filename = QFileInfo(file.key()).fileName();
        writeFile(QDir(directory.path()).filePath(filename.compare(QStringLiteral("Masks.xml"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("Masks.xml") : filename), file.value());
    }
    const auto masks = fls::loadLiveryMasks(directory.path(), &error);
    require(masks.valid() && masks.loadedMasks > 0, qPrintable(error));
    for (const auto &side : masks.sides) {
        if (!side.mask.valid()) continue;
        QByteArray source;
        for (auto file = files.cbegin(); file != files.cend(); ++file) {
            if (QFileInfo(file.key()).completeBaseName().compare(side.maskName, Qt::CaseInsensitive) == 0) source = file.value();
        }
        const auto native = fls::decodeSwatchMask(source, &error);
        require(native.valid(), qPrintable(error));
        require(side.mask.width >= 2048 && side.mask.height >= 1024, "Archive retained a smaller mask canvas");
        for (int row = 0; row < native.height; ++row) {
            require(std::equal(native.coverage.begin() + static_cast<size_t>(row) * native.width,
                native.coverage.begin() + static_cast<size_t>(row + 1) * native.width,
                side.mask.coverage.begin() + static_cast<size_t>(row) * side.mask.width), "Archive mask pixel spacing changed");
        }
    }
    std::printf("Verified %d masks: %s\n", masks.loadedMasks, qPrintable(path));
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        checkCanvas(1024, 1024, 2048, 1024, -912, 200, -480, 10);
        checkCanvas(2048, 1024, 2048, 1024, -400, 200, 32, 10);
        checkCanvas(1024, 512, 2048, 1024, -912, 456, -480, 266);
        checkCanvas(4096, 1024, 4096, 1024, -400, 200, 32, 10);
        for (int index = 1; index < argc; ++index) checkArchive(QString::fromLocal8Bit(argv[index]));
        std::puts("Mask canvas dimensions, pixel spacing, and region origins passed");

        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());

        return 1;
    }
}
