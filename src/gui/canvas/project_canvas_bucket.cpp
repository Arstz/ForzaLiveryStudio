#include "project_canvas.h"

#include "editor_state.h"
#include "image_io.h"
#include "project_canvas_internal.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

} // namespace

using namespace pc_detail;

bool ProjectCanvas::bucketGuideContext(const QPointF &screenPoint,
                                       const fls::scene::GuideLayer **guide,
                                       QTransform *guideWorld,
                                       QImage *image,
                                       QPoint *imagePoint,
                                       QString *error) const {
    QPointF local;
    if (!bucketGuideGeometryContext(screenPoint, guide, guideWorld, &local, error)) {
        return false;
    }
    const fls::scene::GuideLayer *foundGuide = guide != nullptr ? *guide : nullptr;
    if (foundGuide == nullptr) {
        return false;
    }
    const QImage foundImage = guideImage(*foundGuide);
    if (foundImage.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide layer has no image");
        }
        return false;
    }
    const QSizeF guideSize = sceneNodeSize(*foundGuide, geometry_);
    const QPointF imagePosition = pc_detail::guideLocalToImage(local,
                                                               foundImage.size(),
                                                               guideSize);
    const QPoint pixel(static_cast<int>(std::floor(imagePosition.x())),
                       static_cast<int>(std::floor(imagePosition.y())));
    if (!QRect(QPoint(0, 0), foundImage.size()).contains(pixel)) {
        if (error != nullptr) {
            *error = QStringLiteral("Hover inside the selected guide image");
        }
        return false;
    }
    if (image != nullptr) {
        *image = foundImage;
    }
    if (imagePoint != nullptr) {
        *imagePoint = pixel;
    }
    return true;
}

bool ProjectCanvas::bucketGuideGeometryContext(
    const QPointF &screenPoint,
    const fls::scene::GuideLayer **guide,
    QTransform *guideWorld,
    QPointF *guideLocalPoint,
    QString *error) const {
    if (state_ == nullptr || sceneTree() == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("Open a project and select one guide layer");
        }
        return false;
    }
    const QSet<QString> selected = state_->selectedGuideLayerIds();
    if (selected.size() != 1) {
        if (error != nullptr) {
            *error = selected.isEmpty()
                ? QStringLiteral("Select a guide layer first")
                : QStringLiteral("Select a single guide layer");
        }
        return false;
    }
    if (!options_.guideLayersVisible) {
        if (error != nullptr) {
            *error = QStringLiteral("Guide layers are hidden");
        }
        return false;
    }

    const QString selectedId = *selected.cbegin();
    const fls::scene::GuideLayer *foundGuide = nullptr;
    QTransform foundWorld;
    forEachSceneGuide([&](const fls::scene::GuideLayer &candidate,
                          const QTransform &world,
                          const QString &sectionGroupId) {
        if (candidate.id == selectedId && isSectionActive(sectionGroupId)) {
            foundGuide = &candidate;
            foundWorld = world;
            return false;
        }
        return true;
    }, false);
    if (foundGuide == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide is not in the active section");
        }
        return false;
    }
    if (!foundGuide->visible || foundGuide->opacity <= 0.0) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide layer is hidden");
        }
        return false;
    }

    if (foundGuide->image == nullptr
        || foundGuide->image->width <= 0
        || foundGuide->image->height <= 0) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide layer has invalid dimensions");
        }
        return false;
    }
    const QSizeF guideSize = sceneNodeSize(*foundGuide, geometry_);
    if (guideSize.width() <= 0.0 || guideSize.height() <= 0.0) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide layer has invalid dimensions");
        }
        return false;
    }

    const QTransform localToScreen = foundWorld * camera_.matrix();
    bool invertible = false;
    const QTransform screenToLocal = localToScreen.inverted(&invertible);
    if (!invertible) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide transform is not invertible");
        }
        return false;
    }
    const QPointF local = screenToLocal.map(screenPoint);
    if (!sceneLocalRect(guideSize).contains(local)) {
        if (error != nullptr) {
            *error = QStringLiteral("Hover inside the selected guide image");
        }
        return false;
    }

    if (guide != nullptr) {
        *guide = foundGuide;
    }
    if (guideWorld != nullptr) {
        *guideWorld = foundWorld;
    }
    if (guideLocalPoint != nullptr) {
        *guideLocalPoint = local;
    }
    return true;
}

