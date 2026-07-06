#include "property_panel.h"

#include "editor_state.h"
#include "gui_assets.h"
#include "gui_constants.h"
#include "perf_utils.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QApplication>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>

#include <algorithm>
#include <cmath>
#include <functional>

namespace gui {
namespace {

// Spin-box configuration (previously inlined magic numbers).
constexpr double PositionSpinRange = 100000.0;   // ±range for x/y
constexpr double ScaleSkewSpinRange = 10000.0;   // ±range for scale/skew
constexpr double RotationSpinMax = 359.99999;    // keeps the box inside [0, 360)
constexpr double OpacitySpinStep = 0.05;
constexpr double FloatSpinStep = 0.1;
constexpr int FloatSpinDecimals = 5;

// Vertical label-drag speed in value units per pixel of mouse travel.
constexpr double LabelDragStepDefault = 1.0;
constexpr double LabelDragStepScale = 0.1;
constexpr double LabelDragStepSkew = 0.1;
constexpr double LabelDragStepOpacity = 0.01;

constexpr double kPi = 3.14159265358979323846;

bool isTransformProperty(const QString &property)
{
    return property == QStringLiteral("x") || property == QStringLiteral("y")
        || property == QStringLiteral("scaleX") || property == QStringLiteral("scaleY")
        || property == QStringLiteral("rotation") || property == QStringLiteral("skew");
}

// Property-label row layout.
constexpr int PropertyIconExtent = 14;
constexpr int PropertyLabelSpacing = 5;

// Grey applied to a field whose selected items hold differing values, and to
// the colour button when the selection has mixed colours.
QString mixedValueStyle()
{
    return QStringLiteral("color: #888;");
}

QString mixedColorButtonStyle()
{
    return QStringLiteral("background-color: #888;");
}

// QCheckBox whose indeterminate (PartiallyChecked) state is drawn as an
// unmistakable filled square. Some platform styles render the native
// "partial" indicator as a near-empty or check-like box on a dark UI, which
// made mixed visible/locked/mask flags read as plain on/off. Overlaying our
// own square keeps the native check and empty glyphs for the on/off states
// while guaranteeing a clear "mixed" indicator.
class FlagCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QCheckBox::paintEvent(event);
        if (checkState() != Qt::PartiallyChecked) {
            return;
        }
        QStyleOptionButton opt;
        initStyleOption(&opt);
        const QRect box = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &opt, this);
        if (box.isEmpty()) {
            return;
        }
        const int inset = std::max(2, box.width() / 4);
        const QRect square = box.adjusted(inset, inset, -inset, -inset);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 1));
        painter.setBrush(palette().color(QPalette::Highlight));
        painter.drawRect(square);
    }
};

double normalizeRotation(double value)
{
    if (!std::isfinite(value)) {
        return 0.0;
    }
    double out = std::fmod(value, 360.0);
    if (out < 0.0) {
        out += 360.0;
    }
    return out;
}

// World-space transform about a pivot (translate, scale, rotate, or shear), used to
// drive a whole selection as one unit from its bounding-box centre. Same conventions
// as the canvas group-transform math so numeric edits match the on-canvas handles.
QTransform aboutPivot(const QPointF &pivot, const QTransform &inner)
{
    QTransform out;
    out.translate(pivot.x(), pivot.y());
    out = inner * out;
    QTransform back;
    back.translate(-pivot.x(), -pivot.y());
    return back * out;
}

// Build the world affine for a single transform-field change from `from` to `to`,
// applied about `pivot`. Scale is multiplicative; rotation/skew/translation additive.
QTransform boxAffine(const QString &property, double from, double to, const QPointF &pivot)
{
    if (property == QStringLiteral("x")) {
        return QTransform::fromTranslate(to - from, 0.0);
    }
    if (property == QStringLiteral("y")) {
        return QTransform::fromTranslate(0.0, to - from);
    }
    if (property == QStringLiteral("scaleX")) {
        const double factor = std::abs(from) > 1e-9 ? to / from : 1.0;
        QTransform s;
        s.scale(factor, 1.0);
        return aboutPivot(pivot, s);
    }
    if (property == QStringLiteral("scaleY")) {
        const double factor = std::abs(from) > 1e-9 ? to / from : 1.0;
        QTransform s;
        s.scale(1.0, factor);
        return aboutPivot(pivot, s);
    }
    if (property == QStringLiteral("rotation")) {
        QTransform r;
        r.rotate(to - from);
        return aboutPivot(pivot, r);
    }
    if (property == QStringLiteral("skew")) {
        QTransform k;
        k.shear(to - from, 0.0);
        return aboutPivot(pivot, k);
    }
    return QTransform();
}

// Decompose a shape's resulting local->world matrix back into translate/rotate/shear/
// scale fields (same order as entryTransform + the canvas decomposition). `ok` is false
// when the X axis collapsed; `skew` then falls back to `fallbackSkew`.
struct AffineDecomposition {
    bool ok = false;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    double skew = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};

AffineDecomposition decomposeAffine(const QTransform &result, double fallbackSkew)
{
    const double a = result.m11();
    const double b = result.m12();
    const double c = result.m21();
    const double d = result.m22();
    const double scaleXLen = std::hypot(a, b);
    if (scaleXLen < 1e-9) {
        return {};
    }
    const double det = a * d - b * c;
    AffineDecomposition out;
    out.ok = true;
    out.x = result.dx();
    out.y = result.dy();
    out.rotation = normalizeRotation(std::atan2(b, a) * 180.0 / kPi);
    out.skew = std::abs(det) > 1e-9 ? (a * c + b * d) / det : fallbackSkew;
    out.scaleX = std::clamp(scaleXLen, -100.0, 100.0);
    out.scaleY = std::clamp(det / scaleXLen, -100.0, 100.0);
    return out;
}

// Apply a resulting local->world matrix to a shape by decomposing it back into fields.
void applyDecomposedTransform(fh6::ShapeLayer *layer, const QTransform &result)
{
    const AffineDecomposition dec = decomposeAffine(result, layer->skew);
    if (!dec.ok) {
        return;
    }
    layer->x = dec.x;
    layer->y = dec.y;
    layer->rotation = dec.rotation;
    layer->scaleX = dec.scaleX;
    layer->scaleY = dec.scaleY;
    layer->skew = dec.skew;
}

QString colorStyle(const std::array<quint8, 4> &color)
{
    return QStringLiteral("background-color: rgba(%1,%2,%3,%4);")
        .arg(color[ColorByteRed])
        .arg(color[ColorByteGreen])
        .arg(color[ColorByteBlue])
        .arg(color[ColorByteAlpha]);
}

class DraggablePropertyLabel final : public QWidget {
public:
    using BeginCallback = std::function<bool(const QPoint &)>;
    using MoveCallback = std::function<void(const QPoint &)>;
    using EndCallback = std::function<void(bool)>;

