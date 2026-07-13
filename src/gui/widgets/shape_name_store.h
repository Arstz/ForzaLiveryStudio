#pragma once

#include <QHash>
#include <QString>

namespace gui {

class ShapeNameStore {
public:
    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    QString name(int shapeId) const;
    bool contains(int shapeId) const;

private:
    QHash<int, QString> names_;
};

} // namespace gui
