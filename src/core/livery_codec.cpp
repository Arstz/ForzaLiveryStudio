#include "livery_codec.h"

#include "binary_io.h"
#include "cgroup_codec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <stdexcept>

namespace fh6 {
namespace {

QString resolveLiveryPath(const QString &folderOrFile)
{
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        return QDir(folderOrFile).filePath(QStringLiteral("C_livery"));
    }
    return folderOrFile;
}

} // namespace

LiveryPayload readLiveryPayload(const QString &folderOrFile)
{
    QFile file(resolveLiveryPath(folderOrFile));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(("could not open C_livery: " + file.fileName()).toStdString());
    }

    LiveryPayload payload;
    payload.raw = inflateContainer(file.readAll());

    const int gyvl = payload.raw.indexOf(QByteArray("gyvl", 4));
    if (gyvl < 0) {
        throw std::runtime_error("C_livery has no embedded gyvl chunk");
    }
    payload.gyvlOffset = gyvl;

    // The embedded gyvl section stream starts at gyvl-rel 0x15 and runs until the
    // next chunk (the trailing yrvl section table).
    const int bodyStart = gyvl + 0x15;
    int bodyEnd = payload.raw.indexOf(QByteArray("yrvl", 4), gyvl);
    if (bodyEnd < 0 || bodyEnd < bodyStart) {
        bodyEnd = payload.raw.size();
    }
    if (bodyStart >= payload.raw.size()) {
        throw std::runtime_error("C_livery gyvl body is truncated");
    }
    payload.body = payload.raw.mid(bodyStart, bodyEnd - bodyStart);

    // The yrvl "stats" chunk directly after gyvl holds 11 u32 per-section decal
    // counts (storage order), starting right after its 4-byte tag.
    const int statsTag = bodyEnd;  // first yrvl after gyvl == bodyEnd
    payload.sectionCounts.reserve(11);
    if (statsTag >= 0 && payload.raw.mid(statsTag, 4) == QByteArray("yrvl", 4)) {
        for (int i = 0; i < 11; ++i) {
            const int off = statsTag + 4 + i * 4;
            if (off + 4 <= payload.raw.size()) {
                payload.sectionCounts.push_back(static_cast<int>(detail::readLeU32(payload.raw, off)));
            } else {
                payload.sectionCounts.push_back(0);
            }
        }
    } else {
        for (int i = 0; i < 11; ++i) {
            payload.sectionCounts.push_back(0);
        }
    }
    return payload;
}

} // namespace fh6