    DraggablePropertyLabel(QWidget *parent,
                           const QString &text,
                           const QString &iconName,
                           BeginCallback begin,
                           MoveCallback move,
                           EndCallback end)
        : QWidget(parent)
        , begin_(std::move(begin))
        , move_(std::move(move))
        , end_(std::move(end))
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(PropertyLabelSpacing);
        auto *icon = new QLabel(this);
        icon->setPixmap(assetIcon(iconName).pixmap(PropertyIconExtent, PropertyIconExtent));
        icon->setProperty("fh6PropertyIconName", iconName);
        layout->addWidget(icon);
        auto *label = new QLabel(text, this);
        layout->addWidget(label);
        layout->addStretch(1);
        setCursor(Qt::SizeVerCursor);
    }

    ~DraggablePropertyLabel() override
    {
        if (dragging_ && end_) {
            end_(false);
        }
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && begin_ != nullptr && begin_(event->globalPosition().toPoint())) {
            dragging_ = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_ && move_ != nullptr) {
            move_(event->globalPosition().toPoint());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            if (end_ != nullptr) {
                end_(true);
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

private:
    BeginCallback begin_;
    MoveCallback move_;
    EndCallback end_;
    bool dragging_ = false;
};

QWidget *propertyLabel(QWidget *parent,
                       const QString &text,
                       const QString &iconName,
                       std::function<bool(const QPoint &)> begin = {},
                       std::function<void(const QPoint &)> move = {},
                       std::function<void(bool)> end = {})
{
    if (begin != nullptr) {
        return new DraggablePropertyLabel(parent, text, iconName, std::move(begin), std::move(move), std::move(end));
    }
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(PropertyLabelSpacing);
    auto *icon = new QLabel(widget);
    icon->setPixmap(assetIcon(iconName).pixmap(PropertyIconExtent, PropertyIconExtent));
    icon->setProperty("fh6PropertyIconName", iconName);
    layout->addWidget(icon);
    auto *label = new QLabel(text, widget);
    layout->addWidget(label);
    layout->addStretch(1);
    return widget;
}

double opacityFromAlpha(quint8 alpha)
{
    return static_cast<double>(alpha) / 255.0;
}

template <typename T>
QVector<EntryRef> entryRefs(const QVector<T *> &items)
{
    QVector<EntryRef> entries;
    entries.reserve(items.size());
    for (T *item : items) {
        entries.push_back(EntryRef(item));
    }
    return entries;
}

quint8 alphaFromOpacity(double opacity)
{
    return static_cast<quint8>(std::clamp(static_cast<int>(std::round(opacity * 255.0)), 0, 255));
}

QVector<QWidget *> layerPropertyWidgets(
    QLineEdit *name,
    QSpinBox *shapeId,
    QDoubleSpinBox *x,
    QDoubleSpinBox *y,
    QDoubleSpinBox *scaleX,
    QDoubleSpinBox *scaleY,
    QDoubleSpinBox *rotation,
    QDoubleSpinBox *skew,
    QDoubleSpinBox *opacity,
    QCheckBox *visible,
    QCheckBox *locked,
    QCheckBox *mask,
    QPushButton *colorButton)
{
    return {name, shapeId, x, y, scaleX, scaleY, rotation, skew, opacity, visible, locked, mask, colorButton};
}

QVector<QWidget *> guidePropertyWidgets(
    QLineEdit *name,
    QDoubleSpinBox *x,
    QDoubleSpinBox *y,
    QDoubleSpinBox *scaleX,
    QDoubleSpinBox *scaleY,
    QDoubleSpinBox *rotation,
    QDoubleSpinBox *opacity,
    QCheckBox *visible,
    QCheckBox *locked)
{
    return {name, x, y, scaleX, scaleY, rotation, opacity, visible, locked};
}

} // namespace

PropertyPanel::PropertyPanel(EditorState *state, QWidget *parent)
    : QWidget(parent)
    , state_(state)
{
    auto *layout = new QFormLayout(this);
    name_ = new QLineEdit(this);
    shapeId_ = new QSpinBox(this);
    shapeId_->setRange(0, 0xffff);
    shapeId_->setDisplayIntegerBase(16);
    shapeId_->setPrefix(QStringLiteral("0x"));
    shapeId_->setKeyboardTracking(false);
    x_ = floatBox(-PositionSpinRange, PositionSpinRange);
    y_ = floatBox(-PositionSpinRange, PositionSpinRange);
    scaleX_ = floatBox(-ScaleSkewSpinRange, ScaleSkewSpinRange);
    scaleY_ = floatBox(-ScaleSkewSpinRange, ScaleSkewSpinRange);
    rotation_ = floatBox(0.0, RotationSpinMax);
    skew_ = floatBox(-ScaleSkewSpinRange, ScaleSkewSpinRange);
    visible_ = new FlagCheckBox(this);
    locked_ = new FlagCheckBox(this);
    mask_ = new FlagCheckBox(this);
    opacity_ = floatBox(0.0, 1.0);
    opacity_->setSingleStep(OpacitySpinStep);
    colorButton_ = new QPushButton(QStringLiteral("Color"), this);
    debug_ = new QLabel(this);
    debug_->setWordWrap(true);
    debug_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    widgetProperties_ = {
        {name_, QStringLiteral("name")},
        {shapeId_, QStringLiteral("shapeId")},
        {x_, QStringLiteral("x")},
        {y_, QStringLiteral("y")},
        {scaleX_, QStringLiteral("scaleX")},
        {scaleY_, QStringLiteral("scaleY")},
        {rotation_, QStringLiteral("rotation")},
        {skew_, QStringLiteral("skew")},
        {visible_, QStringLiteral("visible")},
        {locked_, QStringLiteral("locked")},
        {mask_, QStringLiteral("mask")},
        {opacity_, QStringLiteral("opacity")},
    };

    for (auto it = widgetProperties_.begin(); it != widgetProperties_.end(); ++it) {
        QWidget *widget = it.key();
        if (auto *box = qobject_cast<QDoubleSpinBox *>(widget)) {
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, box]() { applyChanged(box); });
        } else if (auto *box = qobject_cast<QSpinBox *>(widget)) {
            connect(box, &QSpinBox::valueChanged, this, [this, box]() { applyChanged(box); });
        } else if (auto *check = qobject_cast<QCheckBox *>(widget)) {
            connect(check, &QCheckBox::checkStateChanged, this, [this, check]() { applyChanged(check); });
        } else if (auto *line = qobject_cast<QLineEdit *>(widget)) {
            connect(line, &QLineEdit::editingFinished, this, [this, line]() { applyChanged(line); });
        }
    }
    connect(colorButton_, &QPushButton::clicked, this, [this]() { pickColor(); });

    const auto dragLabel = [this](const QString &text, const QString &iconName, const QString &property, QDoubleSpinBox *box) {
        return propertyLabel(this,
                             text,
                             iconName,
                             [this, property, box](const QPoint &pos) { return beginValueLabelDrag(property, box, pos); },
                             [this](const QPoint &pos) { updateValueLabelDrag(pos); },
                             [this](bool commit) { endValueLabelDrag(commit); });
    };

    layout->addRow(propertyLabel(this, QStringLiteral("Name"), QStringLiteral("PropertyName.xpm")), name_);
    layout->addRow(propertyLabel(this, QStringLiteral("Shape ID"), QStringLiteral("PropertyShapeID.xpm")), shapeId_);
    layout->addRow(dragLabel(QStringLiteral("Position X"), QStringLiteral("PropertyXY.xpm"), QStringLiteral("x"), x_), x_);
    layout->addRow(dragLabel(QStringLiteral("Position Y"), QStringLiteral("PropertyXY.xpm"), QStringLiteral("y"), y_), y_);
    layout->addRow(dragLabel(QStringLiteral("Scale X"), QStringLiteral("ToolbarScale.xpm"), QStringLiteral("scaleX"), scaleX_), scaleX_);
    layout->addRow(dragLabel(QStringLiteral("Scale Y"), QStringLiteral("ToolbarScale.xpm"), QStringLiteral("scaleY"), scaleY_), scaleY_);
    layout->addRow(dragLabel(QStringLiteral("Rotation"), QStringLiteral("ToolbarRotate.xpm"), QStringLiteral("rotation"), rotation_), rotation_);
    layout->addRow(dragLabel(QStringLiteral("Skew"), QStringLiteral("ToolbarSkew.xpm"), QStringLiteral("skew"), skew_), skew_);
    layout->addRow(dragLabel(QStringLiteral("Opacity"), QStringLiteral("PropertyVisible.xpm"), QStringLiteral("opacity"), opacity_), opacity_);
    layout->addRow(propertyLabel(this, QStringLiteral("Color"), QStringLiteral("PropertyColor.xpm")), colorButton_);
    layout->addRow(propertyLabel(this, QStringLiteral("Visible"), QStringLiteral("PropertyVisible.xpm")), visible_);
    layout->addRow(propertyLabel(this, QStringLiteral("Mask"), QStringLiteral("PropertyMask.xpm")), mask_);
    layout->addRow(propertyLabel(this, QStringLiteral("Locked"), QStringLiteral("PropertyLocked.xpm")), locked_);
    debugLabel_ = propertyLabel(this, QStringLiteral("Debug"), QStringLiteral("PropertyDebug.xpm"));
    layout->addRow(debugLabel_, debug_);
    setDebugVisible(false);
    setEnabled(false);
}

