#include "color_palette_widget.h"

#include "gui_constants.h"

#include <QColor>
#include <QAction>
#include <QGridLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>

#include <algorithm>

namespace gui {
namespace {

constexpr int SwatchSize = 28;
constexpr int SwatchesPerRow = 6;

QString swatchStyle(const QColor &color)
{
    const QColor text = color.lightness() < 128 ? QColor(255, 255, 255) : QColor(32, 34, 37);
    return QStringLiteral("QPushButton { background-color: rgba(%1,%2,%3,%4); color: %5; border: 1px solid palette(mid); }")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha())
        .arg(text.name(QColor::HexRgb));
}

class SwatchButton final : public QPushButton {
public:
    using QPushButton::QPushButton;
    std::function<void()> middleClickCallback;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::MiddleButton && middleClickCallback != nullptr) {
            middleClickCallback();
            event->accept();
            return;
        }
        QPushButton::mousePressEvent(event);
    }
};

} // namespace

ColorPaletteWidget::ColorPaletteWidget(QWidget *parent)
    : QWidget(parent)
{
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(6, 6, 6, 6);
    grid_->setSpacing(6);

    addButton_ = new QPushButton(QStringLiteral("+"), this);
    addButton_->setFixedSize(SwatchSize, SwatchSize);
    addButton_->setToolTip(QStringLiteral("Save selected color"));
    connect(addButton_, &QPushButton::clicked, this, [this]() { addCurrentColor(); });

    rebuild();
}

void ColorPaletteWidget::setSwatches(QVector<Color> *swatches)
{
    swatches_ = swatches;
    rebuild();
}

void ColorPaletteWidget::setCurrentColorProvider(std::function<std::optional<Color>()> provider)
{
    currentColorProvider_ = std::move(provider);
}

void ColorPaletteWidget::setApplyColorCallback(std::function<void(const Color &)> callback)
{
    applyColorCallback_ = std::move(callback);
}

void ColorPaletteWidget::setEditCallbacks(std::function<void()> beginEdit, std::function<void()> commitEdit)
{
    beginEditCallback_ = std::move(beginEdit);
    commitEditCallback_ = std::move(commitEdit);
}

bool ColorPaletteWidget::addColor(const QColor &color)
{
    if (!color.isValid() || swatches_ == nullptr) {
        return false;
    }
    const Color swatch = fromQColor(color);
    if (std::find(swatches_->begin(), swatches_->end(), swatch) != swatches_->end()) {
        return false;
    }
    if (beginEditCallback_ != nullptr) {
        beginEditCallback_();
    }
    swatches_->push_back(swatch);
    if (commitEditCallback_ != nullptr) {
        commitEditCallback_();
    }
    rebuild();
    return true;
}

void ColorPaletteWidget::refreshTheme()
{
    rebuild();
}

void ColorPaletteWidget::rebuild()
{
    while (QLayoutItem *item = grid_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            if (widget != addButton_) {
                widget->deleteLater();
            }
        }
        delete item;
    }

    int row = 0;
    int column = 0;
    const int swatchCount = swatches_ == nullptr ? 0 : swatches_->size();
    for (int i = 0; i < swatchCount; ++i) {
        const Color color = swatches_->at(i);
        auto *button = new SwatchButton(this);
        button->setFixedSize(SwatchSize, SwatchSize);
        button->setToolTip(toQColor(color).name(QColor::HexArgb).toUpper());
        button->setStyleSheet(swatchStyle(toQColor(color)));
        button->middleClickCallback = [this, i]() { removeSwatch(i); };
        connect(button, &QPushButton::clicked, this, [this, color]() {
            if (applyColorCallback_ != nullptr) {
                applyColorCallback_(color);
            }
        });
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested, this, [this, button, i](const QPoint &pos) {
            QMenu menu(this);
            QAction *remove = menu.addAction(QStringLiteral("Remove Swatch"));
            if (menu.exec(button->mapToGlobal(pos)) == remove) {
                removeSwatch(i);
            }
        });
        grid_->addWidget(button, row, column);
        if (++column >= SwatchesPerRow) {
            column = 0;
            ++row;
        }
    }

    addButton_->setEnabled(swatches_ != nullptr);
    grid_->addWidget(addButton_, row, column);
    grid_->setRowStretch(row + 1, 1);
    grid_->setColumnStretch(SwatchesPerRow, 1);
}

void ColorPaletteWidget::addCurrentColor()
{
    if (currentColorProvider_ == nullptr) {
        return;
    }
    const std::optional<Color> color = currentColorProvider_();
    if (!color.has_value()) {
        return;
    }
    addColor(toQColor(color.value()));
}

void ColorPaletteWidget::removeSwatch(int index)
{
    if (swatches_ == nullptr || index < 0 || index >= swatches_->size()) {
        return;
    }
    if (beginEditCallback_ != nullptr) {
        beginEditCallback_();
    }
    swatches_->removeAt(index);
    if (commitEditCallback_ != nullptr) {
        commitEditCallback_();
    }
    rebuild();
}

QColor ColorPaletteWidget::toQColor(const Color &color)
{
    return QColor(color[ColorByteRed], color[ColorByteGreen], color[ColorByteBlue], color[ColorByteAlpha]);
}

ColorPaletteWidget::Color ColorPaletteWidget::fromQColor(const QColor &color)
{
    return {static_cast<quint8>(color.blue()),
            static_cast<quint8>(color.green()),
            static_cast<quint8>(color.red()),
            static_cast<quint8>(color.alpha())};
}

} // namespace gui
