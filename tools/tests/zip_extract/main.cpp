#include "zip_extract.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
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

void writeArchive(const QString &path, const QByteArray &data) {
    const QByteArray name("entry.bin");
    const quint32 size = static_cast<quint32>(data.size());
    const quint32 crc = crc32(0, reinterpret_cast<const Bytef *>(data.constData()), size);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(0x04034b50) << quint16(20) << quint16(0) << quint16(0)
           << quint16(0) << quint16(0) << crc << size << size << quint16(name.size()) << quint16(0);
    stream.writeRawData(name.constData(), name.size());
    stream.writeRawData(data.constData(), data.size());
    const quint32 directory = bytes.size();
    stream << quint32(0x02014b50) << quint16(20) << quint16(20) << quint16(0)
           << quint16(0) << quint16(0) << quint16(0) << crc << size << size
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

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    try {
        require(directory.isValid(), "test directory creation");
        testLeases(QDir(directory.path()).filePath(QStringLiteral("leases.zip")));
        testConcurrentLoads(QDir(directory.path()).filePath(QStringLiteral("concurrent.zip")));
        qInfo("Shared ZIP extraction checks passed");
        return 0;
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
