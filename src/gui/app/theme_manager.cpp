#include "theme_manager.h"

#include <QApplication>
#include <QIconEngine>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleOption>

#include <algorithm>
#include <cmath>
#include <optional>

namespace gui {
namespace {

UiTheme currentTheme = UiTheme::Dark;

constexpr double kGlyphStrokeWidth = 1.5;
constexpr double kCheckStrokeWidth = 2.0;
constexpr int kMenuIconExtent = 16;
constexpr int kResizeGripSpacing = 4;
constexpr int kResizeGripInset = 1;
constexpr int kLightResizeHandleFactor = 105;
constexpr int kSliderGrooveExtent = 6;
constexpr int kSliderHandleExtent = 14;
constexpr int kSliderHandleThickness = 20;
constexpr int kSliderInset = 1;
constexpr int kHueGrooveExtent = 14;
constexpr double kOutlineWidth = 1.0;

int checkBoxExtent(const QWidget *widget) {
    return widget != nullptr && widget->property("uiCompactCheck").toBool()
        ? kUiCompactControlExtent : kUiControlHeight;
}

void drawResizeHandle(QPainter &painter, const QStyleOption &option) {
    const UiColors colors = uiColors(currentUiTheme());
    const QColor background = currentUiTheme() == UiTheme::Light
        ? colors.main.lighter(kLightResizeHandleFactor) : colors.contrast;
    const QRect area = option.rect;
    const QPoint center = area.center();
    const bool vertical = area.height() > area.width();
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const bool pressed = option.state.testFlag(QStyle::State_Sunken);

    painter.save();
    painter.fillRect(area, hovered || pressed ? colors.accent : background);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(pressed ? colors.focus : colors.text);
    for (const int offset : {-kResizeGripSpacing, 0, kResizeGripSpacing}) {
        if (vertical) {
            painter.drawLine(area.left() + kResizeGripInset, center.y() + offset,
                             area.right() - kResizeGripInset, center.y() + offset);
        } else {
            painter.drawLine(center.x() + offset, area.top() + kResizeGripInset,
                             center.x() + offset, area.bottom() - kResizeGripInset);
        }
    }
    painter.restore();
}

bool isHueSlider(const QWidget *widget) {
    return widget != nullptr && widget->objectName() == QStringLiteral("paintColorHueSlider");
}

QRect sliderControlRect(const QStyleOptionSlider &option, QStyle::SubControl control,
                        const QWidget *widget) {
    const QRect area = option.rect.adjusted(kSliderInset, kSliderInset, -kSliderInset, -kSliderInset);
    QRect result;
    const bool horizontal = option.orientation == Qt::Horizontal;
    const int span = horizontal ? area.width() : area.height();
    const int length = std::min(kSliderHandleExtent, span);
    const int grooveExtent = isHueSlider(widget) ? kHueGrooveExtent : kSliderGrooveExtent;
    const int position = QStyle::sliderPositionFromValue(option.minimum, option.maximum,
        option.sliderPosition, std::max(0, span - length), option.upsideDown);

    if (area.isEmpty()) {
        return result;
    }
    if (control == QStyle::SC_SliderGroove) {
        result.setSize(horizontal ? QSize(area.width(), grooveExtent) : QSize(grooveExtent, area.height()));
        result.moveCenter(area.center());
    } else if (control == QStyle::SC_SliderHandle) {
        result.setSize(horizontal ? QSize(length, kSliderHandleThickness) : QSize(kSliderHandleThickness, length));
        result.moveCenter(area.center());
        if (horizontal) {
            result.moveLeft(area.left() + position);
        } else {
            result.moveTop(area.top() + position);
        }
    }

    return result;
}

void drawSlider(QPainter &painter, const QStyleOptionSlider &option, const QWidget *widget) {
    const UiColors colors = uiColors(currentUiTheme());
    QRect groove = sliderControlRect(option, QStyle::SC_SliderGroove, widget);
    const QRect handle = sliderControlRect(option, QStyle::SC_SliderHandle, widget);
    QBrush track(colors.contrast);
    const bool enabled = option.state.testFlag(QStyle::State_Enabled);
    const bool active = enabled && (option.state.testFlag(QStyle::State_HasFocus)
        || (option.activeSubControls.testFlag(QStyle::SC_SliderHandle)
            && (option.state.testFlag(QStyle::State_MouseOver) || option.state.testFlag(QStyle::State_Sunken))));
    const double inset = kOutlineWidth / 2;

    if (groove.isEmpty() || handle.isEmpty()) {
        return;
    }
    if (option.orientation == Qt::Horizontal) {
        groove.adjust(handle.width() / 2, 0, -handle.width() / 2, 0);
    } else {
        groove.adjust(0, handle.height() / 2, 0, -handle.height() / 2);
    }
    if (isHueSlider(widget)) {
        QLinearGradient gradient(groove.topLeft(), groove.topRight());
        gradient.setStops({{0.0, Qt::red}, {1.0 / 6, Qt::yellow}, {2.0 / 6, Qt::green},
                          {0.5, Qt::cyan}, {4.0 / 6, Qt::blue}, {5.0 / 6, Qt::magenta}, {1.0, Qt::red}});
        track = QBrush(gradient);
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (option.subControls.testFlag(QStyle::SC_SliderGroove)) {
        painter.setPen(QPen(colors.border, kOutlineWidth));
        painter.setBrush(track);
        painter.drawRoundedRect(QRectF(groove).adjusted(inset, inset, -inset, -inset),
                                kUiCornerRadius, kUiCornerRadius);
    }
    if (option.subControls.testFlag(QStyle::SC_SliderHandle)) {
        painter.setPen(QPen(active ? colors.focus : enabled ? colors.border : colors.disabledText, kOutlineWidth));
        painter.setBrush(!enabled ? colors.main : active ? colors.secondary : colors.accent);
        painter.drawRoundedRect(QRectF(handle).adjusted(inset, inset, -inset, -inset),
                                kUiCornerRadius, kUiCornerRadius);
    }
    painter.restore();
}

void drawArrow(QPainter &painter, const QRect &area, Qt::ArrowType direction, const QColor &color) {
    QTransform transform;
    const QPolygonF chevron{QPointF(-0.2, -0.3), QPointF(0.2, 0.0), QPointF(-0.2, 0.3)};
    const double extent = std::min(area.width(), area.height());
    const double rotation = direction == Qt::DownArrow ? 90.0
        : direction == Qt::LeftArrow ? 180.0 : direction == Qt::UpArrow ? 270.0 : 0.0;

    transform.translate(QRectF(area).center().x(), QRectF(area).center().y());
    transform.rotate(rotation);
    transform.scale(extent, extent);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, kGlyphStrokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(transform.map(chevron));
    painter.restore();
}

void drawClose(QPainter &painter, const QRect &area, const QColor &color) {
    const double inset = std::min(area.width(), area.height()) * 0.3;
    const QRectF glyph = QRectF(area).adjusted(inset, inset, -inset, -inset);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, kGlyphStrokeWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(glyph.topLeft(), glyph.bottomRight());
    painter.drawLine(glyph.bottomLeft(), glyph.topRight());
    painter.restore();
}

std::optional<Qt::ArrowType> standardArrow(QStyle::StandardPixmap icon, Qt::LayoutDirection direction) {
    switch (icon) {
    case QStyle::SP_ArrowUp: return Qt::UpArrow;
    case QStyle::SP_ArrowDown:
    case QStyle::SP_ToolBarVerticalExtensionButton: return Qt::DownArrow;
    case QStyle::SP_ArrowLeft: return Qt::LeftArrow;
    case QStyle::SP_ArrowRight: return Qt::RightArrow;
    case QStyle::SP_ArrowBack:
        return direction == Qt::RightToLeft ? Qt::RightArrow : Qt::LeftArrow;
    case QStyle::SP_ArrowForward:
    case QStyle::SP_ToolBarHorizontalExtensionButton:
        return direction == Qt::RightToLeft ? Qt::LeftArrow : Qt::RightArrow;
    default: return std::nullopt;
    }
}

class ChromeIconEngine final : public QIconEngine {
public:
    explicit ChromeIconEngine(std::optional<Qt::ArrowType> direction) : direction_(direction) {}

    QIconEngine *clone() const override {
        return new ChromeIconEngine(direction_);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        const UiColors colors = uiColors(currentUiTheme());
        const QColor foreground = mode == QIcon::Disabled ? colors.disabledText : colors.text;

        if (direction_.has_value()) {
            drawArrow(*painter, rect, *direction_, foreground);
        } else {
            drawClose(*painter, rect, foreground);
        }
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, QRect(QPoint(), size), mode, state);

        return result;
    }

private:
    std::optional<Qt::ArrowType> direction_;
};

void drawCheckBox(QPainter &painter, const QStyleOption &option) {
    const UiColors colors = uiColors(currentUiTheme());
    const QRectF box = QRectF(option.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    const bool enabled = option.state.testFlag(QStyle::State_Enabled);
    const bool checked = option.state.testFlag(QStyle::State_On);
    const bool mixed = option.state.testFlag(QStyle::State_NoChange);
    const bool active = enabled && (option.state.testFlag(QStyle::State_HasFocus)
        || option.state.testFlag(QStyle::State_MouseOver));

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(active ? colors.focus : colors.border, 1.0));
    painter.setBrush(!enabled ? colors.main : checked || mixed ? colors.accent : colors.secondary);
    painter.drawRoundedRect(box, kUiCornerRadius, kUiCornerRadius);
    painter.setPen(QPen(enabled ? colors.text : colors.disabledText,
                        kCheckStrokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (mixed) {
        painter.drawLine(QPointF(box.left() + box.width() * 0.25, box.center().y()),
                         QPointF(box.right() - box.width() * 0.25, box.center().y()));
    } else if (checked) {
        painter.drawPolyline(QPolygonF{
            QPointF(box.left() + box.width() * 0.22, box.top() + box.height() * 0.52),
            QPointF(box.left() + box.width() * 0.43, box.top() + box.height() * 0.73),
            QPointF(box.left() + box.width() * 0.79, box.top() + box.height() * 0.28)});
    }
    painter.restore();
}

class ThemeStyle final : public QProxyStyle {
public:
    explicit ThemeStyle(QStyle *style)
        : QProxyStyle(style) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override {
        const UiColors colors = uiColors(currentUiTheme());
        std::optional<Qt::ArrowType> direction;

        switch (element) {
        case PE_IndicatorDockWidgetResizeHandle:
            drawResizeHandle(*painter, *option);
            return;
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck:
            drawCheckBox(*painter, *option);
            return;
        case PE_IndicatorTabClose:
            drawClose(*painter, option->rect,
                      option->state.testFlag(State_Enabled) ? colors.text : colors.disabledText);
            return;
        case PE_IndicatorArrowUp:
        case PE_IndicatorSpinUp: direction = Qt::UpArrow; break;
        case PE_IndicatorArrowDown:
        case PE_IndicatorSpinDown: direction = Qt::DownArrow; break;
        case PE_IndicatorArrowLeft: direction = Qt::LeftArrow; break;
        case PE_IndicatorArrowRight: direction = Qt::RightArrow; break;
        default: break;
        }
        if (direction.has_value()) {
            drawArrow(*painter, option->rect, *direction,
                      option->state.testFlag(State_Enabled) ? colors.text : colors.disabledText);
            return;
        }

        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement element, const QStyleOption *option,
                     QPainter *painter, const QWidget *widget = nullptr) const override {
        if (element == CE_Splitter) {
            drawResizeHandle(*painter, *option);
            return;
        }

        QProxyStyle::drawControl(element, option, painter, widget);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option,
                         SubControl subControl, const QWidget *widget = nullptr) const override {
        if (control == CC_Slider && (subControl == SC_SliderHandle || subControl == SC_SliderGroove)) {
            if (const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
                return sliderControlRect(*slider, subControl, widget);
            }
        }

        return QProxyStyle::subControlRect(control, option, subControl, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
                            QPainter *painter, const QWidget *widget = nullptr) const override {
        if (control == CC_Slider) {
            if (const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
                drawSlider(*painter, *slider, widget);
                if (slider->subControls.testFlag(SC_SliderTickmarks)) {
                    QStyleOptionSlider ticks = *slider;
                    ticks.subControls = SC_SliderTickmarks;
                    QProxyStyle::drawComplexControl(control, &ticks, painter, widget);
                }
                return;
            }
        }

        QProxyStyle::drawComplexControl(control, option, painter, widget);
    }

    QIcon standardIcon(StandardPixmap icon, const QStyleOption *option,
                       const QWidget *widget) const override {
        const Qt::LayoutDirection direction = widget != nullptr ? widget->layoutDirection()
            : option != nullptr ? option->direction : QApplication::layoutDirection();
        const std::optional<Qt::ArrowType> arrow = standardArrow(icon, direction);

        if (arrow.has_value() || icon == SP_LineEditClearButton
            || icon == SP_DockWidgetCloseButton || icon == SP_TitleBarCloseButton) {
            return QIcon(new ChromeIconEngine(arrow));
        }

        return QProxyStyle::standardIcon(icon, option, widget);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
        switch (metric) {
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
            return qobject_cast<const QMenu *>(widget) != nullptr ? kMenuIconExtent : checkBoxExtent(widget);
        case PM_TabBarScrollButtonWidth:
            return kUiCompactControlExtent;
        case PM_SliderLength:
            return kSliderHandleExtent;
        case PM_SliderControlThickness:
            return kSliderHandleThickness;
        case PM_SliderThickness:
            return kUiControlHeight;
        case PM_SplitterWidth:
        case PM_DockWidgetSeparatorExtent:
            return kUiSplitterWidth;
        case PM_LayoutLeftMargin:
        case PM_LayoutTopMargin:
        case PM_LayoutRightMargin:
        case PM_LayoutBottomMargin:
            return kUiMargin;
        case PM_LayoutHorizontalSpacing:
        case PM_LayoutVerticalSpacing:
            return kUiSpacing;
        default:
            return QProxyStyle::pixelMetric(metric, option, widget);
        }
    }
};

QString themeStyleSheet(UiTheme theme) {
    const UiColors colors = uiColors(theme);
    QString sheet = QStringLiteral(R"(
        QWidget { color: @text; }
        QWidget:disabled { color: @disabled; }
        QMainWindow, QDialog { background: @main; color: @text; }
        QDockWidget { color: @text; border: none; }
        QWidget#dockHeader {
            background: @secondary; border: 1px solid @border;
            border-top-left-radius: @radiuspx; border-top-right-radius: @radiuspx;
            border-bottom-left-radius: 0; border-bottom-right-radius: 0;
        }
        QWidget#dockHeader[panelFocused="true"] { border-color: @focus; background: @accent; }
        QWidget#dockHeader QLabel { background: transparent; border: none; margin: 0; padding: 0; }
        QFrame#dockBody {
            background: @secondary; border: 1px solid @border; border-top: none;
            border-top-left-radius: 0; border-top-right-radius: 0;
            border-bottom-left-radius: @radiuspx; border-bottom-right-radius: @radiuspx;
        }
        QLabel#dockTitle { font-weight: 600; background: transparent; border: none; }
        QLabel { color: @text; background: transparent; }
        QLabel:disabled { color: @disabled; }
        QLabel[secondary="true"], QLabel[uiState="mixed"] { color: @muted; }
        QLabel[uiState="warning"] { color: @warning; }
        QLabel[uiState="error"] { color: @error; }
        QPushButton, QToolButton {
            color: @text; background: @secondary;
            border: 1px solid @border; border-radius: @radiuspx;
            min-height: @fieldHeightpx; padding: 3px 8px;
        }
        QToolButton { padding: 3px 6px; }
        QPushButton:hover, QToolButton:hover { background: @hover; border-color: @focus; }
        QPushButton:pressed, QToolButton:pressed,
        QPushButton:checked, QToolButton:checked { background: @selection; border-color: @focus; }
        QPushButton:checked:hover, QToolButton:checked:hover { background: @accent; }
        QPushButton:focus, QPushButton:default, QToolButton:focus { border-color: @focus; }
        QPushButton:disabled, QToolButton:disabled {
            color: @disabled; background: @main; border-color: @border;
        }
        QToolButton[uiCompact="true"] { min-height: 0; min-width: 0; padding: 0; }
        QToolButton[uiFlat="true"] { background: transparent; border-color: transparent; }
        QToolButton[uiFlat="true"]:hover { background: @hover; border-color: @border; }
        QToolButton[uiDockButton="true"] {
            background: @secondary; border: 1px solid @border; border-radius: @radiuspx;
            min-height: @dockExtentpx; max-height: @dockExtentpx;
            min-width: @dockExtentpx; max-width: @dockExtentpx; margin: 0; padding: 0;
        }
        QToolButton[uiDockButton="true"]:hover { background: @hover; border-color: @focus; }
        QToolButton[uiDockButton="true"]:pressed { background: @selection; border-color: @focus; }
        QToolButton[uiDockButton="true"]:disabled { background: @main; border-color: @border; }
        QPushButton[uiSquareButton="true"], QToolButton[uiSquareButton="true"] {
            min-height: @squareExtentpx; max-height: @squareExtentpx;
            min-width: @squareExtentpx; max-width: @squareExtentpx; margin: 0; padding: 0;
        }
        QCheckBox {
            color: @text; background: transparent; spacing: @spacingpx;
            min-height: @controlHeightpx; margin: 0; padding: 0;
        }
        QCheckBox:disabled { color: @disabled; }
        QCheckBox::indicator { width: @controlHeightpx; height: @controlHeightpx; }
        QCheckBox[uiCompactCheck="true"] { min-height: @compactControlpx; }
        QCheckBox[uiCompactCheck="true"]::indicator { width: @compactControlpx; height: @compactControlpx; }
        QLineEdit, QComboBox, QTextEdit, QPlainTextEdit, QAbstractSpinBox {
            color: @text; background: @main; border: 1px solid @border;
            border-radius: @radiuspx; padding: 3px 7px;
            selection-background-color: @selection; selection-color: @text;
        }
        QLineEdit, QComboBox, QAbstractSpinBox { min-height: @fieldHeightpx; }
        QComboBox, QAbstractSpinBox { padding-right: 22px; }
        QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover { border-color: @focus; }
        QLineEdit:focus, QComboBox:focus, QTextEdit:focus,
        QPlainTextEdit:focus, QAbstractSpinBox:focus { border-color: @focus; }
        QLineEdit:disabled, QComboBox:disabled, QTextEdit:disabled,
        QPlainTextEdit:disabled, QAbstractSpinBox:disabled {
            color: @disabled;
        }
        QLineEdit[uiState="mixed"], QAbstractSpinBox[uiState="mixed"],
        QAbstractSpinBox[uiState="mixed"] QLineEdit { color: @muted; }
        QPushButton[uiState="mixed"] { background: @contrast; color: @text; }
        QLineEdit[uiState="error"] { border-color: @error; }
        QAbstractSpinBox QLineEdit {
            background: @main; border: none; border-radius: 0; padding: 0; min-height: 0;
        }
        QComboBox::drop-down { subcontrol-origin: border; width: 20px; border: none; }
        QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
            subcontrol-origin: padding; width: 18px; border: none; background: transparent;
        }
        QAbstractSpinBox::up-button {
            subcontrol-position: top right; border-top-right-radius: @radiuspx;
        }
        QAbstractSpinBox::down-button {
            subcontrol-position: bottom right; border-bottom-right-radius: @radiuspx;
        }
        QAbstractItemView {
            color: @text; background: @secondary; alternate-background-color: @main;
            border: none; selection-background-color: @selection; selection-color: @text;
            outline: 0;
        }
        QAbstractItemView[uiMainSurface="true"] { background: @main; alternate-background-color: @main; }
        QAbstractItemView::item {
            min-height: @fieldHeightpx; padding: 0 4px; border: 1px solid transparent; border-radius: 0;
        }
        QAbstractItemView::item:hover { background: @hover; }
        QAbstractItemView::item:selected { background: @selection; color: @text; }
        QAbstractItemView::item:focus { border: 1px solid @focus; }
        QHeaderView::section {
            color: @text; background: @main; border: none;
            border-bottom: 1px solid @border; padding: 5px 6px; font-weight: 600;
        }
        QScrollArea { border: none; background: @secondary; }
        QScrollArea > QWidget > QWidget { background: @secondary; }
        QWidget#shapeGrid { background: @main; }
        QScrollBar:vertical { background: @main; width: 12px; margin: 0; }
        QScrollBar:horizontal { background: @main; height: 12px; margin: 0; }
        QScrollBar::handle { background: @border; border-radius: @radiuspx; }
        QScrollBar::handle:vertical { min-height: 28px; margin: 2px; }
        QScrollBar::handle:horizontal { min-width: 28px; margin: 2px; }
        QScrollBar::handle:hover { background: @focus; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QSlider:horizontal { min-height: @controlHeightpx; }
        QSlider:vertical { min-width: @controlHeightpx; }
        QToolBar { background: @main; border: none; spacing: @spacingpx; padding: 3px; }
        QToolBar#ToolSettingsBar { margin: 0; padding: 0; spacing: 0; }
        QScrollArea#ToolSettingsPanel, QWidget#ToolSettings { background: @main; margin: 0; padding: 0; }
        QStatusBar { background: @main; color: @muted; }
        QStatusBar::item { border: none; }
        QMenuBar { background: @main; color: @text; }
        QMenuBar::item { padding: 5px 8px; border-radius: @radiuspx; background: transparent; }
        QMenuBar::item:selected { background: @accent; }
        QMenu {
            color: @text; background: @secondary; border: 1px solid @border;
            border-radius: @radiuspx; padding: 2px;
        }
        QMenu::item { padding: 3px 6px; margin: 0; border-radius: @radiuspx; }
        QMenu::indicator { width: @menuIconpx; height: @menuIconpx; }
        QMenu::item:selected { background: @selection; }
        QMenu::item:disabled { color: @disabled; }
        QMenu::separator { height: 1px; background: @border; margin: 2px 4px; }
        QToolTip {
            color: @text; background: @secondary; border: 1px solid @border;
            border-radius: @radiuspx; padding: 5px 8px;
        }
        QGroupBox {
            border: 1px solid @border; border-radius: @radiuspx; margin-top: 12px; padding-top: 8px;
        }
        QGroupBox::title { color: @text; subcontrol-origin: margin; left: 8px; padding: 0 4px; }
        QTabWidget::pane { border: 1px solid @border; border-radius: 0; }
        QTabBar::tab {
            color: @text; background: @secondary; border: 1px solid @border;
            border-radius: 0; padding: 6px 10px;
        }
        QTabBar::tab:top {
            border-top-left-radius: @radiuspx; border-top-right-radius: @radiuspx;
        }
        QTabBar::tab:bottom {
            border-bottom-left-radius: @radiuspx; border-bottom-right-radius: @radiuspx;
        }
        QTabBar::tab:left {
            border-top-left-radius: @radiuspx; border-bottom-left-radius: @radiuspx;
        }
        QTabBar::tab:right {
            border-top-right-radius: @radiuspx; border-bottom-right-radius: @radiuspx;
        }
        QTabBar::tab:selected { background: @accent; border-color: @focus; }
        QTabBar::tab:hover { background: @hover; }
        QTabBar QToolButton { min-height: 0; min-width: 0; margin: 0; padding: 0; }
        QFrame[frameShape="4"] { border: none; background: @border; min-height: 1px; max-height: 1px; }
        QFrame[frameShape="5"] { border: none; background: @border; min-width: 1px; max-width: 1px; }
        QProgressBar {
            background: @secondary; color: @text; border: 1px solid @border;
            border-radius: @radiuspx; text-align: center; min-height: @fieldHeightpx;
        }
        QProgressBar::chunk { background: @accent; border-radius: @radiuspx; }
        QFrame#previewSidePanel {
            background: @main; border: 1px solid @border; border-radius: @radiuspx;
        }
        QFrame#previewSidePanel QScrollArea, QFrame#previewSidePanel QScrollArea > QWidget > QWidget {
            background: transparent;
        }
        QToolButton#previewSidePanelToggle { background: @main; min-height: 0; padding: 0; font-size: 16px; }
        QToolButton#previewSidePanelToggle:hover { background: @hover; }
        QLabel#previewReference, QLabel#previewStatus {
            color: @text; background: @secondary; border: 1px solid @border;
            border-radius: @radiuspx; padding: 3px 6px;
        }
        QLabel#previewReference { font-size: 10px; }
        QLabel#previewStatus { padding: 6px 10px; }
        QFrame#paintColorPopup { background: @main; border: 1px solid @border; border-radius: @radiuspx; }
    )");
    const QMap<QString, QColor> colorTokens = {
        {QStringLiteral("@main"), colors.main},
        {QStringLiteral("@secondary"), colors.secondary},
        {QStringLiteral("@contrast"), colors.contrast},
        {QStringLiteral("@accent"), colors.accent},
        {QStringLiteral("@focus"), colors.focus},
        {QStringLiteral("@border"), colors.border},
        {QStringLiteral("@text"), colors.text},
        {QStringLiteral("@muted"), colors.mutedText},
        {QStringLiteral("@disabled"), colors.disabledText},
        {QStringLiteral("@hover"), colors.hover},
        {QStringLiteral("@selection"), colors.selection},
        {QStringLiteral("@warning"), colors.warning},
        {QStringLiteral("@error"), colors.error},
    };

