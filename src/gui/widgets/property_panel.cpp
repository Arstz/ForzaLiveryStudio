#include "property_panel.h"

#include "editor_state.h"
#include "gui_assets.h"
#include "gui_constants.h"
#include "perf_utils.h"
#include "scene_view.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr double PositionSpinRange = 100000.0;
constexpr double ScaleSkewSpinRange = 10000.0;
constexpr double RotationSpinMax = 359.99999;
constexpr double OpacitySpinStep = 0.05;
constexpr double FloatSpinStep = 0.1;
constexpr int FloatSpinDecimals = 5;

constexpr double LabelDragStepDefault = 1.0;
constexpr double LabelDragStepScale = 0.1;
constexpr double LabelDragStepSkew = 0.1;
constexpr double LabelDragStepOpacity = 0.01;

constexpr double kPi = 3.14159265358979323846;

bool isTransformProperty(const QString &property) {
    return property == QStringLiteral("x") || property == QStringLiteral("y")
        || property == QStringLiteral("scaleX") || property == QStringLiteral("scaleY")
        || property == QStringLiteral("rotation") || property == QStringLiteral("skew");
}

bool isColorableShape(const fh6::scene::Shape *layer) {
    return layer != nullptr && !layer->raster;
}

constexpr int PropertyIconExtent = 14;
constexpr int PropertyLabelSpacing = 5;

QString mixedValueStyle() {
    return QStringLiteral("color: #888;");
}

QString mixedColorButtonStyle() {
    return QStringLiteral("background-color: #888;");
}

class FlagCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void paintEvent(QPaintEvent *event) override {
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

double normalizeRotation(double value) {
    if (!std::isfinite(value)) {
        return 0.0;
    }
    double out = std::fmod(value, 360.0);
    if (out < 0.0) {
        out += 360.0;
    }
    return out;
}

class RotationSpinBox final : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

    void stepBy(int steps) override {
        if (steps == 0) {
            return;
        }
        setValue(normalizeRotation(value() + static_cast<double>(steps) * singleStep()));
    }
};

QTransform aboutPivot(const QPointF &pivot, const QTransform &inner) {
    QTransform out;
    out.translate(pivot.x(), pivot.y());
    out = inner * out;
    QTransform back;
    back.translate(-pivot.x(), -pivot.y());
    return back * out;
}

