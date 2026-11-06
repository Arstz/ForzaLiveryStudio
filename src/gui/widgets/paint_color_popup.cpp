#include "paint_color_popup.h"

#include <QGridLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

namespace gui {
namespace {

constexpr int kColorMaximum = 255;
constexpr int kHueMaximum = 359;
constexpr int kPercentMaximum = 100;
constexpr int kPresetColumns = 8;
constexpr int kPresetSize = 27;
constexpr double kLabelValueThreshold = 0.5;

void addPresets(QVBoxLayout *layout, const QString &title, const QString &prefix,
                const QVector<PaintColorPreset> &presets, PaintColorPopup *popup) {
    layout->addWidget(new QLabel(title, popup));
    if (presets.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("None available"), popup);
        empty->setEnabled(false);
        layout->addWidget(empty);
        return;
    }
    auto *scroll = new QScrollArea(popup);
    auto *content = new QWidget(scroll);
    auto *grid = new QGridLayout(content);
    grid->setContentsMargins(2, 2, 2, 2);
    grid->setSpacing(4);
    for (qsizetype index = 0; index < presets.size(); ++index) {
        const PaintColorPreset &preset = presets[index];
        auto *button = new QPushButton(content);
        button->setObjectName(prefix + QString::number(index));
        button->setFixedSize(kPresetSize, kPresetSize);
        button->setToolTip(preset.name + QLatin1Char('\n') + preset.color.name(QColor::HexRgb).toUpper());
        stylePaintColorButton(button, preset.color);
        grid->addWidget(button, index / kPresetColumns, index % kPresetColumns);
        QObject::connect(button, &QPushButton::clicked, popup, [popup, color = preset.color]() {
            popup->setColor(color);
        });
    }
    grid->setColumnStretch(kPresetColumns, 1);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedHeight(std::min(3, static_cast<int>((presets.size() + kPresetColumns - 1) / kPresetColumns))
                           * (kPresetSize + grid->spacing()) + 6);
    layout->addWidget(scroll);
}

} // namespace

class PaintColorSquare final : public QWidget {
public:
    explicit PaintColorSquare(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("paintColorSquare"));
        setMinimumSize(240, 170);
    }

    void setHsv(double hue, double saturation, double value) {
        hue_ = hue;
        saturation_ = saturation;
        value_ = value;
        update();
    }

    std::function<void(double, double)> changed;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        QLinearGradient saturation(0, 0, width(), 0);
        QLinearGradient value(0, 0, 0, height());
        saturation.setColorAt(0, Qt::white);
        saturation.setColorAt(1, QColor::fromHsvF(hue_, 1, 1));
        value.setColorAt(0, Qt::transparent);
        value.setColorAt(1, Qt::black);
        painter.fillRect(rect(), saturation);
        painter.fillRect(rect(), value);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::black, 3));
        const QPointF point(saturation_ * (width() - 1), (1 - value_) * (height() - 1));
        painter.drawEllipse(point, 5, 5);
        painter.setPen(QPen(Qt::white, 1));
        painter.drawEllipse(point, 5, 5);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) select(event->position());
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons() & Qt::LeftButton) select(event->position());
    }

private:
    void select(const QPointF &position) {
        if (changed) {
            changed(std::clamp(position.x() / std::max(1, width() - 1), 0.0, 1.0),
                    1 - std::clamp(position.y() / std::max(1, height() - 1), 0.0, 1.0));
        }
    }

    double hue_ = 0.0;
    double saturation_ = 0.0;
    double value_ = 0.0;
};

void stylePaintColorButton(QWidget *button, const QColor &color) {
    if (button->property("paintColor").value<QColor>() == color) return;
    const QColor foreground(color.valueF() > kLabelValueThreshold ? Qt::black : Qt::white);
    button->setStyleSheet(QStringLiteral(
        "background-color: %1; color: %2; border: 1px solid #777; border-radius: 3px; padding: 5px; text-align: center;")
        .arg(color.name(QColor::HexRgb), foreground.name(QColor::HexRgb)));
    button->setProperty("paintColor", color);
}

