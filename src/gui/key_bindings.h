#pragma once

#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QVector>

#include <functional>
#include <optional>

class QAction;
class QEvent;
class QKeyEvent;
class QTimer;
class QWidget;

namespace gui {

constexpr int kOverrideKeyBindingPriority = 100;

enum class KeyInteraction {
    CancelActiveFill,
    CanvasPan,
    CanvasRemovePathPoint,
    CanvasCancelInteraction,
    CanvasCommitInteraction,
    CanvasNudgeLeft,
    CanvasNudgeRight,
    CanvasNudgeUp,
    CanvasNudgeDown,
    CanvasNudgeLeftFast,
    CanvasNudgeRightFast,
    CanvasNudgeUpFast,
    CanvasNudgeDownFast,
    PreviewCycleDebugMode,
};

enum class KeyEventPhase {
    Press,
    Release,
};

QKeySequence defaultShortcut(const QString &id);
QString interactionShortcutText(KeyInteraction interaction);
std::optional<QKeySequence> capturedTabShortcut(const QKeyEvent &event);

class KeyBindingRouter final : public QObject {
public:
    enum class Scope {
        Focus,
        Window,
    };

    using InteractionHandler = std::function<bool(KeyInteraction, KeyEventPhase, bool)>;
    using EnabledPredicate = std::function<bool()>;

    explicit KeyBindingRouter(QWidget *window, QObject *parent = nullptr);
    ~KeyBindingRouter() override;

    void registerAction(const QString &id, QAction *action, const QKeySequence &sequence);
    void setActionSequence(const QString &id, const QKeySequence &sequence);
    void registerInteraction(KeyInteraction interaction,
                             QWidget *owner,
                             Scope scope,
                             InteractionHandler handler,
                             EnabledPredicate enabled = {},
                             int priority = 0,
                             bool handleRelease = false);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct ActionBinding {
        QString id;
        QPointer<QAction> action;
        QKeySequence sequence;
    };

    struct InteractionBinding {
        KeyInteraction interaction = KeyInteraction::CanvasPan;
        QPointer<QWidget> owner;
        Scope scope = Scope::Focus;
        InteractionHandler handler;
        EnabledPredicate enabled;
        QKeySequence activeSequence;
        int priority = 0;
        bool handleRelease = false;
        bool activePress = false;
    };

    bool routeKeyPress(QKeyEvent &event);
    bool routeKeyRelease(QKeyEvent &event);
    bool routeInteraction(QKeyEvent &event, KeyEventPhase phase);
    bool routeAction(QKeyEvent &event);
    bool interactionHasFocus(const InteractionBinding &binding) const;
    bool windowCanReceiveBindings() const;
    void releaseActiveInteractions();
    void clearPendingSequence();

    QPointer<QWidget> window_;
    QVector<ActionBinding> actions_;
    QVector<InteractionBinding> interactions_;
    QVector<QKeyCombination> pendingSequence_;
    QTimer *sequenceTimer_ = nullptr;
};

}
