#pragma once

#include "core_types.h"
#include "scene_entry.h"

#include <QHash>
#include <QPoint>
#include <QPointF>
#include <QSet>
#include <QSizeF>
#include <QTransform>
#include <QWidget>

#include <array>
#include <functional>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace gui {

class EditorState;

class PropertyPanel final : public QWidget {
public:
    explicit PropertyPanel(EditorState *state, QWidget *parent = nullptr);

    void setLayers(const QVector<fh6::ShapeLayer *> &layers);
    void setSelection(const QVector<fh6::ShapeLayer *> &layers,
                      const QVector<fh6::GuideLayer *> &guides,
                      const QVector<fh6::LayerGroup *> &groups);
    void setDebugVisible(bool visible);
    // Sprite size lookup (shape id -> local size) so multi/group transforms can
    // pivot about the selection's true visual bounding box. Optional: without it
    // the pivot falls back to the bounding box of item positions.
    void setSpriteSizeFn(std::function<QSizeF(int)> fn);

    std::array<int, 3> flagCheckStates() const;

private:
    // Detach the project's containers and re-resolve the cached selection
    // pointers so edits made through them cannot rewrite a copy-on-write undo
    // snapshot. Must be called after EditorState::beginProjectEdit().
    void detachSelectionForEdit();

    QDoubleSpinBox *floatBox(double low, double high);
    // Shape and guide selections share one refresh path: EntryRef guards the
    // shape-only fields (shapeId/skew/mask/color).
    void setSingleEntry(const EntryRef &entry);
    void setMultipleEntries(const QVector<EntryRef> &entries);
    void clearMixedStyles();
    void applyChanged(QWidget *sender);
    bool beginValueLabelDrag(const QString &property, QDoubleSpinBox *box, const QPoint &globalPos);
    void updateValueLabelDrag(const QPoint &globalPos);
    void endValueLabelDrag(bool commit);
    void applySingle();
    void applyMulti(QWidget *sender, const QString &property);
    // True when the transform fields drive the whole selection as one unit (a group,
    // or 2+ loose shapes) rather than a single shape. Guides are never box selections.
    bool isBoxSelection() const;
    // Enable + reset the transform fields to their neutral box-proxy baseline
    // (x/y = box centre, scale = 1, rotation/skew = 0) for a box selection.
    void setBoxProxyFields();
    // Visual bounding-box centre of the current shape selection, in world units.
    QPointF selectionBoxCenter() const;
    // Apply a box-frame transform for property `property` changing from `fromValue`
    // to `toValue` (about the selection centre) to every selected shape.
    void applyBoxTransform(const QString &property, double fromValue, double toValue);
    void pickColor();
    void updateColorButton();
    QVector<std::array<quint8, 4>> selectionColors() const;

    EditorState *state_ = nullptr;
    std::function<QSizeF(int)> spriteSizeFn_;
    QVector<fh6::ShapeLayer *> layers_;
    QVector<fh6::GuideLayer *> guides_;
    QVector<fh6::LayerGroup *> groups_;
    bool loading_ = false;
    QHash<QWidget *, double> baselines_;
    bool valueLabelDragging_ = false;
    QString valueLabelProperty_;
    QDoubleSpinBox *valueLabelBox_ = nullptr;
    QPoint valueLabelStartGlobal_;
    double valueLabelBoxStartValue_ = 0.0;
    QHash<QString, double> valueLabelLayerStartValues_;
    QHash<QString, double> valueLabelGuideStartValues_;
    QHash<QString, double> valueLabelGroupStartValues_;
    // Box-transform label drag: each shape's local->world transform captured at
    // press, plus the pivot, so the whole selection transforms as a unit from a
    // fixed reference (mirrors the canvas group-drag math).
    bool valueLabelBoxDrag_ = false;
    QPointF valueLabelBoxCenter_;
    QHash<QString, QTransform> valueLabelLayerStartTransforms_;
    QSet<QString> valueLabelLayerIds_;
    QSet<QString> valueLabelGuideIds_;
    QVector<QString> valueLabelGroupIds_;

    QLineEdit *name_ = nullptr;
    QSpinBox *shapeId_ = nullptr;
    QDoubleSpinBox *x_ = nullptr;
    QDoubleSpinBox *y_ = nullptr;
    QDoubleSpinBox *scaleX_ = nullptr;
    QDoubleSpinBox *scaleY_ = nullptr;
    QDoubleSpinBox *rotation_ = nullptr;
    QDoubleSpinBox *skew_ = nullptr;
    QCheckBox *visible_ = nullptr;
    QCheckBox *locked_ = nullptr;
    QCheckBox *mask_ = nullptr;
    QDoubleSpinBox *opacity_ = nullptr;
    QPushButton *colorButton_ = nullptr;
    QWidget *debugLabel_ = nullptr;
    QLabel *debug_ = nullptr;
    bool debugVisible_ = false;
    QHash<QWidget *, QString> widgetProperties_;
};

} // namespace gui
