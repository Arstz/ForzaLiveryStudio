#include "key_bindings.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QEvent>
#include <QHash>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QList>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <limits>

namespace gui {
namespace {

constexpr int kSequenceTimeoutMs = 1000;
constexpr int kMaximumSequenceLength = 4;

struct InteractionDefinition {
    KeyInteraction interaction;
    QKeySequence sequence;
};

QKeyCombination normalizedCombination(const QKeyEvent &event) {
    Qt::KeyboardModifiers modifiers = event.modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    Qt::Key key = static_cast<Qt::Key>(event.key());
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
        modifiers |= Qt::ShiftModifier;
    }

    return QKeyCombination(modifiers, key);
}

QKeySequence sequenceFromCombinations(const QVector<QKeyCombination> &combinations) {
    switch (combinations.size()) {
    case 1:
        return QKeySequence(combinations[0]);
    case 2:
        return QKeySequence(combinations[0], combinations[1]);
    case 3:
        return QKeySequence(combinations[0], combinations[1], combinations[2]);
    case 4:
        return QKeySequence(combinations[0], combinations[1], combinations[2], combinations[3]);
    default:
        return {};
    }
}

const QVector<InteractionDefinition> &interactionDefinitions() {
    static const QVector<InteractionDefinition> definitions = {
        {KeyInteraction::CancelActiveFill, QKeySequence(Qt::Key_Escape)},
        {KeyInteraction::CanvasPan, QKeySequence(Qt::Key_Space)},
        {KeyInteraction::CanvasRemovePathPoint, QKeySequence(Qt::Key_Backspace)},
        {KeyInteraction::CanvasCancelInteraction, QKeySequence(Qt::Key_Escape)},
        {KeyInteraction::CanvasCommitInteraction, QKeySequence(Qt::Key_Return)},
        {KeyInteraction::CanvasCommitInteraction, QKeySequence(Qt::Key_Enter)},
        {KeyInteraction::CanvasNudgeLeft, QKeySequence(Qt::Key_Left)},
        {KeyInteraction::CanvasNudgeRight, QKeySequence(Qt::Key_Right)},
        {KeyInteraction::CanvasNudgeUp, QKeySequence(Qt::Key_Up)},
        {KeyInteraction::CanvasNudgeDown, QKeySequence(Qt::Key_Down)},
        {KeyInteraction::CanvasNudgeLeftFast, QKeySequence(Qt::ShiftModifier | Qt::Key_Left)},
        {KeyInteraction::CanvasNudgeRightFast, QKeySequence(Qt::ShiftModifier | Qt::Key_Right)},
        {KeyInteraction::CanvasNudgeUpFast, QKeySequence(Qt::ShiftModifier | Qt::Key_Up)},
        {KeyInteraction::CanvasNudgeDownFast, QKeySequence(Qt::ShiftModifier | Qt::Key_Down)},
        {KeyInteraction::PreviewCycleDebugMode, QKeySequence(Qt::Key_U)},
    };

    return definitions;
}

bool matchesInteraction(KeyInteraction interaction, const QKeySequence &pressed) {
    return std::any_of(interactionDefinitions().cbegin(), interactionDefinitions().cend(),
                       [interaction, &pressed](const InteractionDefinition &definition) {
        return definition.interaction == interaction
            && definition.sequence.matches(pressed) == QKeySequence::ExactMatch;
    });
}

bool shortcutAutoRepeats(const QString &id) {
    return id != QStringLiteral("flip_selection");
}

bool isTextInput(const QWidget *widget) {
    if (widget == nullptr) {
        return false;
    }
    if (qobject_cast<const QLineEdit *>(widget) != nullptr
        || qobject_cast<const QTextEdit *>(widget) != nullptr
        || qobject_cast<const QPlainTextEdit *>(widget) != nullptr
        || qobject_cast<const QAbstractSpinBox *>(widget) != nullptr
        || qobject_cast<const QKeySequenceEdit *>(widget) != nullptr) {
        return true;
    }
    const auto *combo = qobject_cast<const QComboBox *>(widget);

    return combo != nullptr && combo->isEditable();
}

bool isKeySequenceInput(const QWidget *widget) {
    for (const QWidget *current = widget; current != nullptr; current = current->parentWidget()) {
        if (qobject_cast<const QKeySequenceEdit *>(current) != nullptr) {
            return true;
        }
    }

    return false;
}

bool isUiNavigationKey(const QKeyEvent &event) {
    return event.key() == Qt::Key_Tab || event.key() == Qt::Key_Backtab;
}

bool isUiConfirmKey(const QKeyEvent &event) {
    return event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter;
}

bool textInputClaims(const QKeyEvent &event, const QWidget *focus) {
    if (!isTextInput(focus)) {
        return false;
    }
    if (qobject_cast<const QKeySequenceEdit *>(focus) != nullptr) {
        return true;
    }
    const Qt::KeyboardModifiers modifiers = event.modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier) {
        return true;
    }
    const QKeySequence pressed(normalizedCombination(event));
    static const QList<QKeySequence::StandardKey> editingKeys = {
        QKeySequence::Undo,
        QKeySequence::Redo,
        QKeySequence::Cut,
        QKeySequence::Copy,
        QKeySequence::Paste,
        QKeySequence::Delete,
        QKeySequence::SelectAll,
    };
    for (QKeySequence::StandardKey key : editingKeys) {
        const QList<QKeySequence> sequences = QKeySequence::keyBindings(key);
        if (std::any_of(sequences.cbegin(), sequences.cend(), [&pressed](const QKeySequence &sequence) {
                return sequence.matches(pressed) == QKeySequence::ExactMatch;
            })) {
            return true;
        }
    }

