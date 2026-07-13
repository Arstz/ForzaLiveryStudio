#pragma once

#include <QHash>
#include <QString>
#include <QVector>

class QWidget;

namespace gui {

class CarRegistry {
public:
    struct Entry {
        int id = 0;
        QString name;
    };

    bool loadDefault(QString *error = nullptr);
    bool loadFromFile(const QString &path, QString *error = nullptr);

    QString name(int id) const;
    QString modelCode(int id) const;
    QString displayName(int id) const;
    const QVector<Entry> &entries() const { return sorted_; }
    bool isEmpty() const { return names_.isEmpty(); }

private:
    QHash<int, QString> names_;
    QHash<int, QString> models_;
    QVector<Entry> sorted_;
};

const CarRegistry &sharedCarRegistry();

bool chooseCarModel(QWidget *parent, int currentId, int *outId);

} // namespace gui
