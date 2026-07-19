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
                                       QString *error) const
{
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
    if (!guideLayersVisible_) {
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

    const QTransform localToScreen = foundWorld * worldToScreen_;
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

bool ProjectCanvas::updateBucketPreview(const QPointF &screenPoint)
{
    updateViewTransform();
    const fh6::scene::GuideLayer *guide = nullptr;
    QImage image;
    QPoint seed;
    QString error;
    if (!bucketGuideContext(screenPoint, &guide, nullptr, &image, &seed, &error)) {
        bucketGuideId_.clear();
        bucketSeedPixel_ = QPoint(-1, -1);
        bucketFill_ = BucketFillResult{};
        bucketPreviewImage_ = {};
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_), error});
        update();
        return false;
    }

    if (bucketGuideId_ == guide->id
        && bucketSeedPixel_ == seed
        && bucketFill_.valid()
        && bucketFill_.imageSize == image.size()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
                       QStringLiteral("%1 pixels").arg(bucketFill_.area)});
        update();
        return true;
    }

    if (bucketSourceGuideId_ != guide->id
        || bucketSourceImage_.size() != image.size()
        || bucketSourceImage_.isNull()) {
        bucketSourceGuideId_ = guide->id;
        bucketSourceImage_ = image.convertToFormat(QImage::Format_ARGB32);
    }
    BucketFillResult fill = floodGuideRegion(bucketSourceImage_, seed, bucketTolerance_);
    bucketGuideId_ = guide->id;
    bucketSeedPixel_ = seed;
    bucketFill_ = std::move(fill);
    bucketPreviewImage_ = bucketMaskPreview(bucketFill_);
    if (!bucketFill_.valid()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
                       bucketFill_.error});
        update();
        return false;
    }
    setCursorHint(screenPoint,
                  {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
                   QStringLiteral("%1 pixels").arg(bucketFill_.area)});
    update();
    return true;
}

void ProjectCanvas::adjustBucketTolerance(int delta, const QPointF &screenPoint)
{
    bucketTolerance_ = std::clamp(bucketTolerance_ + delta, 0, 255);
    bucketGuideId_.clear();
    bucketSeedPixel_ = QPoint(-1, -1);
    bucketFill_ = BucketFillResult{};
    bucketPreviewImage_ = {};
    updateBucketPreview(screenPoint);
}

bool ProjectCanvas::commitBucketPreview(const QPointF &screenPoint)
{
    if (!updateBucketPreview(screenPoint) || !bucketFill_.valid()) {
        return false;
    }
    if (!penPoints_.isEmpty()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
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
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_), error});
        update();
        return false;
    }

    RegionExtractionParams traceParams;
    traceParams.traceSpeckle = 0;
    const QPainterPath traced = traceMaskToPath(bucketFill_.mask,
                                                bucketFill_.imageSize.width(),
                                                bucketFill_.imageSize.height(),
                                                bucketFill_.bounds,
                                                traceParams);
    if (traced.isEmpty()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
                       QStringLiteral("Potrace could not trace this region")});
        update();
        return false;
    }

    const RegionPenConversionResult conversion = regionOutlineToPenPoints(traced);
    if (!conversion.valid()) {
        setCursorHint(screenPoint,
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
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
                      {QStringLiteral("Tolerance: %1").arg(bucketTolerance_),
                       worldContour.error.isEmpty()
                           ? QStringLiteral("The guide transform produced an invalid Pen contour")
                           : worldContour.error});
        update();
        return false;
    }

    penPoints_ = std::move(worldPoints);
    normalizePenPointOrder();
    penFillColor_ = bucketFill_.averageColor;
    penLooped_ = true;
    penHoverWorld_ = penPoints_.front().position;
    penCrossings_.clear();
    penError_.clear();
    clearBucketPreview();
    setTool(QStringLiteral("pen"));
    validatePenInteraction();
    refreshPenInteractionHint(screenPoint, QGuiApplication::keyboardModifiers());
    return true;
}

void ProjectCanvas::clearBucketPreview()
{
    bucketGuideId_.clear();
    bucketSourceGuideId_.clear();
    bucketSeedPixel_ = QPoint(-1, -1);
    bucketFill_ = BucketFillResult{};
    bucketSourceImage_ = {};
    bucketPreviewImage_ = {};
    clearCursorHint();
    update();
}

void ProjectCanvas::drawBucketOverlay(QPainter &painter)
{
    if (tool_ != QStringLiteral("bucket")
        || bucketGuideId_.isEmpty()
        || bucketPreviewImage_.isNull()
        || state_ == nullptr
        || !state_->selectedGuideLayerIds().contains(bucketGuideId_)) {
        return;
    }

    const fh6::scene::GuideLayer *guide = nullptr;
    QTransform guideWorld;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &candidate,
                          const QTransform &world,
                          const QString &sectionGroupId) {
        if (candidate.id == bucketGuideId_ && isSectionActive(sectionGroupId)) {
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
    imageToLocal.scale(guideSize.width() / bucketPreviewImage_.width(),
                       guideSize.height() / bucketPreviewImage_.height());

    painter.save();
    painter.setOpacity(1.0);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.setTransform(imageToLocal * guideWorld * worldToScreen_, false);
    painter.drawImage(QPointF(0.0, 0.0), bucketPreviewImage_);
    painter.restore();
}

} // namespace gui