    return false;
}

}

QKeySequence defaultShortcut(const QString &id) {
    static const QHash<QString, QKeySequence> shortcuts = {
        {QStringLiteral("new_project"), QKeySequence(QKeySequence::New)},
        {QStringLiteral("open_project_json"), QKeySequence(QKeySequence::Open)},
        {QStringLiteral("save_project_json"), QKeySequence(QKeySequence::Save)},
        {QStringLiteral("save_project_json_as"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S)},
        {QStringLiteral("import"), {}},
        {QStringLiteral("import_car_model"), {}},
        {QStringLiteral("import_guide_layer"), {}},
        {QStringLiteral("export"), {}},
        {QStringLiteral("exit"), {}},
        {QStringLiteral("undo"), QKeySequence(QKeySequence::Undo)},
        {QStringLiteral("redo"), QKeySequence(QKeySequence::Redo)},
        {QStringLiteral("copy"), QKeySequence(QKeySequence::Copy)},
        {QStringLiteral("cut"), QKeySequence(QKeySequence::Cut)},
        {QStringLiteral("paste"), QKeySequence(QKeySequence::Paste)},
        {QStringLiteral("group_ungroup"), QKeySequence(Qt::CTRL | Qt::Key_G)},
        {QStringLiteral("ungroup_flat"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G)},
        {QStringLiteral("fold_all_groups"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E)},
        {QStringLiteral("delete_selected"), QKeySequence(Qt::Key_Delete)},
        {QStringLiteral("stamp"), QKeySequence(Qt::Key_Y)},
        {QStringLiteral("flip_selection"), QKeySequence(Qt::Key_Tab)},
        {QStringLiteral("center_view_on_selection"), QKeySequence(Qt::Key_F1)},
        {QStringLiteral("align_top"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Up)},
        {QStringLiteral("align_bottom"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Down)},
        {QStringLiteral("align_left"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Left)},
        {QStringLiteral("align_right"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Right)},
        {QStringLiteral("align_centre"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C)},
        {QStringLiteral("align_vertical_centre"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D)},
        {QStringLiteral("distribute_vertical"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V)},
        {QStringLiteral("distribute_horizontal"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H)},
        {QStringLiteral("set_target_car"), {}},
        {QStringLiteral("set_project_name"), {}},
        {QStringLiteral("set_creator_name"), {}},
        {QStringLiteral("preprocess_image"), {}},
        {QStringLiteral("create_regions"), {}},
        {QStringLiteral("fill_regions"), {}},
        {QStringLiteral("toggle_insert_last_color"), {}},
        {QStringLiteral("toggle_insert_last_scale"), {}},
        {QStringLiteral("toggle_property_debug"), {}},
        {QStringLiteral("toggle_move_auto_select"), {}},
        {QStringLiteral("toggle_allow_move_outside_bounding_box"), {}},
        {QStringLiteral("toggle_selection_flash"), QKeySequence(QStringLiteral("\\"))},
        {QStringLiteral("show_guidelines"), QKeySequence(Qt::CTRL | Qt::Key_Semicolon)},
        {QStringLiteral("toggle_guidelines_locked"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_Semicolon)},
        {QStringLiteral("delete_all_guidelines"), QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Semicolon)},
        {QStringLiteral("toggle_guide_layer_visibility"), {}},
        {QStringLiteral("toggle_guide_layers_on_top"), QKeySequence(Qt::Key_QuoteLeft)},
        {QStringLiteral("toggle_visibility_borders"), {}},
        {QStringLiteral("toggle_transform_relative"), {}},
        {QStringLiteral("toggle_car_uv_unwrap"), {}},
        {QStringLiteral("tool_select"), QKeySequence(Qt::Key_S)},
        {QStringLiteral("tool_move"), QKeySequence(Qt::Key_V)},
        {QStringLiteral("tool_marquee"), QKeySequence(Qt::Key_F)},
        {QStringLiteral("tool_transform"), QKeySequence(Qt::Key_T)},
        {QStringLiteral("tool_rotate"), QKeySequence(Qt::Key_R)},
        {QStringLiteral("tool_pipette"), QKeySequence(Qt::Key_I)},
        {QStringLiteral("tool_pen"), QKeySequence(Qt::Key_P)},
        {QStringLiteral("tool_lining"), QKeySequence(Qt::Key_L)},
        {QStringLiteral("tool_bucket"), QKeySequence(Qt::Key_B)},
        {QStringLiteral("place_text"), {}},
        {QStringLiteral("save_layout"), {}},
        {QStringLiteral("reset_layout"), {}},
        {QStringLiteral("settings"), QKeySequence(Qt::CTRL | Qt::Key_K)},
    };

    return shortcuts.value(id);
}

