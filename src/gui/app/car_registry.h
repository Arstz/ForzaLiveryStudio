#pragma once

#include <QHash>
#include <QString>
#include <QVector>

class QWidget;

namespace gui {

// Maps FH6 car ids (the u32 a C_livery stores at vlrc 0x10, and each car's
// .carbin stores before its name) to car model names. Loaded once from
// assets/cars/car_ids.json, mirroring ShapeNameStore's asset-path resolution.
class CarRegistry {
public:
    struct Entry {
        int id = 0;
        QString name;  // model name, e.g. "GMC_Syclone_92"
    };

    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    // Friendly display name for an id (e.g. "Toyota 2000GT 69"), or empty when unknown.
    QString name(int id) const;
    // Model code for an id (e.g. "TOY_2000GT_69") — this is how car model files and
    // folders are named on disk. Falls back to the friendly name when no code is stored.
    QString modelCode(int id) const;
    // User-facing label: the model name, or "Unknown car (#id)" for an unknown
    // non-zero id, or an empty string for id 0 (unset).
    QString displayName(int id) const;
    // All entries, sorted by name (case-insensitive) — for the picker.
    const QVector<Entry> &entries() const { return sorted_; }
    bool isEmpty() const { return names_.isEmpty(); }

private:
    QHash<int, QString> names_;
    QHash<int, QString> models_;  // id -> on-disk model code (the "model" field)
    QVector<Entry> sorted_;
};

// Process-wide lazily-loaded registry, shared by the project details view and
// the new-livery car picker.
const CarRegistry &sharedCarRegistry();

// Modal car picker. Returns true and writes the chosen car id to *outId when the
// user confirms a selection; returns false if cancelled. `currentId` preselects
// a row (0 for none).
bool chooseCarModel(QWidget *parent, int currentId, int *outId);

} // namespace gui