void PropertyPanel::setDebugVisible(bool visible)
{
    debugVisible_ = visible;
    if (debugLabel_ != nullptr) {
        debugLabel_->setVisible(visible);
    }
    if (debug_ != nullptr) {
        debug_->setVisible(visible);
    }
}

void PropertyPanel::setSpriteSizeFn(std::function<QSizeF(int)> fn)
{
    spriteSizeFn_ = std::move(fn);
}

void PropertyPanel::setGuideColorSampleFn(std::function<std::optional<QColor>()> fn)
{
    guideColorSampleFn_ = std::move(fn);
}

std::array<int, 3> PropertyPanel::flagCheckStates() const
{
    return {static_cast<int>(visible_->checkState()),
            static_cast<int>(locked_->checkState()),
            static_cast<int>(mask_->checkState())};
}

QDoubleSpinBox *PropertyPanel::floatBox(double low, double high)
{
    auto *box = new QDoubleSpinBox(this);
    box->setRange(low, high);
    box->setDecimals(FloatSpinDecimals);
    box->setSingleStep(FloatSpinStep);
    box->setKeyboardTracking(false);
    return box;
}

void PropertyPanel::setLayers(const QVector<fh6::ShapeLayer *> &layers)
{
    setSelection(layers, {}, {});
}

void PropertyPanel::setSelection(const QVector<fh6::ShapeLayer *> &layers,
                                 const QVector<fh6::GuideLayer *> &guides,
                                 const QVector<fh6::LayerGroup *> &groups)
{
    ScopedPerf perf("PropertyPanel::setSelection");
    loading_ = true;
    layers_ = layers;
    guides_ = guides;
    groups_ = groups;
    baselines_.clear();
    setEnabled(!layers_.isEmpty() || !guides_.isEmpty() || !groups_.isEmpty());
    if (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty()) {
        debug_->setText(QString());
        loading_ = false;
        return;
    }
    if (!guides_.isEmpty() && layers_.isEmpty() && groups_.isEmpty()) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(false);
        }
        for (QWidget *widget : guidePropertyWidgets(name_, x_, y_, scaleX_, scaleY_, rotation_, opacity_, visible_, locked_)) {
            widget->setEnabled(true);
        }
        if (guides_.size() == 1) {
            setSingleEntry(EntryRef(guides_.front()));
        } else {
            setMultipleEntries(entryRefs(guides_));
        }
        loading_ = false;
        return;
    }
    if (!guides_.isEmpty()) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(false);
        }
        debug_->setText(QStringLiteral("Mixed guide and vinyl selection"));
        loading_ = false;
        return;
    }
    if (!groups_.isEmpty()) {
        name_->setEnabled(true);
        shapeId_->setEnabled(false);
        // Transform fields drive the whole group as one unit about its bounding box.
        const bool canTransform = !layers_.isEmpty();
        x_->setEnabled(canTransform);
        y_->setEnabled(canTransform);
        scaleX_->setEnabled(canTransform);
        scaleY_->setEnabled(canTransform);
        rotation_->setEnabled(canTransform);
        skew_->setEnabled(canTransform);
        opacity_->setEnabled(true);
        colorButton_->setEnabled(true);
        const QSignalBlocker visibleBlocker(visible_);
        const QSignalBlocker lockedBlocker(locked_);
        const QSignalBlocker maskBlocker(mask_);
        const QSignalBlocker nameBlocker(name_);
        const QSignalBlocker opacityBlocker(opacity_);
        visible_->setEnabled(true);
        locked_->setEnabled(true);
        mask_->setEnabled(true);
        // Build an id -> layer map once so the tri-state checks below are O(leaves) instead of
        // O(leaves * layers) per group, which matters when selecting large groups.
        QHash<QString, const fh6::ShapeLayer *> layerById;
        if (const fh6::Project *project = state_->project()) {
            layerById.reserve(project->layers.size());
            for (const fh6::ShapeLayer &layer : project->layers) {
                layerById.insert(layer.id, &layer);
            }
        }
        // A group reflects its descendants as a tri-state: all leaves on -> Checked,
        // all off -> Unchecked, mixed -> PartiallyChecked (square). The square then
        // surfaces mixed flags within a single group, not just across several groups.
        auto leafTriState = [this, &layerById](const fh6::LayerGroup &group, auto layerPred) {
            const QVector<QString> ids = state_->leafLayerIdsForEntry(group.id);
            if (ids.isEmpty()) {
                return Qt::Unchecked;
            }
            bool anyTrue = false;
            bool anyFalse = false;
            for (const QString &id : ids) {
                const auto it = layerById.constFind(id);
                if (it == layerById.constEnd()) {
                    continue;
                }
                if (layerPred(*it.value())) {
                    anyTrue = true;
                } else {
                    anyFalse = true;
                }
            }
            if (anyTrue && anyFalse) {
                return Qt::PartiallyChecked;
            }
            return anyTrue ? Qt::Checked : Qt::Unchecked;
        };
        auto setGroupCheck = [this](QCheckBox *box, auto getter) {
            Qt::CheckState combined = getter(*groups_.front());
            for (fh6::LayerGroup *group : groups_) {
                if (getter(*group) != combined) {
                    combined = Qt::PartiallyChecked;
                    break;
                }
            }
            box->setTristate(combined == Qt::PartiallyChecked);
            box->setCheckState(combined);
        };
        setGroupCheck(visible_, [&](const fh6::LayerGroup &group) {
            return leafTriState(group, [](const fh6::ShapeLayer &layer) { return layer.visible; });
        });
        setGroupCheck(mask_, [&](const fh6::LayerGroup &group) {
            return leafTriState(group, [](const fh6::ShapeLayer &layer) { return layer.mask; });
        });
        setGroupCheck(locked_, [&](const fh6::LayerGroup &group) {
            return leafTriState(group, [this](const fh6::ShapeLayer &layer) { return state_->isLayerLocked(layer.id); });
        });
        const QString firstName = groups_.front()->name;
        const bool sameName = std::all_of(groups_.begin(), groups_.end(), [&](const fh6::LayerGroup *group) {
            return group->name == firstName;
        });
        name_->setText(sameName ? firstName : QString());
        name_->setStyleSheet(sameName ? QString() : mixedValueStyle());

        const QVector<std::array<quint8, 4>> colors = selectionColors();
        if (!colors.isEmpty()) {
            double sum = 0.0;
            quint8 minAlpha = colors.front()[3];
            quint8 maxAlpha = minAlpha;
            for (const auto &color : colors) {
                sum += opacityFromAlpha(color[ColorByteAlpha]);
                minAlpha = std::min(minAlpha, color[ColorByteAlpha]);
                maxAlpha = std::max(maxAlpha, color[ColorByteAlpha]);
            }
            opacity_->setValue(sum / colors.size());
            opacity_->setStyleSheet(minAlpha == maxAlpha ? QString() : mixedValueStyle());
        }
        updateColorButton();
        if (canTransform) {
            setBoxProxyFields();
        }
        debug_->setText(QStringLiteral("%1 group%2 selected").arg(groups_.size()).arg(groups_.size() == 1 ? QString() : QStringLiteral("s")));
    } else if (layers_.size() == 1) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(true);
        }
        setSingleEntry(EntryRef(layers_.front()));
    } else {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(true);
        }
        setMultipleEntries(entryRefs(layers_));
        // Multiple loose shapes transform as one unit about their shared bounding box,
        // so the position/scale/rotation/skew fields show a neutral box proxy instead
        // of per-shape averages.
        setBoxProxyFields();
    }
    loading_ = false;
}