QString interactionShortcutText(KeyInteraction interaction) {
    QStringList shortcuts;
    for (const InteractionDefinition &definition : interactionDefinitions()) {
        if (definition.interaction == interaction) {
            shortcuts.push_back(definition.sequence.toString(QKeySequence::NativeText));
        }
    }

    return shortcuts.join(QStringLiteral(" / "));
}

std::optional<QKeySequence> capturedTabShortcut(const QKeyEvent &event) {
    const QKeyCombination combination = normalizedCombination(event);
    if (combination.key() != Qt::Key_Tab) {
        return std::nullopt;
    }

    return QKeySequence(combination);
}

KeyBindingRouter::KeyBindingRouter(QWidget *window, QObject *parent)
    : QObject(parent)
    , window_(window)
    , sequenceTimer_(new QTimer(this)) {
    sequenceTimer_->setSingleShot(true);
    sequenceTimer_->setInterval(kSequenceTimeoutMs);
    connect(sequenceTimer_, &QTimer::timeout, this, &KeyBindingRouter::clearPendingSequence);
    if (qApp != nullptr) {
        qApp->installEventFilter(this);
    }
}

KeyBindingRouter::~KeyBindingRouter() {
    if (qApp != nullptr) {
        qApp->removeEventFilter(this);
    }
}

