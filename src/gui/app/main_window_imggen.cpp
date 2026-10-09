#include "main_window.h"

#include "image_generator.h"

#include <algorithm>
#include <exception>

namespace gui {
namespace {

const QString kSettingsPrefix = QStringLiteral("imageGenerator/");
constexpr int kMinimumEvaluationBudget = 1000;
constexpr int kMaximumEvaluationBudget = 500000;
constexpr int kMaximumLiningSeconds = 600;
constexpr double kMaximumBoundaryAllowance = 5.0;

ImageGeneratorOptions loadGeneratorOptions() {
    QSettings settings;
    ImageGeneratorOptions options;
    auto value = [&](const QString &key, const QVariant &fallback) {
        return settings.value(kSettingsPrefix + key, fallback);
    };
    options.colors = std::clamp(value(QStringLiteral("colors"), options.colors).toInt(), 2, 256);
    options.fragmentTolerance = std::clamp(value(QStringLiteral("fragmentTolerance"), options.fragmentTolerance).toInt(), 0, 255);
    options.fragmentArea = std::clamp(value(QStringLiteral("fragmentArea"), options.fragmentArea).toInt(), 0, 512);
    options.bucketTolerance = std::clamp(value(QStringLiteral("bucketTolerance"), options.bucketTolerance).toInt(), 0, 255);
    options.evaluationBudget = std::clamp(value(QStringLiteral("evaluations"), options.evaluationBudget).toInt(),
                                         kMinimumEvaluationBudget, kMaximumEvaluationBudget);
    options.liningSeconds = std::clamp(value(QStringLiteral("liningSeconds"), options.liningSeconds).toInt(),
                                      1, kMaximumLiningSeconds);
    options.outlineExtension = std::clamp(value(QStringLiteral("extension"), options.outlineExtension).toDouble(),
                                         0.0, kMaximumImageGeneratorExtension);
    options.boundaryAllowance = std::clamp(value(QStringLiteral("allowance"), options.boundaryAllowance).toDouble(),
                                          0.0, kMaximumBoundaryAllowance);
    options.liningMode = static_cast<ImageLiningMode>(std::clamp(
        value(QStringLiteral("liningMode"), static_cast<int>(options.liningMode)).toInt(), 0, 2));
    options.cleanRasterNoise = value(QStringLiteral("cleanRasterNoise"), options.cleanRasterNoise).toBool();
    options.reducePalette = value(QStringLiteral("reducePalette"), options.reducePalette).toBool();
    options.isolateBackground = value(QStringLiteral("background"), options.isolateBackground).toBool();
    options.optimizeTopology = value(QStringLiteral("topology"), options.optimizeTopology).toBool();
    options.detectLining = value(QStringLiteral("detectLining"), options.detectLining).toBool();
    options.useGpu = value(QStringLiteral("gpu"), options.useGpu).toBool();

    return options;
}

void saveGeneratorOptions(const ImageGeneratorOptions &options) {
    QSettings settings;
    settings.setValue(kSettingsPrefix + QStringLiteral("colors"), options.colors);
    settings.setValue(kSettingsPrefix + QStringLiteral("bucketTolerance"), options.bucketTolerance);
    settings.setValue(kSettingsPrefix + QStringLiteral("fragmentArea"), options.fragmentArea);
    settings.setValue(kSettingsPrefix + QStringLiteral("fragmentTolerance"), options.fragmentTolerance);
    settings.setValue(kSettingsPrefix + QStringLiteral("evaluations"), options.evaluationBudget);
    settings.setValue(kSettingsPrefix + QStringLiteral("liningSeconds"), options.liningSeconds);
    settings.setValue(kSettingsPrefix + QStringLiteral("extension"), options.outlineExtension);
    settings.setValue(kSettingsPrefix + QStringLiteral("allowance"), options.boundaryAllowance);
    settings.setValue(kSettingsPrefix + QStringLiteral("liningMode"), static_cast<int>(options.liningMode));
    settings.setValue(kSettingsPrefix + QStringLiteral("reducePalette"), options.reducePalette);
    settings.setValue(kSettingsPrefix + QStringLiteral("cleanRasterNoise"), options.cleanRasterNoise);
    settings.setValue(kSettingsPrefix + QStringLiteral("background"), options.isolateBackground);
    settings.setValue(kSettingsPrefix + QStringLiteral("topology"), options.optimizeTopology);
    settings.setValue(kSettingsPrefix + QStringLiteral("detectLining"), options.detectLining);
    settings.setValue(kSettingsPrefix + QStringLiteral("gpu"), options.useGpu);
}

bool editGeneratorOptions(QWidget *parent, const QSize &sourceSize, ImageGeneratorOptions *options) {
    QDialog dialog(parent);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *noise = new QCheckBox(QStringLiteral("Clean raster noise before tracing"), &dialog);
    auto *palette = new QCheckBox(QStringLiteral("Reduce source palette"), &dialog);
    auto *colors = new QSpinBox(&dialog);
    auto *bucketTolerance = new QSpinBox(&dialog);
    auto *fragmentArea = new QSpinBox(&dialog);
    auto *fragmentTolerance = new QSpinBox(&dialog);
    auto *background = new QCheckBox(QStringLiteral("Keep single-color background in a separate group"), &dialog);
    auto *topology = new QCheckBox(QStringLiteral("Compare simpler contours and same-color merges"), &dialog);
    auto *detect = new QCheckBox(QStringLiteral("Detect lining from source"), &dialog);
    auto *mode = new QComboBox(&dialog);
    auto *extension = new QDoubleSpinBox(&dialog);
    auto *allowance = new QDoubleSpinBox(&dialog);
    auto *evaluations = new QSpinBox(&dialog);
    auto *liningSeconds = new QSpinBox(&dialog);
    auto *gpu = new QCheckBox(QStringLiteral("Use GPU when available"), &dialog);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    dialog.setWindowTitle(QStringLiteral("Image Generator"));
    layout->addWidget(new QLabel(QStringLiteral("Selected source: %1 x %2 pixels").arg(sourceSize.width())
        .arg(sourceSize.height()), &dialog));
    layout->addLayout(form);
    palette->setChecked(options->reducePalette);
    noise->setChecked(options->cleanRasterNoise);
    noise->setEnabled(palette->isChecked());
    QObject::connect(palette, &QCheckBox::toggled, noise, &QCheckBox::setEnabled);
    colors->setRange(2, 256);
    colors->setValue(options->colors);
    colors->setEnabled(palette->isChecked());
    QObject::connect(palette, &QCheckBox::toggled, colors, &QSpinBox::setEnabled);
    bucketTolerance->setRange(0, 255);
    bucketTolerance->setValue(options->bucketTolerance);
    bucketTolerance->setToolTip(QStringLiteral("Group connected colors using the same tolerance as Bucket selection"));
    fragmentArea->setRange(0, 512);
    fragmentArea->setValue(options->fragmentArea);
    fragmentArea->setSuffix(QStringLiteral(" source pixels"));
    fragmentArea->setToolTip(QStringLiteral("Merge small color fragments into similar adjacent regions; 0 disables fragment merging"));
    fragmentTolerance->setRange(0, 255);
    fragmentTolerance->setValue(options->fragmentTolerance);
    fragmentTolerance->setToolTip(QStringLiteral("Maximum RGB channel difference when merging a small fragment; larger differences keep isolated details"));
    background->setChecked(options->isolateBackground);
    topology->setChecked(options->optimizeTopology);
    detect->setChecked(options->detectLining);
    mode->addItems({QStringLiteral("Mixed bottom and top"), QStringLiteral("Bottom only"), QStringLiteral("Top only")});
    mode->setCurrentIndex(static_cast<int>(options->liningMode));
    mode->setEnabled(detect->isChecked());
    QObject::connect(detect, &QCheckBox::toggled, mode, &QComboBox::setEnabled);
    extension->setRange(0.0, kMaximumImageGeneratorExtension);
    extension->setSingleStep(0.5);
    extension->setSuffix(QStringLiteral(" source px"));
    extension->setValue(options->outlineExtension);
    allowance->setRange(0.0, kMaximumBoundaryAllowance);
    allowance->setSingleStep(0.25);
    allowance->setSuffix(QStringLiteral(" source px"));
    allowance->setValue(options->boundaryAllowance);
    evaluations->setRange(kMinimumEvaluationBudget, kMaximumEvaluationBudget);
    evaluations->setSingleStep(kMinimumEvaluationBudget);
    evaluations->setValue(options->evaluationBudget);
    liningSeconds->setRange(1, kMaximumLiningSeconds);
    liningSeconds->setSuffix(QStringLiteral(" s per region"));
    liningSeconds->setValue(options->liningSeconds);
    gpu->setChecked(options->useGpu);
    form->addRow(palette);
    form->addRow(noise);
    form->addRow(QStringLiteral("Palette colors"), colors);
    form->addRow(QStringLiteral("Bucket region tolerance"), bucketTolerance);
    form->addRow(QStringLiteral("Merge fragments below"), fragmentArea);
    form->addRow(QStringLiteral("Fragment color tolerance"), fragmentTolerance);
    form->addRow(background);
    form->addRow(topology);
    form->addRow(detect);
    form->addRow(QStringLiteral("Lining placement"), mode);
    form->addRow(QStringLiteral("Exterior lining extension"), extension);
    form->addRow(QStringLiteral("Compact Fit allowance"), allowance);
    form->addRow(QStringLiteral("Fit evaluation budget"), evaluations);
    form->addRow(QStringLiteral("Lining search limit"), liningSeconds);
    form->addRow(gpu);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Generate"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    options->colors = colors->value();
    options->bucketTolerance = bucketTolerance->value();
    options->fragmentArea = fragmentArea->value();
    options->fragmentTolerance = fragmentTolerance->value();
    options->reducePalette = palette->isChecked();
    options->cleanRasterNoise = noise->isChecked();
    options->isolateBackground = background->isChecked();
    options->optimizeTopology = topology->isChecked();
    options->detectLining = detect->isChecked();
    options->liningMode = static_cast<ImageLiningMode>(mode->currentIndex());
    options->outlineExtension = extension->value();
    options->boundaryAllowance = allowance->value();
    options->evaluationBudget = evaluations->value();
    options->liningSeconds = liningSeconds->value();
    options->useGpu = gpu->isChecked();
    saveGeneratorOptions(*options);

    return true;
}

} // namespace

void MainWindow::generateImageFromSelectedGuide() {
    ImageGeneratorRequest request;
    QTransform imageToWorld;
    QString error;
    const auto guides = state_->selectedGuideLayers();
    if (canvas_ == nullptr || !state_->hasProject() || guides.size() != 1) {
        statusBar()->showMessage(QStringLiteral("Select one source image guide to generate an image"), 5000);
        return;
    }
    const QString guideId = guides.front()->id;
    if (!canvas_->imageGeneratorSource(guideId, &request.source, &imageToWorld)) {
        statusBar()->showMessage(QStringLiteral("The selected source image is unavailable"), 5000);
        return;
    }
    request.options = loadGeneratorOptions();
    if (!editGeneratorOptions(this, request.source.size(), &request.options)) {
        return;
    }
    cancelActiveFills();
    request.primitives = canvas_->penPrimitiveCatalog();
    request.compactPrimitives = canvas_->compactFillPrimitives(&error);
    if (!error.isEmpty() || request.compactPrimitives.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Image Generator: %1").arg(error), 6000);
        return;
    }
    if (request.options.detectLining) {
        request.liningPrimitives = canvas_->thinFillPrimitives(&error);
        if (!error.isEmpty() || request.liningPrimitives.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Image Generator: %1").arg(error), 6000);
            return;
        }
    }
    const auto token = std::make_shared<std::atomic_bool>(false);
    const auto entries = selectedEntryIds();
    const QImage source = request.source;
    const quint64 generation = ++imageGeneratorGeneration_;
    imageGeneratorCancel_ = token;
    regionFillProgress_->setRange(0, 0);
    regionFillProgress_->setFormat(QStringLiteral("Image Generator..."));
    regionFillProgress_->show();
    statusBar()->showMessage(QStringLiteral("Generating image... Press %1 to cancel")
        .arg(interactionShortcutText(KeyInteraction::CancelActiveFill)));
    QPointer<MainWindow> guard(this);
    auto *task = QRunnable::create([guard, token, generation, entries, guideId, imageToWorld, source,
                                   request = std::move(request)]() mutable {
        ImageGeneratorResult result;
        try {
            result = generateImage(request, [guard, generation](const QString &phase, int done, int total) {
                if (guard.isNull()) {
                    return;
                }
                QMetaObject::invokeMethod(guard.data(), [guard, generation, phase, done, total]() {
                    if (guard.isNull() || generation != guard->imageGeneratorGeneration_
                        || guard->imageGeneratorCancel_ == nullptr) {
                        return;
                    }
                    guard->regionFillProgress_->setRange(0, std::max(0, total));
                    guard->regionFillProgress_->setValue(done);
                    guard->regionFillProgress_->setFormat(total > 0
                        ? phase + (phase == QStringLiteral("Finding Bucket regions")
                            ? QStringLiteral(" %p%") : QStringLiteral(" %v/%m")) : phase);
                }, Qt::QueuedConnection);
            }, [token]() { return token->load(std::memory_order_relaxed); });
        } catch (const std::exception &failure) {
            result.error = QString::fromUtf8(failure.what());
        }
        if (guard.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(guard.data(), [guard, token, generation, entries, guideId,
            imageToWorld, source, result = std::move(result)]() mutable {
            if (guard.isNull() || generation != guard->imageGeneratorGeneration_
                || guard->imageGeneratorCancel_ != token) {
                return;
            }
            guard->imageGeneratorCancel_.reset();
            guard->regionFillProgress_->hide();
            if (result.cancelled || token->load(std::memory_order_relaxed)) {
                guard->statusBar()->showMessage(QStringLiteral("Image generation cancelled"), 3000);
                return;
            }
            QImage currentSource;
            QTransform currentTransform;
            if (!guard->state_->hasProject()
                || !guard->canvas_->imageGeneratorSource(guideId, &currentSource, &currentTransform)
                || currentSource != source || currentTransform != imageToWorld) {
                guard->statusBar()->showMessage(QStringLiteral("Image generation discarded: source changed"), 5000);
                return;
            }
            QFile log(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("image_generator.log")));
            if (log.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                log.write(QJsonDocument(result.diagnostics).toJson(QJsonDocument::Indented));
            }
            if (!result.error.isEmpty()) {
                guard->statusBar()->showMessage(QStringLiteral("Image Generator: %1").arg(result.error), 7000);
                return;
            }
            const auto variants = imageGeneratorWorldVariants(result, imageToWorld);
            guard->insertGeneratedRegionVariants(QStringLiteral("Image Generator"), QStringLiteral("Image Generator"),
                variants, entries, {}, guideId);
            guard->statusBar()->showMessage(QStringLiteral("Generated %1 shapes in %2 s; coverage repairs: %3")
                .arg(result.diagnostics.value(QStringLiteral("shapes")).toInt())
                .arg(result.diagnostics.value(QStringLiteral("runtimeMs")).toDouble() / 1000.0, 0, 'f', 1)
                .arg(result.diagnostics.value(QStringLiteral("coverageRepairShapes")).toInt()), 7000);
        }, Qt::QueuedConnection);
    });
    QThreadPool::globalInstance()->start(task);
}

void MainWindow::cancelImageGeneration() {
    if (imageGeneratorCancel_ == nullptr) {
        return;
    }
    imageGeneratorCancel_->store(true, std::memory_order_relaxed);
    imageGeneratorCancel_.reset();
    ++imageGeneratorGeneration_;
    if (regionFillProgress_ != nullptr) {
        regionFillProgress_->hide();
    }
    statusBar()->showMessage(QStringLiteral("Image generation cancelled"), 2000);
}

} // namespace gui
