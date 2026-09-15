#pragma once

#include "image_import_fill.h"

#include <QDialog>
#include <QSize>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;

namespace gui {

// Everything Import Image as Shapes asks before it runs. The raster options
// only matter for raster files; the outline simplification applies to SVG
// files as well. Values persist between sessions.
struct ImageImportOptions {
    RasterImportOptions raster;
    double outlineSimplification = 0.0;

    static ImageImportOptions load();
    void save() const;
};

class ImageImportOptionsDialog final : public QDialog {
public:
    ImageImportOptionsDialog(const ImageImportOptions &options,
                             bool rasterSource,
                             const QSize &imageSize,
                             QWidget *parent = nullptr);

    ImageImportOptions options() const;

private:
    QComboBox *mode_ = nullptr;
    QSpinBox *alphaThreshold_ = nullptr;
    QSpinBox *maximumColors_ = nullptr;
    QSpinBox *minimumRegionArea_ = nullptr;
    QSpinBox *speckleSize_ = nullptr;
    QDoubleSpinBox *traceSmoothing_ = nullptr;
    QSpinBox *maximumDimension_ = nullptr;
    QCheckBox *separateThinLines_ = nullptr;
    QSpinBox *neighbourOverlap_ = nullptr;
    QDoubleSpinBox *outlineSimplification_ = nullptr;
    ImageImportOptions initial_;
};

} // namespace gui
