#pragma once

#include "header_codec.h"

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace gui {

// Detachable editor panel for a project's header metadata. Lives inside a
// QDockWidget so it can be floated, redocked, hidden, or shown like the other
// side panels, replacing the old modal dialog. Seed it with setMetadata()
// whenever the project changes; the owner installs an apply callback to write
// the edited values back.
class HeaderMetadataWidget final : public QWidget {
public:
    explicit HeaderMetadataWidget(QWidget *parent = nullptr);

    // Populate the fields from the project's current metadata. importedDraft
    // controls whether the "rebuild from these fields" option is offered. When
    // hasProject is false the editor is disabled and shows a hint.
    void setMetadata(const fh6::HeaderMetadata &seed, bool importedDraft, bool hasProject);

    // The seeded metadata with edited name/creator/year applied; published is
    // forced false and the description cleared (published vinyls are read-only).
    fh6::HeaderMetadata metadata() const;

    // True when the user opted to rebuild the header from these fields, dropping
    // the original imported bytes. Always false when there were no imported bytes.
    bool rebuildRequested() const;

    // Invoked when the user clicks Apply.
    void setApplyCallback(std::function<void()> callback);

private:
    fh6::HeaderMetadata seed_;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *creatorEdit_ = nullptr;
    QSpinBox *yearSpin_ = nullptr;
    QCheckBox *rebuildCheck_ = nullptr;
    QPushButton *applyButton_ = nullptr;
    QLabel *hint_ = nullptr;
    std::function<void()> applyCallback_;
    bool importedDraft_ = false;
};

} // namespace gui
