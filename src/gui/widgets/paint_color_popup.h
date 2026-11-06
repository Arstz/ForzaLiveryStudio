#pragma once

#include <QColor>
#include <QFrame>
#include <QVector>

class QLineEdit;
class QSlider;
class QSpinBox;

namespace gui {

struct PaintColorPreset {
    QString name;
    QColor color;
    int index = -1;
};

class PaintColorSquare;

class PaintColorPopup final : public QFrame {
    Q_OBJECT
public:
    PaintColorPopup(const QColor &color, const QVector<PaintColorPreset> &manufacturer,
                    const QVector<PaintColorPreset> &swatches, QWidget *parent = nullptr);
    QColor color() const;
    void setColor(const QColor &color);

Q_SIGNALS:
    void colorChanged(const QColor &color);
    void finished(bool accepted);

protected:
    void hideEvent(QHideEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setHsv(double hue, double saturation, double value);
    void syncControls();
    void finish(bool accepted);

    QColor color_;
    PaintColorSquare *square_ = nullptr;
    QSlider *hueSlider_ = nullptr;
    QLineEdit *hex_ = nullptr;
    QSpinBox *red_ = nullptr;
    QSpinBox *green_ = nullptr;
    QSpinBox *blue_ = nullptr;
    QSpinBox *hue_ = nullptr;
    QSpinBox *saturation_ = nullptr;
    QSpinBox *value_ = nullptr;
    bool finished_ = false;
    double hueValue_ = 0.0;
    double saturationValue_ = 0.0;
    double brightnessValue_ = 0.0;
};

void stylePaintColorButton(QWidget *button, const QColor &color);

} // namespace gui
