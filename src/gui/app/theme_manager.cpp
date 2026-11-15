#include "theme_manager.h"
#include "gui_assets.h"

#include <QApplication>
#include <QIconEngine>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QLayout>
#include <QMainWindow>
#include <QTabBar>
#include <QToolBar>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLinearGradient>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QProxyStyle>
#include <QSettings>
#include <QStyleFactory>
#include <QStyleOption>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <optional>

namespace gui {
namespace {

UiTheme currentTheme = kDefaultUiTheme;
std::optional<ThemeColorOverrides> currentColorOverrides;
QMap<UiTheme, UiColors> resolvedColors;

constexpr double kGlyphStrokeWidth = 1.5;
constexpr double kCheckStrokeWidth = 2.0;
constexpr int kMenuIconExtent = 16;
constexpr int kMenuIconPadding = 6;
constexpr int kSliderGrooveExtent = 6;
constexpr int kSliderHandleExtent = 14;
constexpr int kSliderHandleThickness = 20;
constexpr int kSliderInset = 1;
constexpr int kHueGrooveExtent = 14;
constexpr double kOutlineWidth = 1.0;
constexpr int kThemeSettingsVersion = 2;
constexpr double kTextPointSizeIncrease = 1.0;
constexpr int kTextPixelSizeIncrease = 1;
constexpr int kToolbarHoverLightness = 112;

bool isDockTabBar(const QWidget *widget) {
    return qobject_cast<const QTabBar *>(widget) != nullptr
        && qobject_cast<QMainWindow *>(widget->parentWidget()) != nullptr;
}

void drawResizeHandle(QPainter &painter, const QStyleOption &option) {
    const UiColors colors = uiColors(currentUiTheme());
    const QColor background = colors.resizeHandle;
    const QRect area = option.rect;
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    const bool pressed = option.state.testFlag(QStyle::State_Sunken);

    painter.save();
    painter.fillRect(area, pressed ? colors.selection : hovered ? uiSelectionHoverColor(colors.selection) : background);
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
        painter.setPen(active ? Qt::NoPen : QPen(enabled ? colors.border : colors.disabledText, kOutlineWidth));
        painter.setBrush(!enabled ? colors.main : active ? uiSelectionHoverColor(colors.selection) : colors.accent);
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

void drawAdd(QPainter &painter, const QRect &area, const QColor &color) {
    const double scale = static_cast<double>(std::min(area.width(), area.height())) / kUiActionIconExtent;
    const double halfExtent = kUiActionGlyphExtent / 2.0;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(QRectF(area).center());
    painter.scale(scale, scale);
    painter.setPen(QPen(color, kUiActionGlyphStrokeWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(-halfExtent, 0), QPointF(halfExtent, 0));
    painter.drawLine(QPointF(0, -halfExtent), QPointF(0, halfExtent));
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

enum class ChromeAction { Close, Add };

class ChromeIconEngine final : public QIconEngine {
public:
    explicit ChromeIconEngine(std::optional<Qt::ArrowType> direction, ChromeAction action = ChromeAction::Close)
        : direction_(direction), action_(action) {}

    QIconEngine *clone() const override {
        return new ChromeIconEngine(direction_, action_);
    }

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        const UiColors colors = uiColors(currentUiTheme());
        const QColor foreground = mode == QIcon::Disabled ? colors.disabledText : colors.icon;

        if (direction_.has_value()) {
            drawArrow(*painter, rect, *direction_, foreground);
        } else if (action_ == ChromeAction::Add) {
            drawAdd(*painter, rect, foreground);
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
    ChromeAction action_;
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
    painter.setPen(active ? Qt::NoPen : QPen(colors.border, kOutlineWidth));
    painter.setBrush(!enabled ? colors.main : active ? uiSelectionHoverColor(colors.selection)
        : checked || mixed ? colors.selection : colors.secondary);
    painter.drawRoundedRect(box, kUiCornerRadius, kUiCornerRadius);
    painter.setPen(QPen(!enabled ? colors.disabledText : active || checked || mixed ? colors.selectedText : colors.text,
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

    using QProxyStyle::polish;

    void polish(QWidget *widget) override {
        QProxyStyle::polish(widget);
        if (auto *lineEditor = qobject_cast<QLineEdit *>(widget)) {
            lineEditor->setAlignment((lineEditor->alignment() & ~Qt::AlignVertical_Mask) | Qt::AlignVCenter);
        } else if (auto *spinEditor = qobject_cast<QAbstractSpinBox *>(widget)) {
            spinEditor->setAlignment((spinEditor->alignment() & ~Qt::AlignVertical_Mask) | Qt::AlignVCenter);
        } else if (auto *tabs = qobject_cast<QTabBar *>(widget); isDockTabBar(tabs)) {
            const int iconExtent = uiTextPixelSize(tabs->font());

            tabs->setIconSize(QSize(iconExtent, iconExtent));
            tabs->setDrawBase(false);
        }
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override {
        const UiColors colors = uiColors(currentUiTheme());
        std::optional<Qt::ArrowType> direction;

        switch (element) {
        case PE_FrameDockWidget:
            painter->fillRect(option->rect, colors.secondary);
            return;
        case PE_IndicatorDockWidgetResizeHandle:
            drawResizeHandle(*painter, *option);
            return;
        case PE_IndicatorCheckBox:
        case PE_IndicatorItemViewItemCheck:
            drawCheckBox(*painter, *option);
            return;
        case PE_IndicatorTabClose:
            drawClose(*painter, option->rect,
                      option->state.testFlag(State_Enabled) ? colors.icon : colors.disabledText);
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
                      option->state.testFlag(State_Enabled) ? colors.icon : colors.disabledText);
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

    int styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget,
                  QStyleHintReturn *returnData) const override {
        if (hint == SH_UnderlineShortcut && (qobject_cast<const QMenuBar *>(widget) != nullptr
            || qobject_cast<const QMenu *>(widget) != nullptr)) {
            return false;
        }

        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override {
        switch (metric) {
        case PM_LineEditIconSize:
            return uiTextPixelSize(widget != nullptr ? widget->font() : uiTextFont());
        case PM_IndicatorWidth:
        case PM_IndicatorHeight:
            return qobject_cast<const QMenu *>(widget) != nullptr ? kMenuIconExtent
                : kUiControlHeight;
        case PM_TabBarScrollButtonWidth:
            return kUiCompactControlExtent;
        case PM_TabBarTabShiftVertical:
            return isDockTabBar(widget) ? 0 : QProxyStyle::pixelMetric(metric, option, widget);
        case PM_SliderLength:
            return kSliderHandleExtent;
        case PM_SliderControlThickness:
            return kSliderHandleThickness;
        case PM_SliderThickness:
            return kUiControlHeight;
        case PM_SplitterWidth:
        case PM_DockWidgetSeparatorExtent:
        case PM_DockWidgetFrameWidth:
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
    const QFont secondaryFont = uiSecondaryTextFont();
    const QMargins textMargins = uiTextInsets(uiTextFont());
    const int textTopInset = textMargins.top();
    const int textBottomInset = textMargins.bottom();
    const int textInsets = textTopInset + textBottomInset;
    const int controlContent = kUiControlHeight - 2 * kUiControlBorderWidth - textInsets;
    QString sheet = QStringLiteral(R"(
        QWidget { color: @text; }
        QWidget:disabled { color: @disabled; }
        QMainWindow, QDialog { background: @main; color: @text; }
        QDockWidget { color: @text; }
        QWidget#dockHeader {
            background: @secondary; border: none; border-bottom: @separatorExtentpx solid @border;
        }
        QWidget#dockHeader[panelFocused="true"] { background: @accent; }
        QWidget#dockHeader QLabel { background: transparent; border: none; margin: 0; padding: 0; }
        QFrame#dockBody {
            background: @secondary; border: none;
        }
        QLabel#dockTitle { font-weight: 600; background: transparent; border: none; }
        QLabel { color: @text; background: transparent; }
        QLabel[uiControlLabel="true"] { padding: @baselineTopInsetpx 0 @baselineBottomInsetpx 0; }
        QLabel:disabled { color: @disabled; }
        QLabel[secondary="true"], QLabel[uiState="mixed"] { color: @muted; }
        QLabel[uiState="warning"] { color: @warning; }
        QLabel[uiState="error"] { color: @error; }
        QPushButton, QToolButton {
            color: @text; background: @buttonBackground;
            border: 1px solid @border; border-radius: @radiuspx;
            min-height: @controlContentpx; max-height: @controlContentpx;
            padding: @baselineTopInsetpx 8px @baselineBottomInsetpx 8px;
        }
        QToolButton { padding: @baselineTopInsetpx 6px @baselineBottomInsetpx 6px; }
        QPushButton:hover, QToolButton:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QPushButton:pressed, QToolButton:pressed,
        QPushButton:checked, QToolButton:checked { color: @selectedText; background: @selection; border-color: transparent; }
        QPushButton:checked:hover, QToolButton:checked:hover { background: @highlightFill; }
        QPushButton:focus, QPushButton:default, QToolButton:focus { border-color: @focus; }
        QPushButton:focus:hover, QPushButton:default:hover, QToolButton:focus:hover { border-color: transparent; }
        QPushButton:disabled, QToolButton:disabled {
            color: @disabled; background: @main; border-color: @border;
        }
        QToolBar#MainToolBar QToolButton {
            background: transparent; border-color: transparent;
            padding: @baselineTopInsetpx @toolbarPaddingpx @baselineBottomInsetpx @toolbarPaddingpx;
        }
        QToolBar#MainToolBar QToolButton:checked,
        QToolBar#MainToolBar QToolButton:pressed { color: @selectedText; background: @selection; border-color: transparent; }
        QToolBar#MainToolBar QToolButton:hover,
        QToolBar#MainToolBar QToolButton:checked:hover {
            color: @selectedText; background: @highlightFill; border-color: transparent;
        }
        QToolBar#MainToolBar QToolButton:pressed:hover { background: @selection; border-color: transparent; }
        QToolBar#MainToolBar QToolButton:disabled {
            color: @disabled; background: transparent; border-color: transparent;
        }
        QToolButton[uiCompact="true"] { min-height: 0; min-width: 0; padding: 0; }
        QToolButton[uiFlat="true"] { background: transparent; border-color: transparent; }
        QToolButton[uiFlat="true"]:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QToolButton[uiDockButton="true"] {
            background: transparent; border: 1px solid transparent; border-radius: @radiuspx;
            min-height: @dockExtentpx; max-height: @dockExtentpx;
            min-width: @dockExtentpx; max-width: @dockExtentpx; margin: 0; padding: 0;
        }
        QToolButton[uiDockButton="true"]:hover { background: @highlightFill; border-color: transparent; }
        QToolButton[uiDockButton="true"]:pressed { background: @selection; border-color: transparent; }
        QToolButton[uiDockButton="true"]:disabled { background: transparent; border-color: transparent; }
        QPushButton[uiSquareButton="true"], QToolButton[uiSquareButton="true"] {
            min-height: @squareExtentpx; max-height: @squareExtentpx;
            min-width: @squareExtentpx; max-width: @squareExtentpx; margin: 0; padding: 0;
        }
        QToolButton[uiHelpButton="true"] {
            min-height: @inputContentpx; max-height: @inputContentpx;
            min-width: @squareExtentpx; max-width: @squareExtentpx; margin: 0;
            padding: @baselineTopInsetpx 0 @baselineBottomInsetpx 0;
        }
        QCheckBox {
            color: @text; background: transparent; spacing: @spacingpx;
            min-height: @checkRowContentpx; max-height: @checkRowContentpx; margin: 0;
            padding: @baselineTopInsetpx 0 @baselineBottomInsetpx 0;
        }
        QCheckBox:disabled { color: @disabled; }
        QCheckBox::indicator {
            width: @checkExtentpx; height: @checkExtentpx;
            subcontrol-origin: margin; subcontrol-position: center left;
        }
        QLineEdit, QComboBox, QTextEdit, QPlainTextEdit, QAbstractSpinBox {
            color: @text; background: @editorBackground; border: 1px solid @border;
            border-radius: @radiuspx; padding: 3px 7px;
            selection-background-color: @selection; selection-color: @selectedText;
        }
        QLineEdit, QComboBox, QAbstractSpinBox {
            min-height: @inputContentpx; max-height: @inputContentpx;
            padding-top: @baselineTopInsetpx; padding-bottom: @baselineBottomInsetpx;
        }
        QComboBox, QAbstractSpinBox { padding-right: 22px; }
        QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover,
        QTextEdit:hover, QPlainTextEdit:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QLineEdit:focus, QComboBox:focus, QTextEdit:focus,
        QPlainTextEdit:focus, QAbstractSpinBox:focus { border-color: @focus; }
        QLineEdit:focus:hover, QComboBox:focus:hover, QAbstractSpinBox:focus:hover,
        QTextEdit:focus:hover, QPlainTextEdit:focus:hover { border-color: transparent; }
        QLineEdit:disabled, QComboBox:disabled, QTextEdit:disabled,
        QPlainTextEdit:disabled, QAbstractSpinBox:disabled {
            color: @disabled;
        }
        QLineEdit[uiState="mixed"], QAbstractSpinBox[uiState="mixed"],
        QAbstractSpinBox[uiState="mixed"] QLineEdit { color: @muted; }
        QPushButton[uiState="mixed"] { background: @contrast; color: @text; }
        QLineEdit[uiState="error"] { border-color: @error; }
        QAbstractSpinBox QLineEdit, QComboBox QLineEdit {
            background: @editorBackground; border: none; border-radius: 0; padding: 0; min-height: 0;
            max-height: @innerEditorLimitpx;
        }
        QLineEdit > QToolButton {
            background: transparent; border: none; margin: 0; padding: 0;
            min-width: 0; min-height: 0; max-width: @innerEditorLimitpx; max-height: @innerEditorLimitpx;
        }
        QLineEdit > QToolButton:hover { background: @highlightFill; }
        QAbstractSpinBox:hover QLineEdit, QComboBox:hover QLineEdit {
            color: @selectedText; background: @highlightFill;
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
            border: none; selection-background-color: @selection; selection-color: @selectedText;
            outline: 0;
        }
        QAbstractItemView[uiMainSurface="true"] { background: @main; alternate-background-color: @main; }
        QAbstractItemView::item {
            min-height: @fieldHeightpx; padding: 0 4px; border: 1px solid transparent; border-radius: @radiuspx;
        }
        QAbstractItemView::item:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QAbstractItemView::item:selected { background: @selection; color: @selectedText; }
        QAbstractItemView::item:focus { border: 1px solid @focus; }
        QAbstractItemView::item:focus:hover { border-color: transparent; }
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
        QScrollBar::handle:hover { background: @highlightFill; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QSlider:horizontal { min-height: @controlHeightpx; }
        QSlider:vertical { min-width: @controlHeightpx; }
        QToolBar { background: @toolbarBackground; border: none; margin: 0; spacing: @spacingpx; padding: 0; }
        QToolBar#MainToolBar:horizontal { border-bottom: @separatorExtentpx solid @border; }
        QToolBar#MainToolBar:vertical { border-right: @separatorExtentpx solid @border; }
        QToolBar#ToolSettingsBar {
            background: @toolbarBackground; border-bottom: @separatorExtentpx solid @border;
            margin: 0; padding: 0; spacing: 0;
        }
        QScrollArea#ToolSettingsPanel, QWidget#ToolSettings { background: @toolbarBackground; margin: 0; padding: 0; }
        QScrollArea#ToolSettingsPanel > QWidget,
        QScrollArea#ToolSettingsPanel > QWidget > QWidget { background: @toolbarBackground; }
        QScrollArea#ToolSettingsPanel QScrollBar:horizontal { background: @toolbarBackground; height: @scrollExtentpx; }
        QScrollArea#ToolSettingsPanel QScrollBar::handle:horizontal { margin: 0; }
        QStatusBar { background: @statusBackground; color: @muted; }
        QStatusBar::item { border: none; }
        QMenuBar { background: @main; color: @text; border-bottom: @separatorExtentpx solid @border; }
        QMenuBar::item {
            padding: 5px 8px; margin: @highlightMarginpx @highlightMarginpx @highlightBottompx @highlightMarginpx;
            border-radius: @radiuspx; background: transparent;
        }
        QMenuBar::item:selected { color: @selectedText; background: @highlightFill; }
        QMenu {
            color: @text; background: @secondary; border: 1px solid @border;
            border-radius: @radiuspx; padding: 2px;
        }
        QMenu::item { padding: 3px @menuItemPaddingpx; margin: 0; border-radius: @radiuspx; }
        QMenu::icon, QMenu::indicator {
            subcontrol-origin: padding; subcontrol-position: left center; left: @menuIconPaddingpx;
        }
        QMenu::indicator { width: @menuIconpx; height: @menuIconpx; }
        QMenu::item:selected { color: @selectedText; background: @highlightFill; }
        QMenu::item:disabled { color: @disabled; }
        QMenu::separator { height: 1px; background: @border; margin: 2px 4px; }
        QToolTip {
            color: @tooltipText; background: @tooltipBackground; border: 1px solid @border;
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
        QTabBar::tab:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QTabBar QToolButton { min-height: 0; min-width: 0; margin: 0; padding: 0; }
        QMainWindow > QTabBar {
            background: @secondary; border: none; border-top: @separatorExtentpx solid @border;
            margin: 0; padding: 0;
        }
        QMainWindow > QTabBar::tab {
            color: @text; background: transparent; border-color: transparent;
            border-radius: @radiuspx;
            margin: @highlightBottompx @highlightMarginpx @highlightMarginpx @highlightMarginpx;
            padding: @baselineTopInsetpx @spacingpx @baselineBottomInsetpx @spacingpx;
        }
        QMainWindow > QTabBar::tab:top, QMainWindow > QTabBar::tab:bottom {
            min-height: @inputContentpx; max-height: @inputContentpx;
        }
        QMainWindow > QTabBar::tab:left, QMainWindow > QTabBar::tab:right {
            min-width: @inputContentpx; max-width: @inputContentpx;
            padding: @spacingpx @baselineBottomInsetpx @spacingpx @baselineTopInsetpx;
        }
        QMainWindow > QTabBar::tab:selected { color: @selectedText; background: @selection; border-color: transparent; }
        QMainWindow > QTabBar::tab:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QMainWindow > QTabBar::tab:selected:hover { background: @highlightFill; }
        QMainWindow > QTabBar::tab:disabled { color: @disabled; background: transparent; border-color: transparent; }
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
        QToolButton#previewSidePanelToggle:hover { color: @selectedText; background: @highlightFill; border-color: transparent; }
        QLabel#previewReference, QLabel#previewStatus {
            color: @text; background: @secondary; border: 1px solid @border;
            border-radius: @radiuspx; padding: 3px 6px;
        }
        QLabel#previewReference { font-size: @smallFontSize; }
        QLabel#previewStatus { padding: 6px 10px; }
        QFrame#paintColorPopup { background: @main; border: 1px solid @border; border-radius: @radiuspx; }
    )");
    const QMap<QString, QColor> colorTokens = {
        {QStringLiteral("@highlightFill"), uiSelectionHoverColor(colors.selection)},
        {QStringLiteral("@selectedText"), colors.selectedText},
        {QStringLiteral("@editorBackground"), colors.editorBackground},
        {QStringLiteral("@buttonBackground"), colors.buttonBackground},
        {QStringLiteral("@toolbarBackground"), colors.toolbarBackground},
        {QStringLiteral("@statusBackground"), colors.statusBackground},
        {QStringLiteral("@tooltipBackground"), colors.tooltipBackground},
        {QStringLiteral("@tooltipText"), colors.tooltipText},

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
        const QColor color = token.value();
        sheet.replace(token.key(), QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha()));
    }
    sheet.replace(QStringLiteral("@radius"), QString::number(kUiCornerRadius));
    sheet.replace(QStringLiteral("@fieldHeight"), QString::number(kUiControlHeight - 8));
    sheet.replace(QStringLiteral("@controlHeight"), QString::number(kUiControlHeight));
    sheet.replace(QStringLiteral("@controlContent"), QString::number(controlContent));
    sheet.replace(QStringLiteral("@inputContent"), QString::number(controlContent));
    sheet.replace(QStringLiteral("@checkExtent"), QString::number(kUiControlHeight));
    sheet.replace(QStringLiteral("@checkRowContent"), QString::number(kUiControlHeight - textInsets));
    sheet.replace(QStringLiteral("@baselineTopInset"), QString::number(textTopInset));
    sheet.replace(QStringLiteral("@baselineBottomInset"), QString::number(textBottomInset));
    sheet.replace(QStringLiteral("@toolbarPadding"), QString::number(kUiToolbarButtonPadding));
    sheet.replace(QStringLiteral("@separatorExtent"), QString::number(kUiControlBorderWidth));
    sheet.replace(QStringLiteral("@highlightMargin"), QString::number(kUiHighlightMargin));
    sheet.replace(QStringLiteral("@highlightBottom"), QString::number(kUiHighlightMargin + kUiControlBorderWidth));
    sheet.replace(QStringLiteral("@innerEditorLimit"), QString::number(QWIDGETSIZE_MAX));
    sheet.replace(QStringLiteral("@scrollExtent"), QString::number(kUiSplitterWidth));
    sheet.replace(QStringLiteral("@dockExtent"), QString::number(kUiControlHeight - 2 * kUiControlBorderWidth));
    sheet.replace(QStringLiteral("@squareExtent"), QString::number(kUiControlHeight - 2 * kUiControlBorderWidth));
    sheet.replace(QStringLiteral("@menuItemPadding"), QString::number(2 * kMenuIconPadding));
    sheet.replace(QStringLiteral("@menuIconPadding"), QString::number(kMenuIconPadding));
    sheet.replace(QStringLiteral("@menuIcon"), QString::number(kMenuIconExtent));
    sheet.replace(QStringLiteral("@spacing"), QString::number(kUiSpacing));
    sheet.replace(QStringLiteral("@smallFontSize"), secondaryFont.pointSizeF() > 0
        ? QStringLiteral("%1pt").arg(secondaryFont.pointSizeF())
        : QStringLiteral("%1px").arg(secondaryFont.pixelSize()));

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

const QVector<UiColorRole> &uiColorRoles() {
    static const QVector<UiColorRole> roles = {
        {QStringLiteral("main"), QStringLiteral("Main surface"), &UiColors::main},
        {QStringLiteral("secondary"), QStringLiteral("Secondary surface"), &UiColors::secondary},
        {QStringLiteral("contrast"), QStringLiteral("Contrast surface"), &UiColors::contrast},
        {QStringLiteral("accent"), QStringLiteral("Accent"), &UiColors::accent},
        {QStringLiteral("focus"), QStringLiteral("Focus outline"), &UiColors::focus},
        {QStringLiteral("border"), QStringLiteral("Border"), &UiColors::border},
        {QStringLiteral("text"), QStringLiteral("Text"), &UiColors::text},
        {QStringLiteral("mutedText"), QStringLiteral("Secondary text"), &UiColors::mutedText},
        {QStringLiteral("disabledText"), QStringLiteral("Disabled text"), &UiColors::disabledText},
        {QStringLiteral("hover"), QStringLiteral("Hover surface"), &UiColors::hover},
        {QStringLiteral("selection"), QStringLiteral("Selection surface"), &UiColors::selection},
        {QStringLiteral("warning"), QStringLiteral("Warning"), &UiColors::warning},
        {QStringLiteral("error"), QStringLiteral("Error"), &UiColors::error},
        {QStringLiteral("guide"), QStringLiteral("Guide layer"), &UiColors::guide},
        {QStringLiteral("selectedText"), QStringLiteral("Selected text"), &UiColors::selectedText, &UiColors::text},
        {QStringLiteral("icon"), QStringLiteral("Icons"), &UiColors::icon, &UiColors::text},
        {QStringLiteral("canvas"), QStringLiteral("Canvas background"), &UiColors::canvas, &UiColors::secondary},
        {QStringLiteral("preview"), QStringLiteral("Thumbnail background"), &UiColors::preview, &UiColors::secondary},
        {QStringLiteral("checkerboard"), QStringLiteral("Checkerboard alternate"), &UiColors::checkerboard, &UiColors::contrast},
        {QStringLiteral("carPreview"), QStringLiteral("3D preview background"), &UiColors::carPreview, &UiColors::secondary},
        {QStringLiteral("rulerBackground"), QStringLiteral("Ruler background"), &UiColors::rulerBackground, &UiColors::secondary},
        {QStringLiteral("rulerDivider"), QStringLiteral("Ruler divider"), &UiColors::rulerDivider, &UiColors::border},
        {QStringLiteral("rulerText"), QStringLiteral("Ruler text"), &UiColors::rulerText, &UiColors::mutedText},
        {QStringLiteral("resizeHandle"), QStringLiteral("Resize divider"), &UiColors::resizeHandle, &UiColors::contrast},
        {QStringLiteral("editorBackground"), QStringLiteral("Input background"), &UiColors::editorBackground, &UiColors::main},
        {QStringLiteral("buttonBackground"), QStringLiteral("Button background"), &UiColors::buttonBackground, &UiColors::secondary},
        {QStringLiteral("toolbarBackground"), QStringLiteral("Toolbar background"), &UiColors::toolbarBackground, &UiColors::main},
        {QStringLiteral("statusBackground"), QStringLiteral("Status bar background"), &UiColors::statusBackground, &UiColors::secondary},
        {QStringLiteral("tooltipBackground"), QStringLiteral("Tooltip background"), &UiColors::tooltipBackground, &UiColors::secondary},
        {QStringLiteral("tooltipText"), QStringLiteral("Tooltip text"), &UiColors::tooltipText, &UiColors::text},
        {QStringLiteral("browserBackground"), QStringLiteral("Browser tile background"), &UiColors::browserBackground, &UiColors::secondary},
        {QStringLiteral("browserPreview"), QStringLiteral("Browser preview background"), &UiColors::browserPreview, &UiColors::contrast},
    };

    return roles;
}

const QVector<UiThemeDefinition> &uiThemes() {
    static const QVector<UiThemeDefinition> themes = []() {
        QVector<UiThemeDefinition> result;
        QFile file(assetPath(QStringLiteral("themes.json")));
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonArray definitions = QJsonDocument::fromJson(file.readAll()).array();
            for (const QJsonValue &value : definitions) {
                const QJsonObject object = value.toObject();
                UiThemeDefinition definition;
                definition.id = object.value(QStringLiteral("id")).toString().trimmed().toLower();
                definition.name = object.value(QStringLiteral("name")).toString().trimmed();
                definition.dark = object.value(QStringLiteral("dark")).toBool(true);
                if (definition.id.isEmpty() || definition.name.isEmpty()
                    || definition.id.contains(QLatin1Char('/'))
                    || definition.id.contains(QLatin1Char('\\'))
                    || std::any_of(result.cbegin(), result.cend(), [&](const auto &existing) {
                        return existing.id == definition.id;
                    })) {
                    continue;
                }
                const QJsonObject colors = object.value(QStringLiteral("colors")).toObject();
                for (const UiColorRole &role : uiColorRoles()) {
                    const QColor color(colors.value(role.id).toString());
                    if (color.isValid()) {
                        definition.colors.insert(role.id, color);
                    }
                }
                result.push_back(definition);
            }
        }
        if (std::none_of(result.cbegin(), result.cend(), [](const auto &definition) {
                return definition.id == kDefaultUiTheme;
            })) {
            result.prepend({kDefaultUiTheme, QStringLiteral("Mahogany Dark"), {}, true});
        }

        return result;
    }();

    return themes;
}

ThemeColorOverrides themeColorOverrides() {
    if (!currentColorOverrides.has_value()) {
        ThemeColorOverrides overrides;
        QSettings settings;
        settings.beginGroup(QStringLiteral("ui/themeColors"));
        for (const QString &theme : settings.childGroups()) {
            settings.beginGroup(theme);
            for (const UiColorRole &role : uiColorRoles()) {
                const QColor color(settings.value(role.id).toString());
                if (color.isValid()) {
                    overrides[theme].insert(role.id, color);
                }
            }
            settings.endGroup();
        }
        currentColorOverrides = overrides;
    }

    return *currentColorOverrides;
}

void setThemeColorOverrides(const ThemeColorOverrides &overrides) {
    if (themeColorOverrides() == overrides) {
        return;
    }
    currentColorOverrides = overrides;
    resolvedColors.clear();
    QPixmapCache::clear();
}

void saveThemeColorOverrides() {
    const ThemeColorOverrides overrides = themeColorOverrides();
    QSettings settings;
    settings.beginGroup(QStringLiteral("ui/themeColors"));
    settings.remove(QString());
    for (auto theme = overrides.cbegin(); theme != overrides.cend(); ++theme) {
        settings.beginGroup(theme.key());
        for (const UiColorRole &role : uiColorRoles()) {
            const QColor color = theme.value().value(role.id);
            if (color.isValid()) {
                settings.setValue(role.id, color.name(QColor::HexArgb));
            }
        }
        settings.endGroup();
    }
}

UiColors uiColors(UiTheme theme) {
    static const UiColors defaults = {
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
    const auto cached = resolvedColors.constFind(theme);
    if (cached != resolvedColors.cend()) {
        return cached.value();
    }
    UiColors colors = defaults;
    const auto &definitions = uiThemes();
    const auto found = std::find_if(definitions.cbegin(), definitions.cend(), [&](const auto &definition) {
        return definition.id == theme;
    });
    const QMap<QString, QColor> definition = found == definitions.cend()
        ? QMap<QString, QColor>() : found->colors;
    const QMap<QString, QColor> overrides = themeColorOverrides().value(theme);
    const auto applyColor = [&](const UiColorRole &role) {
        const QColor color = overrides.value(role.id, definition.value(role.id));
        if (color.isValid()) {
            colors.*role.member = color;
        }
    };

    for (const UiColorRole &role : uiColorRoles()) {
        if (role.fallback == nullptr) {
            applyColor(role);
        }
    }
    for (const UiColorRole &role : uiColorRoles()) {
        if (role.fallback != nullptr) {
            colors.*role.member = colors.*role.fallback;
            applyColor(role);
        }
    }

    resolvedColors.insert(theme, colors);

    return colors;
}

QFont uiTextFont() {
    QFont font = uiSecondaryTextFont();
    if (font.pointSizeF() > 0) {
        font.setPointSizeF(font.pointSizeF() + kTextPointSizeIncrease);
    } else {
        font.setPixelSize(font.pixelSize() + kTextPixelSizeIncrease);
    }

    return font;
}

QFont uiSecondaryTextFont() {
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont);
}

int uiTextPixelSize(const QFont &font) {
    return std::max(1, QFontInfo(font).pixelSize());
}

QMargins uiTextInsets(const QFont &font) {
    const QFontMetrics metrics(font);
    const int offset = qRound((metrics.ascent() - metrics.descent() - metrics.capHeight()) / 2.0);

    return QMargins(0, std::max(0, -2 * offset), 0, std::max(0, 2 * offset));
}

QColor uiSelectionHoverColor(const QColor &color) {
    return color.lighter(kToolbarHoverLightness);
}

void applyUiToolbarMetrics(QToolBar &toolbar) {
    const int iconExtent = uiTextPixelSize(toolbar.font());
    const bool mainToolbar = toolbar.objectName() == QStringLiteral("MainToolBar");
    const bool settingsToolbar = toolbar.objectName() == QStringLiteral("ToolSettingsBar");
    const bool horizontal = toolbar.orientation() == Qt::Horizontal;

    toolbar.ensurePolished();
    toolbar.setIconSize(QSize(iconExtent, iconExtent));
    toolbar.setContentsMargins(0, 0, 0, 0);
    if (QLayout *layout = toolbar.layout()) {
        layout->setContentsMargins(kUiHighlightMargin, kUiHighlightMargin,
            kUiHighlightMargin + (mainToolbar && !horizontal ? kUiControlBorderWidth : 0),
            kUiHighlightMargin + ((mainToolbar && horizontal) || settingsToolbar ? kUiControlBorderWidth : 0));
    }
}

QIcon uiAddIcon() {
    return QIcon(new ChromeIconEngine(std::nullopt, ChromeAction::Add));
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
        "QPushButton:focus, QToolButton:focus { border-color: palette(text); }"
        "QPushButton:hover, QToolButton:hover { border-color: transparent; }")
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
    return themeFromSettingsValue(theme);
}

UiTheme themeFromSettingsValue(const QString &value) {
    const QString id = value.trimmed().toLower();
    const auto &themes = uiThemes();
    const auto found = std::find_if(themes.cbegin(), themes.cend(), [&](const auto &theme) {
        return theme.id == id;
    });

    return found != themes.cend() ? found->id : kDefaultUiTheme;
}

UiTheme loadUiTheme() {
    QSettings settings;
    QString theme = settings.value(QStringLiteral("ui/theme"), kDefaultUiTheme).toString();
    if (settings.value(QStringLiteral("ui/themeVersion"), 0).toInt() < kThemeSettingsVersion) {
        if (theme.compare(QStringLiteral("dark"), Qt::CaseInsensitive) == 0
            || theme.compare(QStringLiteral("light"), Qt::CaseInsensitive) == 0) {
            theme = QStringLiteral("mahogany-") + theme.toLower();
        }
        settings.setValue(QStringLiteral("ui/theme"), themeFromSettingsValue(theme));
        settings.setValue(QStringLiteral("ui/themeVersion"), kThemeSettingsVersion);
    }

    return themeFromSettingsValue(theme);
}

void saveUiTheme(UiTheme theme) {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/theme"), themeSettingsValue(theme));
    settings.setValue(QStringLiteral("ui/themeVersion"), kThemeSettingsVersion);
}

QColor defaultCanvasColor(UiTheme theme) {
    return uiColors(theme).canvas;
}

CanvasColorSettings loadCanvasColorSettings() {
    QSettings settings;
    CanvasColorSettings result;
    const QColor legacy(settings.value(QStringLiteral("ui/canvasColor")).toString());
    for (const UiThemeDefinition &theme : uiThemes()) {
        CanvasColorPreference preference;
        const QString prefix = QStringLiteral("ui/canvas/themes/%1/").arg(theme.id);
        QString mode = settings.value(prefix + QStringLiteral("mode")).toString();
        QColor custom(settings.value(prefix + QStringLiteral("custom")).toString());
        if (!settings.contains(prefix + QStringLiteral("mode"))
            && (theme.id == QStringLiteral("mahogany-dark") || theme.id == QStringLiteral("mahogany-light"))) {
            const QString oldPrefix = QStringLiteral("ui/canvas/%1").arg(
                theme.id == QStringLiteral("mahogany-dark") ? QStringLiteral("dark") : QStringLiteral("light"));
            mode = settings.value(oldPrefix + QStringLiteral("Mode")).toString();
            custom = QColor(settings.value(oldPrefix + QStringLiteral("Custom")).toString());
            if (theme.id == QStringLiteral("mahogany-dark") && legacy.isValid()) {
                custom = validColor(custom, legacy);
                if (mode.isEmpty()) {
                    mode = QStringLiteral("custom");
                }
            }
        }
        preference.mode = mode == QStringLiteral("custom") ? CanvasColorMode::Custom : CanvasColorMode::ThemeDefault;
        preference.custom = validColor(custom, defaultCanvasColor(theme.id));
        result.insert(theme.id, preference);
    }

    return result;
}

void saveCanvasColorSettings(const CanvasColorSettings &settings) {
    QSettings stored;
    for (auto theme = settings.cbegin(); theme != settings.cend(); ++theme) {
        const QString prefix = QStringLiteral("ui/canvas/themes/%1/").arg(theme.key());
        stored.setValue(prefix + QStringLiteral("mode"), theme->mode == CanvasColorMode::Custom
            ? QStringLiteral("custom") : QStringLiteral("default"));
        stored.setValue(prefix + QStringLiteral("custom"),
                        validColor(theme->custom, defaultCanvasColor(theme.key())).name(QColor::HexRgb));
    }
    stored.remove(QStringLiteral("ui/canvasColor"));
    stored.remove(QStringLiteral("ui/canvas/darkMode"));
    stored.remove(QStringLiteral("ui/canvas/lightMode"));
    stored.remove(QStringLiteral("ui/canvas/darkCustom"));
    stored.remove(QStringLiteral("ui/canvas/lightCustom"));
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
    result.penColorAwareSnapping = settings.value(QStringLiteral("ui/behavior/penColorAwareSnapping"), result.penColorAwareSnapping).toBool();
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
    qsettings.setValue(QStringLiteral("ui/behavior/penColorAwareSnapping"), settings.penColorAwareSnapping);
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
        QStringLiteral("ui/previews/bufferCustom"), defaultCanvasColor(kDefaultUiTheme).name()).toString());
    result.layers.custom = QColor(settings.value(
        QStringLiteral("ui/previews/layersCustom"), defaultCanvasColor(kDefaultUiTheme).name()).toString());
    result.buffer.custom = validColor(result.buffer.custom, defaultCanvasColor(kDefaultUiTheme));
    result.layers.custom = validColor(result.layers.custom, defaultCanvasColor(kDefaultUiTheme));

    return result;
}

void savePreviewBackgroundSettings(const PreviewBackgroundSettings &settings) {
    QSettings qsettings;
    qsettings.setValue(QStringLiteral("ui/previews/bufferMode"),
                       previewBackgroundModeValue(settings.buffer.mode));
    qsettings.setValue(QStringLiteral("ui/previews/layersMode"),
                       previewBackgroundModeValue(settings.layers.mode));
    qsettings.setValue(QStringLiteral("ui/previews/bufferCustom"),
                       validColor(settings.buffer.custom, defaultCanvasColor(kDefaultUiTheme)).name(QColor::HexRgb));
    qsettings.setValue(QStringLiteral("ui/previews/layersCustom"),
                       validColor(settings.layers.custom, defaultCanvasColor(kDefaultUiTheme)).name(QColor::HexRgb));
}

QColor defaultPreviewBackgroundColor(UiTheme theme) {
    return uiColors(theme).preview;
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
    const QColor alternate = uiColors(theme).checkerboard;
    QImage tile(kCheckerTileExtent, kCheckerTileExtent, QImage::Format_RGB32);
    tile.fill(base);
    QPainter painter(&tile);
    painter.fillRect(0, 0, kCheckerExtent, kCheckerExtent, alternate);
    painter.fillRect(kCheckerExtent, kCheckerExtent, kCheckerExtent, kCheckerExtent, alternate);

    return QBrush(QPixmap::fromImage(tile));
}

QColor canvasColorForTheme(UiTheme theme, const CanvasColorSettings &settings) {
    const CanvasColorPreference preference = settings.value(theme);

    return preference.mode == CanvasColorMode::Custom && preference.custom.isValid()
        ? preference.custom : defaultCanvasColor(theme);
}

bool isDarkTheme(UiTheme theme) {
    const auto &themes = uiThemes();
    const auto found = std::find_if(themes.cbegin(), themes.cend(), [&](const auto &definition) {
        return definition.id == theme;
    });

    return found == themes.cend() || found->dark;
}

QColor iconColorForTheme(UiTheme theme) {
    return uiColors(theme).icon;
}

QPalette paletteForTheme(UiTheme theme) {
    const UiColors colors = uiColors(theme);
    QPalette palette;
    palette.setColor(QPalette::Window, colors.main);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::Base, colors.secondary);
    palette.setColor(QPalette::AlternateBase, colors.main);
    palette.setColor(QPalette::ToolTipBase, colors.tooltipBackground);
    palette.setColor(QPalette::ToolTipText, colors.tooltipText);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::Button, colors.buttonBackground);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::Highlight, colors.selection);
    palette.setColor(QPalette::HighlightedText, colors.selectedText);
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
    app.setFont(uiTextFont());
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    app.styleHints()->setColorScheme(isDarkTheme(theme) ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif
    app.setPalette(paletteForTheme(theme));
    app.setStyleSheet(themeStyleSheet(theme));
    for (QWidget *widget : app.allWidgets()) {
        if (auto *toolbar = qobject_cast<QToolBar *>(widget)) {
            applyUiToolbarMetrics(*toolbar);
        }
    }
}

UiTheme currentUiTheme() {
    return currentTheme;
}

} // namespace gui