void KeyBindingRouter::registerAction(const QString &id, QAction *action, const QKeySequence &sequence) {
    if (action == nullptr) {
        return;
    }
    action->setShortcut({});
    action->setAutoRepeat(shortcutAutoRepeats(id));
    actions_.push_back({id, action, sequence});
}

void KeyBindingRouter::setActionSequence(const QString &id, const QKeySequence &sequence) {
    for (ActionBinding &binding : actions_) {
        if (binding.id == id) {
            binding.sequence = sequence;
            break;
        }
    }
    clearPendingSequence();
}

void KeyBindingRouter::registerInteraction(KeyInteraction interaction,
                                           QWidget *owner,
                                           Scope scope,
                                           InteractionHandler handler,
                                           EnabledPredicate enabled,
                                           int priority,
                                           bool handleRelease) {
    interactions_.push_back({interaction, owner, scope, std::move(handler), std::move(enabled),
                             {}, priority, handleRelease, false});
}

bool KeyBindingRouter::eventFilter(QObject *watched, QEvent *event) {
    Q_UNUSED(watched);
    if (event == nullptr) {
        return false;
    }
    if (event->type() == QEvent::WindowDeactivate) {
        releaseActiveInteractions();
        clearPendingSequence();
        return false;
    }
    if (event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (!isKeySequenceInput(QApplication::focusWidget())
            && (isUiNavigationKey(*keyEvent) || isUiConfirmKey(*keyEvent))) {
            event->accept();
            return true;
        }
        return false;
    }
    if (event->type() == QEvent::KeyPress) {
        auto &keyEvent = *static_cast<QKeyEvent *>(event);
        if (routeKeyPress(keyEvent)) {
            return true;
        }
        QWidget *focus = QApplication::focusWidget();
        if (!isKeySequenceInput(focus) && isUiNavigationKey(keyEvent)) {
            keyEvent.accept();
            return true;
        }
        if (!isKeySequenceInput(focus) && !isTextInput(focus) && isUiConfirmKey(keyEvent)) {
            keyEvent.accept();
            return true;
        }
        return false;
    }
    if (event->type() == QEvent::KeyRelease) {
        auto &keyEvent = *static_cast<QKeyEvent *>(event);
        if (routeKeyRelease(keyEvent)) {
            return true;
        }
        QWidget *focus = QApplication::focusWidget();
        if (!isKeySequenceInput(focus)
            && (isUiNavigationKey(keyEvent) || (!isTextInput(focus) && isUiConfirmKey(keyEvent)))) {
            keyEvent.accept();
            return true;
        }
        return false;
    }

    return false;
}

bool KeyBindingRouter::routeKeyPress(QKeyEvent &event) {
    if (!windowCanReceiveBindings()) {
        clearPendingSequence();
        return false;
    }
    if (routeInteraction(event, KeyEventPhase::Press) || routeAction(event)) {
        event.accept();
        return true;
    }

    return false;
}

bool KeyBindingRouter::routeKeyRelease(QKeyEvent &event) {
    if (routeInteraction(event, KeyEventPhase::Release)) {
        event.accept();
        return true;
    }

    return false;
}

