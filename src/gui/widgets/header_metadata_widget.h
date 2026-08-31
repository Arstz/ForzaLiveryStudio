#pragma once

#include "header_codec.h"

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace gui {

class HeaderMetadataWidget final : public QWidget {
public:
    explicit HeaderMetadataWidget(QWidget *parent = nullptr);

    void setMetadata(const fls::HeaderMetadata &seed, const QString &targetCar,
                     bool hasProject, bool canChangeTargetCar,
                     bool targetCarSupported = true);

    fls::HeaderMetadata metadata() const;

    void setMetadataChangedCallback(std::function<void()> callback);
    void setChangeTargetCarCallback(std::function<void()> callback);

private:
    fls::HeaderMetadata seed_;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *creatorEdit_ = nullptr;
    QSpinBox *yearSpin_ = nullptr;
    QCheckBox *publishedCheck_ = nullptr;
    QPlainTextEdit *descriptionEdit_ = nullptr;
    QLabel *targetCar_ = nullptr;
    QPushButton *changeTargetCar_ = nullptr;
    QLabel *hint_ = nullptr;
    std::function<void()> metadataChangedCallback_;
    std::function<void()> changeTargetCarCallback_;
};

} // namespace gui