    for (auto token = colorTokens.cbegin(); token != colorTokens.cend(); ++token) {
        sheet.replace(token.key(), token.value().name());
    }
    sheet.replace(QStringLiteral("@radius"), QString::number(kUiCornerRadius));
    sheet.replace(QStringLiteral("@fieldHeight"), QString::number(kUiControlHeight - 8));
    sheet.replace(QStringLiteral("@controlHeight"), QString::number(kUiControlHeight));
    sheet.replace(QStringLiteral("@compactControl"), QString::number(kUiCompactControlExtent));
    sheet.replace(QStringLiteral("@dockExtent"), QString::number(kUiCompactControlExtent - 2));
    sheet.replace(QStringLiteral("@squareExtent"), QString::number(kUiControlHeight - 2));
    sheet.replace(QStringLiteral("@menuIcon"), QString::number(kMenuIconExtent));
    sheet.replace(QStringLiteral("@spacing"), QString::number(kUiSpacing));

    return sheet;
}

QColor validColor(const QColor &color, const QColor &fallback) {
    return color.isValid() ? color : fallback;
}

PreviewBackgroundMode previewBackgroundMode(const QString &value) {
    if (value == QStringLiteral("checkerboard")) {
        return PreviewBackgroundMode::Checkerboard;
    }
    if (value == QStringLiteral("custom")) {
        return PreviewBackgroundMode::Custom;
    }
    return PreviewBackgroundMode::ThemeDefault;
}