bool ProjectCanvas::updateBucketPreview(const QPointF &screenPoint) {
    updateViewTransform();
    const fls::scene::GuideLayer *vectorGuide = nullptr;
    QPointF vectorLocal;
    QString vectorContextError;
    if (bucketGuideGeometryContext(screenPoint,
                                   &vectorGuide,
                                   nullptr,
                                   &vectorLocal,
                                   &vectorContextError)
        && vectorGuide->image != nullptr
        && isSvgGuideFormat(vectorGuide->image->format)) {
        const auto *source = vectorGuide->image.get();
        const quint64 sourceHash = static_cast<quint64>(qHash(source->encoded));
        if (bucket_.vectorSourceGuideId != vectorGuide->id
            || bucket_.vectorSourceHash != sourceHash) {
            bucket_.vectorSourceGuideId = vectorGuide->id;
            bucket_.vectorSourceHash = sourceHash;
            bucket_.vectorDocument = extractSvgVectorObjects(
                source->encoded, QSize(source->width, source->height));
        }
        bucket_.vectorFallbackReason = bucket_.vectorDocument.fallbackReason;
        if (bucket_.vectorDocument.supportsObjectSelection()) {
            const QSize sourceSize(source->width, source->height);
            const QSizeF guideSize = sceneNodeSize(*vectorGuide, geometry_);
            QPointF sourcePoint = pc_detail::guideLocalToImage(
                vectorLocal, sourceSize, guideSize);
            if (!vectorGuide->imageTopDown) {
                sourcePoint.setY(sourceSize.height() - sourcePoint.y());
            }
            bucket_.vectorHit = svgVectorObjectAt(bucket_.vectorDocument, sourcePoint);
            bucket_.vectorMode = true;
            bucket_.guideId = vectorGuide->id;
            bucket_.seedPixel = QPoint(-1, -1);
            bucket_.fill = BucketFillResult{};
            bucket_.sourceImage = {};
            bucket_.previewImage = {};
            if (!bucket_.vectorHit.valid()) {
                setCursorHint(screenPoint,
                              {QStringLiteral("Vector object selection"),
                               QStringLiteral("No filled SVG object here")});
                update();
                return false;
            }
            setCursorHint(screenPoint,
                          {QStringLiteral("Vector object selection"),
                           QStringLiteral("Object %1 of %2")
                               .arg(bucket_.vectorHit.objectIndex + 1)
                               .arg(bucket_.vectorDocument.objects.size())});
            update();
            return true;
        }
    }

    bucket_.vectorMode = false;
    bucket_.vectorHit = SvgVectorObjectHit{};
    const fls::scene::GuideLayer *guide = nullptr;
    QImage image;
    QPoint seed;
    QString error;
    if (!bucketGuideContext(screenPoint, &guide, nullptr, &image, &seed, &error)) {
        bucket_.guideId.clear();
        bucket_.seedPixel = QPoint(-1, -1);
        bucket_.fill = BucketFillResult{};
        bucket_.previewImage = {};
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance), error});
        update();
        return false;
    }

    QStringList fallbackHint;
    fallbackHint.push_back(QStringLiteral("Tolerance: %1").arg(bucket_.tolerance));
    if (!bucket_.vectorFallbackReason.isEmpty()
        && guide->image != nullptr
        && isSvgGuideFormat(guide->image->format)) {
        fallbackHint.push_back(QStringLiteral("Raster fallback: %1")
                                   .arg(bucket_.vectorFallbackReason));
    }

    if (bucket_.guideId == guide->id
        && bucket_.seedPixel == seed
        && bucket_.fill.valid()
        && bucket_.fill.imageSize == image.size()) {
        fallbackHint.push_back(QStringLiteral("%1 pixels").arg(bucket_.fill.area));
        setCursorHint(screenPoint, fallbackHint);
        update();
        return true;
    }

    if (bucket_.sourceGuideId != guide->id
        || bucket_.sourceImage.size() != image.size()
        || bucket_.sourceImage.isNull()) {
        bucket_.sourceGuideId = guide->id;
        bucket_.sourceImage = image.convertToFormat(QImage::Format_ARGB32);
    }
    BucketFillResult fill = floodGuideRegion(bucket_.sourceImage, seed, bucket_.tolerance);
    bucket_.guideId = guide->id;
    bucket_.seedPixel = seed;
    bucket_.fill = std::move(fill);
    bucket_.previewImage = bucketMaskPreview(bucket_.fill);
    if (!bucket_.fill.valid()) {
        fallbackHint.push_back(bucket_.fill.error);
        setCursorHint(screenPoint, fallbackHint);
        update();
        return false;
    }
    fallbackHint.push_back(QStringLiteral("%1 pixels").arg(bucket_.fill.area));
    setCursorHint(screenPoint, fallbackHint);
    update();
    return true;
}