QTransform boxAffine(const QString &property, double from, double to, const QPointF &pivot) {
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

struct AffineDecomposition {
    bool ok = false;
    double x = 0.0;
    double y = 0.0;
    double rotation = 0.0;
    double skew = 0.0;
    double scaleX = 1.0;
    double scaleY = 1.0;
};

AffineDecomposition decomposeAffine(const QTransform &result, double fallbackSkew) {
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

void applyDecomposedTransform(fh6::scene::Shape *layer, const QTransform &result) {
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

struct TransformAverage {
    int count = 0;
    double scaleX = 0.0;
    double scaleY = 0.0;
    double rotationSin = 0.0;
    double rotationCos = 0.0;
    double skew = 0.0;

    void add(const AffineDecomposition &dec) {
        if (!dec.ok) {
            return;
        }
        const double radians = dec.rotation * kPi / 180.0;
        ++count;
        scaleX += dec.scaleX;
        scaleY += dec.scaleY;
        rotationSin += std::sin(radians);
        rotationCos += std::cos(radians);
        skew += dec.skew;
    }

    double averageScaleX() const { return count > 0 ? scaleX / count : 1.0; }
    double averageScaleY() const { return count > 0 ? scaleY / count : 1.0; }
    double averageSkew() const { return count > 0 ? skew / count : 0.0; }

    double averageRotation() const {
        if (count == 0 || (std::abs(rotationSin) < 1e-9 && std::abs(rotationCos) < 1e-9)) {
            return 0.0;
        }
        return normalizeRotation(std::atan2(rotationSin, rotationCos) * 180.0 / kPi);
    }
};

QString colorStyle(const std::array<quint8, 4> &color) {
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
        , end_(std::move(end)) {
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

    ~DraggablePropertyLabel() override {
        if (dragging_ && end_) {
            end_(false);
        }
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && begin_ != nullptr && begin_(event->globalPosition().toPoint())) {
            dragging_ = true;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (dragging_ && move_ != nullptr) {
            move_(event->globalPosition().toPoint());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
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
                       std::function<void(bool)> end = {}) {
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

double opacityFromAlpha(quint8 alpha) {
    return static_cast<double>(alpha) / 255.0;
}

QTransform flatEntryTransform(const fh6::scene::Shape &layer) {
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

QTransform parentWorldTransform(const fh6::scene::Layer &node) {
    const fh6::scene::Layer *parent = node.parent();
    return parent != nullptr ? sceneWorldTransform(*parent) : QTransform();
}

QTransform localResultForWorldTransform(const fh6::scene::Layer &node,
                                        const QTransform &startLocal,
                                        const QTransform &worldTransform) {
    const QTransform parentWorld = parentWorldTransform(node);
    bool invertible = false;
    const QTransform parentWorldInverse = parentWorld.inverted(&invertible);
    if (!invertible) {
        return startLocal;
    }
    return startLocal * parentWorld * worldTransform * parentWorldInverse;
}

QSet<QString> coveredLayerIdsForGroups(EditorState *state, const QVector<fh6::scene::Group *> &groups) {
    QSet<QString> ids;
    if (state == nullptr) {
        return ids;
    }
    for (const fh6::scene::Group *group : groups) {
        if (group == nullptr) {
            continue;
        }
        for (const QString &id : state->leafLayerIdsForEntry(group->id)) {
            ids.insert(id);
        }
    }
    return ids;
}

QString boxProxySignature(EditorState *state,
                          const QVector<fh6::scene::Shape *> &layers,
                          const QVector<fh6::scene::Group *> &groups,
                          QSet<QString> *groupedLayerIdsOut = nullptr) {
    const QSet<QString> groupedLayerIds = coveredLayerIdsForGroups(state, groups);
    if (groupedLayerIdsOut != nullptr) {
        *groupedLayerIdsOut = groupedLayerIds;
    }
    QStringList parts;
    for (const fh6::scene::Group *group : groups) {
        if (group != nullptr) {
            parts.push_back(QStringLiteral("g:%1").arg(group->id));
        }
    }
    for (const fh6::scene::Shape *layer : layers) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            parts.push_back(QStringLiteral("l:%1").arg(layer->id));
        }
    }
    parts.sort();
    return parts.join(QLatin1Char('|'));
}

QRectF shapeVisualLocalRect(const fh6::scene::Shape &layer,
                            const std::function<QSizeF(int)> &spriteSizeFn,
                            const std::function<QRectF(int)> &shapeVisualBoundsFn) {
    if (layer.raster) {
        return sceneLocalRect(QSizeF(layer.rasterWidth, layer.rasterHeight));
    }
    if (shapeVisualBoundsFn) {
        const QRectF bounds = shapeVisualBoundsFn(layer.shapeId);
        if (bounds.isValid() && !bounds.isEmpty()) {
            return bounds;
        }
    }
    const QSizeF size = spriteSizeFn ? spriteSizeFn(layer.shapeId) : QSizeF(0.0, 0.0);
    return sceneLocalRect(size);
}

QRectF selectionWorldBounds(const QVector<fh6::scene::Shape *> &layers,
                            const std::function<QSizeF(int)> &spriteSizeFn,
                            const std::function<QRectF(int)> &shapeVisualBoundsFn) {
    BoundsAccumulator acc;
    for (const fh6::scene::Shape *layer : layers) {
        if (layer == nullptr) {
            continue;
        }
        acc.add(sceneWorldTransform(*layer), shapeVisualLocalRect(*layer, spriteSizeFn, shapeVisualBoundsFn));
    }
    return acc.hasBounds() ? acc.bounds() : QRectF();
}

TransformAverage boxTargetAverage(const QVector<fh6::scene::Shape *> &layers,
                                  const QVector<fh6::scene::Group *> &groups,
                                  const QSet<QString> &groupedLayerIds) {
    TransformAverage average;
    for (const fh6::scene::Group *group : groups) {
        if (group != nullptr) {
            average.add(decomposeAffine(sceneWorldTransform(*group), group->skew));
        }
    }
    for (const fh6::scene::Shape *layer : layers) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            average.add(decomposeAffine(sceneWorldTransform(*layer), layer->skew));
        }
    }
    return average;
}

QVector<QString> transformTargetIdsForSelection(EditorState *state,
                                                const QVector<fh6::scene::Shape *> &layers,
                                                const QVector<fh6::scene::GuideLayer *> &guides,
                                                const QVector<fh6::scene::Group *> &groups) {
    QVector<QString> ids;
    QSet<QString> seen;
    const auto add = [&](const QString &id) {
        if (!id.isEmpty() && !seen.contains(id)) {
            ids.push_back(id);
            seen.insert(id);
        }
    };
    const QSet<QString> groupedLayerIds = coveredLayerIdsForGroups(state, groups);
    for (const fh6::scene::Group *group : groups) {
        if (group != nullptr) {
            add(group->id);
        }
    }
    for (const fh6::scene::Shape *layer : layers) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            add(layer->id);
        }
    }
    for (const fh6::scene::GuideLayer *guide : guides) {
        if (guide != nullptr) {
            add(guide->id);
        }
    }
    return ids;
}

quint8 alphaFromOpacity(double opacity) {
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
    QPushButton *colorButton) {
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
    QCheckBox *locked) {
    return {name, x, y, scaleX, scaleY, rotation, opacity, visible, locked};
}

} // namespace

PropertyPanel::PropertyPanel(EditorState *state, QWidget *parent)
    : QWidget(parent)
    , state_(state) {
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
    rotation_ = new RotationSpinBox(this);
    rotation_->setRange(0.0, RotationSpinMax);
    rotation_->setDecimals(FloatSpinDecimals);
    rotation_->setSingleStep(FloatSpinStep);
    rotation_->setKeyboardTracking(false);
    rotation_->setWrapping(true);
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
        if (qobject_cast<QAbstractSpinBox *>(widget) != nullptr) {
            widget->installEventFilter(this);
        }
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

void PropertyPanel::setDebugVisible(bool visible) {
    debugVisible_ = visible;
    if (debugLabel_ != nullptr) {
        debugLabel_->setVisible(visible);
    }
    if (debug_ != nullptr) {
        debug_->setVisible(visible);
    }
}

void PropertyPanel::setSpriteSizeFn(std::function<QSizeF(int)> fn) {
    spriteSizeFn_ = std::move(fn);
}

void PropertyPanel::setShapeVisualBoundsFn(std::function<QRectF(int)> fn) {
    shapeVisualBoundsFn_ = std::move(fn);
}

void PropertyPanel::setGuideColorSampleFn(std::function<std::optional<QColor>()> fn) {
    guideColorSampleFn_ = std::move(fn);
}

void PropertyPanel::setValueEditingWheelEnabled(bool enabled) {
    valueEditingWheelEnabled_ = enabled;
}

bool PropertyPanel::eventFilter(QObject *watched, QEvent *event) {
    if (!valueEditingWheelEnabled_ && event != nullptr && event->type() == QEvent::Wheel
        && qobject_cast<QAbstractSpinBox *>(watched) != nullptr) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        QScrollArea *scrollArea = nullptr;
        for (QWidget *parent = qobject_cast<QWidget *>(watched); parent != nullptr; parent = parent->parentWidget()) {
            scrollArea = qobject_cast<QScrollArea *>(parent);
            if (scrollArea != nullptr) {
                break;
            }
        }
        if (scrollArea != nullptr && scrollArea->viewport() != nullptr) {
            QWheelEvent forwarded(QPointF(scrollArea->viewport()->mapFromGlobal(wheel->globalPosition().toPoint())),
                                  wheel->globalPosition(),
                                  wheel->pixelDelta(),
                                  wheel->angleDelta(),
                                  wheel->buttons(),
                                  wheel->modifiers(),
                                  wheel->phase(),
                                  wheel->inverted(),
                                  wheel->source(),
                                  wheel->pointingDevice());
            QCoreApplication::sendEvent(scrollArea->viewport(), &forwarded);
        }
        event->ignore();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

std::array<int, 3> PropertyPanel::flagCheckStates() const {
    return {static_cast<int>(visible_->checkState()),
            static_cast<int>(locked_->checkState()),
            static_cast<int>(mask_->checkState())};
}

QDoubleSpinBox *PropertyPanel::floatBox(double low, double high) {
    auto *box = new QDoubleSpinBox(this);
    box->setRange(low, high);
    box->setDecimals(FloatSpinDecimals);
    box->setSingleStep(FloatSpinStep);
    box->setKeyboardTracking(false);
    return box;
}

void PropertyPanel::setLayers(const QVector<fh6::scene::Shape *> &layers) {
    setSelection(layers, {}, {});
}

void PropertyPanel::setSelection(const QVector<fh6::scene::Shape *> &layers,
                                 const QVector<fh6::scene::GuideLayer *> &guides,
                                 const QVector<fh6::scene::Group *> &groups) {
    if (applyingChange_ || valueLabelDragging_) {
        return;
    }
    ScopedPerf perf("PropertyPanel::setSelection");
    loading_ = true;
    layers_ = layers;
    guides_ = guides;
    groups_ = groups;
    baselines_.clear();
    setEnabled(!layers_.isEmpty() || !guides_.isEmpty() || !groups_.isEmpty());
    if (!isBoxSelection()) {
        boxProxyBaseValid_ = false;
        boxProxySignature_.clear();
    }
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
            setSingleGuide(guides_.front());
        } else {
            setMultipleGuides(guides_);
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
        auto leafTriState = [this](const fh6::scene::Group &group, auto layerPred) {
            const QVector<QString> ids = state_->leafLayerIdsForEntry(group.id);
            if (ids.isEmpty()) {
                return Qt::Unchecked;
            }
            bool anyTrue = false;
            bool anyFalse = false;
            for (const QString &id : ids) {
                const auto *layer = dynamic_cast<const fh6::scene::Shape *>(state_->sceneNode(id));
                if (layer == nullptr) {
                    continue;
                }
                if (layerPred(*layer)) {
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
            for (fh6::scene::Group *group : groups_) {
                if (getter(*group) != combined) {
                    combined = Qt::PartiallyChecked;
                    break;
                }
            }
            box->setTristate(combined == Qt::PartiallyChecked);
            box->setCheckState(combined);
        };
        setGroupCheck(visible_, [&](const fh6::scene::Group &group) {
            return leafTriState(group, [](const fh6::scene::Shape &layer) { return layer.visible; });
        });
        setGroupCheck(mask_, [&](const fh6::scene::Group &group) {
            return leafTriState(group, [](const fh6::scene::Shape &layer) { return layer.mask; });
        });
        setGroupCheck(locked_, [&](const fh6::scene::Group &group) {
            return leafTriState(group, [this](const fh6::scene::Shape &layer) { return state_->isLayerLocked(layer.id); });
        });
        const QString firstName = groups_.front()->name;
        const bool sameName = std::all_of(groups_.begin(), groups_.end(), [&](const fh6::scene::Group *group) {
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
            setBoxProxyFields(false);
        }
        debug_->setText(QStringLiteral("%1 group%2 selected").arg(groups_.size()).arg(groups_.size() == 1 ? QString() : QStringLiteral("s")));
    } else if (layers_.size() == 1) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(true);
        }
        setSingleLayer(layers_.front());
    } else {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(true);
        }
        setMultipleLayers(layers_);
        setBoxProxyFields(false);
    }
    loading_ = false;
}

void PropertyPanel::refreshTransformFields() {
    if (loading_ || applyingChange_ || valueLabelDragging_
        || (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty())) {
        return;
    }
    loading_ = true;
    if (!guides_.isEmpty() && layers_.isEmpty() && groups_.isEmpty()) {
        if (guides_.size() == 1) {
            const fh6::scene::GuideLayer *guide = guides_.front();
            const QSignalBlocker bx(x_);
            const QSignalBlocker by(y_);
            const QSignalBlocker bsx(scaleX_);
            const QSignalBlocker bsy(scaleY_);
            const QSignalBlocker br(rotation_);
            x_->setValue(guide->x);
            y_->setValue(guide->y);
            scaleX_->setValue(guide->scaleX);
            scaleY_->setValue(guide->scaleY);
            rotation_->setValue(guide->rotation);
        } else {
            setMultipleGuides(guides_);
        }
    } else if (layers_.size() == 1 && groups_.isEmpty()) {
        const fh6::scene::Shape *layer = layers_.front();
        const QSignalBlocker bx(x_);
        const QSignalBlocker by(y_);
        const QSignalBlocker bsx(scaleX_);
        const QSignalBlocker bsy(scaleY_);
        const QSignalBlocker br(rotation_);
        const QSignalBlocker bk(skew_);
        x_->setValue(layer->x);
        y_->setValue(layer->y);
        scaleX_->setValue(layer->scaleX);
        scaleY_->setValue(layer->scaleY);
        rotation_->setValue(layer->rotation);
        skew_->setValue(layer->skew);
    } else if (isBoxSelection()) {
        setBoxProxyFields(false);
    }
    loading_ = false;
}

void PropertyPanel::refreshTransformFieldsFromBox(const QPointF &center,
                                                  double width,
                                                  double height,
                                                  const QTransform &boxFrame,
                                                  bool valid) {
    if (loading_ || applyingChange_ || valueLabelDragging_ || !isBoxSelection()) {
        return;
    }
    if (!valid || width <= 1e-9 || height <= 1e-9) {
        return;
    }

    QSet<QString> groupedLayerIds;
    const QString signature = boxProxySignature(state_, layers_, groups_, &groupedLayerIds);
    int looseLayerCount = 0;
    for (const fh6::scene::Shape *layer : layers_) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            ++looseLayerCount;
        }
    }
    const int targetCount = groups_.size() + looseLayerCount;

    const AffineDecomposition frame = decomposeAffine(boxFrame, 0.0);
    if (!frame.ok) {
        return;
    }

    double scaleXValue = 1.0;
    double scaleYValue = 1.0;
    double rotationValue = 0.0;
    double skewValue = 0.0;
    if (targetCount == 1 && groups_.size() == 1 && looseLayerCount == 0 && groups_.front() != nullptr) {
        scaleXValue = frame.scaleX;
        scaleYValue = frame.scaleY;
        rotationValue = frame.rotation;
        skewValue = frame.skew;
        boxProxyBaseValid_ = false;
        boxProxySignature_.clear();
    } else {
        const bool resetProxy = !boxProxyBaseValid_
            || boxProxySignature_ != signature
            || boxProxyBaseWidth_ <= 1e-9
            || boxProxyBaseHeight_ <= 1e-9;
        if (resetProxy) {
            boxProxySignature_ = signature;
            boxProxyBaseWidth_ = width;
            boxProxyBaseHeight_ = height;
            boxProxyBaseScaleX_ = frame.scaleX;
            boxProxyBaseScaleY_ = frame.scaleY;
            boxProxyBaseRotation_ = frame.rotation;
            boxProxyBaseSkew_ = frame.skew;
            boxProxyBaseValid_ = true;
        } else {
            const double baseWorldWidth = boxProxyBaseScaleX_ * boxProxyBaseWidth_;
            const double baseWorldHeight = boxProxyBaseScaleY_ * boxProxyBaseHeight_;
            if (std::abs(baseWorldWidth) > 1e-9) {
                scaleXValue = (frame.scaleX * width) / baseWorldWidth;
            }
            if (std::abs(baseWorldHeight) > 1e-9) {
                scaleYValue = (frame.scaleY * height) / baseWorldHeight;
            }
            rotationValue = normalizeRotation(frame.rotation - boxProxyBaseRotation_);
            skewValue = frame.skew - boxProxyBaseSkew_;
        }
    }

    const QSignalBlocker bx(x_);
    const QSignalBlocker by(y_);
    const QSignalBlocker bsx(scaleX_);
    const QSignalBlocker bsy(scaleY_);
    const QSignalBlocker br(rotation_);
    const QSignalBlocker bk(skew_);
    const auto setProxy = [this](QDoubleSpinBox *box, double value) {
        box->setValue(value);
        box->setStyleSheet(QString());
        baselines_.insert(box, value);
    };
    setProxy(x_, center.x());
    setProxy(y_, center.y());
    setProxy(scaleX_, scaleXValue);
    setProxy(scaleY_, scaleYValue);
    setProxy(rotation_, normalizeRotation(rotationValue));
    setProxy(skew_, skewValue);
}

