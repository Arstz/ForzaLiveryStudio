#pragma once

#include <QByteArray>
#include <QHash>
#include <QSize>
#include <QString>
#include <QVector>

namespace fh6 {

struct RasterDecal {
    quint32 id = 0;
    int width = 0;
    int height = 0;
    QByteArray rgba;

    bool valid() const { return id != 0 && width > 0 && height > 0 && !rgba.isEmpty(); }
};

class RasterDecalPack {
public:
    bool load(const QString &path, QString *error = nullptr);
    bool isLoaded() const { return loaded_; }
    QString path() const { return path_; }
    QVector<quint32> ids() const;
    QSize decalSize(quint32 id) const;
    RasterDecal decal(quint32 id) const;

private:
    struct Entry {
        quint32 id = 0;
        quint32 width = 0;
        quint32 height = 0;
        quint32 offset = 0;
        quint32 size = 0;
        quint32 flags = 0;
    };

    QByteArray data_;
    QHash<quint32, Entry> entries_;
    QString path_;
    bool loaded_ = false;
};

const RasterDecalPack &sharedRasterDecals();

} // namespace fh6
