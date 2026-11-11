#include "header_metadata_widget.h"

#include <QtCore>
#include <QtMath>
#include <QtWidgets>

#include <utility>

namespace gui {
namespace {

constexpr int kFirstMonth = 1;
constexpr int kLastMonth = 12;
constexpr int kDescriptionMinimumRows = 3;

} // namespace

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
    monthSpin_ = new QSpinBox(this);
    monthSpin_->setRange(kFirstMonth, kLastMonth);
    form->addRow(QStringLiteral("Month"), monthSpin_);
    publishedCheck_ = new QCheckBox(QStringLiteral("Published"), this);
    publishedCheck_->setChecked(false);
    publishedCheck_->setEnabled(false);
    publishedCheck_->setStyleSheet(QStringLiteral("QCheckBox:disabled { color: palette(mid); }"));
    form->addRow(QString(), publishedCheck_);
    descriptionEdit_ = new QTextEdit(this);
    descriptionEdit_->setPlaceholderText(QStringLiteral("Description (published only)"));
    descriptionEdit_->setReadOnly(true);
    descriptionEdit_->setAcceptRichText(false);
    descriptionEdit_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    descriptionEdit_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    descriptionEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(descriptionEdit_->document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this]() { updateDescriptionHeight(); });
    updateDescriptionHeight();
    form->addRow(QStringLiteral("Description"), descriptionEdit_);

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
    connect(monthSpin_, &QSpinBox::editingFinished, this, commit);
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
    const fls::HeaderMetadata &seed, const QString &targetCar,
    bool hasProject, bool canChangeTargetCar, bool targetCarSupported) {
    seed_ = seed;

    nameEdit_->setText(seed.name);
    creatorEdit_->setText(seed.creatorName);
    yearSpin_->setValue(seed.year == 0 ? QDate::currentDate().year() : seed.year);
    monthSpin_->setValue(seed.month == 0 ? QDate::currentDate().month() : seed.month);
    publishedCheck_->setChecked(seed.published);
    descriptionEdit_->setPlainText(seed.description);
    targetCar_->setText(targetCar.isEmpty() ? QStringLiteral("Not set") : targetCar);
    targetCar_->setStyleSheet(
        !targetCar.isEmpty() && !targetCarSupported
            ? QStringLiteral("color: rgb(255, 213, 79);")
            : QString());

    nameEdit_->setEnabled(hasProject);
    creatorEdit_->setEnabled(hasProject);
    yearSpin_->setEnabled(hasProject);
    monthSpin_->setEnabled(hasProject);
    changeTargetCar_->setEnabled(hasProject && canChangeTargetCar);
    hint_->setVisible(!hasProject);
}

fls::HeaderMetadata HeaderMetadataWidget::metadata() const {
    fls::HeaderMetadata meta = seed_;
    meta.name = nameEdit_->text();
    meta.creatorName = creatorEdit_->text();
    meta.year = static_cast<quint16>(yearSpin_->value());
    meta.month = static_cast<quint8>(monthSpin_->value());

    return meta;
}

void HeaderMetadataWidget::setMetadataChangedCallback(std::function<void()> callback) {
    metadataChangedCallback_ = std::move(callback);
}

void HeaderMetadataWidget::setChangeTargetCarCallback(std::function<void()> callback) {
    changeTargetCarCallback_ = std::move(callback);
}

void HeaderMetadataWidget::updateDescriptionHeight() {
    const int minimumHeight = kDescriptionMinimumRows * descriptionEdit_->fontMetrics().lineSpacing()
        + qCeil(2 * descriptionEdit_->document()->documentMargin());
    const int height = qMax(minimumHeight, qCeil(descriptionEdit_->document()->size().height()))
        + 2 * descriptionEdit_->frameWidth();

    if (descriptionEdit_->height() == height) return;
    descriptionEdit_->setFixedHeight(height);
}

} // namespace gui