void PropertyPanel::setSingleLayer(const fh6::scene::Shape *layer) {
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

    name_->setText(layer->name);
    x_->setValue(layer->x);
    y_->setValue(layer->y);
    scaleX_->setValue(layer->scaleX);
    scaleY_->setValue(layer->scaleY);
    rotation_->setValue(layer->rotation);
    opacity_->setValue(opacityFromAlpha(layer->color[ColorByteAlpha]));
    visible_->setTristate(false);
    locked_->setTristate(false);
    mask_->setTristate(false);
    visible_->setChecked(layer->visible);
    clearMixedStyles();
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
}

void PropertyPanel::setSingleGuide(const fh6::scene::GuideLayer *guide) {
    const QSignalBlocker b1(name_);
    const QSignalBlocker b3(x_);
    const QSignalBlocker b4(y_);
    const QSignalBlocker b5(scaleX_);
    const QSignalBlocker b6(scaleY_);
    const QSignalBlocker b7(rotation_);
    const QSignalBlocker b9(visible_);
    const QSignalBlocker b10(locked_);
    const QSignalBlocker b11(mask_);
    const QSignalBlocker b12(opacity_);

    name_->setText(guide->name);
    x_->setValue(guide->x);
    y_->setValue(guide->y);
    scaleX_->setValue(guide->scaleX);
    scaleY_->setValue(guide->scaleY);
    rotation_->setValue(guide->rotation);
    opacity_->setValue(guide->opacity);
    visible_->setTristate(false);
    locked_->setTristate(false);
    mask_->setTristate(false);
    visible_->setChecked(guide->visible);
    clearMixedStyles();
    locked_->setChecked(guide->locked);
    mask_->setChecked(false);
    colorButton_->setText(QStringLiteral("N/A"));
    colorButton_->setStyleSheet(QString());
    const int width = guide->image ? guide->image->width : 0;
    const int height = guide->image ? guide->image->height : 0;
    const QString format = guide->image ? guide->image->format : QString();
    debug_->setText(QStringLiteral("%1 x %2\nformat: %3")
                        .arg(width)
                        .arg(height)
                        .arg(format));
}