void PropertyPanel::setSingleEntry(const EntryRef &entry)
{
    const QSignalBlocker b1(name_);
    const QSignalBlocker b2(shapeId_);
    const QSignalBlocker b3(x_);
    const QSignalBlocker b4(y_);
    const QSignalBlocker b5(scaleX_);
    const QSignalBlocker b6(scaleY_);
    const QSignalBlocker b7(rotation_);
    const QSignalBlocker b8(skew_);
    const QSignalBlocker b9(visible_);
    const QSignalBlocker b10(locked_);
    const QSignalBlocker b11(mask_);
    const QSignalBlocker b12(opacity_);

    name_->setText(entry.name());
    x_->setValue(entry.x());
    y_->setValue(entry.y());
    scaleX_->setValue(entry.scaleX());
    scaleY_->setValue(entry.scaleY());
    rotation_->setValue(entry.rotation());
    opacity_->setValue(entry.opacity());
    visible_->setTristate(false);
    locked_->setTristate(false);
    mask_->setTristate(false);
    visible_->setChecked(entry.visible());
    clearMixedStyles();
    if (const fh6::ShapeLayer *layer = entry.layer()) {
        shapeId_->setValue(layer->shapeId);
        skew_->setValue(layer->skew);
        locked_->setChecked(state_->isLayerLocked(layer->id));
        mask_->setChecked(layer->mask);
        updateColorButton();
        debug_->setText(QStringLiteral("source_shape: %1\nabs_offset: %2\nmarker: %3\nflags: %4")
                            .arg(layer->sourceShape)
                            .arg(layer->absOffset)
                            .arg(QString::fromLatin1(layer->marker.toHex()))
                            .arg(layer->flags));
    } else {
        const fh6::GuideLayer *guide = entry.guide();
        locked_->setChecked(guide->locked);
        mask_->setChecked(false);
        colorButton_->setText(QStringLiteral("N/A"));
        colorButton_->setStyleSheet(QString());
        debug_->setText(QStringLiteral("%1 x %2\nformat: %3")
                            .arg(guide->width)
                            .arg(guide->height)
                            .arg(guide->imageFormat));
    }
}

