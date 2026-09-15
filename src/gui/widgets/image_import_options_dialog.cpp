#include "image_import_options_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

const QString kSettingsPrefix = QStringLiteral("imageImport/");

QString key(const char *name) {
    return kSettingsPrefix + QLatin1String(name);
}

} // namespace

ImageImportOptions ImageImportOptions::load() {
    ImageImportOptions options;
    const RasterImportOptions defaults;
    QSettings settings;
    options.raster.alphaThreshold = std::clamp(
        settings.value(key("alphaThreshold"), defaults.alphaThreshold).toDouble(), 0.0, 1.0);
    options.raster.maximumColors = std::clamp(
        settings.value(key("maximumColors"), defaults.maximumColors).toInt(), 2, 64);
    options.raster.minimumRegionArea = std::clamp(
        settings.value(key("minimumRegionArea"), defaults.minimumRegionArea).toInt(), 1, 4096);
    options.raster.speckleSize = std::clamp(
        settings.value(key("speckleSize"), defaults.speckleSize).toInt(), 0, 64);
    options.raster.traceSmoothing = std::clamp(
        settings.value(key("traceSmoothing"), defaults.traceSmoothing).toDouble(), 0.0, 1.3334);
    options.raster.maximumDimension = std::clamp(
        settings.value(key("maximumDimension"), defaults.maximumDimension).toInt(), 0, 8192);
    options.raster.separateThinLines =
        settings.value(key("separateThinLines"), defaults.separateThinLines).toBool();
    options.raster.neighbourOverlap = std::clamp(
        settings.value(key("neighbourOverlap"), defaults.neighbourOverlap).toInt(), 0, 3);
    options.outlineSimplification = std::clamp(
        settings.value(key("outlineSimplification"), 0.0).toDouble(), 0.0, 10.0);
    return options;
}

void ImageImportOptions::save() const {
    QSettings settings;
    settings.setValue(key("alphaThreshold"), raster.alphaThreshold);
    settings.setValue(key("maximumColors"), raster.maximumColors);
    settings.setValue(key("minimumRegionArea"), raster.minimumRegionArea);
    settings.setValue(key("speckleSize"), raster.speckleSize);
    settings.setValue(key("traceSmoothing"), raster.traceSmoothing);
    settings.setValue(key("maximumDimension"), raster.maximumDimension);
    settings.setValue(key("separateThinLines"), raster.separateThinLines);
    settings.setValue(key("neighbourOverlap"), raster.neighbourOverlap);
    settings.setValue(key("outlineSimplification"), outlineSimplification);
}

