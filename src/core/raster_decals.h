#pragma once

#include <QByteArray>
#include <QHash>
#include <QSize>
#include <QString>
#include <QVector>

namespace fls {

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
    QVector<quint32> ids(const QString &game) const;
    quint32 textureKey(quint32 id, const QString &game = {}) const;
    QSize decalSize(quint32 id, const QString &game = {}) const;
    RasterDecal decal(quint32 id, const QString &game = {}) const;

private:
    struct Entry {
        QSize size;
        quint32 offset = 0;
        quint32 length = 0;
    };

    const Entry *entry(quint32 id, const QString &game) const;

    QByteArray data_;
    QVector<Entry> entries_;
    QHash<QString, QHash<quint32, quint32>> games_;
    QString path_;
    bool loaded_ = false;
    bool webp_ = false;
};

const RasterDecalPack &sharedRasterDecals();

} // namespace fls
