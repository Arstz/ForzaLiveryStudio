#pragma once

#include <QListWidget>
#include <QString>
#include <QVector>

namespace gui {

class LiverySectionBar final : public QListWidget {
    Q_OBJECT

public:
    struct SectionInfo {
        QString id;
        QString name;
        int decalCount = 0;
        bool overShapeLimit = false;
    };

    explicit LiverySectionBar(QWidget *parent = nullptr);
    void refreshTheme();

    void setSections(const QVector<SectionInfo> &sections);

Q_SIGNALS:
    void sectionActivated(const QString &sectionId);

private:
    bool switching_ = false;
};

} // namespace gui
