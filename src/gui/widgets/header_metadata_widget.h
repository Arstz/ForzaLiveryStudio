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

class HeaderMetadataWidget final : public QWidget {
public:
    explicit HeaderMetadataWidget(QWidget *parent = nullptr);

    void setMetadata(const fls::HeaderMetadata &seed, bool importedDraft, bool hasProject);

    fls::HeaderMetadata metadata() const;

    bool rebuildRequested() const;

    void setApplyCallback(std::function<void()> callback);

private:
    fls::HeaderMetadata seed_;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *creatorEdit_ = nullptr;
    QSpinBox *yearSpin_ = nullptr;
    QCheckBox *publishedCheck_ = nullptr;
    QCheckBox *rebuildCheck_ = nullptr;
    QPushButton *applyButton_ = nullptr;
    QLabel *hint_ = nullptr;
    std::function<void()> applyCallback_;
    bool importedDraft_ = false;
};

} // namespace gui
