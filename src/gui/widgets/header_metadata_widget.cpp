#include "header_metadata_widget.h"

#include <QtCore>
#include <QtWidgets>

#include <utility>

namespace gui {

HeaderMetadataWidget::HeaderMetadataWidget(QWidget *parent)
    : QWidget(parent) {
    auto *outer = new QVBoxLayout(this);

    auto *form = new QFormLayout();
    outer->addLayout(form);

    nameEdit_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Vinyl name"), nameEdit_);
    creatorEdit_ = new QLineEdit(this);
    form->addRow(QStringLiteral("Creator"), creatorEdit_);
    yearSpin_ = new QSpinBox(this);
    yearSpin_->setRange(2000, 2100);
    yearSpin_->setValue(QDate::currentDate().year());
    form->addRow(QStringLiteral("Year"), yearSpin_);
    publishedCheck_ = new QCheckBox(QStringLiteral("Published"), this);
    publishedCheck_->setChecked(false);
    publishedCheck_->setEnabled(false);
    publishedCheck_->setStyleSheet(QStringLiteral("QCheckBox:disabled { color: palette(mid); }"));
    form->addRow(QString(), publishedCheck_);
    auto *descEdit = new QPlainTextEdit(this);
    descEdit->setPlaceholderText(QStringLiteral("Description (published only)"));
    descEdit->setReadOnly(true);
    form->addRow(QStringLiteral("Description"), descEdit);

    auto *targetCarRow = new QWidget(this);
    auto *targetCarLayout = new QHBoxLayout(targetCarRow);
    targetCarLayout->setContentsMargins(0, 0, 0, 0);
    targetCar_ = new QLabel(targetCarRow);
    targetCar_->setWordWrap(true);
    targetCarLayout->addWidget(targetCar_, 1);
    changeTargetCar_ = new QPushButton(QStringLiteral("Change"), targetCarRow);
    targetCarLayout->addWidget(changeTargetCar_);
    form->addRow(QStringLiteral("Target car"), targetCarRow);

    const auto commit = [this]() {
        if (metadataChangedCallback_) {
            metadataChangedCallback_();
        }
    };
    connect(nameEdit_, &QLineEdit::editingFinished, this, commit);
    connect(creatorEdit_, &QLineEdit::editingFinished, this, commit);
    connect(yearSpin_, &QSpinBox::editingFinished, this, commit);
    connect(changeTargetCar_, &QPushButton::clicked, this, [this]() {
        if (changeTargetCarCallback_) {
            changeTargetCarCallback_();
        }
    });

    hint_ = new QLabel(QStringLiteral("Create or open a project first."), this);
    hint_->setWordWrap(true);
    outer->addWidget(hint_);

    outer->addStretch(1);

    setMetadata({}, {}, false, false);
}

void HeaderMetadataWidget::setMetadata(
    const fh6::HeaderMetadata &seed, const QString &targetCar,
    bool hasProject, bool canChangeTargetCar) {
    seed_ = seed;

    nameEdit_->setText(seed.name);
    creatorEdit_->setText(seed.creatorName);
    yearSpin_->setValue(seed.year == 0 ? QDate::currentDate().year() : seed.year);
    publishedCheck_->setChecked(seed.published);
    targetCar_->setText(targetCar.isEmpty() ? QStringLiteral("Not set") : targetCar);

    nameEdit_->setEnabled(hasProject);
    creatorEdit_->setEnabled(hasProject);
    yearSpin_->setEnabled(hasProject);
    changeTargetCar_->setEnabled(hasProject && canChangeTargetCar);
    hint_->setVisible(!hasProject);
}

fh6::HeaderMetadata HeaderMetadataWidget::metadata() const {
    fh6::HeaderMetadata meta = seed_;
    meta.name = nameEdit_->text();
    meta.creatorName = creatorEdit_->text();
    meta.year = static_cast<quint16>(yearSpin_->value());
    meta.published = false;
    meta.description.clear();
    return meta;
}

void HeaderMetadataWidget::setMetadataChangedCallback(std::function<void()> callback) {
    metadataChangedCallback_ = std::move(callback);
}

void HeaderMetadataWidget::setChangeTargetCarCallback(std::function<void()> callback) {
    changeTargetCarCallback_ = std::move(callback);
}

} // namespace gui
