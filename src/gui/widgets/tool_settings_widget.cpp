#include "tool_settings_widget.h"

#include <QtWidgets>
#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int kOptionSpacing = 6;
constexpr int kFieldSpacing = 4;
constexpr int kSliderWidth = 60;

} // namespace

ToolSettingsWidget::ToolSettingsWidget(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ToolSettings"));
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(kFieldSpacing, 1, kFieldSpacing, 1);
    layout_->setSpacing(kOptionSpacing);
    addNumber(QStringLiteral("bucketTolerance"), QStringLiteral("Tolerance"), 0, 255, 0, 1, {QStringLiteral("bucket")});
    addCheck(QStringLiteral("moveToolAutoSelect"), QStringLiteral("Auto Select"), {QStringLiteral("move")});
    addCheck(QStringLiteral("automaticSnapping"), QStringLiteral("Automatic snapping"),
             {QStringLiteral("move"), QStringLiteral("transform")});
    const QStringList selectionTools{QStringLiteral("select"), QStringLiteral("marquee")};
    addCheck(QStringLiteral("selectionFlashEnabled"), QStringLiteral("Selection Flash"), selectionTools);
    addCheck(QStringLiteral("selectShapesByBoundingBox"), QStringLiteral("Pick by bounding box"), selectionTools);
    addCheck(QStringLiteral("showSelectedShapeOutlines"), QStringLiteral("Selected outlines"), selectionTools);
    addCheck(QStringLiteral("relativeMode"), QStringLiteral("Relative Transform"), {QStringLiteral("transform")});
    addCheck(QStringLiteral("displayAnchorsDuringTransformDrag"), QStringLiteral("Show handles while dragging"), {QStringLiteral("transform")});
    addChoice(QStringLiteral("contourFillMode"), QStringLiteral("Mode"), {QStringLiteral("Analytical"), QStringLiteral("Compact")}, {QStringLiteral("pen")});
    addCheck(QStringLiteral("autoSavePenPaths"), QStringLiteral("Save Path"), {QStringLiteral("pen")});
    addNumber(QStringLiteral("liningWidth"), QStringLiteral("Width"), 0.1, 256, 2, 0.5, {QStringLiteral("lining")});
    addNumber(QStringLiteral("liningOutsideMargin"), QStringLiteral("Outside margin"), 0, 200, 0, 5, {QStringLiteral("lining")}, QStringLiteral("%"));
    addCheck(QStringLiteral("autoSaveLiningPaths"), QStringLiteral("Save Path"), {QStringLiteral("lining")});
    addCheck(QStringLiteral("pipetteAutoReturn"), QStringLiteral("Auto Return"), {QStringLiteral("pipette")});
    layout_->addStretch();
    setTool(QStringLiteral("select"));
}

QSize ToolSettingsWidget::sizeHint() const {
    QSize size = QWidget::sizeHint();
    const auto margins = layout_->contentsMargins();
    int height = 0;
    for (auto it = tools_.cbegin(); it != tools_.cend(); ++it) {
        height = std::max(height, it.key()->sizeHint().height());
    }
    size.setHeight(height + margins.top() + margins.bottom());

    return size;
}

QSize ToolSettingsWidget::minimumSizeHint() const {
    QSize size = QWidget::minimumSizeHint();
    size.setHeight(sizeHint().height());

    return size;
}

void ToolSettingsWidget::setTool(const QString &tool) {
    for (auto it = tools_.cbegin(); it != tools_.cend(); ++it) {
        it.key()->setVisible(it.value().contains(tool));
    }
}

void ToolSettingsWidget::setValue(const QString &key, const QVariant &value) {
    QWidget *field = fields_.value(key);
    if (field == nullptr) return;
    const QSignalBlocker blocker(field);
    if (auto *check = qobject_cast<QCheckBox *>(field)) check->setChecked(value.toBool());
    else if (auto *choice = qobject_cast<QComboBox *>(field)) choice->setCurrentIndex(value.toInt());
    else if (auto *number = qobject_cast<QDoubleSpinBox *>(field)) {
        number->setValue(value.toDouble());
        auto *slider = field->parentWidget()->findChild<QSlider *>();
        const QSignalBlocker sliderBlocker(slider);
        slider->setValue(qRound(number->value() * slider->property("valueScale").toDouble()));
    }
}

