#pragma once

// Per-tool interaction strategies for ProjectCanvas. The canvas keeps the
// shared drag machinery (pan, selection resolution, transform-command
// lifecycle, drag-start capture, per-dragMode move dispatch) and delegates the
// tool-specific decisions to the active tool: whether a press picks the item
// under the cursor, the press/release shortcuts for tools that need no
// selection, the drag-mode choice once a selection box exists, and hover/idle
// cursors. Tools hold no state of their own; all drag state stays on the
// canvas, so switching tools mid-session is free.

#include <QCursor>
#include <QPointF>
#include <QString>

class QMouseEvent;

namespace gui {

class ProjectCanvas;

class CanvasTool {
public:
    explicit CanvasTool(ProjectCanvas &canvas)
        : canvas_(canvas)
    {
    }
    virtual ~CanvasTool() = default;

    virtual QString name() const = 0;

    // True when a left press picks/keeps the item under the cursor and can
    // start a transform drag (move/transform/rotate).
    virtual bool picksUnderCursor() const { return false; }

    // Full-press shortcut for tools that resolve no selection on press.
    // Returns true when the press was consumed.
    virtual bool handlePress(QMouseEvent *event);

    // Chooses the drag mode (and captures rotate/scale references) once a
    // selection and its drag-start box exist. Leaves the mode None when the
    // press does not begin a drag.
    virtual void beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld);

    // Tool-specific release behaviour (select's click-selection, marquee's
    // rubber-band selection). Returns true when the release was consumed.
    virtual bool handleRelease(QMouseEvent *event);

    // Tool-specific hover cursor override; returns true and sets *cursor when
    // the tool decides, false to fall through to the shared cursor logic.
    virtual bool hoverCursor(const QPointF &point, QCursor *cursor) const;

    // Idle cursor shape used by the shared fallback when no drag is active.
    virtual Qt::CursorShape idleCursorShape(const QPointF &point) const;

protected:
    ProjectCanvas &canvas_;
};

class SelectTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool handlePress(QMouseEvent *event) override;
    bool handleRelease(QMouseEvent *event) override;
};

class MoveTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool picksUnderCursor() const override { return true; }
    void beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

class MarqueeTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool handlePress(QMouseEvent *event) override;
    bool handleRelease(QMouseEvent *event) override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

class TransformTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool picksUnderCursor() const override { return true; }
    void beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) override;
    bool hoverCursor(const QPointF &point, QCursor *cursor) const override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

class RotateTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool picksUnderCursor() const override { return true; }
    void beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld) override;
    bool hoverCursor(const QPointF &point, QCursor *cursor) const override;
};

class PipetteTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool handlePress(QMouseEvent *event) override;
    bool hoverCursor(const QPointF &point, QCursor *cursor) const override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

} // namespace gui
