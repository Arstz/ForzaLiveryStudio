#pragma once

#include "image_preprocessor.h"

#include <QDialog>
#include <QImage>

class QDialogButtonBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;
class QWidget;

namespace gui {

class ImagePreviewWidget;

class ImagePreprocessDialog final : public QDialog {
public:
    explicit ImagePreprocessDialog(const QImage &source,
                                   const QVector<QColor> &projectSwatches = {},
                                   QWidget *parent = nullptr);

    QImage resultImage() const;
    int retainedColorCount() const;
    QVector<QColor> retainedPalette() const;
    ImagePreprocessSettings selectedSettings() const;

protected:
    void reject() override;

private:
    void schedulePreview();
    void startPreview();
    void finishPreview(quint64 generation, const ImagePreprocessResult &result);
    void beginFullResolutionProcessing();
    void finishFullResolutionProcessing(const ImagePreprocessResult &result);
    void setControlsEnabled(bool enabled);
    void applySharedView(double scale, const QPointF &center, bool centerValid);
    void importProjectSwatches();
    void choosePaletteColor();
    void beginEyedropper();
    void addPaletteColor(const QColor &color);
    void removeSelectedPaletteColor();
    void chooseLineColor();
    void updateLineColorButton();
    void rebuildPaletteList();
    void setEyedropperActive(bool active);

    QImage source_;
    QImage displaySource_;
    QImage previewSource_;
    QImage result_;
    int retainedColorCount_ = 0;
    QVector<QColor> retainedPalette_;
    QVector<QColor> projectSwatches_;
    QVector<QColor> paletteColors_;
    bool fixedPalette_ = false;
    ImagePreviewWidget *originalView_ = nullptr;
    ImagePreviewWidget *processedView_ = nullptr;
    QWidget *settingsPanel_ = nullptr;
    QWidget *advancedPanel_ = nullptr;
    QToolButton *advancedButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
    QLabel *paletteModeLabel_ = nullptr;
    QListWidget *paletteList_ = nullptr;
    QPushButton *importSwatchesButton_ = nullptr;
    QPushButton *eyedropperButton_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    QTimer *previewTimer_ = nullptr;

    QSlider *colors_ = nullptr;
    QSlider *flattenStrength_ = nullptr;
    QSlider *saturationRestore_ = nullptr;
    QSlider *detailRestore_ = nullptr;
    QSlider *smoothingPasses_ = nullptr;
    QSlider *smoothingDiameter_ = nullptr;
    QSlider *sigmaColor_ = nullptr;
    QSlider *sigmaSpace_ = nullptr;
    QSlider *saturationThreshold_ = nullptr;
    QSlider *flattenRadius_ = nullptr;
    QSlider *detailRadius_ = nullptr;
    QSlider *quantizationIterations_ = nullptr;
    QSlider *minimumColorFraction_ = nullptr;
    QSlider *speckleSize_ = nullptr;
    QCheckBox *noDetailNearEdges_ = nullptr;
    QSlider *edgeCleanupPasses_ = nullptr;
    QComboBox *edgeCleanupWindow_ = nullptr;
    QCheckBox *forceFlatFills_ = nullptr;
    QSlider *flatFillMinimumArea_ = nullptr;
    QCheckBox *lineMode_ = nullptr;
    QPushButton *lineColorButton_ = nullptr;
    QColor lineColor_;

    double viewScale_ = 0.0;
    QPointF viewCenter_;
    bool viewCenterValid_ = false;

    quint64 requestedGeneration_ = 0;
    quint64 runningGeneration_ = 0;
    bool previewRunning_ = false;
    bool fullResolutionRunning_ = false;
};

} // namespace gui
