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

    void setMetadata(const fh6::HeaderMetadata &seed, const QString &targetCar,
                     bool hasProject, bool canChangeTargetCar);

    fh6::HeaderMetadata metadata() const;

    void setMetadataChangedCallback(std::function<void()> callback);
    void setChangeTargetCarCallback(std::function<void()> callback);

private:
    fh6::HeaderMetadata seed_;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *creatorEdit_ = nullptr;
    QSpinBox *yearSpin_ = nullptr;
    QCheckBox *publishedCheck_ = nullptr;
    QLabel *targetCar_ = nullptr;
    QPushButton *changeTargetCar_ = nullptr;
    QLabel *hint_ = nullptr;
    std::function<void()> metadataChangedCallback_;
    std::function<void()> changeTargetCarCallback_;
};

} // namespace gui
