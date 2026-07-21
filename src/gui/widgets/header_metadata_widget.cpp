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
    auto *publishedCheck = new QCheckBox(QStringLiteral("Published"), this);
    publishedCheck->setChecked(false);
    publishedCheck->setEnabled(false);
    form->addRow(QString(), publishedCheck);
    auto *descEdit = new QPlainTextEdit(this);
    descEdit->setPlaceholderText(QStringLiteral("Description (published only)"));
    descEdit->setReadOnly(true);
    form->addRow(QStringLiteral("Description"), descEdit);

    rebuildCheck_ = new QCheckBox(QStringLiteral("Rebuild header from these fields (drops original bytes)"), this);
    rebuildCheck_->setVisible(false);
    form->addRow(QString(), rebuildCheck_);

    applyButton_ = new QPushButton(QStringLiteral("Apply"), this);
    outer->addWidget(applyButton_);
    connect(applyButton_, &QPushButton::clicked, this, [this]() {
        if (applyCallback_) {
            applyCallback_();
        }
    });

    hint_ = new QLabel(QStringLiteral("Create or open a project first."), this);
    hint_->setWordWrap(true);
    outer->addWidget(hint_);

    outer->addStretch(1);

    setMetadata({}, false, false);
}

void HeaderMetadataWidget::setMetadata(const fh6::HeaderMetadata &seed, bool importedDraft, bool hasProject) {
    seed_ = seed;
    importedDraft_ = importedDraft;

    nameEdit_->setText(seed.name);
    creatorEdit_->setText(seed.creatorName);
    yearSpin_->setValue(seed.year == 0 ? QDate::currentDate().year() : seed.year);

    rebuildCheck_->setVisible(importedDraft);
    if (!importedDraft) {
        rebuildCheck_->setChecked(false);
    }

    nameEdit_->setEnabled(hasProject);
    creatorEdit_->setEnabled(hasProject);
    yearSpin_->setEnabled(hasProject);
    rebuildCheck_->setEnabled(hasProject);
    applyButton_->setEnabled(hasProject);
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

bool HeaderMetadataWidget::rebuildRequested() const {
    return importedDraft_ && rebuildCheck_->isChecked();
}

void HeaderMetadataWidget::setApplyCallback(std::function<void()> callback) {
    applyCallback_ = std::move(callback);
}

} // namespace gui