void ProjectCanvas::adjustBucketTolerance(int delta, const QPointF &screenPoint) {
    bucket_.tolerance = std::clamp(bucket_.tolerance + delta, 0, 255);
    bucket_.guideId.clear();
    bucket_.seedPixel = QPoint(-1, -1);
    bucket_.fill = BucketFillResult{};
    bucket_.previewImage = {};
    updateBucketPreview(screenPoint);
}

bool ProjectCanvas::commitBucketPreview(const QPointF &screenPoint,
                                        bool outlineOnly) {
    if (!updateBucketPreview(screenPoint)
        || (!bucket_.vectorMode && !bucket_.fill.valid())
        || (bucket_.vectorMode && !bucket_.vectorHit.valid())) {
        return false;
    }
    if (!pen_.points.isEmpty() || !pen_.cutouts.isEmpty()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       QStringLiteral("Finish or cancel the existing Pen path first")});
        update();
        return false;
    }

    const fls::scene::GuideLayer *guide = nullptr;
    QTransform guideWorld;
    QPointF local;
    QString error;
    if (!bucketGuideGeometryContext(screenPoint, &guide, &guideWorld, &local, &error)) {
        setCursorHint(screenPoint,
                      {bucket_.vectorMode
                           ? QStringLiteral("Vector object selection")
                           : QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       error});
        update();
        return false;
    }

    QPainterPath outline;
    QSize sourceSize;
    QColor fillColor;
    bool fillMask = false;
    RegionPenLoopConversionOptions conversionOptions;
    if (bucket_.vectorMode) {
        outline = bucket_.vectorHit.path;
        sourceSize = QSize(guide->image->width, guide->image->height);
        fillColor = bucket_.vectorHit.color;
        const double vectorScale = std::max(sourceSize.width(), sourceSize.height());
        conversionOptions.simplifyEpsilon = std::max(0.01, vectorScale / 2048.0);
        conversionOptions.minimumCurveBow = conversionOptions.simplifyEpsilon * 0.75;
    } else {
        QImage image;
        QPoint seed;
        if (!bucketGuideContext(screenPoint, &guide, &guideWorld, &image, &seed, &error)) {
            setCursorHint(screenPoint,
                          {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance), error});
            update();
            return false;
        }
        RegionExtractionParams traceParams;
        traceParams.traceSpeckle = 0;
        outline = traceMaskToPath(bucket_.fill.mask,
                                  bucket_.fill.imageSize.width(),
                                  bucket_.fill.imageSize.height(),
                                  bucket_.fill.bounds,
                                  traceParams);
        if (outline.isEmpty()) {
            setCursorHint(screenPoint,
                          {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                           QStringLiteral("Potrace could not trace this region")});
            update();
            return false;
        }
        sourceSize = image.size();
        fillColor = bucket_.fill.transparentTarget
            ? kTransparentBucketColor
            : bucket_.fill.averageColor;
        fillMask = bucket_.fill.transparentTarget;
        conversionOptions.fallback.comparisonImageSize = image.size();
        conversionOptions.discardedCutoutAreaCeiling = 5.0;
        conversionOptions.discardedCutoutBoundaryClearance = 2.0;
    }
    RegionPenLoopConversionResult conversion =
        regionOutlineToPenLoops(outline, conversionOptions);
    if (!conversion.valid()) {
        setCursorHint(screenPoint,
                      {bucket_.vectorMode
                           ? QStringLiteral("Vector object selection")
                           : QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       conversion.error.isEmpty()
                           ? QStringLiteral("The selected object is not a valid Pen contour")
                           : conversion.error});
        update();
        return false;
    }
    QVector<PenLoop> imageLoops = std::move(conversion.loops);
    if (outlineOnly && imageLoops.size() > 1) {
        imageLoops.resize(1);
    }

    const QSizeF guideSize = sceneNodeSize(*guide, geometry_);
    QTransform imageToWorld = pc_detail::guideImageToLocal(sourceSize, guideSize);
    if (bucket_.vectorMode && !guide->imageTopDown) {
        QTransform mirror;
        mirror.translate(0.0, sourceSize.height());
        mirror.scale(1.0, -1.0);
        imageToWorld = mirror * imageToWorld;
    }
    imageToWorld = imageToWorld
        * guideWorld;

    QVector<PenLoop> worldLoops = std::move(imageLoops);
    for (PenLoop &loop : worldLoops) {
        for (PenPoint &point : loop.points) {
            point.position = imageToWorld.map(point.position);
        }
    }
    const PenContour worldContour = buildPenContour(worldLoops);
    if (!worldContour.valid()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       worldContour.error.isEmpty()
                           ? QStringLiteral("The guide transform produced an invalid Pen contour")
                           : worldContour.error});
        update();
        return false;
    }

    beginPathEdit(pen_);
    pen_.points = std::move(worldLoops.front().points);
    pen_.cutouts.clear();
    for (int loopIndex = 1; loopIndex < worldLoops.size(); ++loopIndex) {
        pen_.cutouts.push_back(std::move(worldLoops[loopIndex].points));
    }
    pen_.activeCutout = pen_.cutouts.isEmpty() ? -1 : pen_.cutouts.size() - 1;
    pen_.cutoutClosed = true;
    normalizePenPointOrder();
    pen_.fillColor = fillColor;
    pen_.fillMask = fillMask;
    pen_.closed = true;
    pen_.hoverWorld = pen_.points.front().position;
    pen_.crossings.clear();
    pen_.error.clear();
    commitPathEdit(pen_);
    clearBucketPreview();
    setTool(QStringLiteral("pen"));
    validatePenInteraction();
    refreshPenInteractionHint(screenPoint, QGuiApplication::keyboardModifiers());
    return true;
}