QString previewBackgroundModeValue(PreviewBackgroundMode mode) {
    switch (mode) {
    case PreviewBackgroundMode::Checkerboard:
        return QStringLiteral("checkerboard");
    case PreviewBackgroundMode::Custom:
        return QStringLiteral("custom");
    case PreviewBackgroundMode::ThemeDefault:
        return QStringLiteral("default");
    }
    return QStringLiteral("default");
}

} // namespace

UiColors uiColors(UiTheme theme) {
    if (theme == UiTheme::Light) {
        return {
            .main = QColor(QStringLiteral("#f6dcb9")),
            .secondary = QColor(QStringLiteral("#fef1eb")),
            .contrast = QColor(QStringLiteral("#cfbfbf")),
            .accent = QColor(QStringLiteral("#cfdad4")),
            .focus = QColor(QStringLiteral("#526b5b")),
            .border = QColor(QStringLiteral("#947e78")),
            .text = QColor(QStringLiteral("#352e2c")),
            .mutedText = QColor(QStringLiteral("#6e5b54")),
            .disabledText = QColor(QStringLiteral("#8b7770")),
            .hover = QColor(QStringLiteral("#e9dfd5")),
            .selection = QColor(QStringLiteral("#cfdad4")),
            .warning = QColor(QStringLiteral("#885d25")),
            .error = QColor(QStringLiteral("#a34950")),
            .guide = QColor(QStringLiteral("#ead5b4")),
        };
    }

    return {
        .main = QColor(QStringLiteral("#252426")),
        .secondary = QColor(QStringLiteral("#302d30")),
        .contrast = QColor(QStringLiteral("#40383b")),
        .accent = QColor(QStringLiteral("#54655c")),
        .focus = QColor(QStringLiteral("#adc4b5")),
        .border = QColor(QStringLiteral("#67585e")),
        .text = QColor(QStringLiteral("#f3e7df")),
        .mutedText = QColor(QStringLiteral("#c3afa9")),
        .disabledText = QColor(QStringLiteral("#8e8080")),
        .hover = QColor(QStringLiteral("#443e40")),
        .selection = QColor(QStringLiteral("#46574e")),
        .warning = QColor(QStringLiteral("#edc797")),
        .error = QColor(QStringLiteral("#e4a3aa")),
        .guide = QColor(QStringLiteral("#514637")),
    };
}