PaintColorPopup::PaintColorPopup(const QColor &color, const QVector<PaintColorPreset> &manufacturer,
                               const QVector<PaintColorPreset> &swatches, QWidget *parent)
    : QFrame(parent, Qt::Popup) {
    setObjectName(QStringLiteral("paintColorPopup"));
    setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(7);
    square_ = new PaintColorSquare(this);
    square_->changed = [this](double saturation, double value) { setHsv(hueValue_, saturation, value); };
    layout->addWidget(square_);
    hueSlider_ = new QSlider(Qt::Horizontal, this);
    hueSlider_->setObjectName(QStringLiteral("paintColorHueSlider"));
    hueSlider_->setRange(0, kHueMaximum);
    hueSlider_->setStyleSheet(QStringLiteral(
        "QSlider::groove:horizontal { height: 14px; background: qlineargradient(x1:0,y1:0,x2:1,y2:0,"
        "stop:0 red,stop:0.1667 yellow,stop:0.3333 lime,stop:0.5 cyan,stop:0.6667 blue,stop:0.8333 magenta,stop:1 red); }"
        "QSlider::handle:horizontal { width: 7px; background: transparent; border: 2px solid white; margin: -3px 0; }"));
    layout->addWidget(hueSlider_);
    auto *fields = new QGridLayout;
    fields->addWidget(new QLabel(QStringLiteral("Hex"), this), 0, 0);
    hex_ = new QLineEdit(this);
    hex_->setObjectName(QStringLiteral("paintColorHex"));
    hex_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("#?[0-9A-Fa-f]{6}")), hex_));
    fields->addWidget(hex_, 0, 1, 1, 5);
    const auto addField = [this, fields](const QString &label, int row, int column, int maximum) {
        auto *field = new QSpinBox(this);
        field->setObjectName(QStringLiteral("paintColor") + label);
        field->setRange(0, maximum);
        field->setKeyboardTracking(true);
        fields->addWidget(new QLabel(label, this), row, column * 2);
        fields->addWidget(field, row, column * 2 + 1);
        return field;
    };
    red_ = addField(QStringLiteral("R"), 1, 0, kColorMaximum);
    green_ = addField(QStringLiteral("G"), 1, 1, kColorMaximum);
    blue_ = addField(QStringLiteral("B"), 1, 2, kColorMaximum);
    hue_ = addField(QStringLiteral("H"), 2, 0, kHueMaximum);
    saturation_ = addField(QStringLiteral("S"), 2, 1, kPercentMaximum);
    value_ = addField(QStringLiteral("V"), 2, 2, kPercentMaximum);
    layout->addLayout(fields);
    const auto rgbChanged = [this]() { setColor(QColor(red_->value(), green_->value(), blue_->value())); };
    for (QSpinBox *field : {red_, green_, blue_}) {
        connect(field, &QSpinBox::valueChanged, this, rgbChanged);
    }
    const auto hsvChanged = [this]() {
        setHsv(hue_->value() / 360.0, saturation_->value() / 100.0, value_->value() / 100.0);
    };
    for (QSpinBox *field : {hue_, saturation_, value_}) {
        connect(field, &QSpinBox::valueChanged, this, hsvChanged);
    }
    connect(hueSlider_, &QSlider::valueChanged, this, [this](int value) { setHsv(value / 360.0, saturationValue_, brightnessValue_); });
    connect(hex_, &QLineEdit::textEdited, this, [this](QString text) {
        if (!hex_->hasAcceptableInput()) return;
        if (!text.startsWith(QLatin1Char('#'))) text.prepend(QLatin1Char('#'));
        setColor(QColor(text));
    });
    addPresets(layout, QStringLiteral("Manufacturer colors"), QStringLiteral("paintManufacturerColor."), manufacturer, this);
    addPresets(layout, QStringLiteral("Swatches"), QStringLiteral("paintSwatchColor."), swatches, this);
    auto *buttons = new QHBoxLayout;
    auto *cancel = new QPushButton(QStringLiteral("Cancel"), this);
    auto *done = new QPushButton(QStringLiteral("Done"), this);
    cancel->setObjectName(QStringLiteral("paintColorCancel"));
    done->setObjectName(QStringLiteral("paintColorDone"));
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(done);
    layout->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, [this]() { finish(false); });
    connect(done, &QPushButton::clicked, this, [this]() { finish(true); });
    setColor(color);
    setFixedWidth(330);
}

QColor PaintColorPopup::color() const {
    return color_;
}

void PaintColorPopup::setColor(const QColor &color) {
    if (!color.isValid()) return;
    const QColor opaque(color.red(), color.green(), color.blue());
    const double hue = opaque.hsvHueF() >= 0.0 ? opaque.hsvHueF() : hueValue_;
    setHsv(hue, opaque.hsvSaturationF(), opaque.valueF());
}

void PaintColorPopup::setHsv(double hue, double saturation, double value) {
    const QColor previous = color_;
    hueValue_ = hue;
    saturationValue_ = saturation;
    brightnessValue_ = value;
    color_ = QColor::fromHsvF(hue, saturation, value).toRgb();
    syncControls();
    if (color_.rgb() != previous.rgb()) Q_EMIT colorChanged(color_);
}

void PaintColorPopup::syncControls() {
    const QSignalBlocker red(red_), green(green_), blue(blue_), hue(hue_), saturation(saturation_), value(value_),
        slider(hueSlider_), hex(hex_);
    red_->setValue(color_.red());
    green_->setValue(color_.green());
    blue_->setValue(color_.blue());
    hue_->setValue(static_cast<int>(std::lround(hueValue_ * 360)));
    saturation_->setValue(static_cast<int>(std::lround(saturationValue_ * 100)));
    value_->setValue(static_cast<int>(std::lround(brightnessValue_ * 100)));
    hueSlider_->setValue(hue_->value());
    hex_->setText(color_.name(QColor::HexRgb).toUpper());
    square_->setHsv(hueValue_, saturationValue_, brightnessValue_);
}

void PaintColorPopup::finish(bool accepted) {
    if (finished_) return;
    finished_ = true;
    Q_EMIT finished(accepted);
    close();
}

void PaintColorPopup::hideEvent(QHideEvent *event) {
    QFrame::hideEvent(event);
    if (!finished_) {
        finished_ = true;
        Q_EMIT finished(true);
    }
}

void PaintColorPopup::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) {
        finish(false);
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        finish(true);
    } else {
        QFrame::keyPressEvent(event);
    }
}

} // namespace gui
