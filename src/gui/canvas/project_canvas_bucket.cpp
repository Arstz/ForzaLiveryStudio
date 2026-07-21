#include "project_canvas.h"

#include "editor_state.h"
#include "project_canvas_internal.h"

#include <algorithm>
#include <cmath>

namespace gui {

using namespace pc_detail;

bool ProjectCanvas::bucketGuideContext(const QPointF &screenPoint,
                                       const fh6::scene::GuideLayer **guide,
                                       QTransform *guideWorld,
                                       QImage *image,
                                       QPoint *imagePoint,
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
    const fh6::scene::GuideLayer *foundGuide = nullptr;
    QTransform foundWorld;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &candidate,
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

    const QImage foundImage = guideImage(*foundGuide);
    if (foundImage.isNull()) {
        if (error != nullptr) {
            *error = QStringLiteral("The selected guide layer has no image");
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
    const double imageX = (local.x() + guideSize.width() * 0.5)
        * foundImage.width() / guideSize.width();
    const double imageY = (local.y() + guideSize.height() * 0.5)
        * foundImage.height() / guideSize.height();
    const QPoint pixel(static_cast<int>(std::floor(imageX)),
                       static_cast<int>(std::floor(imageY)));
    if (!QRect(QPoint(0, 0), foundImage.size()).contains(pixel)) {
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
    if (image != nullptr) {
        *image = foundImage;
    }
    if (imagePoint != nullptr) {
        *imagePoint = pixel;
    }
    return true;
}

bool ProjectCanvas::updateBucketPreview(const QPointF &screenPoint) {
    updateViewTransform();
    const fh6::scene::GuideLayer *guide = nullptr;
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

    if (bucket_.guideId == guide->id
        && bucket_.seedPixel == seed
        && bucket_.fill.valid()
        && bucket_.fill.imageSize == image.size()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       QStringLiteral("%1 pixels").arg(bucket_.fill.area)});
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
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       bucket_.fill.error});
        update();
        return false;
    }
    setCursorHint(screenPoint,
                  {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                   QStringLiteral("%1 pixels").arg(bucket_.fill.area)});
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

bool ProjectCanvas::commitBucketPreview(const QPointF &screenPoint) {
    if (!updateBucketPreview(screenPoint) || !bucket_.fill.valid()) {
        return false;
    }
    if (!pen_.points.isEmpty()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       QStringLiteral("Finish or cancel the existing Pen path first")});
        update();
        return false;
    }

    const fh6::scene::GuideLayer *guide = nullptr;
    QTransform guideWorld;
    QImage image;
    QPoint seed;
    QString error;
    if (!bucketGuideContext(screenPoint, &guide, &guideWorld, &image, &seed, &error)) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance), error});
        update();
        return false;
    }

    RegionExtractionParams traceParams;
    traceParams.traceSpeckle = 0;
    const QPainterPath traced = traceMaskToPath(bucket_.fill.mask,
                                                bucket_.fill.imageSize.width(),
                                                bucket_.fill.imageSize.height(),
                                                bucket_.fill.bounds,
                                                traceParams);
    if (traced.isEmpty()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       QStringLiteral("Potrace could not trace this region")});
        update();
        return false;
    }

    RegionPenConversionOptions conversionOptions;
    conversionOptions.comparisonImageSize = image.size();
    const RegionPenConversionResult conversion =
        regionOutlineToPenPoints(traced, conversionOptions);
    if (!conversion.valid()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       conversion.error.isEmpty()
                           ? QStringLiteral("The traced region is not a valid Pen contour")
                           : conversion.error});
        update();
        return false;
    }

    const QSizeF guideSize = sceneNodeSize(*guide, geometry_);
    QTransform imageToLocal;
    imageToLocal.translate(-guideSize.width() * 0.5, -guideSize.height() * 0.5);
    imageToLocal.scale(guideSize.width() / image.width(),
                       guideSize.height() / image.height());
    const QTransform imageToWorld = imageToLocal * guideWorld;

    QVector<PenPoint> worldPoints = conversion.points;
    for (PenPoint &point : worldPoints) {
        point.position = imageToWorld.map(point.position);
    }
    const PenContour worldContour = buildPenContour(worldPoints);
    if (!worldContour.valid()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucket_.tolerance),
                       worldContour.error.isEmpty()
                           ? QStringLiteral("The guide transform produced an invalid Pen contour")
                           : worldContour.error});
        update();
        return false;
    }

    pen_.points = std::move(worldPoints);
    normalizePenPointOrder();
    pen_.fillColor = bucket_.fill.averageColor;
    pen_.closed = true;
    pen_.hoverWorld = pen_.points.front().position;
    pen_.crossings.clear();
    pen_.error.clear();
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
    bucket_.sourceImage = {};
    bucket_.previewImage = {};
    clearCursorHint();
    update();
}

void ProjectCanvas::drawBucketOverlay(QPainter &painter) {
    if (tool_ != QStringLiteral("bucket")
        || bucket_.guideId.isEmpty()
        || bucket_.previewImage.isNull()
        || state_ == nullptr
        || !state_->selectedGuideLayerIds().contains(bucket_.guideId)) {
        return;
    }

    const fh6::scene::GuideLayer *guide = nullptr;
    QTransform guideWorld;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &candidate,
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
    QTransform imageToLocal;
    imageToLocal.translate(-guideSize.width() * 0.5, -guideSize.height() * 0.5);
    imageToLocal.scale(guideSize.width() / bucket_.previewImage.width(),
                       guideSize.height() / bucket_.previewImage.height());

    painter.save();
    painter.setOpacity(1.0);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setTransform(imageToLocal * guideWorld * camera_.matrix(), false);
    painter.drawImage(QPointF(0.0, 0.0), bucket_.previewImage);
    painter.restore();
}

} // namespace gui
