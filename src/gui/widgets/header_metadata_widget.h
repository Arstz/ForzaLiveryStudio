#pragma once

#include "header_codec.h"

#include <QWidget>

#include <functional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTextEdit;

namespace gui {

class HeaderMetadataWidget final : public QWidget {
public:
    explicit HeaderMetadataWidget(QWidget *parent = nullptr);

    void setMetadata(const fls::HeaderMetadata &seed, const QString &targetCar,
                     bool hasProject, bool canChangeTargetCar);
    void setTargetCarLabel(const QString &targetCar);

    fls::HeaderMetadata metadata() const;

    void setMetadataChangedCallback(std::function<void()> callback);
    void setChangeTargetCarCallback(std::function<void()> callback);

private:
    void updateDescriptionHeight();

    fls::HeaderMetadata seed_;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *creatorEdit_ = nullptr;
    QSpinBox *yearSpin_ = nullptr;
    QSpinBox *monthSpin_ = nullptr;
    QCheckBox *publishedCheck_ = nullptr;
    QTextEdit *descriptionEdit_ = nullptr;
    QLabel *targetCar_ = nullptr;
    QPushButton *changeTargetCar_ = nullptr;
    QLabel *hint_ = nullptr;
    std::function<void()> metadataChangedCallback_;
    std::function<void()> changeTargetCarCallback_;
};

} // namespace gui