QString colorSwatchStyle(const QColor &color) {
    constexpr double kTextLuminanceThreshold = 0.179;
    const auto linearChannel = [](int channel) {
        const double value = channel / 255.0;

        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linearChannel(color.red())
        + 0.7152 * linearChannel(color.green()) + 0.0722 * linearChannel(color.blue());
    const QString foreground = luminance > kTextLuminanceThreshold
        ? QStringLiteral("#202020") : QStringLiteral("#ffffff");

    return QStringLiteral(
        "QPushButton, QToolButton { background-color: rgba(%1,%2,%3,%4); color: %5; }"
        "QPushButton:hover, QToolButton:hover, QPushButton:focus, QToolButton:focus { border-color: palette(text); }")
        .arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha()).arg(foreground);
}

void setUiState(QWidget *widget, const QString &state) {
    if (widget->property("uiState").toString() == state) {
        return;
    }
    widget->setProperty("uiState", state);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    for (QWidget *child : widget->findChildren<QWidget *>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
        child->update();
    }
    widget->update();
}

QString themeSettingsValue(UiTheme theme) {
    return theme == UiTheme::Light ? QStringLiteral("light") : QStringLiteral("dark");
}

UiTheme themeFromSettingsValue(const QString &value) {
    return value.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0 ? UiTheme::Light : UiTheme::Dark;
}

