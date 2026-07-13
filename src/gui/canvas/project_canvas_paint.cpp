#include "project_canvas.h"

#include "project_canvas_internal.h"

namespace gui {

using namespace pc_detail;

namespace {

const QColor SelectionAccentColor(255, 200, 50);
const QColor OverlayHaloColor(0, 0, 0);
const QColor SelectionFrameColor(255, 255, 255);
constexpr double HoverHaloWidth = 4.0;
constexpr double HoverAccentWidth = 2.0;
constexpr double SelectionFrameHaloWidth = 3.0;
constexpr double SelectionFrameLineWidth = 1.0;
constexpr double HandleBorderWidth = 2.0;
constexpr int MarqueeFillAlpha = 32;
const QColor VisibilityViewportColor(70, 170, 230, 190);
const QColor VisibilityLooseColor(255, 210, 80, 160);
constexpr double VisibilityBorderHaloWidth = 3.0;
constexpr double VisibilityBorderLineWidth = 1.0;
constexpr double PositionLimitHalfWidth1080p = 1024.0;
constexpr double PositionLimitHalfHeight1080p = 604.0;

const QColor CursorHintBorderColor(0, 0, 0, 180);
const QColor CursorHintFillColor(20, 20, 22, 210);
const QColor CursorHintTextColor(245, 246, 248);
constexpr double CursorHintCornerRadius = 4.0;
constexpr int CursorHintPaddingX = 8;
constexpr int CursorHintPaddingY = 6;
constexpr double CursorHintCursorOffset = 18.0;
constexpr double CursorHintScreenMargin = 4.0;

const QColor EmptyCanvasTextColor(190, 194, 201);

} // namespace


void ProjectCanvas::clearCursorHint()
{
    cursorHintLines_.clear();
}

void ProjectCanvas::setCursorHint(const QPointF &point, const QStringList &lines)
{
    cursorHintPoint_ = point;
    cursorHintLines_ = lines;
}

void ProjectCanvas::drawCursorHint(QPainter &painter)
{
    if (cursorHintLines_.isEmpty()) {
        return;
    }

    painter.save();
    painter.resetTransform();
    const QFontMetrics metrics(painter.font());
    int textWidth = 0;
    for (const QString &line : cursorHintLines_) {
        textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
    }
    const int paddingX = CursorHintPaddingX;
    const int paddingY = CursorHintPaddingY;
    const int lineHeight = metrics.height();
    const QSize hintSize(textWidth + paddingX * 2,
                         cursorHintLines_.size() * lineHeight + paddingY * 2);
    QPointF topLeft = cursorHintPoint_ + QPointF(CursorHintCursorOffset, CursorHintCursorOffset);
    topLeft.setX(std::min(topLeft.x(), static_cast<double>(width() - hintSize.width() - CursorHintScreenMargin)));
    topLeft.setY(std::min(topLeft.y(), static_cast<double>(height() - hintSize.height() - CursorHintScreenMargin)));
    topLeft.setX(std::max(topLeft.x(), CursorHintScreenMargin));
    topLeft.setY(std::max(topLeft.y(), CursorHintScreenMargin));
    topLeft.setX(std::round(topLeft.x()));
    topLeft.setY(std::round(topLeft.y()));

    const qreal dpr = devicePixelRatioF();
    QImage bubble(QSize(std::ceil(hintSize.width() * dpr), std::ceil(hintSize.height() * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    bubble.setDevicePixelRatio(dpr);
    bubble.fill(Qt::transparent);

    QPainter hintPainter(&bubble);
    hintPainter.setRenderHint(QPainter::Antialiasing, true);
    hintPainter.setPen(QPen(CursorHintBorderColor, 1));
    hintPainter.setBrush(CursorHintFillColor);
    hintPainter.drawRoundedRect(QRectF(QPointF(0.0, 0.0), hintSize), CursorHintCornerRadius, CursorHintCornerRadius);
    hintPainter.setPen(CursorHintTextColor);
    hintPainter.setRenderHint(QPainter::TextAntialiasing, true);
    int textY = paddingY;
    for (const QString &line : cursorHintLines_) {
        hintPainter.drawText(QRectF(paddingX, textY, textWidth, lineHeight), Qt::AlignLeft | Qt::AlignVCenter, line);
        textY += lineHeight;
    }
    hintPainter.end();

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(topLeft, bubble);
    painter.restore();
}

void ProjectCanvas::drawVisibilityBorders(QPainter &painter)
{
    if (project_ == nullptr || (!visibilityBordersEnabled_ && !positionLimitBorderEnabled_)) {
        return;
    }

    const auto drawWorldRect = [this, &painter](const QRectF &worldRect, const QColor &color, Qt::PenStyle style) {
        const QPolygonF polygon({
            worldToScreen(worldRect.topLeft()),
            worldToScreen(worldRect.topRight()),
            worldToScreen(worldRect.bottomRight()),
            worldToScreen(worldRect.bottomLeft()),
        });
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(OverlayHaloColor, VisibilityBorderHaloWidth, style));
        painter.drawPolygon(polygon);
        painter.setPen(QPen(color, VisibilityBorderLineWidth, style));
        painter.drawPolygon(polygon);
    };

    const QRectF viewportRect(-visibilityBorderResolution_.width() * 0.5,
                              -visibilityBorderResolution_.height() * 0.5,
                              visibilityBorderResolution_.width(),
                              visibilityBorderResolution_.height());
    if (visibilityBordersEnabled_) {
        drawWorldRect(viewportRect, VisibilityViewportColor, Qt::SolidLine);
    }
    if (positionLimitBorderEnabled_) {
        const double scale = static_cast<double>(visibilityBorderResolution_.height()) / 1080.0;
        const QRectF positionLimitRect(-PositionLimitHalfWidth1080p * scale,
                                       -PositionLimitHalfHeight1080p * scale,
                                       PositionLimitHalfWidth1080p * 2.0 * scale,
                                       PositionLimitHalfHeight1080p * 2.0 * scale);
        drawWorldRect(positionLimitRect, VisibilityLooseColor, Qt::DashLine);
    }
}

void ProjectCanvas::drawOverlay(QPainter &painter)
{
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (carUnwrapVisible_ && !carUnwrapOverlay_.isNull()) {
        const QTransform imageToWorld(1.0, 0.0, 0.0, 1.0, -1024.0, -512.0);
        painter.save();
        painter.setTransform(imageToWorld * worldToScreen_, false);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setOpacity(0.45);
        painter.drawImage(QPointF(0.0, 0.0), carUnwrapOverlay_);
        painter.restore();
    }

    drawVisibilityBorders(painter);

    if (!hoverPolygon_.isEmpty()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(OverlayHaloColor, HoverHaloWidth));
        painter.drawPolygon(hoverPolygon_);
        painter.setPen(QPen(SelectionAccentColor, HoverAccentWidth));
        painter.drawPolygon(hoverPolygon_);
    }

    const SelectionBox box = currentSelectionBox();
    if (box.valid && !box.localRect.isEmpty()) {
        const QTransform toScreen = boxToScreen(box);
        const QRectF &lr = box.localRect;
        const QPointF topLeft = toScreen.map(lr.topLeft());
        const QPointF topRight = toScreen.map(lr.topRight());
        const QPointF bottomRight = toScreen.map(lr.bottomRight());
        const QPointF bottomLeft = toScreen.map(lr.bottomLeft());
        const QPolygonF boxPolygon({topLeft, topRight, bottomRight, bottomLeft});

        if (!isTransformDrag()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(OverlayHaloColor, SelectionFrameHaloWidth));
            painter.drawPolygon(boxPolygon);
            painter.setPen(QPen(SelectionFrameColor, SelectionFrameLineWidth));
            painter.drawPolygon(boxPolygon);
        }
        if (tool_ == QStringLiteral("transform")) {
            QVector<QPointF> handles = {
                topLeft, topRight, bottomLeft, bottomRight,
                toScreen.map(QPointF(lr.left(), lr.center().y())),
                toScreen.map(QPointF(lr.right(), lr.center().y())),
                toScreen.map(QPointF(lr.center().x(), lr.top())),
                toScreen.map(QPointF(lr.center().x(), lr.bottom())),
            };
            {
                const QPointF topCenter = toScreen.map(QPointF(lr.center().x(), lr.top()));
                const QPointF inward = toScreen.map(QPointF(lr.center().x(), lr.top() + 1.0));
                QPointF up = topCenter - inward;
                const double upLen = std::hypot(up.x(), up.y());
                if (upLen > 1e-9) {
                    up /= upLen;
                    handles.push_back(topCenter + up * SkewHandleOffset);
                }
            }
            for (const QPointF &handle : handles) {
                QRectF rect(handle.x() - HandleHalf, handle.y() - HandleHalf, HandleHalf * 2.0, HandleHalf * 2.0);
                painter.fillRect(rect, SelectionFrameColor);
                painter.setPen(QPen(OverlayHaloColor, HandleBorderWidth));
                painter.drawRect(rect);
            }
        }
    }

    if (dragMode_ == DragMode::Marquee && marqueeRect_.isValid()) {
        QColor marqueeFill = SelectionAccentColor;
        marqueeFill.setAlpha(MarqueeFillAlpha);
        painter.setBrush(marqueeFill);
        painter.setPen(QPen(SelectionAccentColor, 1, Qt::DashLine));
        painter.drawRect(marqueeRect_);
    }
    drawCursorHint(painter);
    painter.restore();
}

void ProjectCanvas::updateSelectionFlashState()
{
    if (!selectionFlashEnabled_) {
        setFlashingLayerIds({});
        return;
    }
    QSet<QString> selected;
    if (state_ != nullptr) {
        selected = state_->selectedLayerIds();
    }
    setFlashingLayerIds(selected);
}

void ProjectCanvas::setFlashingLayerIds(const QSet<QString> &ids)
{
    const QSet<QString> selected = selectionFlashEnabled_ ? ids : QSet<QString>{};
    const bool selectionChanged = selected != flashingLayerIds_;
    if (selectionChanged) {
        flashingLayerIds_ = selected;
        selectionFlashClock_.restart();
    }
    if (flashingLayerIds_.isEmpty()) {
        selectionFlashTimer_.stop();
    } else if (selectionChanged || !selectionFlashTimer_.isActive()) {
        scheduleSelectionFlashTimer();
    }
}

void ProjectCanvas::scheduleSelectionFlashTimer()
{
    if (flashingLayerIds_.isEmpty()) {
        selectionFlashTimer_.stop();
        return;
    }
    const qint64 elapsed = selectionFlashClock_.elapsed() % SelectionFlashPeriodMs;
    if (elapsed < SelectionFlashDurationMs) {
        selectionFlashTimer_.start(SelectionFlashFrameMs);
        return;
    }
    selectionFlashTimer_.start(static_cast<int>(SelectionFlashPeriodMs - elapsed));
}

std::optional<double> ProjectCanvas::selectionFlashProgress() const
{
    if (flashingLayerIds_.isEmpty() || !selectionFlashClock_.isValid()) {
        return std::nullopt;
    }
    const qint64 elapsed = selectionFlashClock_.elapsed() % SelectionFlashPeriodMs;
    if (elapsed >= SelectionFlashDurationMs) {
        return std::nullopt;
    }
    return static_cast<double>(elapsed) / static_cast<double>(SelectionFlashDurationMs);
}

double ProjectCanvas::selectionFlashHue() const
{
    return selectionFlashProgress().value_or(-1.0);
}

double ProjectCanvas::selectionFlashStrength() const
{
    const std::optional<double> progress = selectionFlashProgress();
    return progress.has_value() ? 0.18 + 0.72 * std::sin(*progress * kPi) : 0.0;
}

QImage ProjectCanvas::guideImage(const fh6::scene::GuideLayer &guide) const
{
    const fh6::scene::RasterContainer *img = guide.image.get();
    const int width = img != nullptr ? img->width : 0;
    const int height = img != nullptr ? img->height : 0;
    const QString format = img != nullptr ? img->format : QString();
    const QByteArray &pixelBytes = img != nullptr ? img->pixels : QByteArray();
    const QByteArray &encodedBytes = img != nullptr ? img->encoded : QByteArray();
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5|%6")
        .arg(guide.id)
        .arg(width)
        .arg(height)
        .arg(format)
        .arg(pixelBytes.isEmpty() ? encodedBytes.size() : pixelBytes.size())
        .arg(QString::number(reinterpret_cast<quintptr>(img), 16));
    const auto cached = guideImageCache_.constFind(cacheKey);
    if (cached != guideImageCache_.constEnd()) {
        return cached.value();
    }
    QImage image;
    if (!pixelBytes.isEmpty() && width > 0 && height > 0) {
        image = QImage(reinterpret_cast<const uchar *>(pixelBytes.constData()),
                       width,
                       height,
                       width * 4,
                       QImage::Format_ARGB32_Premultiplied).copy();
    } else {
        image.loadFromData(encodedBytes, format.toLatin1().constData());
    }
    if (!image.isNull()) {
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    guideImageCache_.insert(cacheKey, image);
    return image;
}

QString ProjectCanvas::sectionCanvasCacheKey() const
{
    if (project_ == nullptr || state_ == nullptr || !project_->isLivery || state_->activeSectionId_.isEmpty() || size().isEmpty()) {
        return {};
    }
    return QStringLiteral("%1|%2x%3|%4|%5,%6,%7,%8,%9,%10|%11|%12")
        .arg(state_->activeSectionId_)
        .arg(width())
        .arg(height())
        .arg(QString::number(devicePixelRatioF(), 'g', 12))
        .arg(QString::number(worldToScreen_.m11(), 'g', 17))
        .arg(QString::number(worldToScreen_.m12(), 'g', 17))
        .arg(QString::number(worldToScreen_.m21(), 'g', 17))
        .arg(QString::number(worldToScreen_.m22(), 'g', 17))
        .arg(QString::number(worldToScreen_.dx(), 'g', 17))
        .arg(QString::number(worldToScreen_.dy(), 'g', 17))
        .arg(canvasColor_.rgba())
        .arg(guideLayersOnTop_ ? 1 : 0);
}

void ProjectCanvas::storeSectionCanvasCache(const QString &key)
{
    if (key.isEmpty()) {
        return;
    }
    QImage image = grabFramebuffer();
    if (image.isNull()) {
        return;
    }
    constexpr int SectionCanvasCacheCap = 16;
    if (sectionCanvasCache_.size() >= SectionCanvasCacheCap) {
        sectionCanvasCache_.clear();
    }
    sectionCanvasCache_.insert(key, image);
}

void ProjectCanvas::drawGuideLayers(QPainter &painter)
{
    if (sceneTree() == nullptr) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (!guide.visible || guide.opacity <= 0.0 || !isSectionActive(sectionGroupId)) {
            return true;
        }
        const QSizeF size = sceneNodeSize(guide, geometry_);
        const QRectF localRect(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
        if (!(world * worldToScreen_).mapRect(localRect).intersects(QRectF(rect()).adjusted(-1.0, -1.0, 1.0, 1.0))) {
            return true;
        }
        const QImage image = guideImage(guide);
        if (image.isNull()) {
            return true;
        }
        painter.save();
        painter.setOpacity(std::clamp(guide.opacity, 0.0, 1.0));
        painter.setTransform(world * worldToScreen_, true);
        painter.drawImage(localRect, image);
        painter.restore();
        return true;
    }, /*reverse=*/false);
    painter.restore();
}

void ProjectCanvas::initializeGL()
{
    renderer_.initialize();
    rendererGeometryDirty_ = true;
}

void ProjectCanvas::paintGL()
{
    if (project_ == nullptr) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), canvasColor_);
        painter.setPen(EmptyCanvasTextColor);
        painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter, QStringLiteral("Open a C_group file or folder"));
        return;
    }

    updateViewTransform();
    updateSelectionFlashState();
    if (rendererGeometryDirty_ && renderer_.isInitialized()) {
        renderer_.uploadGeometry(geometry_);
        rendererGeometryDirty_ = false;
    }

    const std::optional<double> flashProgress = selectionFlashProgress();
    const bool flashActive = flashProgress.has_value() && !flashingLayerIds_.isEmpty();
    const QString sectionCacheKey = flashActive ? QString() : sectionCanvasCacheKey();
    if (!sectionCacheKey.isEmpty()) {
        const auto cached = sectionCanvasCache_.constFind(sectionCacheKey);
        if (cached != sectionCanvasCache_.constEnd()) {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.drawImage(rect(), cached.value());
            drawOverlay(painter);
            return;
        }
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), canvasColor_);
    if (!guideLayersOnTop_) {
        drawGuideLayers(painter);
    }

    const qreal dpr = devicePixelRatioF();
    const QSize deviceSize(std::lround(width() * dpr), std::lround(height() * dpr));
    QTransform deviceCamera = worldToScreen_;
    deviceCamera *= QTransform::fromScale(dpr, dpr);

    painter.beginNativePainting();
    if (state_ != nullptr) {
        renderer_.render(state_->renderEntries(), geometry_, deviceCamera, deviceSize,
                         flashingLayerIds_, selectionFlashHue(), selectionFlashStrength(), false);
    } else {
        renderer_.render(*project_, geometry_, deviceCamera, deviceSize,
                         flashingLayerIds_, selectionFlashHue(), selectionFlashStrength(), false);
    }
    painter.endNativePainting();

    if (guideLayersOnTop_) {
        drawGuideLayers(painter);
    }

    if (!sectionCacheKey.isEmpty()) {
        painter.end();
        storeSectionCanvasCache(sectionCacheKey);
        painter.begin(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
    }

    drawOverlay(painter);
}

} // namespace gui
