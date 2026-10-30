#include "property_panel.h"

#include "editor_state.h"
#include "gui_assets.h"
#include "gui_constants.h"
#include "perf_utils.h"
#include "scene_view.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace gui {
namespace {

constexpr double kPositionSpinRange = 100000.0;
constexpr double kScaleSkewSpinRange = 10000.0;
constexpr double kRotationSpinMax = 359.99999;
constexpr double kOpacitySpinStep = 0.05;
constexpr double kFloatSpinStep = 0.1;
constexpr int kFloatSpinDecimals = 5;

constexpr double kLabelDragStepDefault = 1.0;
constexpr double kLabelDragStepScale = 0.1;
constexpr double kLabelDragStepSkew = 0.1;
constexpr double kLabelDragStepOpacity = 0.01;

bool isTransformProperty(const QString &property) {
    return property == QStringLiteral("x") || property == QStringLiteral("y")
        || property == QStringLiteral("scaleX") || property == QStringLiteral("scaleY")
        || property == QStringLiteral("rotation") || property == QStringLiteral("skew");
}

bool isColorableShape(const fls::scene::Shape *layer) {
    return layer != nullptr && !layer->raster;
}

constexpr int kPropertyIconExtent = 14;
constexpr int kPropertyLabelSpacing = 5;

QString mixedValueStyle() {
    return QStringLiteral("color: #888;");
}

QString mixedColorButtonStyle() {
    return QStringLiteral("background-color: #888;");
}

class ArithmeticExpressionParser final {
public:
    explicit ArithmeticExpressionParser(QString expression)
        : expression_(std::move(expression)) {}

    std::optional<double> evaluate() {
        position_ = 0;
        const std::optional<double> result = parseExpression();
        skipSpaces();
        if (!result.has_value() || position_ != expression_.size() || !std::isfinite(*result)) {
            return std::nullopt;
        }

        return result;
    }

private:
    std::optional<double> parseExpression() {
        std::optional<double> result = parseTerm();
        while (result.has_value()) {
            skipSpaces();
            if (!consume(QLatin1Char('+')) && !consume(QLatin1Char('-'))) {
                break;
            }
            const QChar operation = expression_[position_ - 1];
            const std::optional<double> operand = parseTerm();
            if (!operand.has_value()) {
                return std::nullopt;
            }
            *result = operation == QLatin1Char('+') ? *result + *operand : *result - *operand;
        }

        return result;
    }

    std::optional<double> parseTerm() {
        std::optional<double> result = parseFactor();
        while (result.has_value()) {
            skipSpaces();
            if (!consume(QLatin1Char('*')) && !consume(QLatin1Char('/'))
                && !consume(QLatin1Char('%'))) {
                break;
            }
            const QChar operation = expression_[position_ - 1];
            const std::optional<double> operand = parseFactor();
            if (!operand.has_value() || ((operation == QLatin1Char('/') || operation == QLatin1Char('%'))
                                          && std::abs(*operand) <= std::numeric_limits<double>::epsilon())) {
                return std::nullopt;
            }
            if (operation == QLatin1Char('*')) {
                *result *= *operand;
            } else if (operation == QLatin1Char('/')) {
                *result /= *operand;
            } else {
                *result = std::fmod(*result, *operand);
            }
        }

        return result;
    }

    std::optional<double> parseFactor() {
        skipSpaces();
        if (consume(QLatin1Char('+'))) {
            return parseFactor();
        }
        if (consume(QLatin1Char('-'))) {
            const std::optional<double> value = parseFactor();
            return value.has_value() ? std::optional<double>(-*value) : std::nullopt;
        }
        if (consume(QLatin1Char('('))) {
            const std::optional<double> value = parseExpression();
            skipSpaces();
            return value.has_value() && consume(QLatin1Char(')')) ? value : std::nullopt;
        }

        return parseNumber();
    }

    std::optional<double> parseNumber() {
        skipSpaces();
        const qsizetype start = position_;
        bool haveDigit = false;
        while (position_ < expression_.size() && expression_[position_].isDigit()) {
            haveDigit = true;
            ++position_;
        }
        if (consume(QLatin1Char('.'))) {
            while (position_ < expression_.size() && expression_[position_].isDigit()) {
                haveDigit = true;
                ++position_;
            }
        }
        if (!haveDigit) {
            return std::nullopt;
        }
        if (position_ < expression_.size()
            && (expression_[position_] == QLatin1Char('e') || expression_[position_] == QLatin1Char('E'))) {
            const qsizetype exponentStart = position_++;
            if (position_ < expression_.size()
                && (expression_[position_] == QLatin1Char('+') || expression_[position_] == QLatin1Char('-'))) {
                ++position_;
            }
            const qsizetype exponentDigits = position_;
            while (position_ < expression_.size() && expression_[position_].isDigit()) {
                ++position_;
            }
            if (position_ == exponentDigits) {
                position_ = exponentStart;
            }
        }
        bool ok = false;
        const double value = expression_.mid(start, position_ - start).toDouble(&ok);

        return ok ? std::optional<double>(value) : std::nullopt;
    }

