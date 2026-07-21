#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include <array>
#include <functional>
#include <optional>

class QGridLayout;
class QPushButton;

namespace gui {

class ColorPaletteWidget final : public QWidget {
public:
    using Color = std::array<quint8, 4>;

    explicit ColorPaletteWidget(QWidget *parent = nullptr);

    void setSwatches(QVector<Color> *swatches);
    void setCurrentColorProvider(std::function<std::optional<Color>()> provider);
    void setApplyColorCallback(std::function<void(const Color &)> callback);
    void setEditCallbacks(std::function<void()> beginEdit, std::function<void()> commitEdit);
    bool addColor(const QColor &color);
    void refreshTheme();

private:
    void rebuild();
    void editSwatches(const std::function<void()> &edit);
    void addCurrentColor();
    void removeSwatch(int index);
    static QColor toQColor(const Color &color);
    static Color fromQColor(const QColor &color);

    QVector<Color> *swatches_ = nullptr;
    std::function<std::optional<Color>()> currentColorProvider_;
    std::function<void(const Color &)> applyColorCallback_;
    std::function<void()> beginEditCallback_;
    std::function<void()> commitEditCallback_;
    QGridLayout *grid_ = nullptr;
    QPushButton *addButton_ = nullptr;
};

} // namespace gui
