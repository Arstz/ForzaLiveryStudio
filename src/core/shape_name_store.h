#pragma once

#include <QHash>
#include <QString>

namespace fls {

class ShapeNameStore {
public:
    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    QString name(int shapeId) const;
    bool contains(int shapeId) const;

private:
    QHash<int, QString> names_;
};

QString friendlyShapeLayerName(quint16 shapeId);

} // namespace fls
