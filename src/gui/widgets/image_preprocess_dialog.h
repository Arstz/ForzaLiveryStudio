#pragma once

#include "image_preprocessor.h"

#include <QDialog>
#include <QImage>

class QDialogButtonBox;
class QLabel;
class QSlider;
class QTimer;
class QToolButton;
class QWidget;

namespace gui {

class ImagePreviewWidget;

class ImagePreprocessDialog final : public QDialog {
public:
    explicit ImagePreprocessDialog(const QImage &source, QWidget *parent = nullptr);

    QImage resultImage() const;
    int retainedColorCount() const;
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

    QImage source_;
    QImage displaySource_;
    QImage previewSource_;
    QImage result_;
    int retainedColorCount_ = 0;
    ImagePreviewWidget *originalView_ = nullptr;
    ImagePreviewWidget *processedView_ = nullptr;
    QWidget *settingsPanel_ = nullptr;
    QWidget *advancedPanel_ = nullptr;
    QToolButton *advancedButton_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *zoomLabel_ = nullptr;
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

    double viewScale_ = 0.0;
    QPointF viewCenter_;
    bool viewCenterValid_ = false;

    quint64 requestedGeneration_ = 0;
    quint64 runningGeneration_ = 0;
    bool previewRunning_ = false;
    bool fullResolutionRunning_ = false;
};

} // namespace gui
