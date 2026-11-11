#include "zip_extract.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <zlib.h>

#include <future>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void writeArchive(const QString &path, const QByteArray &data, quint16 method = 0,
                  const QByteArray &compressed = {}) {
    const QByteArray name("entry.bin");
    const QByteArray payload = method == 0 ? data : compressed;
    const quint32 size = static_cast<quint32>(data.size());
    const quint32 compressedSize = static_cast<quint32>(payload.size());
    const quint32 crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), size);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(0x04034b50) << quint16(20) << quint16(0) << method
           << quint16(0) << quint16(0) << crc << compressedSize << size << quint16(name.size()) << quint16(0);
    stream.writeRawData(name.constData(), name.size());
    stream.writeRawData(payload.constData(), payload.size());
    const quint32 directory = bytes.size();
    stream << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0)
           << method << quint16(0) << quint16(0) << crc << compressedSize << size
           << quint16(name.size()) << quint16(0) << quint16(0) << quint16(0)
           << quint16(0) << quint32(0) << quint32(0);
    stream.writeRawData(name.constData(), name.size());
    const quint32 directorySize = bytes.size() - directory;
    stream << quint32(0x06054b50) << quint16(0) << quint16(0) << quint16(1)
           << quint16(1) << directorySize << directory << quint16(0);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "archive creation");
    require(file.write(bytes) == bytes.size(), "archive write");
}

QByteArray entry(const QString &directory) {
    QFile file(QDir(directory).filePath(QStringLiteral("entry.bin")));
    require(file.open(QIODevice::ReadOnly), "extracted entry read");
    return file.readAll();
}

void testLeases(const QString &path) {
    writeArchive(path, "first");
    QString error;
    bool reused = true;
    auto first = fls::sharedZipExtraction(path, &error, &reused);
    require(first && !reused && entry(first->path()) == "first", "initial extraction");
    auto second = fls::sharedZipExtraction(path, &error, &reused);
    require(second == first && reused, "unchanged archive was extracted twice");
    const QString oldDirectory = first->path();
    writeArchive(path, "updated payload");
    auto updated = fls::sharedZipExtraction(path, &error, &reused);
    require(updated && !reused && updated != first, "changed archive reused stale extraction");
    require(entry(updated->path()) == "updated payload", "updated archive contents");
    require(entry(oldDirectory) == "first", "live extraction changed underneath its lease");
    first.reset();
    require(QDir(oldDirectory).exists(), "shared extraction removed while still in use");
    second.reset();
    require(!QDir(oldDirectory).exists(), "released extraction retained on disk");
    const QString updatedDirectory = updated->path();
    updated.reset();
    require(!QDir(updatedDirectory).exists(), "updated extraction retained after release");
}

void testConcurrentLoads(const QString &path) {
    writeArchive(path, "concurrent");
    std::vector<std::future<std::shared_ptr<QTemporaryDir>>> requests;
    std::vector<std::shared_ptr<QTemporaryDir>> leases;
    for (int index = 0; index < 8; ++index) {
        requests.push_back(std::async(std::launch::async, [&path]() {
            return fls::sharedZipExtraction(path);
        }));
    }
    for (auto &request : requests) {
        leases.push_back(request.get());
        require(leases.back() && leases.back() == leases.front(), "concurrent loads duplicated extraction");
    }
    const QString directory = leases.front()->path();
    require(entry(directory) == "concurrent", "concurrent entry contents");
    leases.clear();
    require(!QDir(directory).exists(), "concurrent extraction retained after release");
}

void testLzxArchives(const QString &path) {
    const QByteArray source = QByteArray("ABCD").repeated(25000);
    QFile fixtureFile(QStringLiteral(FLS_SOURCE_DIR "/tools/tests/zip_extract/lzx_frames.json"));
    require(fixtureFile.open(QIODevice::ReadOnly), "Could not read LZX fixtures");
    const QJsonObject fixtures = QJsonDocument::fromJson(fixtureFile.readAll()).object();
    require(fixtures.size() == 4, "Missing LZX window-size fixtures");
    QString error;
    for (auto fixture = fixtures.begin(); fixture != fixtures.end(); ++fixture) {
        const QByteArray compressed = QByteArray::fromBase64(fixture.value().toString().toLatin1());
        writeArchive(path, source, 21, compressed);
        require(fls::readZipEntry(path, QStringLiteral("ENTRY.BIN"), &error) == source, qPrintable(error));
        require(fls::readZipEntries(path, {QStringLiteral("entry.bin")}, &error).value(QStringLiteral("entry.bin")) == source,
                "LZX selective extraction failed");
        const auto extracted = fls::sharedZipExtraction(path, &error);
        require(extracted && entry(extracted->path()) == source, qPrintable(error));
        for (const QByteArray &trailing : {QByteArray(2, '\0'), QByteArray(5, '\0'), QByteArray::fromHex("ff00000000")}) {
            writeArchive(path, source, 21, compressed + trailing);
            require(fls::readZipEntry(path, QStringLiteral("entry.bin"), &error) == source, "LZX terminator rejected");
        }
        for (const QByteArray &invalid : {compressed.chopped(1), compressed + QByteArray(1, 'x')}) {
            QTemporaryDir destination;
            writeArchive(path, source, 21, invalid);
            error.clear();
            require(fls::readZipEntries(path, {QStringLiteral("entry.bin")}, &error).isEmpty() && !error.isEmpty(),
                    "Malformed LZX frames were accepted by entry reads");
            require(!fls::extractZipArchive(path, destination.path(), &error), "Malformed LZX frames were extracted");
        }
        QByteArray wrongSource = source;
        wrongSource[0] = 'x';
        writeArchive(path, wrongSource, 21, compressed);
        error.clear();
        require(fls::readZipEntries(path, {QStringLiteral("entry.bin")}, &error).isEmpty() && !error.isEmpty(),
                "LZX CRC mismatch was accepted");
    }
    writeArchive(path, {}, 21);
    const auto empty = fls::readZipEntries(path, {QStringLiteral("entry.bin")}, &error);
    require(empty.contains(QStringLiteral("entry.bin")) && empty.value(QStringLiteral("entry.bin")).isEmpty(),
            "Empty LZX entry failed");
}

void testDeflateArchive(const QString &path) {
    const QByteArray source = QByteArray("Deflate content").repeated(100);
    QByteArray compressed(compressBound(source.size()), Qt::Uninitialized);
    z_stream stream{};
    require(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK,
            "Could not create deflate fixture");
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(source.constData()));
    stream.avail_in = source.size();
    stream.next_out = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_out = compressed.size();
    require(deflate(&stream, Z_FINISH) == Z_STREAM_END, "Could not compress deflate fixture");
    compressed.resize(stream.total_out);
    deflateEnd(&stream);
    writeArchive(path, source, 8, compressed);
    QString error;
    require(fls::readZipEntry(path, QStringLiteral("entry.bin"), &error) == source, "Deflate entry read regressed");
    const auto extracted = fls::sharedZipExtraction(path, &error);
    require(extracted && entry(extracted->path()) == source, "Deflate extraction regressed");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    try {
        require(directory.isValid(), "test directory creation");
        testLeases(QDir(directory.path()).filePath(QStringLiteral("leases.zip")));
        testConcurrentLoads(QDir(directory.path()).filePath(QStringLiteral("concurrent.zip")));
        testLzxArchives(QDir(directory.path()).filePath(QStringLiteral("lzx.zip")));
        testDeflateArchive(QDir(directory.path()).filePath(QStringLiteral("deflate.zip")));
        qInfo("Shared ZIP extraction checks passed");
        return 0;
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