bool ToolSettingsWidget::eventFilter(QObject *watched, QEvent *event) {
    auto *number = qobject_cast<QDoubleSpinBox *>(watched);
    bool finished = event->type() == QEvent::Wheel;
    if (number && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            number->interpretText();
            event->accept();
            finishEditing();

            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonRelease) {
        if (number) {
            const auto *editor = number->findChild<QLineEdit *>();
            const auto *mouse = static_cast<QMouseEvent *>(event);
            finished = editor && !editor->geometry().contains(mouse->position().toPoint());
        } else {
            finished = qobject_cast<QSlider *>(watched) != nullptr;
        }
    }
    if (finished) {
        QTimer::singleShot(0, this, [this]() { finishEditing(); });
    }

    return QWidget::eventFilter(watched, event);
}

void ToolSettingsWidget::finishEditing() {
    if (editingFinished) editingFinished();
}

void ToolSettingsWidget::addCheck(const QString &key, const QString &text, const QStringList &tools) {
    auto *check = new QCheckBox(text, this);
    check->setFocusPolicy(Qt::NoFocus);
    registerField(key, check, check, tools);
    connect(check, &QCheckBox::toggled, this, [this, key](bool value) {
        if (optionChanged) optionChanged(key, value);
    });
    connect(check, &QCheckBox::clicked, this, [this]() { finishEditing(); });
}

void ToolSettingsWidget::addChoice(const QString &key, const QString &text, const QStringList &choices, const QStringList &tools) {
    auto *container = new QWidget(this);
    auto *row = new QHBoxLayout(container);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kFieldSpacing);
    row->addWidget(new QLabel(text, container));
    auto *choice = new QComboBox(container);
    choice->addItems(choices);
    choice->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    choice->setFocusPolicy(Qt::NoFocus);
    choice->installEventFilter(this);
    row->addWidget(choice);
    registerField(key, choice, container, tools);
    connect(choice, &QComboBox::currentIndexChanged, this, [this, key](int value) {
        if (optionChanged) optionChanged(key, value);
    });
    connect(choice, &QComboBox::activated, this, [this]() { finishEditing(); });
}

void ToolSettingsWidget::addNumber(const QString &key, const QString &text, double minimum, double maximum,
                                   int decimals, double step, const QStringList &tools, const QString &suffix) {
    auto *container = new QWidget(this);
    auto *row = new QHBoxLayout(container);
    auto *slider = new QSlider(Qt::Horizontal, container);
    auto *number = new QDoubleSpinBox(container);
    const double scale = std::pow(10.0, decimals);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(kFieldSpacing);
    row->addWidget(new QLabel(text, container));
    slider->setObjectName(QStringLiteral("toolSettings.") + key + QStringLiteral(".slider"));
    slider->setProperty("valueScale", scale);
    slider->setRange(qRound(minimum * scale), qRound(maximum * scale));
    slider->setSingleStep(qRound(step * scale));
    slider->setFixedWidth(kSliderWidth);
    slider->setFocusPolicy(Qt::NoFocus);
    slider->installEventFilter(this);
    number->setDecimals(decimals);
    number->setRange(minimum, maximum);
    number->setSingleStep(step);
    number->setSuffix(suffix);
    number->setKeyboardTracking(false);
    number->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    number->setFocusPolicy(Qt::ClickFocus);
    number->installEventFilter(this);
    row->addWidget(slider);
    row->addWidget(number);
    registerField(key, number, container, tools);
    connect(slider, &QSlider::valueChanged, number, [number, scale](int value) { number->setValue(value / scale); });
    connect(slider, &QSlider::sliderReleased, this, [this]() { finishEditing(); });
    connect(number, &QDoubleSpinBox::editingFinished, this, [this, number]() {
        if (number->hasFocus()) finishEditing();
    });
    connect(number, &QDoubleSpinBox::valueChanged, this, [this, key, slider, scale](double value) {
        const QSignalBlocker blocker(slider);
        slider->setValue(qRound(value * scale));
        if (optionChanged) optionChanged(key, value);
    });
}

void ToolSettingsWidget::registerField(const QString &key, QWidget *field, QWidget *container, const QStringList &tools) {
    field->setObjectName(QStringLiteral("toolSettings.") + key);
    fields_.insert(key, field);
    tools_.insert(container, tools);
    container->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    layout_->addWidget(container);
}

} // namespace gui