void PropertyPanel::setMultipleEntries(const QVector<EntryRef> &entries)
{
    const QSignalBlocker b1(name_);
    const QSignalBlocker b2(shapeId_);
    const QSignalBlocker b3(x_);
    const QSignalBlocker b4(y_);
    const QSignalBlocker b5(scaleX_);
    const QSignalBlocker b6(scaleY_);
    const QSignalBlocker b7(rotation_);
    const QSignalBlocker b8(skew_);
    const QSignalBlocker b9(visible_);
    const QSignalBlocker b10(locked_);
    const QSignalBlocker b11(mask_);
    const QSignalBlocker b12(opacity_);

    auto setDouble = [this, &entries](QDoubleSpinBox *box, auto getter) {
        double sum = 0.0;
        double minValue = getter(entries.front());
        double maxValue = minValue;
        for (const EntryRef &entry : entries) {
            const double value = getter(entry);
            sum += value;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
        const double average = sum / entries.size();
        baselines_.insert(box, average);
        box->setValue(average);
        box->setStyleSheet(minValue == maxValue ? QString() : mixedValueStyle());
    };
    auto setCheck = [&entries](QCheckBox *box, auto getter) {
        bool first = getter(entries.front());
        bool mixed = false;
        for (const EntryRef &entry : entries) {
            if (getter(entry) != first) {
                mixed = true;
                break;
            }
        }
        box->setTristate(mixed);
        box->setCheckState(mixed ? Qt::PartiallyChecked : (first ? Qt::Checked : Qt::Unchecked));
    };

    setDouble(x_, [](const EntryRef &entry) { return entry.x(); });
    setDouble(y_, [](const EntryRef &entry) { return entry.y(); });
    setDouble(scaleX_, [](const EntryRef &entry) { return entry.scaleX(); });
    setDouble(scaleY_, [](const EntryRef &entry) { return entry.scaleY(); });
    setDouble(rotation_, [](const EntryRef &entry) { return entry.rotation(); });
    const bool shapeEntries = entries.front().isLayer();
    if (shapeEntries) {
        setDouble(skew_, [](const EntryRef &entry) { return entry.skew(); });
    }
    setDouble(opacity_, [](const EntryRef &entry) { return entry.opacity(); });
    setCheck(visible_, [](const EntryRef &entry) { return entry.visible(); });
    setCheck(locked_, [this](const EntryRef &entry) {
        return entry.isLayer() ? state_->isLayerLocked(entry.id()) : entry.locked();
    });

    if (shapeEntries) {
        setCheck(mask_, [](const EntryRef &entry) { return entry.layer()->mask; });

        const QString firstName = entries.front().name();
        const bool sameName = std::all_of(entries.begin(), entries.end(), [&](const EntryRef &entry) {
            return entry.name() == firstName;
        });
        name_->setText(sameName ? firstName : QString());
        name_->setStyleSheet(sameName ? QString() : mixedValueStyle());

        const quint16 firstShapeId = entries.front().layer()->shapeId;
        const bool sameShapeId = std::all_of(entries.begin(), entries.end(), [&](const EntryRef &entry) {
            return entry.layer()->shapeId == firstShapeId;
        });
        shapeId_->setValue(firstShapeId);
        shapeId_->setStyleSheet(sameShapeId ? QString() : mixedValueStyle());

        updateColorButton();
        debug_->setText(QStringLiteral("%1 layers selected").arg(entries.size()));
    } else {
        // Multi-guide selections keep their historical quirks: the name reads as
        // mixed even when every guide shares one, and there is no mask/colour.
        mask_->setTristate(false);
        mask_->setChecked(false);
        name_->setText(QString());
        name_->setStyleSheet(mixedValueStyle());
        colorButton_->setText(QStringLiteral("N/A"));
        colorButton_->setStyleSheet(QString());
        debug_->setText(QStringLiteral("%1 guide layers selected").arg(entries.size()));
    }
}

bool PropertyPanel::isBoxSelection() const
{
    return guides_.isEmpty() && !layers_.isEmpty() && (!groups_.isEmpty() || layers_.size() > 1);
}

QPointF PropertyPanel::selectionBoxCenter() const
{
    BoundsAccumulator acc;
    for (const fh6::ShapeLayer *layer : layers_) {
        const QSizeF size = spriteSizeFn_ ? spriteSizeFn_(layer->shapeId) : QSizeF(0.0, 0.0);
        acc.add(entryTransform(*layer), entryLocalRect(size));
    }
    return acc.hasBounds() ? acc.bounds().center() : QPointF(0.0, 0.0);
}

void PropertyPanel::setBoxProxyFields()
{
    const QPointF center = selectionBoxCenter();
    const QSignalBlocker bx(x_);
    const QSignalBlocker by(y_);
    const QSignalBlocker bsx(scaleX_);
    const QSignalBlocker bsy(scaleY_);
    const QSignalBlocker br(rotation_);
    const QSignalBlocker bk(skew_);
    // Neutral proxy: position tracks the box centre; scale is 1x, rotation/skew 0.
    // Each edit is applied about the box and the fields reset to this baseline, so a
    // value is always "transform the selection by" rather than a per-shape absolute.
    const auto setProxy = [this](QDoubleSpinBox *box, double value) {
        box->setValue(value);
        box->setStyleSheet(QString());
        baselines_.insert(box, value);
    };
    setProxy(x_, center.x());
    setProxy(y_, center.y());
    setProxy(scaleX_, 1.0);
    setProxy(scaleY_, 1.0);
    setProxy(rotation_, 0.0);
    setProxy(skew_, 0.0);
}

void PropertyPanel::applyBoxTransform(const QString &property, double fromValue, double toValue)
{
    const QTransform transform = boxAffine(property, fromValue, toValue, selectionBoxCenter());
    if (transform.isIdentity()) {
        return;
    }
    for (fh6::ShapeLayer *layer : layers_) {
        applyDecomposedTransform(layer, entryTransform(*layer) * transform);
    }
}

void PropertyPanel::clearMixedStyles()
{
    name_->setStyleSheet(QString());
    shapeId_->setStyleSheet(QString());
    for (QDoubleSpinBox *box : {x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_}) {
        box->setStyleSheet(QString());
    }
}

void PropertyPanel::applyChanged(QWidget *sender)
{
    if (loading_ || (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty())) {
        return;
    }
    const QString property = widgetProperties_.value(sender);
    if (property.isEmpty()) {
        return;
    }
    if (!guides_.isEmpty() && (sender != locked_)) {
        for (const fh6::GuideLayer *guide : guides_) {
            if (guide->locked) {
                setSelection(layers_, guides_, groups_);
                return;
            }
        }
    }
    if (groups_.isEmpty() && guides_.isEmpty() && sender != locked_) {
        const QSet<QString> lockedIds = state_->lockedLayerIds();
        for (const fh6::ShapeLayer *layer : layers_) {
            if (lockedIds.contains(layer->id)) {
                setSelection(layers_, guides_, groups_);
                return;
            }
        }
    }
    state_->beginProjectEdit();
    // The branches below write through the cached layers_/guides_/groups_ raw
    // pointers. beginProjectEdit() snapshots the project via copy-on-write, so
    // detach first and re-resolve those pointers; otherwise the writes mutate
    // the shared undo "before" snapshot and the edit cannot be undone.
    detachSelectionForEdit();
    if (!guides_.isEmpty() && layers_.isEmpty() && groups_.isEmpty()) {
        const QSet<QString> selectedGuideIds = state_->selectedGuideLayerIds();
        if (fh6::Project *project = state_->project()) {
            project->guideLayers.data();
            guides_.clear();
            for (fh6::GuideLayer &guide : project->guideLayers) {
                if (selectedGuideIds.contains(guide.id)) {
                    guides_.push_back(&guide);
                }
            }
        }
        if (sender == locked_ && locked_->checkState() != Qt::PartiallyChecked) {
            const bool value = locked_->checkState() == Qt::Checked;
            for (fh6::GuideLayer *guide : guides_) {
                guide->locked = value;
            }
        } else if (sender == visible_ && visible_->checkState() != Qt::PartiallyChecked) {
            const bool value = visible_->checkState() == Qt::Checked;
            for (fh6::GuideLayer *guide : guides_) {
                guide->visible = value;
            }
        } else if (guides_.size() == 1) {
            fh6::GuideLayer *guide = guides_.front();
            guide->name = name_->text();
            guide->x = x_->value();
            guide->y = y_->value();
            guide->scaleX = scaleX_->value();
            guide->scaleY = scaleY_->value();
            guide->rotation = normalizeRotation(rotation_->value());
            guide->opacity = opacity_->value();
        } else if (auto *box = qobject_cast<QDoubleSpinBox *>(sender)) {
            const double old = baselines_.value(box, box->value());
            const double delta = box->value() - old;
            baselines_.insert(box, box->value());
            for (fh6::GuideLayer *guide : guides_) {
                if (property == QStringLiteral("x")) {
                    guide->x += delta;
                } else if (property == QStringLiteral("y")) {
                    guide->y += delta;
                } else if (property == QStringLiteral("scaleX")) {
                    guide->scaleX += delta;
                } else if (property == QStringLiteral("scaleY")) {
                    guide->scaleY += delta;
                } else if (property == QStringLiteral("rotation")) {
                    guide->rotation = normalizeRotation(guide->rotation + delta);
                } else if (property == QStringLiteral("opacity")) {
                    guide->opacity = std::clamp(guide->opacity + delta, 0.0, 1.0);
                }
            }
        } else if (auto *line = qobject_cast<QLineEdit *>(sender)) {
            for (fh6::GuideLayer *guide : guides_) {
                guide->name = line->text();
            }
        }
    } else if (!groups_.isEmpty()) {
        if (sender == name_) {
            for (fh6::LayerGroup *group : groups_) {
                group->name = name_->text();
            }
        } else if (sender == opacity_) {
            for (fh6::LayerGroup *group : groups_) {
                state_->setGroupDescendantOpacity(group->id, opacity_->value());
            }
        } else if (sender == locked_ && locked_->checkState() != Qt::PartiallyChecked) {
            const bool value = locked_->checkState() == Qt::Checked;
            for (fh6::LayerGroup *group : groups_) {
                state_->setGroupAndDescendantLocked(group->id, value);
            }
        } else if (sender == visible_ && visible_->checkState() != Qt::PartiallyChecked) {
            const bool value = visible_->checkState() == Qt::Checked;
            for (fh6::LayerGroup *group : groups_) {
                state_->setGroupDescendantVisible(group->id, value);
            }
        } else if (sender == mask_ && mask_->checkState() != Qt::PartiallyChecked) {
            const bool value = mask_->checkState() == Qt::Checked;
            for (fh6::LayerGroup *group : groups_) {
                state_->setGroupDescendantMask(group->id, value);
            }
        } else if (auto *box = qobject_cast<QDoubleSpinBox *>(sender); box != nullptr && isTransformProperty(property)) {
            applyBoxTransform(property, baselines_.value(box, box->value()), box->value());
        }
    } else if (sender == locked_ && locked_->checkState() != Qt::PartiallyChecked) {
        const bool value = locked_->checkState() == Qt::Checked;
        for (fh6::ShapeLayer *layer : layers_) {
            state_->setLayerLockScope(layer->id, value);
        }
    } else if (layers_.size() == 1) {
        applySingle();
    } else {
        applyMulti(sender, property);
    }
    state_->commitProjectEdit();
    if (property == QStringLiteral("visible") || property == QStringLiteral("mask") || property == QStringLiteral("shapeId")
        || (property == QStringLiteral("opacity") && guides_.isEmpty())) {
        state_->noteProjectGeometryChanged(true);
    } else if (property == QStringLiteral("x") || property == QStringLiteral("y") || property == QStringLiteral("scaleX")
               || property == QStringLiteral("scaleY") || property == QStringLiteral("rotation") || property == QStringLiteral("skew")
               || property == QStringLiteral("opacity")) {
        state_->noteProjectGeometryChanged(false);
    } else {
        state_->noteProjectStructureChanged();
    }
    setSelection(layers_, guides_, groups_);
}

bool PropertyPanel::beginValueLabelDrag(const QString &property, QDoubleSpinBox *box, const QPoint &globalPos)
{
    if (loading_ || valueLabelDragging_ || box == nullptr || !box->isEnabled()
        || (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty())) {
        return false;
    }
    // Groups expose opacity plus the box-frame transform fields; nothing else.
    if (!groups_.isEmpty() && property != QStringLiteral("opacity") && !isTransformProperty(property)) {
        return false;
    }
    if (!guides_.isEmpty() && (property == QStringLiteral("skew"))) {
        return false;
    }
    if (!guides_.isEmpty()) {
        for (const fh6::GuideLayer *guide : guides_) {
            if (guide->locked) {
                return false;
            }
        }
    }
    if (groups_.isEmpty() && guides_.isEmpty()) {
        const QSet<QString> lockedIds = state_->lockedLayerIds();
        for (const fh6::ShapeLayer *layer : layers_) {
            if (lockedIds.contains(layer->id)) {
                return false;
            }
        }
    }

    valueLabelDragging_ = true;
    valueLabelProperty_ = property;
    valueLabelBox_ = box;
    valueLabelStartGlobal_ = globalPos;
    valueLabelBoxStartValue_ = box->value();
    valueLabelLayerIds_ = state_->selectedLayerIds();
    valueLabelGuideIds_ = state_->selectedGuideLayerIds();
    valueLabelGroupIds_.clear();
    for (const fh6::LayerGroup *group : groups_) {
        if (group != nullptr) {
            valueLabelGroupIds_.push_back(group->id);
        }
    }
    valueLabelLayerStartValues_.clear();
    valueLabelGuideStartValues_.clear();
    valueLabelGroupStartValues_.clear();

    state_->beginProjectEdit();
    if (fh6::Project *project = state_->project()) {
        project->layers.data();
        project->guideLayers.data();
        project->groups.data();
        layers_.clear();
        guides_.clear();
        groups_.clear();
        for (fh6::ShapeLayer &layer : project->layers) {
            if (valueLabelLayerIds_.contains(layer.id)) {
                layers_.push_back(&layer);
            }
        }
        for (fh6::GuideLayer &guide : project->guideLayers) {
            if (valueLabelGuideIds_.contains(guide.id)) {
                guides_.push_back(&guide);
            }
        }
        for (const QString &groupId : valueLabelGroupIds_) {
            for (fh6::LayerGroup &group : project->groups) {
                if (group.id == groupId) {
                    groups_.push_back(&group);
                    break;
                }
            }
        }
    }

    const auto layerValue = [&](const fh6::ShapeLayer &layer) {
        if (property == QStringLiteral("x")) return layer.x;
        if (property == QStringLiteral("y")) return layer.y;
        if (property == QStringLiteral("scaleX")) return layer.scaleX;
        if (property == QStringLiteral("scaleY")) return layer.scaleY;
        if (property == QStringLiteral("rotation")) return layer.rotation;
        if (property == QStringLiteral("skew")) return layer.skew;
        return opacityFromAlpha(layer.color[ColorByteAlpha]);
    };
    const auto guideValue = [&](const fh6::GuideLayer &guide) {
        if (property == QStringLiteral("x")) return guide.x;
        if (property == QStringLiteral("y")) return guide.y;
        if (property == QStringLiteral("scaleX")) return guide.scaleX;
        if (property == QStringLiteral("scaleY")) return guide.scaleY;
        if (property == QStringLiteral("rotation")) return guide.rotation;
        return guide.opacity;
    };
    for (const fh6::ShapeLayer *layer : layers_) {
        valueLabelLayerStartValues_.insert(layer->id, layerValue(*layer));
    }
    for (const fh6::GuideLayer *guide : guides_) {
        valueLabelGuideStartValues_.insert(guide->id, guideValue(*guide));
    }
    for (const QString &groupId : valueLabelGroupIds_) {
        valueLabelGroupStartValues_.insert(groupId, box->value());
    }

    // Box-frame drag: capture the pivot and each shape's start matrix so the whole
    // selection transforms as a unit from a fixed reference, exactly like the canvas
    // group handles. box->value() (captured above) is the neutral proxy baseline.
    valueLabelBoxDrag_ = isBoxSelection() && isTransformProperty(property);
    valueLabelLayerStartTransforms_.clear();
    if (valueLabelBoxDrag_) {
        valueLabelBoxCenter_ = selectionBoxCenter();
        for (const fh6::ShapeLayer *layer : layers_) {
            valueLabelLayerStartTransforms_.insert(layer->id, entryTransform(*layer));
        }
    }

    QPixmap pixmap(assetPath(QStringLiteral("ToolScaleY.xpm")));
    if (!pixmap.isNull()) {
        pixmap = pixmap.scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QApplication::setOverrideCursor(QCursor(pixmap, pixmap.width() / 2, pixmap.height() / 2));
    } else {
        QApplication::setOverrideCursor(QCursor(Qt::SizeVerCursor));
    }
    return true;
}

void PropertyPanel::updateValueLabelDrag(const QPoint &globalPos)
{
    if (!valueLabelDragging_ || valueLabelBox_ == nullptr) {
        return;
    }

    const auto step = [this]() {
        if (valueLabelProperty_ == QStringLiteral("scaleX") || valueLabelProperty_ == QStringLiteral("scaleY")) {
            return LabelDragStepScale;
        }
        if (valueLabelProperty_ == QStringLiteral("skew")) {
            return LabelDragStepSkew;
        }
        if (valueLabelProperty_ == QStringLiteral("opacity")) {
            return LabelDragStepOpacity;
        }
        return LabelDragStepDefault;
    };
    const double delta = static_cast<double>(valueLabelStartGlobal_.y() - globalPos.y()) * step();
    const auto clampBox = [this](double value) {
        return std::clamp(value, valueLabelBox_->minimum(), valueLabelBox_->maximum());
    };
    const auto adjusted = [&](double start) {
        double value = start + delta;
        if (valueLabelProperty_ == QStringLiteral("rotation")) {
            return normalizeRotation(value);
        }
        return clampBox(value);
    };

    // Box-frame drag: transform every shape about the captured pivot from its start
    // matrix, so a group / multi-selection moves as a unit instead of each shape in place.
    if (valueLabelBoxDrag_) {
        const double proxy = adjusted(valueLabelBoxStartValue_);
        const QTransform transform = boxAffine(valueLabelProperty_, valueLabelBoxStartValue_, proxy, valueLabelBoxCenter_);
        for (fh6::ShapeLayer *layer : layers_) {
            const QTransform start = valueLabelLayerStartTransforms_.value(layer->id, entryTransform(*layer));
            applyDecomposedTransform(layer, start * transform);
        }
        {
            const QSignalBlocker blocker(valueLabelBox_);
            valueLabelBox_->setValue(proxy);
        }
        state_->noteCanvasRepaint();
        if (globalPos.x() != valueLabelStartGlobal_.x()) {
            QCursor::setPos(valueLabelStartGlobal_.x(), globalPos.y());
        }
        return;
    }

    for (fh6::ShapeLayer *layer : layers_) {
        const double value = adjusted(valueLabelLayerStartValues_.value(layer->id, 0.0));
        if (valueLabelProperty_ == QStringLiteral("x")) {
            layer->x = value;
        } else if (valueLabelProperty_ == QStringLiteral("y")) {
            layer->y = value;
        } else if (valueLabelProperty_ == QStringLiteral("scaleX")) {
            layer->scaleX = value;
        } else if (valueLabelProperty_ == QStringLiteral("scaleY")) {
            layer->scaleY = value;
        } else if (valueLabelProperty_ == QStringLiteral("rotation")) {
            layer->rotation = value;
        } else if (valueLabelProperty_ == QStringLiteral("skew")) {
            layer->skew = value;
        } else if (valueLabelProperty_ == QStringLiteral("opacity")) {
            layer->color[ColorByteAlpha] = alphaFromOpacity(value);
        }
    }
    for (fh6::GuideLayer *guide : guides_) {
        const double value = adjusted(valueLabelGuideStartValues_.value(guide->id, 0.0));
        if (valueLabelProperty_ == QStringLiteral("x")) {
            guide->x = value;
        } else if (valueLabelProperty_ == QStringLiteral("y")) {
            guide->y = value;
        } else if (valueLabelProperty_ == QStringLiteral("scaleX")) {
            guide->scaleX = value;
        } else if (valueLabelProperty_ == QStringLiteral("scaleY")) {
            guide->scaleY = value;
        } else if (valueLabelProperty_ == QStringLiteral("rotation")) {
            guide->rotation = value;
        } else if (valueLabelProperty_ == QStringLiteral("opacity")) {
            guide->opacity = value;
        }
    }
    for (fh6::LayerGroup *group : groups_) {
        const double value = adjusted(valueLabelGroupStartValues_.value(group->id, valueLabelBox_->value()));
        state_->setGroupDescendantOpacity(group->id, value);
    }

    {
        const QSignalBlocker blocker(valueLabelBox_);
        valueLabelBox_->setValue(adjusted(valueLabelBoxStartValue_));
    }
    if (valueLabelProperty_ == QStringLiteral("opacity")) {
        updateColorButton();
    }
    state_->noteCanvasRepaint();

    if (globalPos.x() != valueLabelStartGlobal_.x()) {
        QCursor::setPos(valueLabelStartGlobal_.x(), globalPos.y());
    }
}

void PropertyPanel::endValueLabelDrag(bool commit)
{
    if (!valueLabelDragging_) {
        return;
    }
    QApplication::restoreOverrideCursor();
    const QString property = valueLabelProperty_;
    valueLabelDragging_ = false;
    valueLabelProperty_.clear();
    valueLabelBox_ = nullptr;
    valueLabelBoxStartValue_ = 0.0;
    valueLabelBoxDrag_ = false;
    valueLabelLayerStartValues_.clear();
    valueLabelGuideStartValues_.clear();
    valueLabelGroupStartValues_.clear();
    valueLabelLayerStartTransforms_.clear();

    if (commit) {
        state_->commitProjectEdit();
        const bool refreshPreviews = property == QStringLiteral("opacity") && guides_.isEmpty();
        state_->noteProjectGeometryChanged(refreshPreviews);
    } else {
        state_->cancelProjectEdit();
    }

    QVector<fh6::ShapeLayer *> layers;
    QVector<fh6::GuideLayer *> guides;
    QVector<fh6::LayerGroup *> groups;
    if (fh6::Project *project = state_->project()) {
        for (fh6::ShapeLayer &layer : project->layers) {
            if (valueLabelLayerIds_.contains(layer.id)) {
                layers.push_back(&layer);
            }
        }
        for (fh6::GuideLayer &guide : project->guideLayers) {
            if (valueLabelGuideIds_.contains(guide.id)) {
                guides.push_back(&guide);
            }
        }
        for (const QString &groupId : valueLabelGroupIds_) {
            for (fh6::LayerGroup &group : project->groups) {
                if (group.id == groupId) {
                    groups.push_back(&group);
                    break;
                }
            }
        }
    }
    setSelection(layers, guides, groups);
    valueLabelLayerIds_.clear();
    valueLabelGuideIds_.clear();
    valueLabelGroupIds_.clear();
}

void PropertyPanel::detachSelectionForEdit()
{
    fh6::Project *project = state_->project();
    if (project == nullptr) {
        return;
    }
    // Read the selection ids before detaching invalidates the cached pointers.
    QSet<QString> layerIds;
    QSet<QString> guideIds;
    QVector<QString> groupIds;
    layerIds.reserve(layers_.size());
    guideIds.reserve(guides_.size());
    groupIds.reserve(groups_.size());
    for (const fh6::ShapeLayer *layer : layers_) {
        layerIds.insert(layer->id);
    }
    for (const fh6::GuideLayer *guide : guides_) {
        guideIds.insert(guide->id);
    }
    for (const fh6::LayerGroup *group : groups_) {
        groupIds.push_back(group->id);
    }
    // Force the copy-on-write detach so later in-place writes hit a buffer the
    // undo snapshot does not share.
    project->layers.data();
    project->guideLayers.data();
    project->groups.data();
    layers_.clear();
    guides_.clear();
    groups_.clear();
    for (fh6::ShapeLayer &layer : project->layers) {
        if (layerIds.contains(layer.id)) {
            layers_.push_back(&layer);
        }
    }
    for (fh6::GuideLayer &guide : project->guideLayers) {
        if (guideIds.contains(guide.id)) {
            guides_.push_back(&guide);
        }
    }
    for (const QString &groupId : groupIds) {
        for (fh6::LayerGroup &group : project->groups) {
            if (group.id == groupId) {
                groups_.push_back(&group);
                break;
            }
        }
    }
}

void PropertyPanel::applySingle()
{
    fh6::ShapeLayer *layer = layers_.front();
    layer->name = name_->text();
    layer->shapeId = static_cast<quint16>(shapeId_->value());
    layer->x = x_->value();
    layer->y = y_->value();
    layer->scaleX = scaleX_->value();
    layer->scaleY = scaleY_->value();
    layer->rotation = normalizeRotation(rotation_->value());
    layer->skew = skew_->value();
    layer->color[ColorByteAlpha] = alphaFromOpacity(opacity_->value());
    layer->visible = visible_->isChecked();
    layer->locked = locked_->isChecked();
    layer->mask = mask_->isChecked();
}

void PropertyPanel::applyMulti(QWidget *sender, const QString &property)
{
    if (auto *box = qobject_cast<QDoubleSpinBox *>(sender)) {
        // Multiple shapes transform as one unit about their shared bounding box.
        if (isTransformProperty(property)) {
            applyBoxTransform(property, baselines_.value(box, box->value()), box->value());
            return;
        }
        // Opacity has no spatial box frame; apply the delta to each shape.
        const double old = baselines_.value(box, box->value());
        const double delta = box->value() - old;
        baselines_.insert(box, box->value());
        for (fh6::ShapeLayer *layer : layers_) {
            if (property == QStringLiteral("opacity")) {
                layer->color[ColorByteAlpha] = alphaFromOpacity(std::clamp(opacityFromAlpha(layer->color[ColorByteAlpha]) + delta, 0.0, 1.0));
            }
        }
    } else if (auto *box = qobject_cast<QSpinBox *>(sender)) {
        for (fh6::ShapeLayer *layer : layers_) {
            layer->shapeId = static_cast<quint16>(box->value());
        }
    } else if (auto *line = qobject_cast<QLineEdit *>(sender)) {
        for (fh6::ShapeLayer *layer : layers_) {
            layer->name = line->text();
        }
    } else if (auto *check = qobject_cast<QCheckBox *>(sender)) {
        if (check->checkState() == Qt::PartiallyChecked) {
            return;
        }
        const bool value = check->checkState() == Qt::Checked;
        for (fh6::ShapeLayer *layer : layers_) {
            if (property == QStringLiteral("visible")) {
                layer->visible = value;
            } else if (property == QStringLiteral("locked")) {
                layer->locked = value;
            } else if (property == QStringLiteral("mask")) {
                layer->mask = value;
            }
        }
    }
}

QVector<std::array<quint8, 4>> PropertyPanel::selectionColors() const
{
    ScopedPerf perf("PropertyPanel::selectionColors");
    QVector<std::array<quint8, 4>> colors;
    for (const fh6::ShapeLayer *layer : layers_) {
        colors.push_back(layer->color);
    }
    if (!groups_.isEmpty()) {
        const fh6::Project *project = state_->project();
        if (project != nullptr) {
            // One id -> layer map, then O(leaves) lookups per group instead of a full layer
            // scan for every leaf.
            QHash<QString, const fh6::ShapeLayer *> layerById;
            layerById.reserve(project->layers.size());
            for (const fh6::ShapeLayer &layer : project->layers) {
                layerById.insert(layer.id, &layer);
            }
            for (const fh6::LayerGroup *group : groups_) {
                const QVector<QString> ids = state_->leafLayerIdsForEntry(group->id);
                for (const QString &id : ids) {
                    const auto it = layerById.constFind(id);
                    if (it != layerById.constEnd()) {
                        colors.push_back(it.value()->color);
                    }
                }
            }
        }
    }
    return colors;
}

std::optional<std::array<quint8, 4>> PropertyPanel::currentSelectionColor() const
{
    const QVector<std::array<quint8, 4>> colors = selectionColors();
    if (colors.isEmpty()) {
        return std::nullopt;
    }
    return colors.front();
}

void PropertyPanel::applyColorToSelection(const std::array<quint8, 4> &color)
{
    if (selectionColors().isEmpty()) {
        return;
    }
    state_->beginProjectEdit();
    detachSelectionForEdit();
    for (fh6::ShapeLayer *layer : layers_) {
        layer->color = color;
    }
    for (fh6::LayerGroup *group : groups_) {
        state_->setGroupDescendantColor(group->id, color);
    }
    state_->commitProjectEdit();
    state_->noteProjectGeometryChanged(true);
    updateColorButton();
}

bool PropertyPanel::sampleGuideColorToSelection()
{
    if (guideColorSampleFn_ == nullptr) {
        return false;
    }
    const std::optional<QColor> sampled = guideColorSampleFn_();
    if (!sampled.has_value() || !sampled->isValid()) {
        return false;
    }
    applyColorToSelection({static_cast<quint8>(sampled->blue()),
                           static_cast<quint8>(sampled->green()),
                           static_cast<quint8>(sampled->red()),
                           static_cast<quint8>(sampled->alpha())});
    return true;
}

void PropertyPanel::pickColor()
{
    const QVector<std::array<quint8, 4>> colors = selectionColors();
    if (colors.isEmpty()) {
        return;
    }
    const std::array<quint8, 4> current = colors.front();
    const QSet<QString> selectedLayerIds = state_->selectedLayerIds();
    QVector<QString> selectedGroupIds;
    selectedGroupIds.reserve(groups_.size());
    for (const fh6::LayerGroup *group : groups_) {
        if (group != nullptr) {
            selectedGroupIds.push_back(group->id);
        }
    }

    // Open the picker non-modally-driven (live): every colour change is applied to the
    // selection immediately so the canvas updates as the user drags, instead of only on
    // OK. The whole interaction is wrapped in one project edit so undo captures a single
    // before/after step; cancelling restores the original colours.
    state_->beginProjectEdit();
    // beginProjectEdit() snapshots the Qt containers via copy-on-write. Detach and then
    // re-resolve our cached raw pointers before live writes, otherwise the preview writes can
    // mutate the undo "before" snapshot and Cancel cannot restore the original colours.
    if (fh6::Project *project = state_->project()) {
        project->layers.data();
        project->groups.data();
        layers_.clear();
        groups_.clear();
        for (fh6::ShapeLayer &layer : project->layers) {
            if (selectedLayerIds.contains(layer.id)) {
                layers_.push_back(&layer);
            }
        }
        for (const QString &groupId : selectedGroupIds) {
            for (fh6::LayerGroup &group : project->groups) {
                if (group.id == groupId) {
                    groups_.push_back(&group);
                    break;
                }
            }
        }
    }

    QColorDialog dialog(this);
    dialog.setOption(QColorDialog::ShowAlphaChannel, true);
    dialog.setWindowTitle(QStringLiteral("Layer Color"));
    dialog.setCurrentColor(QColor(current[2], current[1], current[0], current[3]));

    connect(&dialog, &QColorDialog::currentColorChanged, this, [this, &dialog](const QColor &picked) {
        if (!picked.isValid()) {
            return;
        }
        QColor effective = picked;
        if (guideColorSampleFn_ != nullptr) {
            const std::optional<QColor> sampledGuideColor = guideColorSampleFn_();
            if (sampledGuideColor.has_value() && sampledGuideColor->isValid()) {
                effective = sampledGuideColor.value();
                if (effective != picked) {
                    const QSignalBlocker blocker(&dialog);
                    dialog.setCurrentColor(effective);
                }
            }
        }
        const std::array<quint8, 4> color = {
            static_cast<quint8>(effective.blue()),
            static_cast<quint8>(effective.green()),
            static_cast<quint8>(effective.red()),
            static_cast<quint8>(effective.alpha()),
        };
        for (fh6::ShapeLayer *layer : layers_) {
            layer->color = color;
        }
        for (fh6::LayerGroup *group : groups_) {
            state_->setGroupDescendantColor(group->id, color);
        }
        // Live drag: repaint only the canvas and set the swatch directly. Regenerating tree
        // thumbnails and rebuilding the property panel every tick is far too costly, so that
        // is deferred to accept below.
        state_->noteCanvasRepaint();
        colorButton_->setText(QStringLiteral("Color"));
        colorButton_->setStyleSheet(colorStyle(color));
    });

    if (dialog.exec() == QDialog::Accepted) {
        state_->commitProjectEdit();
        state_->noteProjectGeometryChanged(true);
        updateColorButton();
    } else {
        // Restores the captured "before" colours and refreshes the tree/property UI;
        // do not touch the cached layer pointers afterwards as they are reseated by that refresh.
        state_->cancelProjectEdit();
    }
}

void PropertyPanel::updateColorButton()
{
    const QVector<std::array<quint8, 4>> colors = selectionColors();
    if (colors.isEmpty()) {
        colorButton_->setText(QStringLiteral("Color"));
        colorButton_->setStyleSheet(QString());
        return;
    }
    const std::array<quint8, 4> color = colors.front();
    const bool mixed = std::any_of(colors.begin(), colors.end(), [&](const std::array<quint8, 4> &other) {
        return other != color;
    });
    if (mixed) {
        colorButton_->setText(QStringLiteral("Mixed"));
        colorButton_->setStyleSheet(mixedColorButtonStyle());
    } else {
        colorButton_->setText(QStringLiteral("Color"));
        colorButton_->setStyleSheet(colorStyle(color));
    }
}

} // namespace gui