bool KeyBindingRouter::routeInteraction(QKeyEvent &event, KeyEventPhase phase) {
    const QKeySequence pressed(normalizedCombination(event));
    for (InteractionBinding &binding : interactions_) {
        if (!binding.activePress
            || binding.activeSequence.matches(pressed) != QKeySequence::ExactMatch) {
            continue;
        }
        if (phase == KeyEventPhase::Press) {
            binding.handler(binding.interaction, phase, event.isAutoRepeat());
        } else {
            binding.activePress = false;
            binding.activeSequence = {};
            if (binding.handleRelease) {
                binding.handler(binding.interaction, phase, event.isAutoRepeat());
            }
        }
        return true;
    }
    if (phase == KeyEventPhase::Release) {
        return false;
    }
    int bestIndex = -1;
    int bestPriority = std::numeric_limits<int>::min();
    for (int index = 0; index < interactions_.size(); ++index) {
        const InteractionBinding &binding = interactions_[index];
        if (!interactionHasFocus(binding) || binding.owner == nullptr || !binding.handler
            || (binding.enabled && !binding.enabled())
            || !matchesInteraction(binding.interaction, pressed)
            || binding.priority <= bestPriority) {
            continue;
        }
        bestIndex = index;
        bestPriority = binding.priority;
    }
    if (bestIndex < 0) {
        return false;
    }
    InteractionBinding &binding = interactions_[bestIndex];
    const bool handled = binding.handler(binding.interaction, phase, event.isAutoRepeat());
    binding.activePress = handled;
    binding.activeSequence = handled ? pressed : QKeySequence();

    return handled;
}

bool KeyBindingRouter::routeAction(QKeyEvent &event) {
    QWidget *focus = QApplication::focusWidget();
    if (textInputClaims(event, focus)) {
        clearPendingSequence();
        return false;
    }
    const QKeyCombination combination = normalizedCombination(event);
    if (combination.key() == Qt::Key_unknown) {
        return false;
    }
    pendingSequence_.push_back(combination);
    if (pendingSequence_.size() > kMaximumSequenceLength) {
        pendingSequence_.removeFirst();
    }
    const QKeySequence pressed = sequenceFromCombinations(pendingSequence_);
    bool partialMatch = false;
    for (const ActionBinding &binding : std::as_const(actions_)) {
        if (binding.action == nullptr || !binding.action->isEnabled() || binding.sequence.isEmpty()) {
            continue;
        }
        const QKeySequence::SequenceMatch match = binding.sequence.matches(pressed);
        if (match == QKeySequence::ExactMatch) {
            if (!event.isAutoRepeat() || binding.action->autoRepeat()) {
                binding.action->trigger();
            }
            clearPendingSequence();
            return true;
        }
        partialMatch = partialMatch || match == QKeySequence::PartialMatch;
    }
    if (partialMatch) {
        sequenceTimer_->start();
        return true;
    }
    if (pendingSequence_.size() > 1) {
        pendingSequence_.clear();
        return routeAction(event);
    }
    clearPendingSequence();

    return false;
}

bool KeyBindingRouter::interactionHasFocus(const InteractionBinding &binding) const {
    if (binding.scope == Scope::Window) {
        return windowCanReceiveBindings();
    }
    QWidget *focus = QApplication::focusWidget();

    return focus != nullptr && binding.owner != nullptr
        && (focus == binding.owner || binding.owner->isAncestorOf(focus));
}

bool KeyBindingRouter::windowCanReceiveBindings() const {
    if (window_ == nullptr || QApplication::activeModalWidget() != nullptr
        || QApplication::activePopupWidget() != nullptr) {
        return false;
    }
    QWidget *active = QApplication::activeWindow();
    if (active == window_) {
        return true;
    }
    if (qobject_cast<QDockWidget *>(active) == nullptr) {
        return false;
    }
    for (QWidget *widget = active; widget != nullptr; widget = widget->parentWidget()) {
        if (widget == window_) {
            return true;
        }
    }

    return false;
}

void KeyBindingRouter::releaseActiveInteractions() {
    for (InteractionBinding &binding : interactions_) {
        if (!binding.activePress) {
            continue;
        }
        binding.activePress = false;
        binding.activeSequence = {};
        if (binding.handleRelease && binding.owner != nullptr && binding.handler) {
            binding.handler(binding.interaction, KeyEventPhase::Release, false);
        }
    }
}

void KeyBindingRouter::clearPendingSequence() {
    pendingSequence_.clear();
    sequenceTimer_->stop();
}

}