UiTheme loadUiTheme() {
    return themeFromSettingsValue(QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("dark")).toString());
}

void saveUiTheme(UiTheme theme) {
    QSettings().setValue(QStringLiteral("ui/theme"), themeSettingsValue(theme));
}

QColor defaultCanvasColor(UiTheme theme) {
    return uiColors(theme).secondary;
}

CanvasColorSettings loadCanvasColorSettings() {
    QSettings settings;
    CanvasColorSettings result;
    result.darkMode = settings.value(QStringLiteral("ui/canvas/darkMode"), QStringLiteral("default")).toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom
        : CanvasColorMode::ThemeDefault;
    result.lightMode = settings.value(QStringLiteral("ui/canvas/lightMode"), QStringLiteral("default")).toString() == QStringLiteral("custom")
        ? CanvasColorMode::Custom
        : CanvasColorMode::ThemeDefault;

    const QColor legacy(settings.value(QStringLiteral("ui/canvasColor")).toString());
    const QColor dark(settings.value(QStringLiteral("ui/canvas/darkCustom"),
                                     legacy.isValid() ? legacy.name(QColor::HexRgb)
                                                      : defaultCanvasColor(UiTheme::Dark).name(QColor::HexRgb)).toString());
    const QColor light(settings.value(QStringLiteral("ui/canvas/lightCustom"),
                                      defaultCanvasColor(UiTheme::Light).name(QColor::HexRgb)).toString());
    result.darkCustom = validColor(dark, defaultCanvasColor(UiTheme::Dark));
    result.lightCustom = validColor(light, defaultCanvasColor(UiTheme::Light));
    if (legacy.isValid() && !settings.contains(QStringLiteral("ui/canvas/darkMode"))) {
        result.darkMode = CanvasColorMode::Custom;
    }
    return result;
}

