#pragma once

#include "layer.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <array>
#include <functional>
#include <optional>

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

    void setLayers(const QVector<fh6::scene::Shape *> &layers);
    void setSelection(const QVector<fh6::scene::Shape *> &layers,
                      const QVector<fh6::scene::GuideLayer *> &guides,
                      const QVector<fh6::scene::Group *> &groups);
    void refreshTransformFields();
    void refreshTransformFieldsFromBox(const QPointF &center,
                                       double width,
                                       double height,
                                       const QTransform &boxFrame,
                                       bool valid);
    void setDebugVisible(bool visible);
    // Sprite size lookup (shape id -> local size) so multi/group transforms can
    // pivot about the selection's true visual bounding box. Optional: without it
    // the pivot falls back to the bounding box of item positions.
    void setSpriteSizeFn(std::function<QSizeF(int)> fn);
    void setShapeVisualBoundsFn(std::function<QRectF(int)> fn);
    void setGuideColorSampleFn(std::function<std::optional<QColor>()> fn);
    void setValueEditingWheelEnabled(bool enabled);
    std::optional<std::array<quint8, 4>> currentSelectionColor() const;
    void applyColorToSelection(const std::array<quint8, 4> &color);
    bool sampleGuideColorToSelection();

    std::array<int, 3> flagCheckStates() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Detach the project's containers and re-resolve the cached selection
    // pointers so edits made through them cannot rewrite a copy-on-write undo
    // snapshot. Must be called after EditorState::beginProjectEdit().
    void detachSelectionForEdit();
    void setSelectionByIds(const QSet<QString> &layerIds,
                           const QSet<QString> &guideIds,
                           const QVector<QString> &groupIds);

    QDoubleSpinBox *floatBox(double low, double high);
    // Shape and guide selections have separate single/multi refresh paths (guides
    // lack the shape-only fields: shapeId/skew/mask/color).
    void setSingleLayer(const fh6::scene::Shape *layer);
    void setSingleGuide(const fh6::scene::GuideLayer *guide);
    void setMultipleLayers(const QVector<fh6::scene::Shape *> &layers);
    void setMultipleGuides(const QVector<fh6::scene::GuideLayer *> &guides);
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
    void setBoxProxyFields(bool neutralTransformValues = true);
    // Visual bounding-box centre of the current shape selection, in world units.
    QPointF selectionBoxCenter() const;
    // Apply a box-frame transform for property `property` changing from `fromValue`
    // to `toValue` (about the selection centre) to every selected shape.
    void applyBoxTransform(const QString &property, double fromValue, double toValue);
    void pickColor();
    void updateColorButton();
    QVector<std::array<quint8, 4>> selectionColors() const;
    QVector<std::array<quint8, 4>> colorableSelectionColors() const;

    EditorState *state_ = nullptr;
    std::function<QSizeF(int)> spriteSizeFn_;
    std::function<QRectF(int)> shapeVisualBoundsFn_;
    std::function<std::optional<QColor>()> guideColorSampleFn_;
    QVector<fh6::scene::Shape *> layers_;
    QVector<fh6::scene::GuideLayer *> guides_;
    QVector<fh6::scene::Group *> groups_;
    bool loading_ = false;
    bool applyingChange_ = false;
    QHash<QWidget *, double> baselines_;
    QString boxProxySignature_;
    QRectF boxProxyBaseBounds_;
    double boxProxyBaseWidth_ = 0.0;
    double boxProxyBaseHeight_ = 0.0;
    double boxProxyBaseScaleX_ = 1.0;
    double boxProxyBaseScaleY_ = 1.0;
    double boxProxyBaseRotation_ = 0.0;
    double boxProxyBaseSkew_ = 0.0;
    bool boxProxyBaseValid_ = false;
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
    bool valueLabelUsesTransformCommand_ = false;
    QPointF valueLabelBoxCenter_;
    QHash<QString, QTransform> valueLabelLayerStartTransforms_;
    // Each selected group's own frame captured at drag start, so a box drag moves
    // the group frame from a fixed reference (mirrors the leaf start transforms).
    QHash<QString, QTransform> valueLabelGroupStartFrames_;
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
    bool valueEditingWheelEnabled_ = true;
    QHash<QWidget *, QString> widgetProperties_;
};

} // namespace gui
