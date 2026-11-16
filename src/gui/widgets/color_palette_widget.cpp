#include "color_palette_widget.h"

#include "gui_constants.h"
#include "theme_manager.h"

#include <QtGui>
#include <QtWidgets>

#include <algorithm>

namespace gui {
namespace {

constexpr int kSwatchSize = kUiControlHeight;

class SwatchLayout final : public QLayout {
public:
    explicit SwatchLayout(QWidget *parent) : QLayout(parent) {
        setContentsMargins(kUiSpacing, kUiSpacing, kUiSpacing, kUiSpacing);
        setSpacing(kUiSpacing);
    }

    ~SwatchLayout() override {
        while (QLayoutItem *item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem *item) override {
        items_.push_back(item);
        invalidate();
    }

    int count() const override {
        return static_cast<int>(items_.size());
    }

    QLayoutItem *itemAt(int index) const override {
        return items_.value(index);
    }

    QLayoutItem *takeAt(int index) override {
        if (index < 0 || index >= count()) {
            return nullptr;
        }
        QLayoutItem *item = items_.takeAt(index);
        invalidate();

        return item;
    }

    Qt::Orientations expandingDirections() const override {
        return Qt::Horizontal;
    }

    QSize minimumSize() const override {
        const QMargins margins = contentsMargins();

        return QSize(kSwatchSize + margins.left() + margins.right(),
            kSwatchSize + margins.top() + margins.bottom());
    }

    QSize sizeHint() const override {
        return minimumSize();
    }

    bool hasHeightForWidth() const override {
        return true;
    }

    int heightForWidth(int width) const override {
        const QMargins margins = contentsMargins();
        const int columns = columnsForWidth(width);
        const int rows = (count() + columns - 1) / columns;

        return rows * kSwatchSize + std::max(0, rows - 1) * spacing()
            + margins.top() + margins.bottom();
    }

    void setGeometry(const QRect &rect) override {
        const QMargins margins = contentsMargins();
        const QPoint origin = rect.topLeft() + QPoint(margins.left(), margins.top());
        const int columns = columnsForWidth(rect.width());

        QLayout::setGeometry(rect);
        for (int index = 0; index < count(); ++index) {
            const QPoint position = origin + QPoint(
                (index % columns) * (kSwatchSize + spacing()),
                (index / columns) * (kSwatchSize + spacing()));
            items_[index]->setGeometry(QRect(position, QSize(kSwatchSize, kSwatchSize)));
        }
    }

private:
    int columnsForWidth(int width) const {
        const QMargins margins = contentsMargins();
        const int contentWidth = width - margins.left() - margins.right();

        return std::max(1, (contentWidth + spacing()) / (kSwatchSize + spacing()));
    }

    QList<QLayoutItem *> items_;
};

class SwatchButton final : public QPushButton {
public:
    using QPushButton::QPushButton;
    std::function<void()> middleClickCallback;

protected:
    void mousePressEvent(QMouseEvent *event) override {
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
    : QWidget(parent) {
    layout_ = new SwatchLayout(this);

    addButton_ = new QPushButton(this);
    addButton_->setIcon(uiAddIcon());
    addButton_->setIconSize(QSize(kUiActionIconExtent, kUiActionIconExtent));
    addButton_->setObjectName(QStringLiteral("saveSwatchButton"));
    addButton_->setProperty("uiSquareButton", true);
    addButton_->setFixedSize(kSwatchSize, kSwatchSize);
    addButton_->setToolTip(QStringLiteral("Save selected color"));
    addButton_->setAccessibleName(addButton_->toolTip());
    connect(addButton_, &QPushButton::clicked, this, [this]() { addCurrentColor(); });

    rebuild();
}

void ColorPaletteWidget::setSwatches(QVector<Color> *swatches) {
    swatches_ = swatches;
    rebuild();
}

void ColorPaletteWidget::setCurrentColorProvider(std::function<std::optional<Color>()> provider) {
    currentColorProvider_ = std::move(provider);
}

void ColorPaletteWidget::setApplyColorCallback(std::function<void(const Color &)> callback) {
    applyColorCallback_ = std::move(callback);
}

void ColorPaletteWidget::setEditCallbacks(std::function<void()> beginEdit, std::function<void()> commitEdit) {
    beginEditCallback_ = std::move(beginEdit);
    commitEditCallback_ = std::move(commitEdit);
}

bool ColorPaletteWidget::addColor(const QColor &color) {
    if (!color.isValid() || swatches_ == nullptr) {
        return false;
    }
    const Color swatch = fromQColor(color);
    if (std::find(swatches_->begin(), swatches_->end(), swatch) != swatches_->end()) {
        return false;
    }
    editSwatches([this, swatch]() { swatches_->push_back(swatch); });
    return true;
}

void ColorPaletteWidget::refreshTheme() {
    rebuild();
}

void ColorPaletteWidget::rebuild() {
    while (QLayoutItem *item = layout_->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            if (widget != addButton_) {
                widget->deleteLater();
            }
        }
        delete item;
    }

    const int swatchCount = swatches_ == nullptr ? 0 : swatches_->size();
    for (int i = 0; i < swatchCount; ++i) {
        const Color color = swatches_->at(i);
        auto *button = new SwatchButton(this);
        button->setProperty("uiSquareButton", true);
        button->setFixedSize(kSwatchSize, kSwatchSize);
        button->setToolTip(toQColor(color).name(QColor::HexArgb).toUpper());
        button->setStyleSheet(colorSwatchStyle(toQColor(color)));
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
        layout_->addWidget(button);
    }

    addButton_->setEnabled(swatches_ != nullptr);
    layout_->addWidget(addButton_);
}

void ColorPaletteWidget::editSwatches(const std::function<void()> &edit) {
    if (beginEditCallback_ != nullptr) {
        beginEditCallback_();
    }
    edit();
    if (commitEditCallback_ != nullptr) {
        commitEditCallback_();
    }
    rebuild();
}

void ColorPaletteWidget::addCurrentColor() {
    if (currentColorProvider_ == nullptr) {
        return;
    }
    const std::optional<Color> color = currentColorProvider_();
    if (!color.has_value()) {
        return;
    }
    addColor(toQColor(color.value()));
}

void ColorPaletteWidget::removeSwatch(int index) {
    if (swatches_ == nullptr || index < 0 || index >= swatches_->size()) {
        return;
    }
    editSwatches([this, index]() { swatches_->removeAt(index); });
}

QColor ColorPaletteWidget::toQColor(const Color &color) {
    return QColor(color[ColorByteRed], color[ColorByteGreen], color[ColorByteBlue], color[ColorByteAlpha]);
}

ColorPaletteWidget::Color ColorPaletteWidget::fromQColor(const QColor &color) {
    return {static_cast<quint8>(color.blue()),
            static_cast<quint8>(color.green()),
            static_cast<quint8>(color.red()),
            static_cast<quint8>(color.alpha())};
}

} // namespace gui
