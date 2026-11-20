#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

class QWidget;

namespace gui {

class CarRegistry {
public:
    struct Entry {
        int id = 0;
        QString name;
        QString version;
        QString manufacturer;
        QString modelName;
    };

    struct ModelSource {
        QString path;
        QString game;
    };

    struct ModelAlternative {
        QString name;
        ModelSource source;
    };

    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    QString name(int id) const;
    QString modelCode(int id, const QString &game = {}) const;
    QString version(int id) const;
    int year(int id) const;
    bool isFh6Supported(int id) const;
    bool isFh6Unsupported(int id) const;
    bool isSupported(int id, const QString &game) const;
    ModelSource findModel(int id, const QMap<QString, QString> &gameFolders,
                          const QString &preferredGame = {}) const;
    ModelAlternative findClosestModel(int id, const QMap<QString, QString> &gameFolders,
                                      const QString &preferredGame) const;
    QHash<int, QStringList> modelAvailability(const QMap<QString, QString> &gameFolders) const;
    QString displayName(int id) const;
    const QVector<Entry> &entries() const { return sorted_; }
    bool isEmpty() const { return names_.isEmpty(); }

private:
    QHash<int, QString> names_;
    QHash<int, QString> models_;
    QHash<int, QHash<QString, QString>> gameModels_;
    QHash<int, QString> versions_;
    QHash<int, int> years_;
    QVector<Entry> sorted_;
};

const CarRegistry &sharedCarRegistry();

enum class MissingCarModelChoice { Cancel, Continue, LoadClosest };

MissingCarModelChoice chooseMissingCarModel(
    QWidget *parent, int carId, const QString &targetGame, const QString &alternativeModel = {});
bool chooseCarModel(QWidget *parent, int currentId, int *outId, bool confirmMissingModel = false);

} // namespace gui
