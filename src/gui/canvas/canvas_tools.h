#pragma once

#include <QCursor>
#include <QPointF>
#include <QString>

class QMouseEvent;
class QWheelEvent;

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

    virtual bool picksUnderCursor() const { return false; }

    virtual bool handlePress(QMouseEvent *event);

    virtual bool handleMove(QMouseEvent *event);

    virtual bool handleWheel(QWheelEvent *event);

    virtual void beginDrag(const QPointF &screenPos, const QPointF &boxCenterWorld);

    virtual bool handleRelease(QMouseEvent *event);

    virtual bool handleDoubleClick(QMouseEvent *event);

    virtual bool hoverCursor(const QPointF &point, QCursor *cursor) const;

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

class PenTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool handlePress(QMouseEvent *event) override;
    bool handleMove(QMouseEvent *event) override;
    bool handleRelease(QMouseEvent *event) override;
    bool handleDoubleClick(QMouseEvent *event) override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

class LiningTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool handlePress(QMouseEvent *event) override;
    bool handleMove(QMouseEvent *event) override;
    bool handleWheel(QWheelEvent *event) override;
    bool handleRelease(QMouseEvent *event) override;
    bool handleDoubleClick(QMouseEvent *event) override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

class BucketTool final : public CanvasTool {
public:
    using CanvasTool::CanvasTool;
    QString name() const override;
    bool handlePress(QMouseEvent *event) override;
    bool handleMove(QMouseEvent *event) override;
    bool handleWheel(QWheelEvent *event) override;
    Qt::CursorShape idleCursorShape(const QPointF &point) const override;
};

} // namespace gui
