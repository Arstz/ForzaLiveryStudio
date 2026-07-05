#pragma once

#include <QHash>
#include <QString>

namespace gui {

// Human-readable shape names sourced from the vision pipeline
// (assets/vector/shape_names.json). Parsed fresh on every launch so edits to
// the JSON take effect the next time the application starts.
class ShapeNameStore {
public:
    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    // Returns the mapped name for the shape, or an empty string when the shape
    // has no entry in the names asset.
    QString name(int shapeId) const;
    bool contains(int shapeId) const;

private:
    QHash<int, QString> names_;
};

} // namespace gui