void saveCanvasColorSettings(const CanvasColorSettings &settings) {
    QSettings qsettings;
    qsettings.remove(QStringLiteral("ui/canvasColor"));
    qsettings.setValue(QStringLiteral("ui/canvas/darkMode"), settings.darkMode == CanvasColorMode::Custom ? QStringLiteral("custom") : QStringLiteral("default"));
    qsettings.setValue(QStringLiteral("ui/canvas/lightMode"), settings.lightMode == CanvasColorMode::Custom ? QStringLiteral("custom") : QStringLiteral("default"));
    qsettings.setValue(QStringLiteral("ui/canvas/darkCustom"),
                       validColor(settings.darkCustom, defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
    qsettings.setValue(QStringLiteral("ui/canvas/lightCustom"),
                       validColor(settings.lightCustom, defaultCanvasColor(UiTheme::Light)).name(QColor::HexRgb));
}

TransformModeSettings loadTransformModeSettings() {
    TransformModeSettings result;
    result.relativeMode = QSettings().value(QStringLiteral("ui/transform/relativeModeOption"), false).toBool();
    return result;
}

void saveTransformModeSettings(const TransformModeSettings &settings) {
    QSettings qsettings;
    qsettings.remove(QStringLiteral("ui/transform/relativeMode"));
    qsettings.setValue(QStringLiteral("ui/transform/relativeModeOption"), settings.relativeMode);
}

BehaviorSettings loadBehaviorSettings() {
    QSettings settings;
    const BehaviorSettings defaults;
    BehaviorSettings result = defaults;
    result.insertShapeWithLastSelectedColor = settings.value(QStringLiteral("ui/behavior/insertShapeWithLastSelectedColor"), result.insertShapeWithLastSelectedColor).toBool();
    result.insertShapeWithLastSelectedScale = settings.value(QStringLiteral("ui/behavior/insertShapeWithLastSelectedScale"), result.insertShapeWithLastSelectedScale).toBool();
    const QString fillMode = settings.value(QStringLiteral("ui/behavior/contourFillMode"),
        QStringLiteral("analytic")).toString();
    if (fillMode == QStringLiteral("compact") || fillMode == QStringLiteral("catalog")
        || fillMode == QStringLiteral("thin")) {
        result.contourFillMode = ContourFillMode::CompactFit;
    }
    const bool legacySavePaths = settings.value(QStringLiteral("ui/behavior/autoSavePaths"), false).toBool();
    result.autoSavePenPaths = settings.value(QStringLiteral("ui/behavior/autoSavePenPaths"), legacySavePaths).toBool();
    result.autoSaveLiningPaths = settings.value(QStringLiteral("ui/behavior/autoSaveLiningPaths"), legacySavePaths).toBool();
    result.bucketTolerance = std::clamp(settings.value(QStringLiteral("ui/behavior/bucketTolerance"), result.bucketTolerance).toInt(), 0, 255);
    result.liningWidth = settings.value(QStringLiteral("ui/behavior/liningWidth"), result.liningWidth).toDouble();
    result.liningOutsideMargin = settings.value(QStringLiteral("ui/behavior/liningOutsideMargin"), result.liningOutsideMargin).toDouble();
    if (!std::isfinite(result.liningWidth)) result.liningWidth = defaults.liningWidth;
    if (!std::isfinite(result.liningOutsideMargin)) result.liningOutsideMargin = defaults.liningOutsideMargin;
    result.liningWidth = std::clamp(result.liningWidth, 0.1, 256.0);
    result.liningOutsideMargin = std::clamp(result.liningOutsideMargin, 0.0, 2.0);
    result.showPropertyDebug = settings.value(QStringLiteral("ui/behavior/showPropertyDebug"), result.showPropertyDebug).toBool();
    result.moveToolAutoSelect = settings.value(QStringLiteral("ui/behavior/moveToolAutoSelect"), result.moveToolAutoSelect).toBool();
    const bool previousSnapping = settings.value(QStringLiteral("ui/behavior/snapToGuidelines"), result.automaticSnapping).toBool()
        || settings.value(QStringLiteral("ui/behavior/snapToShapes"), result.automaticSnapping).toBool();
    result.automaticSnapping = settings.value(QStringLiteral("ui/behavior/automaticSnapping"), previousSnapping).toBool();
    result.pipetteAutoReturn = settings.value(
        QStringLiteral("ui/behavior/pipetteAutoReturn"), result.pipetteAutoReturn).toBool();
    result.selectionFlashEnabled = settings.value(QStringLiteral("ui/behavior/selectionFlashEnabled"), result.selectionFlashEnabled).toBool();
    result.displayAnchorsDuringTransformDrag = settings.value(QStringLiteral("ui/behavior/displayAnchorsDuringTransformDrag"), result.displayAnchorsDuringTransformDrag).toBool();
    result.generatePreviewsWithTransformations = settings.value(QStringLiteral("ui/behavior/generatePreviewsWithTransformations"), result.generatePreviewsWithTransformations).toBool();
    result.guideLayersVisible = settings.value(QStringLiteral("ui/behavior/guideLayersVisible"), result.guideLayersVisible).toBool();
    result.guideLayersOnTop = settings.value(QStringLiteral("ui/behavior/guideLayersOnTop"), result.guideLayersOnTop).toBool();
    result.guidelinesVisible = settings.value(QStringLiteral("ui/behavior/guidelinesVisible"), result.guidelinesVisible).toBool();
    result.guidelinesLocked = settings.value(QStringLiteral("ui/behavior/guidelinesLocked"), result.guidelinesLocked).toBool();
    result.guidelineColor = QColor(settings.value(QStringLiteral("ui/behavior/guidelineColor"),
                                                 result.guidelineColor.name(QColor::HexArgb)).toString());
    result.visibilityBordersEnabled = settings.value(QStringLiteral("ui/behavior/visibilityBordersEnabled"), result.visibilityBordersEnabled).toBool();
    result.valueEditingWheelEnabled = settings.value(QStringLiteral("ui/behavior/valueEditingWheelEnabled"), result.valueEditingWheelEnabled).toBool();
    const QSize resolution = settings.value(QStringLiteral("ui/behavior/visibilityBorderResolution"), result.visibilityBorderResolution).toSize();
    result.visibilityBorderResolution = resolution.isValid() ? resolution : defaults.visibilityBorderResolution;
    result.nudgeStep = settings.value(QStringLiteral("ui/behavior/nudgeStep"), result.nudgeStep).toDouble();
    result.nudgeShiftStep = settings.value(QStringLiteral("ui/behavior/nudgeShiftStep"), result.nudgeShiftStep).toDouble();
    result.liveryTextureScale = settings.value(QStringLiteral("ui/behavior/liveryTextureScale"), result.liveryTextureScale).toInt();
    result.autosaveIntervalMinutes = settings.value(QStringLiteral("ui/behavior/autosaveIntervalMinutes"), result.autosaveIntervalMinutes).toInt();
    if (settings.contains(QStringLiteral("ui/behavior/autosaveEnabled"))
        && !settings.value(QStringLiteral("ui/behavior/autosaveEnabled")).toBool()) {
        result.autosaveIntervalMinutes = 0;
    }
    const QVariantMap gameFolders = settings.value(QStringLiteral("ui/behavior/gameFolders")).toMap();
    for (auto folder = gameFolders.cbegin(); folder != gameFolders.cend(); ++folder) {
        result.gameFolders.insert(folder.key(), folder.value().toString());
    }
    if (!result.gameFolders.contains(QStringLiteral("FH6"))) {
        result.gameFolders.insert(QStringLiteral("FH6"), settings.value(QStringLiteral("ui/behavior/gameFolder")).toString());
    }
    result.discardModelOnLiveryOpen = settings.value(QStringLiteral("ui/behavior/discardModelOnLiveryOpen"), result.discardModelOnLiveryOpen).toBool();
    result.loadCarTextures = settings.value(QStringLiteral("ui/behavior/loadCarTextures"), result.loadCarTextures).toBool();
    result.pasteInPlace = settings.value(
        QStringLiteral("ui/behavior/pasteInPlace"), result.pasteInPlace).toBool();
    result.verticalToolbar = settings.value(QStringLiteral("ui/behavior/verticalToolbar"), result.verticalToolbar).toBool();
    result.separateSelectAndMarqueeTools = settings.value(
        QStringLiteral("ui/behavior/separateSelectAndMarqueeTools"), result.separateSelectAndMarqueeTools).toBool();
    result.selectShapesByBoundingBox = settings.value(
        QStringLiteral("ui/behavior/selectShapesByBoundingBox"), result.selectShapesByBoundingBox).toBool();
    result.showSelectedShapeOutlines = settings.value(
        QStringLiteral("ui/behavior/showSelectedShapeOutlines"), result.showSelectedShapeOutlines).toBool();
    result.separateOpacityAndSkewTools = settings.value(
        QStringLiteral("ui/behavior/separateOpacityAndSkewTools"),
        result.separateOpacityAndSkewTools).toBool();
    result.guidelineColor = validColor(result.guidelineColor, defaults.guidelineColor);
    if (result.nudgeStep <= 0.0) {
        result.nudgeStep = defaults.nudgeStep;
    }
    if (result.nudgeShiftStep <= 0.0) {
        result.nudgeShiftStep = defaults.nudgeShiftStep;
    }
    result.liveryTextureScale = std::clamp(result.liveryTextureScale, 1, 8);
    result.autosaveIntervalMinutes = std::clamp(result.autosaveIntervalMinutes, 0, 1440);
    return result;
}

void saveBehaviorSettings(const BehaviorSettings &settings) {
    QSettings qsettings;
    QVariantMap gameFolders;
    const BehaviorSettings defaults;
    qsettings.setValue(QStringLiteral("ui/behavior/insertShapeWithLastSelectedColor"), settings.insertShapeWithLastSelectedColor);
    qsettings.setValue(QStringLiteral("ui/behavior/insertShapeWithLastSelectedScale"), settings.insertShapeWithLastSelectedScale);
    const QString fillMode = settings.contourFillMode == ContourFillMode::CompactFit
        ? QStringLiteral("compact") : QStringLiteral("analytic");
    qsettings.setValue(QStringLiteral("ui/behavior/contourFillMode"), fillMode);
    qsettings.remove(QStringLiteral("ui/behavior/differentiablePenFill"));
    qsettings.remove(QStringLiteral("ui/behavior/differentialContourFill"));
    qsettings.remove(QStringLiteral("ui/behavior/autoSavePaths"));
    qsettings.setValue(QStringLiteral("ui/behavior/autoSavePenPaths"), settings.autoSavePenPaths);
    qsettings.setValue(QStringLiteral("ui/behavior/autoSaveLiningPaths"), settings.autoSaveLiningPaths);
    qsettings.remove(QStringLiteral("ui/behavior/liningFillMode"));
    qsettings.remove(QStringLiteral("ui/behavior/bucketRegionMode"));
    qsettings.setValue(QStringLiteral("ui/behavior/bucketTolerance"), settings.bucketTolerance);
    qsettings.setValue(QStringLiteral("ui/behavior/liningWidth"), settings.liningWidth);
    qsettings.setValue(QStringLiteral("ui/behavior/liningOutsideMargin"), settings.liningOutsideMargin);
    qsettings.setValue(QStringLiteral("ui/behavior/showPropertyDebug"), settings.showPropertyDebug);
    qsettings.setValue(QStringLiteral("ui/behavior/moveToolAutoSelect"), settings.moveToolAutoSelect);
    qsettings.setValue(QStringLiteral("ui/behavior/automaticSnapping"), settings.automaticSnapping);
    qsettings.remove(QStringLiteral("ui/behavior/snapToGuidelines"));
    qsettings.remove(QStringLiteral("ui/behavior/snapToShapes"));
    qsettings.remove(QStringLiteral("ui/behavior/allowMoveOutsideBoundingBox"));
    qsettings.setValue(QStringLiteral("ui/behavior/pipetteAutoReturn"), settings.pipetteAutoReturn);
    qsettings.setValue(QStringLiteral("ui/behavior/selectionFlashEnabled"), settings.selectionFlashEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/displayAnchorsDuringTransformDrag"), settings.displayAnchorsDuringTransformDrag);
    qsettings.setValue(QStringLiteral("ui/behavior/generatePreviewsWithTransformations"), settings.generatePreviewsWithTransformations);
    qsettings.setValue(QStringLiteral("ui/behavior/guideLayersVisible"), settings.guideLayersVisible);
    qsettings.setValue(QStringLiteral("ui/behavior/guideLayersOnTop"), settings.guideLayersOnTop);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelinesVisible"), settings.guidelinesVisible);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelinesLocked"), settings.guidelinesLocked);
    qsettings.setValue(QStringLiteral("ui/behavior/guidelineColor"),
                       validColor(settings.guidelineColor, defaults.guidelineColor).name(QColor::HexArgb));
    qsettings.setValue(QStringLiteral("ui/behavior/visibilityBordersEnabled"), settings.visibilityBordersEnabled);
    qsettings.remove(QStringLiteral("ui/behavior/positionLimitBorderEnabled"));
    qsettings.setValue(QStringLiteral("ui/behavior/valueEditingWheelEnabled"), settings.valueEditingWheelEnabled);
    qsettings.setValue(QStringLiteral("ui/behavior/visibilityBorderResolution"), settings.visibilityBorderResolution);
    qsettings.setValue(QStringLiteral("ui/behavior/nudgeStep"), settings.nudgeStep);
    qsettings.setValue(QStringLiteral("ui/behavior/nudgeShiftStep"), settings.nudgeShiftStep);
    qsettings.setValue(QStringLiteral("ui/behavior/liveryTextureScale"), std::clamp(settings.liveryTextureScale, 1, 8));
    qsettings.setValue(QStringLiteral("ui/behavior/autosaveIntervalMinutes"), std::clamp(settings.autosaveIntervalMinutes, 0, 1440));
    qsettings.remove(QStringLiteral("ui/behavior/autosaveEnabled"));
    for (auto folder = settings.gameFolders.cbegin(); folder != settings.gameFolders.cend(); ++folder) {
        gameFolders.insert(folder.key(), folder.value());
    }
    qsettings.setValue(QStringLiteral("ui/behavior/gameFolders"), gameFolders);
    qsettings.remove(QStringLiteral("ui/behavior/gameFolder"));
    qsettings.setValue(QStringLiteral("ui/behavior/discardModelOnLiveryOpen"), settings.discardModelOnLiveryOpen);
    qsettings.setValue(QStringLiteral("ui/behavior/loadCarTextures"), settings.loadCarTextures);
    qsettings.setValue(QStringLiteral("ui/behavior/pasteInPlace"), settings.pasteInPlace);
    qsettings.setValue(QStringLiteral("ui/behavior/verticalToolbar"), settings.verticalToolbar);
    qsettings.setValue(QStringLiteral("ui/behavior/separateSelectAndMarqueeTools"), settings.separateSelectAndMarqueeTools);
    qsettings.setValue(QStringLiteral("ui/behavior/selectShapesByBoundingBox"), settings.selectShapesByBoundingBox);
    qsettings.setValue(QStringLiteral("ui/behavior/showSelectedShapeOutlines"), settings.showSelectedShapeOutlines);
    qsettings.setValue(QStringLiteral("ui/behavior/separateOpacityAndSkewTools"),
                       settings.separateOpacityAndSkewTools);
}

PreviewBackgroundSettings loadPreviewBackgroundSettings() {
    QSettings settings;
    PreviewBackgroundSettings result;
    result.buffer.mode = previewBackgroundMode(
        settings.value(QStringLiteral("ui/previews/bufferMode"), QStringLiteral("default")).toString());
    result.layers.mode = previewBackgroundMode(
        settings.value(QStringLiteral("ui/previews/layersMode"), QStringLiteral("default")).toString());
    result.buffer.custom = QColor(settings.value(
        QStringLiteral("ui/previews/bufferCustom"), defaultCanvasColor(UiTheme::Dark).name()).toString());
    result.layers.custom = QColor(settings.value(
        QStringLiteral("ui/previews/layersCustom"), defaultCanvasColor(UiTheme::Dark).name()).toString());
    result.buffer.custom = validColor(result.buffer.custom, defaultCanvasColor(UiTheme::Dark));
    result.layers.custom = validColor(result.layers.custom, defaultCanvasColor(UiTheme::Dark));

    return result;
}

void savePreviewBackgroundSettings(const PreviewBackgroundSettings &settings) {
    QSettings qsettings;
    qsettings.setValue(QStringLiteral("ui/previews/bufferMode"),
                       previewBackgroundModeValue(settings.buffer.mode));
    qsettings.setValue(QStringLiteral("ui/previews/layersMode"),
                       previewBackgroundModeValue(settings.layers.mode));
    qsettings.setValue(QStringLiteral("ui/previews/bufferCustom"),
                       validColor(settings.buffer.custom, defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
    qsettings.setValue(QStringLiteral("ui/previews/layersCustom"),
                       validColor(settings.layers.custom, defaultCanvasColor(UiTheme::Dark)).name(QColor::HexRgb));
}

QColor defaultPreviewBackgroundColor(UiTheme theme) {
    return paletteForTheme(theme).color(QPalette::Base);
}

QColor previewBackgroundColor(UiTheme theme, const PreviewBackground &background) {
    if (background.mode == PreviewBackgroundMode::Custom && background.custom.isValid()) {
        return background.custom;
    }

    return defaultPreviewBackgroundColor(theme);
}

QBrush previewBackgroundBrush(UiTheme theme, const PreviewBackground &background) {
    if (background.mode != PreviewBackgroundMode::Checkerboard) {
        return QBrush(previewBackgroundColor(theme, background));
    }
    constexpr int kCheckerExtent = 8;
    constexpr int kCheckerTileExtent = kCheckerExtent * 2;
    const QColor base = defaultPreviewBackgroundColor(theme);
    const QColor alternate = isDarkTheme(theme) ? base.lighter(145) : base.darker(112);
    QImage tile(kCheckerTileExtent, kCheckerTileExtent, QImage::Format_RGB32);
    tile.fill(base);
    QPainter painter(&tile);
    painter.fillRect(0, 0, kCheckerExtent, kCheckerExtent, alternate);
    painter.fillRect(kCheckerExtent, kCheckerExtent, kCheckerExtent, kCheckerExtent, alternate);

    return QBrush(QPixmap::fromImage(tile));
}

QColor canvasColorForTheme(UiTheme theme, const CanvasColorSettings &settings) {
    const CanvasColorMode mode = theme == UiTheme::Light ? settings.lightMode : settings.darkMode;
    const QColor custom = theme == UiTheme::Light ? settings.lightCustom : settings.darkCustom;

    return mode == CanvasColorMode::Custom && custom.isValid() ? custom : defaultCanvasColor(theme);
}

bool isDarkTheme(UiTheme theme) {
    return theme == UiTheme::Dark;
}

QColor iconColorForTheme(UiTheme theme) {
    return uiColors(theme).text;
}

QPalette paletteForTheme(UiTheme theme) {
    const UiColors colors = uiColors(theme);
    QPalette palette;
    palette.setColor(QPalette::Window, colors.main);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::Base, colors.secondary);
    palette.setColor(QPalette::AlternateBase, colors.main);
    palette.setColor(QPalette::ToolTipBase, colors.secondary);
    palette.setColor(QPalette::ToolTipText, colors.text);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::Button, colors.secondary);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::Highlight, colors.selection);
    palette.setColor(QPalette::HighlightedText, colors.text);
    palette.setColor(QPalette::PlaceholderText, colors.mutedText);
    palette.setColor(QPalette::Link, colors.focus);
    palette.setColor(QPalette::Mid, colors.border);
    palette.setColor(QPalette::Midlight, colors.border);
    palette.setColor(QPalette::Dark, colors.border);
    palette.setColor(QPalette::Light, colors.border);
    palette.setColor(QPalette::Shadow, colors.text);
    palette.setColor(QPalette::BrightText, colors.text);
    palette.setColor(QPalette::LinkVisited, colors.focus);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    palette.setColor(QPalette::Accent, colors.focus);
#endif
    palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, colors.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors.disabledText);

    return palette;
}

void applyUiTheme(QApplication &app, UiTheme theme) {
    currentTheme = theme;
    QPixmapCache::clear();
    if (QStyleFactory::keys().contains(QStringLiteral("Fusion"))) {
        app.setStyle(new ThemeStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    }
    app.setPalette(paletteForTheme(theme));
    app.setStyleSheet(themeStyleSheet(theme));
}

UiTheme currentUiTheme() {
    return currentTheme;
}

} // namespace gui