ImageImportOptionsDialog::ImageImportOptionsDialog(const ImageImportOptions &options,
                                                   bool rasterSource,
                                                   const QSize &imageSize,
                                                   QWidget *parent)
    : QDialog(parent)
    , initial_(options) {
    setWindowTitle(QStringLiteral("Import Image as Shapes"));
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    if (rasterSource) {
        mode_ = new QComboBox(this);
        mode_->addItem(QStringLiteral("Colour regions"));
        mode_->addItem(QStringLiteral("Silhouette (not available yet)"));
        mode_->setCurrentIndex(0);
        auto *model = qobject_cast<QStandardItemModel *>(mode_->model());
        if (model != nullptr && model->item(1) != nullptr) {
            model->item(1)->setEnabled(false);
        }
        mode_->setToolTip(QStringLiteral(
            "Colour regions: one group of shapes per colour, transparent pixels left empty"));
        form->addRow(QStringLiteral("Mode"), mode_);

        alphaThreshold_ = new QSpinBox(this);
        alphaThreshold_->setRange(0, 100);
        alphaThreshold_->setSuffix(QStringLiteral(" %"));
        alphaThreshold_->setValue(qRound(options.raster.alphaThreshold * 100.0));
        alphaThreshold_->setToolTip(QStringLiteral(
            "Pixels less opaque than this are treated as empty"));
        form->addRow(QStringLiteral("Alpha threshold"), alphaThreshold_);

        maximumColors_ = new QSpinBox(this);
        maximumColors_->setRange(2, 64);
        maximumColors_->setValue(options.raster.maximumColors);
        maximumColors_->setToolTip(QStringLiteral(
            "Palette size the image is reduced to. Fewer colours fold anti-aliased edge "
            "blends into their neighbours instead of making slivers of their own"));
        form->addRow(QStringLiteral("Maximum colours"), maximumColors_);

        minimumRegionArea_ = new QSpinBox(this);
        minimumRegionArea_->setRange(1, 4096);
        minimumRegionArea_->setSuffix(QStringLiteral(" px"));
        minimumRegionArea_->setValue(options.raster.minimumRegionArea);
        minimumRegionArea_->setToolTip(QStringLiteral(
            "Regions smaller than this merge into the neighbouring region with the closest colour"));
        form->addRow(QStringLiteral("Minimum region area"), minimumRegionArea_);

        speckleSize_ = new QSpinBox(this);
        speckleSize_->setRange(0, 64);
        speckleSize_->setSuffix(QStringLiteral(" px"));
        speckleSize_->setValue(options.raster.speckleSize);
        speckleSize_->setToolTip(QStringLiteral(
            "Specks up to this many pixels are dropped while tracing a region's outline"));
        form->addRow(QStringLiteral("Speckle size"), speckleSize_);

        traceSmoothing_ = new QDoubleSpinBox(this);
        traceSmoothing_->setRange(0.0, 1.33);
        traceSmoothing_->setSingleStep(0.1);
        traceSmoothing_->setDecimals(2);
        traceSmoothing_->setValue(options.raster.traceSmoothing);
        traceSmoothing_->setToolTip(QStringLiteral(
            "How readily traced corners become curves; 0 keeps every corner sharp"));
        form->addRow(QStringLiteral("Trace smoothing"), traceSmoothing_);

        maximumDimension_ = new QSpinBox(this);
        maximumDimension_->setRange(0, 8192);
        maximumDimension_->setSpecialValueText(QStringLiteral("Full size"));
        maximumDimension_->setSuffix(QStringLiteral(" px"));
        maximumDimension_->setValue(options.raster.maximumDimension);
        maximumDimension_->setToolTip(QStringLiteral(
            "Longest side the image is processed at; larger images are scaled down first so "
            "big regions do not run out of fill time. Shapes are placed at the source size"));
        form->addRow(QStringLiteral("Process at most"), maximumDimension_);

        neighbourOverlap_ = new QSpinBox(this);
        neighbourOverlap_->setRange(0, 3);
        neighbourOverlap_->setSuffix(QStringLiteral(" px"));
        neighbourOverlap_->setValue(options.raster.neighbourOverlap);
        neighbourOverlap_->setToolTip(QStringLiteral(
            "How far each colour region extends underneath the smaller regions drawn on "
            "top of it, so adjacent shapes overlap instead of leaving a hairline seam. "
            "Nothing visible changes size. 0 traces regions exactly as extracted"));
        form->addRow(QStringLiteral("Overlap neighbours"), neighbourOverlap_);

        separateThinLines_ = new QCheckBox(QStringLiteral("Separate thin lines"), this);
        separateThinLines_->setChecked(options.raster.separateThinLines);
        separateThinLines_->setToolTip(QStringLiteral(
            "Give thin high-contrast strokes their own regions instead of the nearest colour's"));
        form->addRow(QString(), separateThinLines_);

        if (!imageSize.isEmpty()) {
            auto *sizeLabel = new QLabel(
                QStringLiteral("Image is %1 x %2 pixels").arg(imageSize.width()).arg(imageSize.height()),
                this);
            sizeLabel->setEnabled(false);
            form->addRow(QString(), sizeLabel);
        }
    }

    outlineSimplification_ = new QDoubleSpinBox(this);
    outlineSimplification_->setRange(0.0, 10.0);
    outlineSimplification_->setSingleStep(0.25);
    outlineSimplification_->setDecimals(2);
    outlineSimplification_->setSpecialValueText(QStringLiteral("Keep source curves"));
    outlineSimplification_->setSuffix(QStringLiteral(" px"));
    outlineSimplification_->setValue(options.outlineSimplification);
    outlineSimplification_->setToolTip(QStringLiteral(
        "Resample every outline to within this distance before fitting shapes: fewer shapes, "
        "less faithful curves. Keep source curves uses the outline exactly as drawn"));
    form->addRow(QStringLiteral("Outline simplification"), outlineSimplification_);

    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setMinimumWidth(380);
}

ImageImportOptions ImageImportOptionsDialog::options() const {
    ImageImportOptions result = initial_;
    if (alphaThreshold_ != nullptr) {
        result.raster.alphaThreshold = alphaThreshold_->value() / 100.0;
        result.raster.maximumColors = maximumColors_->value();
        result.raster.minimumRegionArea = minimumRegionArea_->value();
        result.raster.speckleSize = speckleSize_->value();
        result.raster.traceSmoothing = traceSmoothing_->value();
        result.raster.maximumDimension = maximumDimension_->value();
        result.raster.separateThinLines = separateThinLines_->isChecked();
        result.raster.neighbourOverlap = neighbourOverlap_->value();
    }
    result.outlineSimplification = outlineSimplification_->value();
    return result;
}

} // namespace gui
