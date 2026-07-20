#include "image_preprocess_dialog.h"

#include <QtCore>
#include <QtWidgets>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace gui {

class ImagePreviewWidget final : public QWidget {
public:
    using ViewChanged = std::function<void(double, const QPointF &, bool)>;
    using ColorPicked = std::function<void(const QColor &)>;

    explicit ImagePreviewWidget(ViewChanged viewChanged, ColorPicked colorPicked,
                                QWidget *parent = nullptr)
        : QWidget(parent)
        , viewChanged_(std::move(viewChanged))
        , colorPicked_(std::move(colorPicked))
    {
        setMinimumSize(280, 240);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setToolTip(QStringLiteral("Mouse wheel: zoom · Left/middle drag: pan · Double-click: fit"));
    }

    void setImage(const QImage &image)
    {
        image_ = image;
        update();
    }

    void setSharedView(double scale, const QPointF &center, bool centerValid)
    {
        scale_ = scale;
        center_ = center;
        centerValid_ = centerValid;
        update();
    }

    void setEyedropperActive(bool active)
    {
        eyedropperActive_ = active;
        setCursor(active ? Qt::CrossCursor : Qt::ArrowCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::Base));
        constexpr int tile = 12;
        const QColor light(205, 205, 205);
        const QColor dark(165, 165, 165);
        for (int y = 0; y < height(); y += tile) {
            for (int x = 0; x < width(); x += tile) {
                painter.fillRect(QRect(x, y, tile, tile), ((x / tile) + (y / tile)) % 2 == 0 ? light : dark);
            }
        }
        if (image_.isNull()) {
            painter.setPen(palette().color(QPalette::Text));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Processing preview…"));
            return;
        }
        const double scale = effectiveScale();
        const QPointF center = effectiveCenter();
        const QSizeF targetSize(image_.width() * scale, image_.height() * scale);
        const QPointF topLeft(width() * 0.5 - center.x() * scale,
                              height() * 0.5 - center.y() * scale);
        const QRectF target(topLeft, targetSize);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(target, image_);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(target.adjusted(0, 0, -1, -1));
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (image_.isNull()) {
            return;
        }
        const double oldScale = effectiveScale();
        const QPointF oldCenter = effectiveCenter();
        const QPointF widgetCenter(width() * 0.5, height() * 0.5);
        const QPointF imageUnderCursor = oldCenter + (event->position() - widgetCenter) / oldScale;
        const double steps = event->angleDelta().y() / 120.0;
        const double newScale = std::clamp(oldScale * std::pow(1.18, steps), 0.02, 32.0);
        const QPointF newCenter = imageUnderCursor - (event->position() - widgetCenter) / newScale;
        viewChanged_(newScale, newCenter, true);
        event->accept();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (eyedropperActive_ && event->button() == Qt::LeftButton && !image_.isNull()) {
            const double scale = effectiveScale();
            const QPointF imagePoint = effectiveCenter()
                + (event->position() - QPointF(width() * 0.5, height() * 0.5)) / scale;
            if (imagePoint.x() >= 0.0 && imagePoint.y() >= 0.0
                && imagePoint.x() < image_.width() && imagePoint.y() < image_.height()) {
                colorPicked_(image_.pixelColor(qFloor(imagePoint.x()), qFloor(imagePoint.y())));
            }
            event->accept();
            return;
        }
        if ((event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) && !image_.isNull()) {
            panning_ = true;
            panLast_ = event->position();
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (panning_) {
            const double scale = effectiveScale();
            const QPointF center = effectiveCenter() - (event->position() - panLast_) / scale;
            panLast_ = event->position();
            viewChanged_(scale, center, true);
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (panning_ && (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton)) {
            panning_ = false;
            setCursor(eyedropperActive_ ? Qt::CrossCursor : Qt::ArrowCursor);
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            viewChanged_(0.0, {}, false);
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    double fitScale() const
    {
        if (image_.isNull()) {
            return 1.0;
        }
        return std::max(0.001, std::min(std::max(1, width() - 16) / static_cast<double>(image_.width()),
                                       std::max(1, height() - 16) / static_cast<double>(image_.height())));
    }

    double effectiveScale() const
    {
        return scale_ > 0.0 ? scale_ : fitScale();
    }

    QPointF effectiveCenter() const
    {
        return centerValid_ ? center_ : QPointF(image_.width() * 0.5, image_.height() * 0.5);
    }

    QImage image_;
    ViewChanged viewChanged_;
    ColorPicked colorPicked_;
    double scale_ = 0.0;
    QPointF center_;
    bool centerValid_ = false;
    bool panning_ = false;
    bool eyedropperActive_ = false;
    QPointF panLast_;
};

namespace {

QSlider *addSlider(QFormLayout *form, const QString &label, int minimum, int maximum, int value,
                   const std::function<QString(int)> &format, const std::function<void()> &changed)
{
    auto *row = new QWidget(form->parentWidget());
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *slider = new QSlider(Qt::Horizontal, row);
    slider->setRange(minimum, maximum);
    slider->setValue(std::clamp(value, minimum, maximum));
    auto *valueLabel = new QLabel(format(slider->value()), row);
    valueLabel->setMinimumWidth(54);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(slider, 1);
    layout->addWidget(valueLabel);
    QObject::connect(slider, &QSlider::valueChanged, row, [valueLabel, format, changed](int current) {
        valueLabel->setText(format(current));
        changed();
    });
    form->addRow(label, row);
    return slider;
}

QString percentValue(int value)
{
    return QStringLiteral("%1%").arg(value);
}

ImagePreprocessResult preprocessSafely(const QImage &input, const ImagePreprocessSettings &settings)
{
    try {
        return preprocessImageDetailed(input, settings);
    } catch (...) {
        return {};
    }
}

QVector<QColor> uniqueOpaqueColors(const QVector<QColor> &colors)
{
    QVector<QColor> result;
    result.reserve(std::min(256, static_cast<int>(colors.size())));
    for (const QColor &input : colors) {
        if (!input.isValid()) {
            continue;
        }
        const QColor color(input.red(), input.green(), input.blue(), 255);
        const bool duplicate = std::any_of(result.cbegin(), result.cend(), [&](const QColor &existing) {
            return existing.rgb() == color.rgb();
        });
        if (!duplicate) {
            result.push_back(color);
            if (result.size() == 256) {
                break;
            }
        }
    }
    return result;
}

} // namespace

ImagePreprocessDialog::ImagePreprocessDialog(const QImage &source,
                                             const QVector<QColor> &projectSwatches,
                                             QWidget *parent)
    : QDialog(parent)
    , source_(source.convertToFormat(QImage::Format_ARGB32_Premultiplied))
    , projectSwatches_(uniqueOpaqueColors(projectSwatches))
{
    setWindowTitle(QStringLiteral("Preprocess Image"));
    resize(1100, 680);
    setMinimumSize(820, 560);

    // Guide pixels are stored in canvas/world orientation. The canvas' Y axis
    // flips them for display, so do the same in this conventional Qt dialog.
    // The accepted result is mirrored back before it leaves the dialog.
    displaySource_ = source_.mirrored(false, true);
    const int previewMaxSide = 720;
    previewSource_ = displaySource_.size().boundedTo(QSize(previewMaxSide, previewMaxSide)) == displaySource_.size()
        ? displaySource_
        : displaySource_.scaled(QSize(previewMaxSide, previewMaxSide), Qt::KeepAspectRatio, Qt::SmoothTransformation);

    auto *root = new QVBoxLayout(this);
    auto *content = new QSplitter(Qt::Horizontal, this);
    root->addWidget(content, 1);

    auto *images = new QWidget(content);
    auto *imagesLayout = new QVBoxLayout(images);
    auto *viewToolbar = new QHBoxLayout;
    viewToolbar->addStretch(1);
    auto *fitButton = new QPushButton(QStringLiteral("Fit"), images);
    auto *actualButton = new QPushButton(QStringLiteral("100%"), images);
    fitButton->setToolTip(QStringLiteral("Fit both images in their views"));
    actualButton->setToolTip(QStringLiteral("Show both preview images at one screen pixel per image pixel"));
    zoomLabel_ = new QLabel(QStringLiteral("Fit"), images);
    zoomLabel_->setMinimumWidth(52);
    zoomLabel_->setAlignment(Qt::AlignCenter);
    viewToolbar->addWidget(fitButton);
    viewToolbar->addWidget(actualButton);
    viewToolbar->addWidget(zoomLabel_);
    imagesLayout->addLayout(viewToolbar);
    auto *imageRow = new QWidget(images);
    auto *imageRowLayout = new QHBoxLayout(imageRow);
    imageRowLayout->setContentsMargins(0, 0, 0, 0);
    const auto viewChanged = [this](double scale, const QPointF &center, bool centerValid) {
        applySharedView(scale, center, centerValid);
    };
    const auto colorPicked = [this](const QColor &color) {
        setEyedropperActive(false);
        addPaletteColor(color);
    };
    auto makeImageColumn = [imageRow, viewChanged, colorPicked](const QString &title, ImagePreviewWidget **view) {
        auto *column = new QWidget(imageRow);
        auto *layout = new QVBoxLayout(column);
        auto *heading = new QLabel(title, column);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        heading->setAlignment(Qt::AlignCenter);
        *view = new ImagePreviewWidget(viewChanged, colorPicked, column);
        layout->addWidget(heading);
        layout->addWidget(*view, 1);
        return column;
    };
    imageRowLayout->addWidget(makeImageColumn(QStringLiteral("Original"), &originalView_), 1);
    imageRowLayout->addWidget(makeImageColumn(QStringLiteral("Preview"), &processedView_), 1);
    imagesLayout->addWidget(imageRow, 1);
    originalView_->setImage(previewSource_);
    applySharedView(0.0, {}, false);
    QObject::connect(fitButton, &QPushButton::clicked, this, [this]() {
        applySharedView(0.0, {}, false);
    });
    QObject::connect(actualButton, &QPushButton::clicked, this, [this]() {
        applySharedView(1.0, QPointF(previewSource_.width() * 0.5,
                                    previewSource_.height() * 0.5), true);
    });
    content->addWidget(images);

    settingsPanel_ = new QWidget(content);
    settingsPanel_->setMinimumWidth(310);
    settingsPanel_->setMaximumWidth(390);
    auto *settingsLayout = new QVBoxLayout(settingsPanel_);
    auto *mainForm = new QFormLayout;
    settingsLayout->addLayout(mainForm);
    const ImagePreprocessSettings defaults = ImagePreprocessSettings::animeDetail();
    const auto changed = [this]() { schedulePreview(); };
    colors_ = addSlider(mainForm, QStringLiteral("Colors"), 1, 256, defaults.colors,
                        [](int value) { return QString::number(value); }, changed);
    flattenStrength_ = addSlider(mainForm, QStringLiteral("Flatten"), 0, 100,
                                 qRound(defaults.flattenStrength * 100.0), percentValue, changed);
    saturationRestore_ = addSlider(mainForm, QStringLiteral("Saturation restore"), 0, 100,
                                   qRound(defaults.saturationRestore * 100.0), percentValue, changed);
    detailRestore_ = addSlider(mainForm, QStringLiteral("Detail restore"), 0, 100,
                               qRound(defaults.detailRestore * 100.0), percentValue, changed);

    advancedButton_ = new QToolButton(settingsPanel_);
    advancedButton_->setText(QStringLiteral("Advanced settings"));
    advancedButton_->setCheckable(true);
    advancedButton_->setChecked(false);
    advancedButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedButton_->setArrowType(Qt::RightArrow);
    settingsLayout->addWidget(advancedButton_);

    advancedPanel_ = new QWidget(settingsPanel_);
    auto *advancedForm = new QFormLayout(advancedPanel_);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    smoothingPasses_ = addSlider(advancedForm, QStringLiteral("Smoothing passes"), 0, 4,
                                 defaults.smoothingPasses, [](int value) { return QString::number(value); }, changed);
    smoothingDiameter_ = addSlider(advancedForm, QStringLiteral("Smoothing diameter"), 0, 7,
                                   defaults.smoothingDiameter / 2,
                                   [](int value) { return QString::number(value * 2 + 1); }, changed);
    sigmaColor_ = addSlider(advancedForm, QStringLiteral("Color sigma"), 1, 100,
                            qRound(defaults.sigmaColor), [](int value) { return QString::number(value); }, changed);
    sigmaSpace_ = addSlider(advancedForm, QStringLiteral("Spatial sigma"), 1, 100,
                            qRound(defaults.sigmaSpace), [](int value) { return QString::number(value); }, changed);
    saturationThreshold_ = addSlider(advancedForm, QStringLiteral("Saturation threshold"), 0, 255,
                                     defaults.saturationThreshold, [](int value) { return QString::number(value); }, changed);
    flattenRadius_ = addSlider(advancedForm, QStringLiteral("Flatten radius"), 0, 6,
                               defaults.flattenRadius, [](int value) { return QString::number(value); }, changed);
    detailRadius_ = addSlider(advancedForm, QStringLiteral("Detail radius"), 1, 8,
                              defaults.detailRadius, [](int value) { return QString::number(value); }, changed);
    quantizationIterations_ = addSlider(advancedForm, QStringLiteral("Palette iterations"), 1, 60,
                                        defaults.quantizationIterations,
                                        [](int value) { return QString::number(value); }, changed);
    minimumColorFraction_ = addSlider(
        advancedForm, QStringLiteral("Minimum color share"), 0, 500,
        qRound(defaults.minimumColorFraction * 10000.0),
        [](int value) { return QStringLiteral("%1%").arg(value / 100.0, 0, 'f', 2); }, changed);
    speckleSize_ = addSlider(advancedForm, QStringLiteral("Speckle size"), 0, 64,
                             defaults.speckleSize,
                             [](int value) { return QString::number(value); }, changed);
    advancedPanel_->hide();
    settingsLayout->addWidget(advancedPanel_);

    auto *paletteGroup = new QGroupBox(QStringLiteral("Palette"), settingsPanel_);
    auto *paletteLayout = new QVBoxLayout(paletteGroup);
    paletteModeLabel_ = new QLabel(QStringLiteral("Generated HSV palette"), paletteGroup);
    paletteModeLabel_->setWordWrap(true);
    paletteLayout->addWidget(paletteModeLabel_);
    paletteList_ = new QListWidget(paletteGroup);
    paletteList_->setViewMode(QListView::IconMode);
    paletteList_->setFlow(QListView::LeftToRight);
    paletteList_->setWrapping(true);
    paletteList_->setResizeMode(QListView::Adjust);
    paletteList_->setIconSize(QSize(24, 24));
    paletteList_->setGridSize(QSize(42, 42));
    paletteList_->setMaximumHeight(96);
    paletteLayout->addWidget(paletteList_);
    auto *paletteButtons = new QGridLayout;
    importSwatchesButton_ = new QPushButton(QStringLiteral("Import swatches"), paletteGroup);
    auto *chooseColorButton = new QPushButton(QStringLiteral("Choose…"), paletteGroup);
    eyedropperButton_ = new QPushButton(QStringLiteral("Pick from image"), paletteGroup);
    eyedropperButton_->setCheckable(true);
    auto *removeColorButton = new QPushButton(QStringLiteral("Remove"), paletteGroup);
    importSwatchesButton_->setEnabled(!projectSwatches_.isEmpty());
    if (projectSwatches_.isEmpty()) {
        importSwatchesButton_->setToolTip(QStringLiteral("This project has no swatches"));
    }
    paletteButtons->addWidget(importSwatchesButton_, 0, 0);
    paletteButtons->addWidget(chooseColorButton, 0, 1);
    paletteButtons->addWidget(eyedropperButton_, 1, 0);
    paletteButtons->addWidget(removeColorButton, 1, 1);
    paletteLayout->addLayout(paletteButtons);
    settingsLayout->addWidget(paletteGroup);
    settingsLayout->addStretch(1);
    QObject::connect(advancedButton_, &QToolButton::toggled, this, [this](bool shown) {
        advancedPanel_->setVisible(shown);
        advancedButton_->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    });
    QObject::connect(importSwatchesButton_, &QPushButton::clicked,
                     this, &ImagePreprocessDialog::importProjectSwatches);
    QObject::connect(chooseColorButton, &QPushButton::clicked,
                     this, &ImagePreprocessDialog::choosePaletteColor);
    QObject::connect(eyedropperButton_, &QPushButton::clicked,
                     this, &ImagePreprocessDialog::beginEyedropper);
    QObject::connect(removeColorButton, &QPushButton::clicked,
                     this, &ImagePreprocessDialog::removeSelectedPaletteColor);
    QObject::connect(colors_, &QSlider::valueChanged, this, [this](int value) {
        if (!fixedPalette_ && value < paletteColors_.size()) {
            colors_->setValue(paletteColors_.size());
        }
    });
    rebuildPaletteList();
    content->addWidget(settingsPanel_);
    content->setStretchFactor(0, 1);
    content->setStretchFactor(1, 0);

    statusLabel_ = new QLabel(QStringLiteral("Preparing preview…"), this);
    root->addWidget(statusLabel_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons_);
    QObject::connect(buttons_, &QDialogButtonBox::accepted, this, &ImagePreprocessDialog::beginFullResolutionProcessing);
    QObject::connect(buttons_, &QDialogButtonBox::rejected, this, &ImagePreprocessDialog::reject);

    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(140);
    QObject::connect(previewTimer_, &QTimer::timeout, this, &ImagePreprocessDialog::startPreview);
    schedulePreview();
}

QImage ImagePreprocessDialog::resultImage() const
{
    return result_;
}

int ImagePreprocessDialog::retainedColorCount() const
{
    return retainedColorCount_;
}

QVector<QColor> ImagePreprocessDialog::retainedPalette() const
{
    return retainedPalette_;
}

ImagePreprocessSettings ImagePreprocessDialog::selectedSettings() const
{
    ImagePreprocessSettings settings;
    settings.colors = colors_->value();
    settings.flattenStrength = flattenStrength_->value() / 100.0;
    settings.saturationRestore = saturationRestore_->value() / 100.0;
    settings.detailRestore = detailRestore_->value() / 100.0;
    settings.smoothingPasses = smoothingPasses_->value();
    settings.smoothingDiameter = smoothingDiameter_->value() * 2 + 1;
    settings.sigmaColor = sigmaColor_->value();
    settings.sigmaSpace = sigmaSpace_->value();
    settings.saturationThreshold = saturationThreshold_->value();
    settings.flattenRadius = flattenRadius_->value();
    settings.detailRadius = detailRadius_->value();
    settings.quantizationIterations = quantizationIterations_->value();
    settings.minimumColorFraction = minimumColorFraction_->value() / 10000.0;
    settings.speckleSize = speckleSize_->value();
    settings.paletteColors = paletteColors_;
    settings.fixedPalette = fixedPalette_;
    return settings;
}

void ImagePreprocessDialog::reject()
{
    if (!fullResolutionRunning_) {
        QDialog::reject();
    }
}

void ImagePreprocessDialog::schedulePreview()
{
    ++requestedGeneration_;
    if (previewTimer_ != nullptr) {
        previewTimer_->start();
    }
    if (!fullResolutionRunning_ && statusLabel_ != nullptr) {
        statusLabel_->setText(QStringLiteral("Preview queued…"));
    }
}

void ImagePreprocessDialog::startPreview()
{
    if (fullResolutionRunning_ || previewRunning_) {
        return;
    }
    runningGeneration_ = requestedGeneration_;
    previewRunning_ = true;
    const QImage input = previewSource_;
    const ImagePreprocessSettings settings = selectedSettings();
    const quint64 generation = runningGeneration_;
    const QPointer<ImagePreprocessDialog> guard(this);
    statusLabel_->setText(QStringLiteral("Updating preview…"));
    QThreadPool::globalInstance()->start([guard, input, settings, generation]() {
        const ImagePreprocessResult result = preprocessSafely(input, settings);
        if (guard != nullptr) {
            QMetaObject::invokeMethod(guard, [guard, generation, result]() {
                if (guard != nullptr) {
                    guard->finishPreview(generation, result);
                }
            }, Qt::QueuedConnection);
        }
    });
}

void ImagePreprocessDialog::finishPreview(quint64 generation, const ImagePreprocessResult &result)
{
    previewRunning_ = false;
    if (fullResolutionRunning_) {
        return;
    }
    if (generation == requestedGeneration_) {
        if (!result.image.isNull()) {
            processedView_->setImage(result.image);
            statusLabel_->setText(QStringLiteral("Preview uses %1 × %2 pixels and retains %3 colors. OK processes the original %4 × %5 image.")
                                      .arg(result.image.width()).arg(result.image.height())
                                      .arg(result.retainedColorCount)
                                      .arg(source_.width()).arg(source_.height()));
        } else {
            statusLabel_->setText(QStringLiteral("Could not generate the preview."));
        }
        return;
    }
    startPreview();
}

void ImagePreprocessDialog::beginFullResolutionProcessing()
{
    if (fullResolutionRunning_) {
        return;
    }
    fullResolutionRunning_ = true;
    previewTimer_->stop();
    setControlsEnabled(false);
    statusLabel_->setText(QStringLiteral("Processing the full-resolution image…"));
    const QImage input = displaySource_;
    const ImagePreprocessSettings settings = selectedSettings();
    const QPointer<ImagePreprocessDialog> guard(this);
    QThreadPool::globalInstance()->start([guard, input, settings]() {
        const ImagePreprocessResult result = preprocessSafely(input, settings);
        if (guard != nullptr) {
            QMetaObject::invokeMethod(guard, [guard, result]() {
                if (guard != nullptr) {
                    guard->finishFullResolutionProcessing(result);
                }
            }, Qt::QueuedConnection);
        }
    });
}

void ImagePreprocessDialog::finishFullResolutionProcessing(const ImagePreprocessResult &result)
{
    result_ = result.image.mirrored(false, true);
    retainedColorCount_ = result.retainedColorCount;
    retainedPalette_ = result.retainedPalette;
    if (result_.isNull()) {
        fullResolutionRunning_ = false;
        setControlsEnabled(true);
        statusLabel_->setText(QStringLiteral("Full-resolution processing failed."));
        return;
    }
    fullResolutionRunning_ = false;
    QDialog::accept();
}

void ImagePreprocessDialog::setControlsEnabled(bool enabled)
{
    settingsPanel_->setEnabled(enabled);
    if (QPushButton *ok = buttons_->button(QDialogButtonBox::Ok)) {
        ok->setEnabled(enabled);
    }
    if (QPushButton *cancel = buttons_->button(QDialogButtonBox::Cancel)) {
        cancel->setEnabled(enabled);
    }
}

void ImagePreprocessDialog::applySharedView(double scale, const QPointF &center, bool centerValid)
{
    viewScale_ = scale;
    viewCenter_ = center;
    viewCenterValid_ = centerValid;
    if (originalView_ != nullptr) {
        originalView_->setSharedView(viewScale_, viewCenter_, viewCenterValid_);
    }
    if (processedView_ != nullptr) {
        processedView_->setSharedView(viewScale_, viewCenter_, viewCenterValid_);
    }
    if (zoomLabel_ != nullptr) {
        zoomLabel_->setText(viewScale_ > 0.0
                                ? QStringLiteral("%1%").arg(qRound(viewScale_ * 100.0))
                                : QStringLiteral("Fit"));
    }
}

void ImagePreprocessDialog::importProjectSwatches()
{
    if (projectSwatches_.isEmpty()) {
        statusLabel_->setText(QStringLiteral("This project has no swatches to import."));
        return;
    }
    // Project swatches define fixed mode, so preserve all of them before any
    // manually locked additions if the 256-color image limit is reached.
    paletteColors_ = uniqueOpaqueColors(projectSwatches_ + paletteColors_);
    fixedPalette_ = true;
    setEyedropperActive(false);
    rebuildPaletteList();
    schedulePreview();
}

void ImagePreprocessDialog::choosePaletteColor()
{
    QColor initial = Qt::white;
    if (paletteList_ != nullptr && paletteList_->currentItem() != nullptr) {
        initial = paletteList_->currentItem()->data(Qt::UserRole).value<QColor>();
    }
    const QColor color = QColorDialog::getColor(initial, this, QStringLiteral("Choose Palette Color"));
    if (color.isValid()) {
        addPaletteColor(color);
    }
}

void ImagePreprocessDialog::beginEyedropper()
{
    const bool active = eyedropperButton_ != nullptr && eyedropperButton_->isChecked();
    setEyedropperActive(active);
    if (active) {
        statusLabel_->setText(QStringLiteral("Click either image to add that color to the palette."));
    }
}

void ImagePreprocessDialog::addPaletteColor(const QColor &input)
{
    if (!input.isValid()) {
        return;
    }
    const QColor color(input.red(), input.green(), input.blue(), 255);
    if (std::any_of(paletteColors_.cbegin(), paletteColors_.cend(), [&](const QColor &existing) {
            return existing.rgb() == color.rgb();
        })) {
        statusLabel_->setText(QStringLiteral("That color is already in the preprocessing palette."));
        return;
    }
    if (paletteColors_.size() >= 256) {
        statusLabel_->setText(QStringLiteral("The preprocessing palette is limited to 256 colors."));
        return;
    }
    paletteColors_.push_back(color);
    if (!fixedPalette_ && colors_->value() < paletteColors_.size()) {
        colors_->setValue(paletteColors_.size());
    }
    rebuildPaletteList();
    schedulePreview();
}

void ImagePreprocessDialog::removeSelectedPaletteColor()
{
    if (paletteList_ == nullptr || paletteList_->currentRow() < 0) {
        statusLabel_->setText(QStringLiteral("Select a palette color to remove."));
        return;
    }
    if (fixedPalette_ && paletteColors_.size() <= 1) {
        statusLabel_->setText(QStringLiteral("A fixed palette must contain at least one color."));
        return;
    }
    paletteColors_.removeAt(paletteList_->currentRow());
    rebuildPaletteList();
    schedulePreview();
}

void ImagePreprocessDialog::rebuildPaletteList()
{
    if (paletteList_ == nullptr || colors_ == nullptr) {
        return;
    }
    paletteList_->clear();
    for (const QColor &color : paletteColors_) {
        QPixmap swatch(24, 24);
        swatch.fill(color);
        auto *item = new QListWidgetItem(QIcon(swatch), QString(), paletteList_);
        item->setData(Qt::UserRole, color);
        item->setToolTip(color.name(QColor::HexRgb));
    }
    if (fixedPalette_) {
        colors_->setValue(std::max(1, static_cast<int>(paletteColors_.size())));
        colors_->setEnabled(false);
        paletteModeLabel_->setText(QStringLiteral("Fixed palette: output uses only these %1 colors.")
                                       .arg(paletteColors_.size()));
    } else {
        colors_->setEnabled(true);
        if (colors_->value() < paletteColors_.size()) {
            colors_->setValue(paletteColors_.size());
        }
        paletteModeLabel_->setText(paletteColors_.isEmpty()
                                       ? QStringLiteral("Generated HSV palette")
                                       : QStringLiteral("%1 locked colors; HSV fills the remaining limit.")
                                             .arg(paletteColors_.size()));
    }
}

void ImagePreprocessDialog::setEyedropperActive(bool active)
{
    if (eyedropperButton_ != nullptr) {
        eyedropperButton_->setChecked(active);
    }
    if (originalView_ != nullptr) {
        originalView_->setEyedropperActive(active);
    }
    if (processedView_ != nullptr) {
        processedView_->setEyedropperActive(active);
    }
}

} // namespace gui
