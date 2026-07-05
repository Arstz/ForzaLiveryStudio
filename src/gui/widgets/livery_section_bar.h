#pragma once

#include <QListWidget>
#include <QString>
#include <QVector>

namespace gui {

// The C_livery section selector ("tabs") shown above the layer tree. Hidden
// unless a livery is loaded. Presentation only: the owner feeds it the section
// list via setSections() and reacts to sectionActivated() to swap the tree and
// canvas contents.
class LiverySectionBar final : public QListWidget {
    Q_OBJECT

public:
    struct SectionInfo {
        QString id;
        QString name;
        int decalCount = 0;
    };

    explicit LiverySectionBar(QWidget *parent = nullptr);
    void refreshTheme();

    // Replaces the section list. Greys out empty sections, hides the bar when
    // empty, and selects the first populated section (which emits
    // sectionActivated). Pass an empty list to clear and hide.
    void setSections(const QVector<SectionInfo> &sections);

Q_SIGNALS:
    void sectionActivated(const QString &sectionId);

private:
    bool switching_ = false; // re-entrancy guard while repopulating
};

} // namespace gui
