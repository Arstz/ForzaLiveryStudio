#pragma once

#include <QImage>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

namespace gui {

struct PartRadialChoice {
    QString name;
    QImage preview;
    int optionId = -1;
    bool empty = false;
    bool preserveName = false;
};

class PartRadialSelector final : public QWidget {
    Q_OBJECT
public:
    using PreviewProvider = std::function<QImage(int)>;

    explicit PartRadialSelector(QWidget *parent = nullptr);
    void beginSelection(const QPoint &origin, const QString &title,
        QVector<PartRadialChoice> choices, int selectedOptionId, PreviewProvider previewProvider);
    void cancelSelection();
    bool isSelecting() const;

Q_SIGNALS:
    void optionSelected(int optionId);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void preparePage();
    void changePage(int direction);
    void updateHoveredChoice(const QPointF &position);
    void finishSelection(bool apply);
    int pageChoiceCount() const;

    QVector<PartRadialChoice> choices_;
    PreviewProvider previewProvider_;
    QPointer<QWidget> previousFocus_;
    QString title_;
    QPointF origin_;
    QPointF center_;
    QRectF pageCounterBounds_;
    int selectedOptionId_ = -1;
    int hoveredChoice_ = -1;
    int page_ = 0;
    bool selecting_ = false;
    bool keyboardSelection_ = false;
    qreal outerRadius_ = 0;
};

} // namespace gui