void PropertyPanel::setMultipleLayers(const QVector<fh6::scene::Shape *> &layers) {
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

    auto setDouble = [this, &layers](QDoubleSpinBox *box, auto getter) {
        double sum = 0.0;
        double minValue = getter(*layers.front());
        double maxValue = minValue;
        for (const fh6::scene::Shape *layer : layers) {
            const double value = getter(*layer);
            sum += value;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
        const double average = sum / layers.size();
        baselines_.insert(box, average);
        box->setValue(average);
        box->setStyleSheet(minValue == maxValue ? QString() : mixedValueStyle());
    };
    auto setCheck = [&layers](QCheckBox *box, auto getter) {
        bool first = getter(*layers.front());
        bool mixed = false;
        for (const fh6::scene::Shape *layer : layers) {
            if (getter(*layer) != first) {
                mixed = true;
                break;
            }
        }
        box->setTristate(mixed);
        box->setCheckState(mixed ? Qt::PartiallyChecked : (first ? Qt::Checked : Qt::Unchecked));
    };

    setDouble(x_, [](const fh6::scene::Shape &l) { return l.x; });
    setDouble(y_, [](const fh6::scene::Shape &l) { return l.y; });
    setDouble(scaleX_, [](const fh6::scene::Shape &l) { return l.scaleX; });
    setDouble(scaleY_, [](const fh6::scene::Shape &l) { return l.scaleY; });
    setDouble(rotation_, [](const fh6::scene::Shape &l) { return l.rotation; });
    setDouble(skew_, [](const fh6::scene::Shape &l) { return l.skew; });
    setDouble(opacity_, [](const fh6::scene::Shape &l) { return opacityFromAlpha(l.color[ColorByteAlpha]); });
    setCheck(visible_, [](const fh6::scene::Shape &l) { return l.visible; });
    setCheck(locked_, [this](const fh6::scene::Shape &l) { return state_->isLayerLocked(l.id); });
    setCheck(mask_, [](const fh6::scene::Shape &l) { return l.mask; });

    const QString firstName = layers.front()->name;
    const bool sameName = std::all_of(layers.begin(), layers.end(), [&](const fh6::scene::Shape *l) {
        return l->name == firstName;
    });
    name_->setText(sameName ? firstName : QString());
    name_->setStyleSheet(sameName ? QString() : mixedValueStyle());

    const quint16 firstShapeId = layers.front()->shapeId;
    const bool sameShapeId = std::all_of(layers.begin(), layers.end(), [&](const fh6::scene::Shape *l) {
        return l->shapeId == firstShapeId;
    });
    shapeId_->setValue(firstShapeId);
    shapeId_->setStyleSheet(sameShapeId ? QString() : mixedValueStyle());

    updateColorButton();
    debug_->setText(QStringLiteral("%1 layers selected").arg(layers.size()));
}

void PropertyPanel::setMultipleGuides(const QVector<fh6::scene::GuideLayer *> &guides) {
    const QSignalBlocker b1(name_);
    const QSignalBlocker b3(x_);
    const QSignalBlocker b4(y_);
    const QSignalBlocker b5(scaleX_);
    const QSignalBlocker b6(scaleY_);
    const QSignalBlocker b7(rotation_);
    const QSignalBlocker b9(visible_);
    const QSignalBlocker b10(locked_);
    const QSignalBlocker b11(mask_);
    const QSignalBlocker b12(opacity_);

    auto setDouble = [this, &guides](QDoubleSpinBox *box, auto getter) {
        double sum = 0.0;
        double minValue = getter(*guides.front());
        double maxValue = minValue;
        for (const fh6::scene::GuideLayer *guide : guides) {
            const double value = getter(*guide);
            sum += value;
            minValue = std::min(minValue, value);
            maxValue = std::max(maxValue, value);
        }
        const double average = sum / guides.size();
        baselines_.insert(box, average);
        box->setValue(average);
        box->setStyleSheet(minValue == maxValue ? QString() : mixedValueStyle());
    };
    auto setCheck = [&guides](QCheckBox *box, auto getter) {
        bool first = getter(*guides.front());
        bool mixed = false;
        for (const fh6::scene::GuideLayer *guide : guides) {
            if (getter(*guide) != first) {
                mixed = true;
                break;
            }
        }
        box->setTristate(mixed);
        box->setCheckState(mixed ? Qt::PartiallyChecked : (first ? Qt::Checked : Qt::Unchecked));
    };

    setDouble(x_, [](const fh6::scene::GuideLayer &g) { return g.x; });
    setDouble(y_, [](const fh6::scene::GuideLayer &g) { return g.y; });
    setDouble(scaleX_, [](const fh6::scene::GuideLayer &g) { return g.scaleX; });
    setDouble(scaleY_, [](const fh6::scene::GuideLayer &g) { return g.scaleY; });
    setDouble(rotation_, [](const fh6::scene::GuideLayer &g) { return g.rotation; });
    setDouble(opacity_, [](const fh6::scene::GuideLayer &g) { return g.opacity; });
    setCheck(visible_, [](const fh6::scene::GuideLayer &g) { return g.visible; });
    setCheck(locked_, [](const fh6::scene::GuideLayer &g) { return g.locked; });

    mask_->setTristate(false);
    mask_->setChecked(false);
    name_->setText(QString());
    name_->setStyleSheet(mixedValueStyle());
    colorButton_->setText(QStringLiteral("N/A"));
    colorButton_->setStyleSheet(QString());
    debug_->setText(QStringLiteral("%1 guide layers selected").arg(guides.size()));
}

bool PropertyPanel::isBoxSelection() const {
    return guides_.isEmpty() && (!groups_.isEmpty() || layers_.size() > 1);
}

QPointF PropertyPanel::selectionBoxCenter() const {
    const QRectF bounds = selectionWorldBounds(layers_, spriteSizeFn_, shapeVisualBoundsFn_);
    return bounds.isValid() ? bounds.center() : QPointF(0.0, 0.0);
}

void PropertyPanel::setBoxProxyFields(bool neutralTransformValues) {
    const QPointF center = selectionBoxCenter();
    const QSignalBlocker bx(x_);
    const QSignalBlocker by(y_);
    const QSignalBlocker bsx(scaleX_);
    const QSignalBlocker bsy(scaleY_);
    const QSignalBlocker br(rotation_);
    const QSignalBlocker bk(skew_);
    double scaleXValue = 1.0;
    double scaleYValue = 1.0;
    double rotationValue = 0.0;
    double skewValue = 0.0;
    if (!neutralTransformValues) {
        QSet<QString> groupedLayerIds;
        const QString signature = boxProxySignature(state_, layers_, groups_, &groupedLayerIds);
        int looseLayerCount = 0;
        for (const fh6::scene::Shape *layer : layers_) {
            if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
                ++looseLayerCount;
            }
        }
        const int targetCount = groups_.size() + looseLayerCount;
        if (targetCount == 1 && groups_.size() == 1 && looseLayerCount == 0 && groups_.front() != nullptr) {
            const AffineDecomposition dec = decomposeAffine(sceneWorldTransform(*groups_.front()), groups_.front()->skew);
            if (dec.ok) {
                scaleXValue = dec.scaleX;
                scaleYValue = dec.scaleY;
                rotationValue = dec.rotation;
                skewValue = dec.skew;
            }
            boxProxyBaseValid_ = false;
            boxProxySignature_.clear();
        } else {
            const QRectF bounds = selectionWorldBounds(layers_, spriteSizeFn_, shapeVisualBoundsFn_);
            const TransformAverage average = boxTargetAverage(layers_, groups_, groupedLayerIds);
            const bool resetProxy = !boxProxyBaseValid_
                || boxProxySignature_ != signature
                || !boxProxyBaseBounds_.isValid()
                || boxProxyBaseBounds_.width() <= 1e-9
                || boxProxyBaseBounds_.height() <= 1e-9;
            if (resetProxy) {
                boxProxySignature_ = signature;
                boxProxyBaseBounds_ = bounds;
                boxProxyBaseRotation_ = average.averageRotation();
                boxProxyBaseSkew_ = average.averageSkew();
                boxProxyBaseValid_ = bounds.isValid() && average.count > 0;
            } else if (bounds.isValid() && average.count > 0) {
                scaleXValue = bounds.width() / boxProxyBaseBounds_.width();
                scaleYValue = bounds.height() / boxProxyBaseBounds_.height();
                rotationValue = normalizeRotation(average.averageRotation() - boxProxyBaseRotation_);
                skewValue = average.averageSkew() - boxProxyBaseSkew_;
            }
        }
    }

    const auto setProxy = [this](QDoubleSpinBox *box, double value) {
        box->setValue(value);
        box->setStyleSheet(QString());
        baselines_.insert(box, value);
    };
    setProxy(x_, center.x());
    setProxy(y_, center.y());
    setProxy(scaleX_, scaleXValue);
    setProxy(scaleY_, scaleYValue);
    setProxy(rotation_, normalizeRotation(rotationValue));
    setProxy(skew_, skewValue);
}