void ProjectCanvas::clearBucketPreview() {
    bucket_.guideId.clear();
    bucket_.sourceGuideId.clear();
    bucket_.seedPixel = QPoint(-1, -1);
    bucket_.fill = BucketFillResult{};
    bucket_.vectorHit = SvgVectorObjectHit{};
    bucket_.vectorFallbackReason.clear();
    bucket_.sourceImage = {};
    bucket_.previewImage = {};
    bucket_.vectorMode = false;
    clearCursorHint();
    update();
}

void ProjectCanvas::drawBucketOverlay(QPainter &painter) {
    if (tool_ != QStringLiteral("bucket")
        || bucket_.guideId.isEmpty()
        || (!bucket_.vectorMode && bucket_.previewImage.isNull())
        || (bucket_.vectorMode && !bucket_.vectorHit.valid())
        || state_ == nullptr
        || !state_->selectedGuideLayerIds().contains(bucket_.guideId)) {
        return;
    }

    const fls::scene::GuideLayer *guide = nullptr;
    QTransform guideWorld;
    forEachSceneGuide([&](const fls::scene::GuideLayer &candidate,
                          const QTransform &world,
                          const QString &sectionGroupId) {
        if (candidate.id == bucket_.guideId && isSectionActive(sectionGroupId)) {
            guide = &candidate;
            guideWorld = world;
            return false;
        }
        return true;
    }, false);
    if (guide == nullptr || !guide->visible || guide->opacity <= 0.0) {
        return;
    }

    const QSizeF guideSize = sceneNodeSize(*guide, geometry_);

    painter.save();
    painter.setOpacity(1.0);
    if (bucket_.vectorMode && guide->image != nullptr) {
        const QSize sourceSize(guide->image->width, guide->image->height);
        QTransform sourceToLocal = pc_detail::guideImageToLocal(sourceSize, guideSize);
        if (!guide->imageTopDown) {
            QTransform mirror;
            mirror.translate(0.0, sourceSize.height());
            mirror.scale(1.0, -1.0);
            sourceToLocal = mirror * sourceToLocal;
        }
        painter.setTransform(sourceToLocal * guideWorld * camera_.matrix(), false);
        QColor previewColor = bucket_.vectorHit.color;
        previewColor.setAlpha(112);
        painter.setPen(Qt::NoPen);
        painter.setBrush(previewColor);
        painter.drawPath(bucket_.vectorHit.path);
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setTransform(pc_detail::guideImageToLocal(bucket_.previewImage.size(), guideSize)
                                 * guideWorld * camera_.matrix(),
                             false);
        painter.drawImage(QPointF(0.0, 0.0), bucket_.previewImage);
    }
    painter.restore();
}

} // namespace gui