    void skipSpaces() {
        while (position_ < expression_.size() && expression_[position_].isSpace()) {
            ++position_;
        }
    }

    bool consume(QChar character) {
        if (position_ >= expression_.size() || expression_[position_] != character) {
            return false;
        }
        ++position_;
        return true;
    }

    QString expression_;
    qsizetype position_ = 0;
};

class ExpressionDoubleSpinBox : public QDoubleSpinBox {
public:
    using QDoubleSpinBox::QDoubleSpinBox;

    QValidator::State validate(QString &text, int &position) const override {
        Q_UNUSED(position);
        const QString expression = normalizedExpression(text);
        if (expression.isEmpty()) {
            return QValidator::Intermediate;
        }
        if (ArithmeticExpressionParser(expression).evaluate().has_value()) {
            return QValidator::Acceptable;
        }
        const bool expressionCharactersOnly = std::all_of(
            expression.cbegin(), expression.cend(), [](QChar character) {
                return character.isDigit() || character.isSpace()
                    || QStringLiteral(".+-*/%()eE").contains(character);
            });

        return expressionCharactersOnly ? QValidator::Intermediate : QValidator::Invalid;
    }

protected:
    double valueFromText(const QString &text) const override {
        const std::optional<double> value = ArithmeticExpressionParser(normalizedExpression(text)).evaluate();

        return value.value_or(QDoubleSpinBox::valueFromText(text));
    }

private:
    QString normalizedExpression(QString text) const {
        const QString groupSeparator = locale().groupSeparator();
        if (!groupSeparator.isEmpty()) {
            text.remove(groupSeparator);
        }
        text.replace(locale().decimalPoint(), QStringLiteral("."));

        return text.trimmed();
    }
};

std::vector<QSignalBlocker> makeSignalBlockers(std::initializer_list<QObject *> objects) {
    std::vector<QSignalBlocker> blockers;
    blockers.reserve(objects.size());
    for (QObject *object : objects) {
        blockers.emplace_back(object);
    }

    return blockers;
}

template <typename LayerType, typename Getter>
void setMixedDouble(QDoubleSpinBox *box,
                    const QVector<LayerType *> &layers,
                    QHash<QWidget *, double> &baselines,
                    const Getter &getter) {
    double sum = 0.0;
    double minimum = getter(*layers.front());
    double maximum = minimum;
    for (const LayerType *layer : layers) {
        const double value = getter(*layer);
        sum += value;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    const double average = sum / layers.size();
    baselines.insert(box, average);
    box->setValue(average);
    box->setStyleSheet(minimum == maximum ? QString() : mixedValueStyle());
}

template <typename LayerType, typename Getter>
void setMixedCheck(QCheckBox *box, const QVector<LayerType *> &layers, const Getter &getter) {
    const bool first = getter(*layers.front());
    const bool mixed = std::any_of(layers.cbegin(), layers.cend(), [&](const LayerType *layer) {
        return getter(*layer) != first;
    });
    box->setTristate(mixed);
    box->setCheckState(mixed ? Qt::PartiallyChecked : (first ? Qt::Checked : Qt::Unchecked));
}

class FlagCheckBox final : public QCheckBox {
public:
    using QCheckBox::QCheckBox;

protected:
    void nextCheckState() override {
        setCheckState(checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    }

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

class RotationSpinBox final : public ExpressionDoubleSpinBox {
public:
    using ExpressionDoubleSpinBox::ExpressionDoubleSpinBox;

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
        layout->setSpacing(kPropertyLabelSpacing);
        auto *icon = new QLabel(this);
        icon->setPixmap(assetIcon(iconName).pixmap(kPropertyIconExtent, kPropertyIconExtent));
        icon->setProperty("flsPropertyIconName", iconName);
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
    layout->setSpacing(kPropertyLabelSpacing);
    auto *icon = new QLabel(widget);
    icon->setPixmap(assetIcon(iconName).pixmap(kPropertyIconExtent, kPropertyIconExtent));
    icon->setProperty("flsPropertyIconName", iconName);
    layout->addWidget(icon);
    auto *label = new QLabel(text, widget);
    layout->addWidget(label);
    layout->addStretch(1);
    return widget;
}

double opacityFromAlpha(quint8 alpha) {
    return static_cast<double>(alpha) / 255.0;
}

QTransform flatEntryTransform(const fls::scene::Shape &layer) {
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

QTransform parentWorldTransform(const fls::scene::Layer &node) {
    const fls::scene::Layer *parent = node.parent();
    return parent != nullptr ? sceneWorldTransform(*parent) : QTransform();
}

QTransform localResultForWorldTransform(const fls::scene::Layer &node,
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

QSet<QString> coveredLayerIdsForGroups(EditorState *state, const QVector<fls::scene::Group *> &groups) {
    QSet<QString> ids;
    if (state == nullptr) {
        return ids;
    }
    for (const fls::scene::Group *group : groups) {
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
                          const QVector<fls::scene::Shape *> &layers,
                          const QVector<fls::scene::Group *> &groups,
                          QSet<QString> *groupedLayerIdsOut = nullptr) {
    const QSet<QString> groupedLayerIds = coveredLayerIdsForGroups(state, groups);
    if (groupedLayerIdsOut != nullptr) {
        *groupedLayerIdsOut = groupedLayerIds;
    }
    QStringList parts;
    for (const fls::scene::Group *group : groups) {
        if (group != nullptr) {
            parts.push_back(QStringLiteral("g:%1").arg(group->id));
        }
    }
    for (const fls::scene::Shape *layer : layers) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            parts.push_back(QStringLiteral("l:%1").arg(layer->id));
        }
    }
    parts.sort();
    return parts.join(QLatin1Char('|'));
}

QRectF shapeVisualLocalRect(const fls::scene::Shape &layer,
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

QRectF selectionWorldBounds(const QVector<fls::scene::Shape *> &layers,
                            const std::function<QSizeF(int)> &spriteSizeFn,
                            const std::function<QRectF(int)> &shapeVisualBoundsFn) {
    BoundsAccumulator acc;
    for (const fls::scene::Shape *layer : layers) {
        if (layer == nullptr) {
            continue;
        }
        acc.add(sceneWorldTransform(*layer), shapeVisualLocalRect(*layer, spriteSizeFn, shapeVisualBoundsFn));
    }
    return acc.hasBounds() ? acc.bounds() : QRectF();
}

TransformAverage boxTargetAverage(const QVector<fls::scene::Shape *> &layers,
                                  const QVector<fls::scene::Group *> &groups,
                                  const QSet<QString> &groupedLayerIds) {
    TransformAverage average;
    for (const fls::scene::Group *group : groups) {
        if (group != nullptr) {
            average.add(decomposeAffine(sceneWorldTransform(*group), group->skew));
        }
    }
    for (const fls::scene::Shape *layer : layers) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            average.add(decomposeAffine(sceneWorldTransform(*layer), layer->skew));
        }
    }
    return average;
}

QVector<QString> transformTargetIdsForSelection(EditorState *state,
                                                const QVector<fls::scene::Shape *> &layers,
                                                const QVector<fls::scene::GuideLayer *> &guides,
                                                const QVector<fls::scene::Group *> &groups,
                                                const QVector<fls::scene::ContourFigure *> &contours = {}) {
    QVector<QString> ids;
    QSet<QString> seen;
    const auto add = [&](const QString &id) {
        if (!id.isEmpty() && !seen.contains(id)) {
            ids.push_back(id);
            seen.insert(id);
        }
    };
    const QSet<QString> groupedLayerIds = coveredLayerIdsForGroups(state, groups);
    for (const fls::scene::Group *group : groups) {
        if (group != nullptr) {
            add(group->id);
        }
    }
    for (const fls::scene::Shape *layer : layers) {
        if (layer != nullptr && !groupedLayerIds.contains(layer->id)) {
            add(layer->id);
        }
    }
    for (const fls::scene::GuideLayer *guide : guides) {
        if (guide != nullptr) {
            add(guide->id);
        }
    }
    for (const fls::scene::ContourFigure *contour : contours) {
        if (contour != nullptr && !groupedLayerIds.contains(contour->id)) {
            add(contour->id);
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
    x_ = floatBox(-kPositionSpinRange, kPositionSpinRange);
    y_ = floatBox(-kPositionSpinRange, kPositionSpinRange);
    scaleX_ = floatBox(-kScaleSkewSpinRange, kScaleSkewSpinRange);
    scaleY_ = floatBox(-kScaleSkewSpinRange, kScaleSkewSpinRange);
    rotation_ = new RotationSpinBox(this);
    rotation_->setRange(0.0, kRotationSpinMax);
    rotation_->setDecimals(kFloatSpinDecimals);
    rotation_->setSingleStep(kFloatSpinStep);
    rotation_->setKeyboardTracking(false);
    rotation_->setWrapping(true);
    skew_ = floatBox(-kScaleSkewSpinRange, kScaleSkewSpinRange);
    visible_ = new FlagCheckBox(this);
    locked_ = new FlagCheckBox(this);
    mask_ = new FlagCheckBox(this);
    opacity_ = floatBox(0.0, 1.0);
    opacity_->setSingleStep(kOpacitySpinStep);
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
        widget->setObjectName(QStringLiteral("property.%1").arg(it.value()));
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
    colorButton_->setObjectName(QStringLiteral("property.color"));
    connect(colorButton_, &QPushButton::clicked, this, [this]() { pickColor(); });

    const auto dragLabel = [this](const QString &text, const QString &iconName, const QString &property, QDoubleSpinBox *box) {
        auto *label = propertyLabel(this,
                             text,
                             iconName,
                             [this, property, box](const QPoint &pos) { return beginValueLabelDrag(property, box, pos); },
                             [this](const QPoint &pos) { updateValueLabelDrag(pos); },
                             [this](bool commit) { endValueLabelDrag(commit); });
        label->setObjectName(QStringLiteral("property.label.") + property);

        return label;
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
    auto *box = new ExpressionDoubleSpinBox(this);
    box->setRange(low, high);
    box->setDecimals(kFloatSpinDecimals);
    box->setSingleStep(kFloatSpinStep);
    box->setKeyboardTracking(false);
    return box;
}

void PropertyPanel::setLayers(const QVector<fls::scene::Shape *> &layers) {
    setSelection(layers, {}, {});
}

void PropertyPanel::setSelection(const QVector<fls::scene::Shape *> &layers,
                                 const QVector<fls::scene::GuideLayer *> &guides,
                                 const QVector<fls::scene::Group *> &groups,
                                 const QVector<fls::scene::ContourFigure *> &contours) {
    if (applyingChange_ || valueLabelDragging_) {
        return;
    }
    ScopedPerf perf("PropertyPanel::setSelection");
    loading_ = true;
    layers_ = layers;
    guides_ = guides;
    groups_ = groups;
    contours_ = contours;
    selectionEntryIds_.clear();
    for (const auto *node : layers_) selectionEntryIds_.push_back(node->id);
    for (const auto *node : guides_) selectionEntryIds_.push_back(node->id);
    for (const auto *node : contours_) selectionEntryIds_.push_back(node->id);
    for (const auto *node : groups_) selectionEntryIds_.push_back(node->id);
    selectionEntryIds_ = state_->normalizeEntrySelection(selectionEntryIds_);
    baselines_.clear();
    setEnabled(!layers_.isEmpty() || !guides_.isEmpty() || !groups_.isEmpty() || !contours_.isEmpty());
    if (!isBoxSelection()) {
        boxProxyBaseValid_ = false;
        boxProxySignature_.clear();
    }
    if (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty() && contours_.isEmpty()) {
        debug_->setText(QString());
        loading_ = false;
        return;
    }
    if (!contours_.isEmpty() && layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty()) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(false);
        }
        for (QWidget *widget : guidePropertyWidgets(name_, x_, y_, scaleX_, scaleY_, rotation_, opacity_, visible_, locked_))
            widget->setEnabled(true);
        skew_->setEnabled(true);
        colorButton_->setEnabled(true);
        setContourFields();
        loading_ = false;
        return;
    }
    if (!contours_.isEmpty() && groups_.isEmpty()) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_))
            widget->setEnabled(false);
        debug_->setText(QStringLiteral("Mixed saved-path selection"));
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
    if (!guides_.isEmpty() && groups_.isEmpty()) {
        for (QWidget *widget : layerPropertyWidgets(name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_, visible_, locked_, mask_, colorButton_)) {
            widget->setEnabled(false);
        }
        for (auto [box, property] : {std::pair{visible_, QStringLiteral("visible")},
                                    std::pair{locked_, QStringLiteral("locked")},
                                    std::pair{mask_, QStringLiteral("mask")}}) {
            const QSignalBlocker blocker(box);
            const auto check = state_->propertyCheckState(selectionEntryIds_, property);
            box->setEnabled(!state_->propertyTargets(selectionEntryIds_, property).isEmpty());
            box->setTristate(check == Qt::PartiallyChecked);
            box->setCheckState(check);
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
        for (auto [box, property] : {std::pair{visible_, QStringLiteral("visible")},
                                    std::pair{locked_, QStringLiteral("locked")},
                                    std::pair{mask_, QStringLiteral("mask")}}) {
            const auto check = state_->propertyCheckState(selectionEntryIds_, property);
            box->setTristate(check == Qt::PartiallyChecked);
            box->setCheckState(check);
        }
        const QString firstName = state_->sceneNode(selectionEntryIds_.front())->name;
        const bool sameName = std::all_of(selectionEntryIds_.begin(), selectionEntryIds_.end(), [&](const QString &id) {
            return state_->sceneNode(id)->name == firstName;
        });
        name_->setText(sameName ? firstName : QString());
        name_->setStyleSheet(sameName ? QString() : mixedValueStyle());

        const QVector<std::array<quint8, 4>> colors = selectionColors(false);
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
        || (layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty() && contours_.isEmpty())) {
        return;
    }
    loading_ = true;
    if (!contours_.isEmpty() && layers_.isEmpty() && guides_.isEmpty() && groups_.isEmpty()) {
        setContourFields();
    } else if (!guides_.isEmpty() && layers_.isEmpty() && groups_.isEmpty()) {
        if (guides_.size() == 1) {
            const fls::scene::GuideLayer *guide = guides_.front();
            const auto blockers = makeSignalBlockers({x_, y_, scaleX_, scaleY_, rotation_});
            setPositionScaleRotation(guide->transform);
        } else {
            setMultipleGuides(guides_);
        }
    } else if (layers_.size() == 1 && groups_.isEmpty()) {
        const fls::scene::Shape *layer = layers_.front();
        const auto blockers = makeSignalBlockers({x_, y_, scaleX_, scaleY_, rotation_, skew_});
        setPositionScaleRotation(layer->transform);
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
    for (const fls::scene::Shape *layer : layers_) {
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

    const auto blockers = makeSignalBlockers({x_, y_, scaleX_, scaleY_, rotation_, skew_});
    setBoxProxyValues(center, scaleXValue, scaleYValue, rotationValue, skewValue);
}

void PropertyPanel::setPositionScaleRotation(const fls::scene::Transform2D &transform) {
    x_->setValue(transform.x);
    y_->setValue(transform.y);
    scaleX_->setValue(transform.scaleX);
    scaleY_->setValue(transform.scaleY);
    rotation_->setValue(transform.rotation);
}

void PropertyPanel::setSingleNodeFields(const fls::scene::Layer &node, double opacity) {
    name_->setText(node.name);
    setPositionScaleRotation(node.transform);
    opacity_->setValue(opacity);
    visible_->setTristate(false);
    locked_->setTristate(false);
    mask_->setTristate(false);
    visible_->setChecked(node.visible);
    clearMixedStyles();
}

void PropertyPanel::setSingleLayer(const fls::scene::Shape *layer) {
    const auto blockers = makeSignalBlockers({name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_,
                                              visible_, locked_, mask_, opacity_});

    setSingleNodeFields(*layer, opacityFromAlpha(layer->color[ColorByteAlpha]));
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

void PropertyPanel::setSingleGuide(const fls::scene::GuideLayer *guide) {
    const auto blockers = makeSignalBlockers({name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_,
                                              visible_, locked_, mask_, opacity_});

    setSingleNodeFields(*guide, guide->opacity);
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

void PropertyPanel::setContourFields() {
    const auto blockers = makeSignalBlockers({name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_,
        visible_, locked_, mask_, opacity_});
    const QString firstName = contours_.front()->name;
    const bool sameName = std::all_of(contours_.begin(), contours_.end(), [&](const auto *figure) {
        return figure->name == firstName;
    });

    clearMixedStyles();
    setMixedDouble(x_, contours_, baselines_, [](const auto &figure) { return figure.x; });
    setMixedDouble(y_, contours_, baselines_, [](const auto &figure) { return figure.y; });
    setMixedDouble(scaleX_, contours_, baselines_, [](const auto &figure) { return figure.scaleX; });
    setMixedDouble(scaleY_, contours_, baselines_, [](const auto &figure) { return figure.scaleY; });
    setMixedDouble(rotation_, contours_, baselines_, [](const auto &figure) { return figure.rotation; });
    setMixedDouble(skew_, contours_, baselines_, [](const auto &figure) { return figure.skew; });
    setMixedDouble(opacity_, contours_, baselines_, [](const auto &figure) { return figure.opacity; });
    setMixedCheck(visible_, contours_, [](const auto &figure) { return figure.visible; });
    setMixedCheck(locked_, contours_, [this](const auto &figure) { return state_->isLayerLocked(figure.id); });
    mask_->setTristate(false);
    mask_->setChecked(false);
    name_->setText(sameName ? firstName : QString());
    name_->setStyleSheet(sameName ? QString() : mixedValueStyle());
    updateColorButton();
    debug_->setText(QStringLiteral("%1 saved path(s) selected").arg(contours_.size()));
}

void PropertyPanel::setMultipleLayers(const QVector<fls::scene::Shape *> &layers) {
    const auto blockers = makeSignalBlockers({name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_,
                                              visible_, locked_, mask_, opacity_});

    setMixedDouble(x_, layers, baselines_, [](const fls::scene::Shape &layer) { return layer.x; });
    setMixedDouble(y_, layers, baselines_, [](const fls::scene::Shape &layer) { return layer.y; });
    setMixedDouble(scaleX_, layers, baselines_, [](const fls::scene::Shape &layer) { return layer.scaleX; });
    setMixedDouble(scaleY_, layers, baselines_, [](const fls::scene::Shape &layer) { return layer.scaleY; });
    setMixedDouble(rotation_, layers, baselines_, [](const fls::scene::Shape &layer) { return layer.rotation; });
    setMixedDouble(skew_, layers, baselines_, [](const fls::scene::Shape &layer) { return layer.skew; });
    setMixedDouble(opacity_, layers, baselines_, [](const fls::scene::Shape &layer) {
        return opacityFromAlpha(layer.color[ColorByteAlpha]);
    });
    setMixedCheck(visible_, layers, [](const fls::scene::Shape &layer) { return layer.visible; });
    setMixedCheck(locked_, layers, [this](const fls::scene::Shape &layer) {
        return state_->isLayerLocked(layer.id);
    });
    setMixedCheck(mask_, layers, [](const fls::scene::Shape &layer) { return layer.mask; });

    const QString firstName = layers.front()->name;
    const bool sameName = std::all_of(layers.begin(), layers.end(), [&](const fls::scene::Shape *l) {
        return l->name == firstName;
    });
    name_->setText(sameName ? firstName : QString());
    name_->setStyleSheet(sameName ? QString() : mixedValueStyle());

    const quint16 firstShapeId = layers.front()->shapeId;
    const bool sameShapeId = std::all_of(layers.begin(), layers.end(), [&](const fls::scene::Shape *l) {
        return l->shapeId == firstShapeId;
    });
    shapeId_->setValue(firstShapeId);
    shapeId_->setStyleSheet(sameShapeId ? QString() : mixedValueStyle());

    updateColorButton();
    debug_->setText(QStringLiteral("%1 layers selected").arg(layers.size()));
}

void PropertyPanel::setMultipleGuides(const QVector<fls::scene::GuideLayer *> &guides) {
    const auto blockers = makeSignalBlockers({name_, shapeId_, x_, y_, scaleX_, scaleY_, rotation_, skew_,
                                              visible_, locked_, mask_, opacity_});

    setMixedDouble(x_, guides, baselines_, [](const fls::scene::GuideLayer &guide) { return guide.x; });
    setMixedDouble(y_, guides, baselines_, [](const fls::scene::GuideLayer &guide) { return guide.y; });
    setMixedDouble(scaleX_, guides, baselines_, [](const fls::scene::GuideLayer &guide) { return guide.scaleX; });
    setMixedDouble(scaleY_, guides, baselines_, [](const fls::scene::GuideLayer &guide) { return guide.scaleY; });
    setMixedDouble(rotation_, guides, baselines_, [](const fls::scene::GuideLayer &guide) { return guide.rotation; });
    setMixedDouble(opacity_, guides, baselines_, [](const fls::scene::GuideLayer &guide) { return guide.opacity; });
    setMixedCheck(visible_, guides, [](const fls::scene::GuideLayer &guide) { return guide.visible; });
    setMixedCheck(locked_, guides, [](const fls::scene::GuideLayer &guide) { return guide.locked; });

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

void PropertyPanel::setBoxProxyValues(const QPointF &center, double scaleXValue,
                                       double scaleYValue, double rotationValue,
                                       double skewValue) {
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

void PropertyPanel::setBoxProxyFields(bool neutralTransformValues) {
    const QPointF center = selectionBoxCenter();
    const auto blockers = makeSignalBlockers({x_, y_, scaleX_, scaleY_, rotation_, skew_});
    double scaleXValue = 1.0;
    double scaleYValue = 1.0;
    double rotationValue = 0.0;
    double skewValue = 0.0;
    if (!neutralTransformValues) {
        QSet<QString> groupedLayerIds;
        const QString signature = boxProxySignature(state_, layers_, groups_, &groupedLayerIds);
        int looseLayerCount = 0;
        for (const fls::scene::Shape *layer : layers_) {
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

    setBoxProxyValues(center, scaleXValue, scaleYValue, rotationValue, skewValue);
}

void PropertyPanel::clearMixedStyles() {
    name_->setStyleSheet(QString());
    shapeId_->setStyleSheet(QString());
    for (QDoubleSpinBox *box : {x_, y_, scaleX_, scaleY_, rotation_, skew_, opacity_}) {
        box->setStyleSheet(QString());
    }
}

void PropertyPanel::applyChanged(QWidget *sender) {
    if (loading_ || selectionEntryIds_.isEmpty()) {
        return;
    }
    EditorCommand command;
    command.entries = selectionEntryIds_;
    command.property = widgetProperties_.value(sender);
    if (command.property.isEmpty()) {
        return;
    }
    const auto layerIds = state_->selectedLayerIds();
    const auto guideIds = state_->selectedGuideLayerIds();
    const auto groupIds = state_->fullySelectedTopGroupIds();
    applyingChange_ = true;
    if (auto *check = qobject_cast<QCheckBox *>(sender)) {
        if (check->checkState() == Qt::PartiallyChecked) {
            applyingChange_ = false;
            return;
        }
        command.value = check->checkState() == Qt::Checked;
    } else if (auto *box = qobject_cast<QDoubleSpinBox *>(sender)) {
        const double old = baselines_.value(box, box->value());
        command.value = box->value();
        if (isTransformProperty(command.property) && isBoxSelection()) {
            command.type = EditorCommandType::WorldTransform;
            command.worldTransform = boxAffine(command.property, old, box->value(), selectionBoxCenter());
        } else if ((layers_.size() > 1 || guides_.size() > 1 || contours_.size() > 1) && groups_.isEmpty()) {
            command.relative = true;
            command.value = box->value() - old;
        }
        baselines_.insert(box, box->value());
    } else if (auto *box = qobject_cast<QSpinBox *>(sender)) {
        command.value = box->value();
    } else if (auto *line = qobject_cast<QLineEdit *>(sender)) {
        command.value = line->text();
    }
    QString error;
    const bool accepted = state_->executeCommand(command, &error);
    applyingChange_ = false;
    setSelectionByIds(layerIds, guideIds, groupIds);
    if (!accepted) {
        sender->setToolTip(error);
    }
}

bool PropertyPanel::beginValueLabelDrag(const QString &property, QDoubleSpinBox *box, const QPoint &globalPos) {
    if (loading_ || valueLabelDragging_ || box == nullptr || !box->isEnabled() || selectionEntryIds_.isEmpty()) {
        return false;
    }
    for (const QString &id : state_->propertyTargets(selectionEntryIds_, property)) {
        if (state_->isLayerLocked(id)) {
            return false;
        }
    }
    valueLabelDragging_ = true;
    valueLabelProperty_ = property;
    valueLabelBox_ = box;
    valueLabelStartGlobal_ = globalPos;
    valueLabelBoxStartValue_ = box->value();
    valueLabelLayerIds_ = state_->selectedLayerIds();
    valueLabelGuideIds_ = state_->selectedGuideLayerIds();
    valueLabelGroupIds_ = state_->fullySelectedTopGroupIds();
    valueLabelBoxDrag_ = isBoxSelection() && isTransformProperty(property);
    valueLabelBoxCenter_ = selectionBoxCenter();
    valueLabelNodeStartValues_.clear();
    for (const QString &id : state_->propertyTargets(selectionEntryIds_, property)) {
        const auto *node = state_->sceneNode(id);
        double value = box->value();
        if (property == QStringLiteral("x")) value = node->x;
        else if (property == QStringLiteral("y")) value = node->y;
        else if (property == QStringLiteral("scaleX")) value = node->scaleX;
        else if (property == QStringLiteral("scaleY")) value = node->scaleY;
        else if (property == QStringLiteral("rotation")) value = node->rotation;
        else if (property == QStringLiteral("skew")) value = node->skew;
        else if (const auto *shape = dynamic_cast<const fls::scene::Shape *>(node)) value = opacityFromAlpha(shape->color[3]);
        else value = node->opacity;
        valueLabelNodeStartValues_.insert(id, value);
    }
    valueLabelUsesTransformCommand_ = isTransformProperty(property);
    if (valueLabelUsesTransformCommand_) {
        state_->beginTransformCommand(selectionEntryIds_);
    } else {
        state_->beginProjectEdit();
    }
    QApplication::setOverrideCursor(QCursor(Qt::SizeVerCursor));

    return true;
}

void PropertyPanel::updateValueLabelDrag(const QPoint &globalPos) {
    if (!valueLabelDragging_ || valueLabelBox_ == nullptr) {
        return;
    }
    const double step = valueLabelProperty_ == QStringLiteral("opacity") ? kLabelDragStepOpacity
        : valueLabelProperty_ == QStringLiteral("skew") ? kLabelDragStepSkew
        : valueLabelProperty_ == QStringLiteral("scaleX") || valueLabelProperty_ == QStringLiteral("scaleY")
            ? kLabelDragStepScale : kLabelDragStepDefault;
    const double delta = (valueLabelStartGlobal_.y() - globalPos.y()) * step;
    const auto adjusted = [&](double start) {
        const double value = start + delta;

        return valueLabelProperty_ == QStringLiteral("rotation") ? normalizeRotation(value)
            : std::clamp(value, valueLabelBox_->minimum(), valueLabelBox_->maximum());
    };
    EditorCommand command;
    command.entries = selectionEntryIds_;
    command.property = valueLabelProperty_;
    const double proxy = adjusted(valueLabelBoxStartValue_);
    if (valueLabelBoxDrag_) {
        command.type = EditorCommandType::WorldTransform;
        command.worldTransform = boxAffine(valueLabelProperty_, valueLabelBox_->value(), proxy, valueLabelBoxCenter_);
    } else {
        for (auto it = valueLabelNodeStartValues_.cbegin(); it != valueLabelNodeStartValues_.cend(); ++it) {
            command.values.insert(it.key(), adjusted(it.value()));
        }
    }
    state_->executeCommand(command);
    const QSignalBlocker blocker(valueLabelBox_);
    valueLabelBox_->setValue(proxy);
    if (valueLabelProperty_ == QStringLiteral("opacity")) {
        updateColorButton();
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
    valueLabelDragging_ = false;
    valueLabelBox_ = nullptr;
    if (commit) {
        state_->commitProjectEdit();
        state_->noteProjectGeometryChanged(!valueLabelUsesTransformCommand_, selectionEntryIds_);
    } else {
        state_->cancelProjectEdit();
    }
    valueLabelNodeStartValues_.clear();
    setSelectionByIds(valueLabelLayerIds_, valueLabelGuideIds_, valueLabelGroupIds_);
}

void PropertyPanel::detachSelectionForEdit() {
    if (state_->project() == nullptr) {
        return;
    }
    QSet<QString> layerIds;
    QSet<QString> guideIds;
    QVector<QString> groupIds;
    layerIds.reserve(layers_.size() + contours_.size());
    for (const auto *contour : contours_) {
        layerIds.insert(contour->id);
    }
    guideIds.reserve(guides_.size());
    groupIds.reserve(groups_.size());
    for (const fls::scene::Shape *layer : layers_) {
        layerIds.insert(layer->id);
    }
    for (const fls::scene::GuideLayer *guide : guides_) {
        guideIds.insert(guide->id);
    }
    for (const fls::scene::Group *group : groups_) {
        groupIds.push_back(group->id);
    }
    setSelectionByIds(layerIds, guideIds, groupIds);
}

void PropertyPanel::setSelectionByIds(const QSet<QString> &layerIds,
                                      const QSet<QString> &guideIds,
                                      const QVector<QString> &groupIds) {
    QVector<fls::scene::Shape *> layers;
    QVector<fls::scene::GuideLayer *> guides;
    QVector<fls::scene::Group *> groups;
    QVector<fls::scene::ContourFigure *> contours;
    for (const QString &id : layerIds) {
        if (auto *layer = dynamic_cast<fls::scene::Shape *>(state_->sceneNode(id))) {
            layers.push_back(layer);
        } else if (auto *contour = dynamic_cast<fls::scene::ContourFigure *>(state_->sceneNode(id))) {
            contours.push_back(contour);
        }
    }
    for (const QString &id : guideIds) {
        if (auto *guide = dynamic_cast<fls::scene::GuideLayer *>(state_->sceneNode(id))) {
            guides.push_back(guide);
        }
    }
    for (const QString &groupId : groupIds) {
        if (fls::scene::Group *group = state_->groupForId(groupId)) {
            groups.push_back(group);
        }
    }
    setSelection(layers, guides, groups, contours);
}


QVector<std::array<quint8, 4>> PropertyPanel::selectionColors(bool colorableOnly) const {
    ScopedPerf perf("PropertyPanel::selectionColors");
    QVector<std::array<quint8, 4>> colors;
    const auto appendColor = [&](const fls::scene::Layer *node) {
        if (const auto *layer = dynamic_cast<const fls::scene::Shape *>(node)) {
            if (!colorableOnly || isColorableShape(layer)) {
                colors.push_back(layer->color);
            }
        } else if (const auto *contour = dynamic_cast<const fls::scene::ContourFigure *>(node);
                   contour != nullptr && (!colorableOnly || !state_->isLayerLocked(contour->id))) {
            const QColor color = fls::scene::contourFillColor(contour->data);
            colors.push_back({static_cast<quint8>(color.blue()), static_cast<quint8>(color.green()),
                              static_cast<quint8>(color.red()), static_cast<quint8>(color.alpha())});
        }
    };
    const auto ids = state_->propertyTargets(selectionEntryIds_, colorableOnly ? QStringLiteral("color") : QStringLiteral("opacity"));
    for (const QString &id : ids) {
        appendColor(state_->sceneNode(id));
    }

    return colors;
}

QVector<std::array<quint8, 4>> PropertyPanel::colorableSelectionColors() const {
    return selectionColors(true);
}

std::optional<std::array<quint8, 4>> PropertyPanel::currentSelectionColor() const {
    const QVector<std::array<quint8, 4>> colors = colorableSelectionColors();
    if (colors.isEmpty()) {
        return std::nullopt;
    }
    return colors.front();
}

void PropertyPanel::setSelectionColor(const std::array<quint8, 4> &color) {
    EditorCommand command;
    command.entries = selectionEntryIds_;
    command.property = QStringLiteral("color");
    command.value = QColor(color[2], color[1], color[0], color[3]);
    const QScopedValueRollback<bool> guard(applyingChange_, true);
    state_->executeCommand(command);
}

void PropertyPanel::applyColorToSelection(const std::array<quint8, 4> &color) {
    if (colorableSelectionColors().isEmpty()) {
        return;
    }
    setSelectionColor(color);
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
    for (const fls::scene::Group *group : groups_) {
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
        setSelectionColor(color);
        state_->noteCanvasRepaint();
        colorButton_->setText(QStringLiteral("Color"));
        colorButton_->setStyleSheet(colorStyle(color));
    });

    if (dialog.exec() == QDialog::Accepted) {
        state_->commitProjectEdit();
        state_->noteProjectGeometryChanged(true, transformTargetIdsForSelection(state_, layers_, guides_, groups_, contours_));
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