void PropertyPanel::applyBoxTransform(const QString &property, double fromValue, double toValue) {
    const QTransform transform = boxAffine(property, fromValue, toValue, selectionBoxCenter());
    if (transform.isIdentity()) {
        return;
    }
    const QSet<QString> groupedLayerIds = coveredLayerIdsForGroups(state_, groups_);
    for (fh6::scene::Shape *layer : layers_) {
        if (groupedLayerIds.contains(layer->id)) {
            continue;
        }
        applyDecomposedTransform(layer, localResultForWorldTransform(*layer, flatEntryTransform(*layer), transform));
    }
    if (state_ != nullptr && !groups_.isEmpty()) {
        QVector<QString> groupIds;
        groupIds.reserve(groups_.size());
        for (const fh6::scene::Group *group : groups_) {
            groupIds.push_back(group->id);
        }
        state_->transformGroupFrames(groupIds, transform);
    }
}

void PropertyPanel::clearMixedStyles() {
    name_->setStyleSheet(QString());
    shapeId_->setStyleSheet(QString());
    for (QDoubleSpinBox *box : {x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_}) {
        box->setStyleSheet(QString());
    }
}

void PropertyPanel::applyChanged(QWidget *sender) {
    if (loading_ || (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty())) {
        return;
    }
    const QString property = widgetProperties_.value(sender);
    if (property.isEmpty()) {
        return;
    }
    if (!guides_.isEmpty() && (sender != locked_)) {
        for (const fh6::scene::GuideLayer *guide : guides_) {
            if (guide->locked) {
                setSelection(layers_, guides_, groups_);
                return;
            }
        }
    }
    if (groups_.isEmpty() && guides_.isEmpty() && sender != locked_) {
        const QSet<QString> lockedIds = state_->lockedLayerIds();
        for (const fh6::scene::Shape *layer : layers_) {
            if (lockedIds.contains(layer->id)) {
                setSelection(layers_, guides_, groups_);
                return;
            }
        }
    }
    QSet<QString> refreshLayerIds;
    QSet<QString> refreshGuideIds;
    QVector<QString> refreshGroupIds;
    refreshLayerIds.reserve(layers_.size());
    refreshGuideIds.reserve(guides_.size());
    refreshGroupIds.reserve(groups_.size());
    for (const fh6::scene::Shape *layer : layers_) {
        refreshLayerIds.insert(layer->id);
    }
    for (const fh6::scene::GuideLayer *guide : guides_) {
        refreshGuideIds.insert(guide->id);
    }
    for (const fh6::scene::Group *group : groups_) {
        refreshGroupIds.push_back(group->id);
    }
    applyingChange_ = true;
    const bool transformOnly = isTransformProperty(property);
    if (transformOnly) {
        state_->beginTransformCommand(transformTargetIdsForSelection(state_, layers_, guides_, groups_));
    } else {
        state_->beginProjectEdit();
        detachSelectionForEdit();
    }
    if (!guides_.isEmpty() && layers_.isEmpty() && groups_.isEmpty()) {
        if (sender == locked_ && locked_->checkState() != Qt::PartiallyChecked) {
            const bool value = locked_->checkState() == Qt::Checked;
            for (fh6::scene::GuideLayer *guide : guides_) {
                guide->locked = value;
            }
        } else if (sender == visible_ && visible_->checkState() != Qt::PartiallyChecked) {
            const bool value = visible_->checkState() == Qt::Checked;
            for (fh6::scene::GuideLayer *guide : guides_) {
                guide->visible = value;
            }
        } else if (guides_.size() == 1) {
            fh6::scene::GuideLayer *guide = guides_.front();
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
            for (fh6::scene::GuideLayer *guide : guides_) {
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
            for (fh6::scene::GuideLayer *guide : guides_) {
                guide->name = line->text();
            }
        }
    } else if (!groups_.isEmpty()) {
        if (sender == name_) {
            for (fh6::scene::Group *group : groups_) {
                group->name = name_->text();
            }
        } else if (sender == opacity_) {
            for (fh6::scene::Group *group : groups_) {
                state_->setGroupDescendantOpacity(group->id, opacity_->value());
            }
        } else if (sender == locked_ && locked_->checkState() != Qt::PartiallyChecked) {
            const bool value = locked_->checkState() == Qt::Checked;
            for (fh6::scene::Group *group : groups_) {
                state_->setGroupAndDescendantLocked(group->id, value);
            }
        } else if (sender == visible_ && visible_->checkState() != Qt::PartiallyChecked) {
            const bool value = visible_->checkState() == Qt::Checked;
            for (fh6::scene::Group *group : groups_) {
                state_->setGroupDescendantVisible(group->id, value);
            }
        } else if (sender == mask_ && mask_->checkState() != Qt::PartiallyChecked) {
            const bool value = mask_->checkState() == Qt::Checked;
            for (fh6::scene::Group *group : groups_) {
                state_->setGroupDescendantMask(group->id, value);
            }
        } else if (auto *box = qobject_cast<QDoubleSpinBox *>(sender); box != nullptr && isTransformProperty(property)) {
            applyBoxTransform(property, baselines_.value(box, box->value()), box->value());
        }
    } else if (sender == locked_ && locked_->checkState() != Qt::PartiallyChecked) {
        const bool value = locked_->checkState() == Qt::Checked;
        for (fh6::scene::Shape *layer : layers_) {
            state_->setLayerLockScope(layer->id, value);
        }
    } else if (layers_.size() == 1) {
        applySingle(sender);
    } else {
        applyMulti(sender, property);
    }
    if (transformOnly) {
        state_->commitTransformCommand();
    } else {
        state_->commitProjectEdit();
    }
    const QVector<QString> changedIds = transformTargetIdsForSelection(state_, layers_, guides_, groups_);
    if (property == QStringLiteral("visible") || property == QStringLiteral("mask") || property == QStringLiteral("shapeId")
        || (property == QStringLiteral("opacity") && guides_.isEmpty())) {
        state_->noteProjectGeometryChanged(true, changedIds);
    } else if (property == QStringLiteral("x") || property == QStringLiteral("y") || property == QStringLiteral("scaleX")
               || property == QStringLiteral("scaleY") || property == QStringLiteral("rotation") || property == QStringLiteral("skew")
               || property == QStringLiteral("opacity")) {
        state_->noteProjectGeometryChanged(false, changedIds);
    } else {
        state_->noteProjectStructureChanged();
    }
    applyingChange_ = false;
    QMetaObject::invokeMethod(this, [this, refreshLayerIds, refreshGuideIds, refreshGroupIds]() {
        setSelectionByIds(refreshLayerIds, refreshGuideIds, refreshGroupIds);
    }, Qt::QueuedConnection);
}

bool PropertyPanel::beginValueLabelDrag(const QString &property, QDoubleSpinBox *box, const QPoint &globalPos) {
    if (loading_ || valueLabelDragging_ || box == nullptr || !box->isEnabled()
        || (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty())) {
        return false;
    }
    if (!groups_.isEmpty() && property != QStringLiteral("opacity") && !isTransformProperty(property)) {
        return false;
    }
    if (!guides_.isEmpty() && (property == QStringLiteral("skew"))) {
        return false;
    }
    if (!guides_.isEmpty()) {
        for (const fh6::scene::GuideLayer *guide : guides_) {
            if (guide->locked) {
                return false;
            }
        }
    }
    if (groups_.isEmpty() && guides_.isEmpty()) {
        const QSet<QString> lockedIds = state_->lockedLayerIds();
        for (const fh6::scene::Shape *layer : layers_) {
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
    for (const fh6::scene::Group *group : groups_) {
        if (group != nullptr) {
            valueLabelGroupIds_.push_back(group->id);
        }
    }
    valueLabelLayerStartValues_.clear();
    valueLabelGuideStartValues_.clear();
    valueLabelGroupStartValues_.clear();

    valueLabelUsesTransformCommand_ = isTransformProperty(property);
    if (valueLabelUsesTransformCommand_) {
        state_->beginTransformCommand(transformTargetIdsForSelection(state_, layers_, guides_, groups_));
    } else {
        state_->beginProjectEdit();
    }
    if (fh6::Project *project = state_->project()) {
        Q_UNUSED(project);
        setSelectionByIds(valueLabelLayerIds_, valueLabelGuideIds_, valueLabelGroupIds_);
    }

    const auto layerValue = [&](const fh6::scene::Shape &layer) {
        if (property == QStringLiteral("x")) return layer.x;
        if (property == QStringLiteral("y")) return layer.y;
        if (property == QStringLiteral("scaleX")) return layer.scaleX;
        if (property == QStringLiteral("scaleY")) return layer.scaleY;
        if (property == QStringLiteral("rotation")) return layer.rotation;
        if (property == QStringLiteral("skew")) return layer.skew;
        return opacityFromAlpha(layer.color[ColorByteAlpha]);
    };
    const auto guideValue = [&](const fh6::scene::GuideLayer &guide) {
        if (property == QStringLiteral("x")) return guide.x;
        if (property == QStringLiteral("y")) return guide.y;
        if (property == QStringLiteral("scaleX")) return guide.scaleX;
        if (property == QStringLiteral("scaleY")) return guide.scaleY;
        if (property == QStringLiteral("rotation")) return guide.rotation;
        return guide.opacity;
    };
    for (const fh6::scene::Shape *layer : layers_) {
        valueLabelLayerStartValues_.insert(layer->id, layerValue(*layer));
    }
    for (const fh6::scene::GuideLayer *guide : guides_) {
        valueLabelGuideStartValues_.insert(guide->id, guideValue(*guide));
    }
    for (const QString &groupId : valueLabelGroupIds_) {
        valueLabelGroupStartValues_.insert(groupId, box->value());
    }

    valueLabelBoxDrag_ = isBoxSelection() && isTransformProperty(property);
    valueLabelLayerStartTransforms_.clear();
    valueLabelGroupStartFrames_.clear();
    if (valueLabelBoxDrag_) {
        valueLabelBoxCenter_ = selectionBoxCenter();
        const QSet<QString> groupedLayerIds = coveredLayerIdsForGroups(state_, groups_);
        for (const fh6::scene::Shape *layer : layers_) {
            if (groupedLayerIds.contains(layer->id)) {
                continue;
            }
            valueLabelLayerStartTransforms_.insert(layer->id, flatEntryTransform(*layer));
        }
        if (state_ != nullptr) {
            for (const fh6::scene::Group *group : groups_) {
                valueLabelGroupStartFrames_.insert(group->id, state_->groupLocalFrame(group->id));
            }
        }
    }

    QPixmap pixmap(assetPath(QStringLiteral("ToolScaleY.xpm")));
    if (!pixmap.isNull()) {
        pixmap = pixmap.scaled(21, 21, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QApplication::setOverrideCursor(QCursor(pixmap, pixmap.width() / 2, pixmap.height() / 2));
    } else {
        QApplication::setOverrideCursor(QCursor(Qt::SizeVerCursor));
    }
    return true;
}

void PropertyPanel::updateValueLabelDrag(const QPoint &globalPos) {
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

    if (valueLabelBoxDrag_) {
        const double proxy = adjusted(valueLabelBoxStartValue_);
        const QTransform transform = boxAffine(valueLabelProperty_, valueLabelBoxStartValue_, proxy, valueLabelBoxCenter_);
        for (fh6::scene::Shape *layer : layers_) {
            if (!valueLabelLayerStartTransforms_.contains(layer->id)) {
                continue;
            }
            const QTransform start = valueLabelLayerStartTransforms_.value(layer->id, flatEntryTransform(*layer));
            applyDecomposedTransform(layer, localResultForWorldTransform(*layer, start, transform));
        }
        if (!valueLabelGroupStartFrames_.isEmpty()) {
            state_->setGroupFramesFromStart(valueLabelGroupStartFrames_, transform);
        } {
            const QSignalBlocker blocker(valueLabelBox_);
            valueLabelBox_->setValue(proxy);
        }
        state_->noteTransformLiveChanged(transformTargetIdsForSelection(state_, layers_, guides_, groups_));
        if (globalPos.x() != valueLabelStartGlobal_.x()) {
            QCursor::setPos(valueLabelStartGlobal_.x(), globalPos.y());
        }
        return;
    }

    for (fh6::scene::Shape *layer : layers_) {
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
    for (fh6::scene::GuideLayer *guide : guides_) {
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
    for (fh6::scene::Group *group : groups_) {
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
    if (isTransformProperty(valueLabelProperty_)) {
        state_->noteTransformLiveChanged(transformTargetIdsForSelection(state_, layers_, guides_, groups_));
    } else {
        state_->noteCanvasRepaint();
    }

    if (globalPos.x() != valueLabelStartGlobal_.x()) {
        QCursor::setPos(valueLabelStartGlobal_.x(), globalPos.y());
    }
}

void PropertyPanel::endValueLabelDrag(bool commit) {
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
        if (valueLabelUsesTransformCommand_) {
            state_->commitTransformCommand();
        } else {
            state_->commitProjectEdit();
        }
        const bool refreshPreviews = property == QStringLiteral("opacity") && guides_.isEmpty();
        state_->noteProjectGeometryChanged(refreshPreviews, transformTargetIdsForSelection(state_, layers_, guides_, groups_));
    } else {
        if (valueLabelUsesTransformCommand_) {
            state_->cancelTransformCommand();
        } else {
            state_->cancelProjectEdit();
        }
    }
    valueLabelUsesTransformCommand_ = false;

    QVector<fh6::scene::Shape *> layers;
    QVector<fh6::scene::GuideLayer *> guides;
    QVector<fh6::scene::Group *> groups;
    for (const QString &id : valueLabelLayerIds_) {
        if (auto *layer = dynamic_cast<fh6::scene::Shape *>(state_->sceneNode(id))) {
            layers.push_back(layer);
        }
    }
    for (const QString &id : valueLabelGuideIds_) {
        if (auto *guide = dynamic_cast<fh6::scene::GuideLayer *>(state_->sceneNode(id))) {
            guides.push_back(guide);
        }
    }
    for (const QString &groupId : valueLabelGroupIds_) {
        if (fh6::scene::Group *group = state_->groupForId(groupId)) {
            groups.push_back(group);
        }
    }
    setSelection(layers, guides, groups);
    valueLabelLayerIds_.clear();
    valueLabelGuideIds_.clear();
    valueLabelGroupIds_.clear();
}

void PropertyPanel::detachSelectionForEdit() {
    fh6::Project *project = state_->project();
    if (project == nullptr) {
        return;
    }
    QSet<QString> layerIds;
    QSet<QString> guideIds;
    QVector<QString> groupIds;
    layerIds.reserve(layers_.size());
    guideIds.reserve(guides_.size());
    groupIds.reserve(groups_.size());
    for (const fh6::scene::Shape *layer : layers_) {
        layerIds.insert(layer->id);
    }
    for (const fh6::scene::GuideLayer *guide : guides_) {
        guideIds.insert(guide->id);
    }
    for (const fh6::scene::Group *group : groups_) {
        groupIds.push_back(group->id);
    }
    Q_UNUSED(project);
    setSelectionByIds(layerIds, guideIds, groupIds);
}

void PropertyPanel::setSelectionByIds(const QSet<QString> &layerIds,
                                      const QSet<QString> &guideIds,
                                      const QVector<QString> &groupIds) {
    QVector<fh6::scene::Shape *> layers;
    QVector<fh6::scene::GuideLayer *> guides;
    QVector<fh6::scene::Group *> groups;
    for (const QString &id : layerIds) {
        if (auto *layer = dynamic_cast<fh6::scene::Shape *>(state_->sceneNode(id))) {
            layers.push_back(layer);
        }
    }
    for (const QString &id : guideIds) {
        if (auto *guide = dynamic_cast<fh6::scene::GuideLayer *>(state_->sceneNode(id))) {
            guides.push_back(guide);
        }
    }
    for (const QString &groupId : groupIds) {
        if (fh6::scene::Group *group = state_->groupForId(groupId)) {
            groups.push_back(group);
        }
    }
    setSelection(layers, guides, groups);
}

void PropertyPanel::applySingle(QWidget *sender) {
    fh6::scene::Shape *layer = layers_.front();
    layer->name = name_->text();
    if (sender == shapeId_ && !layer->isRaster()) {
        layer->setVectorShape(static_cast<quint16>(shapeId_->value()));
    }
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

void PropertyPanel::applyMulti(QWidget *sender, const QString &property) {
    if (auto *box = qobject_cast<QDoubleSpinBox *>(sender)) {
        if (isTransformProperty(property)) {
            applyBoxTransform(property, baselines_.value(box, box->value()), box->value());
            return;
        }
        const double old = baselines_.value(box, box->value());
        const double delta = box->value() - old;
        baselines_.insert(box, box->value());
        for (fh6::scene::Shape *layer : layers_) {
            if (property == QStringLiteral("opacity")) {
                layer->color[ColorByteAlpha] = alphaFromOpacity(std::clamp(opacityFromAlpha(layer->color[ColorByteAlpha]) + delta, 0.0, 1.0));
            }
        }
    } else if (auto *box = qobject_cast<QSpinBox *>(sender)) {
        for (fh6::scene::Shape *layer : layers_) {
            if (!layer->isRaster()) {
                layer->setVectorShape(static_cast<quint16>(box->value()));
            }
        }
    } else if (auto *line = qobject_cast<QLineEdit *>(sender)) {
        for (fh6::scene::Shape *layer : layers_) {
            layer->name = line->text();
        }
    } else if (auto *check = qobject_cast<QCheckBox *>(sender)) {
        if (check->checkState() == Qt::PartiallyChecked) {
            return;
        }
        const bool value = check->checkState() == Qt::Checked;
        for (fh6::scene::Shape *layer : layers_) {
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

QVector<std::array<quint8, 4>> PropertyPanel::selectionColors() const {
    ScopedPerf perf("PropertyPanel::selectionColors");
    QVector<std::array<quint8, 4>> colors;
    for (const fh6::scene::Shape *layer : layers_) {
        colors.push_back(layer->color);
    }
    if (!groups_.isEmpty()) {
        for (const fh6::scene::Group *group : groups_) {
            const QVector<QString> ids = state_->leafLayerIdsForEntry(group->id);
            for (const QString &id : ids) {
                const auto *layer = dynamic_cast<const fh6::scene::Shape *>(state_->sceneNode(id));
                if (layer != nullptr) {
                    colors.push_back(layer->color);
                }
            }
        }
    }
    return colors;
}

QVector<std::array<quint8, 4>> PropertyPanel::colorableSelectionColors() const {
    QVector<std::array<quint8, 4>> colors;
    for (const fh6::scene::Shape *layer : layers_) {
        if (isColorableShape(layer)) {
            colors.push_back(layer->color);
        }
    }
    if (!groups_.isEmpty()) {
        for (const fh6::scene::Group *group : groups_) {
            const QVector<QString> ids = state_->leafLayerIdsForEntry(group->id);
            for (const QString &id : ids) {
                const auto *layer = dynamic_cast<const fh6::scene::Shape *>(state_->sceneNode(id));
                if (isColorableShape(layer)) {
                    colors.push_back(layer->color);
                }
            }
        }
    }
    return colors;
}

std::optional<std::array<quint8, 4>> PropertyPanel::currentSelectionColor() const {
    const QVector<std::array<quint8, 4>> colors = colorableSelectionColors();
    if (colors.isEmpty()) {
        return std::nullopt;
    }
    return colors.front();
}

void PropertyPanel::applyColorToSelection(const std::array<quint8, 4> &color) {
    if (colorableSelectionColors().isEmpty()) {
        return;
    }
    state_->beginProjectEdit();
    detachSelectionForEdit();
    for (fh6::scene::Shape *layer : layers_) {
        if (isColorableShape(layer)) {
            layer->color = color;
        }
    }
    for (fh6::scene::Group *group : groups_) {
        state_->setGroupDescendantColor(group->id, color);
    }
    state_->commitProjectEdit();
    state_->noteProjectGeometryChanged(true, transformTargetIdsForSelection(state_, layers_, guides_, groups_));
    updateColorButton();
}

bool PropertyPanel::sampleGuideColorToSelection() {
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

void PropertyPanel::pickColor() {
    const QVector<std::array<quint8, 4>> colors = colorableSelectionColors();
    if (colors.isEmpty()) {
        return;
    }
    const std::array<quint8, 4> current = colors.front();
    const QSet<QString> selectedLayerIds = state_->selectedLayerIds();
    QVector<QString> selectedGroupIds;
    selectedGroupIds.reserve(groups_.size());
    for (const fh6::scene::Group *group : groups_) {
        if (group != nullptr) {
            selectedGroupIds.push_back(group->id);
        }
    }

    state_->beginProjectEdit();
    setSelectionByIds(selectedLayerIds, {}, selectedGroupIds);

    QColorDialog dialog(this);
    dialog.setOption(QColorDialog::ShowAlphaChannel, true);
    dialog.setWindowTitle(QStringLiteral("Layer Color"));
    dialog.setCurrentColor(QColor(current[2], current[1], current[0], current[3]));

    connect(&dialog, &QColorDialog::currentColorChanged, this, [this](const QColor &picked) {
        if (!picked.isValid()) {
            return;
        }
        const std::array<quint8, 4> color = {
            static_cast<quint8>(picked.blue()),
            static_cast<quint8>(picked.green()),
            static_cast<quint8>(picked.red()),
            static_cast<quint8>(picked.alpha()),
        };
        for (fh6::scene::Shape *layer : layers_) {
            if (isColorableShape(layer)) {
                layer->color = color;
            }
        }
        for (fh6::scene::Group *group : groups_) {
            state_->setGroupDescendantColor(group->id, color);
        }
        state_->noteCanvasRepaint();
        colorButton_->setText(QStringLiteral("Color"));
        colorButton_->setStyleSheet(colorStyle(color));
    });

    if (dialog.exec() == QDialog::Accepted) {
        state_->commitProjectEdit();
        state_->noteProjectGeometryChanged(true, transformTargetIdsForSelection(state_, layers_, guides_, groups_));
        updateColorButton();
    } else {
        state_->cancelProjectEdit();
    }
}

void PropertyPanel::updateColorButton() {
    const QVector<std::array<quint8, 4>> colors = colorableSelectionColors();
    if (colors.isEmpty()) {
        colorButton_->setEnabled(false);
        colorButton_->setText(QStringLiteral("N/A"));
        colorButton_->setStyleSheet(QString());
        return;
    }
    colorButton_->setEnabled(true);
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
