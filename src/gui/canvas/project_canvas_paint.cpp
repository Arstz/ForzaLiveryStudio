#include "project_canvas.h"

#include "project_canvas_internal.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>

#include <cstdint>
#include <vector>

namespace gui {

using namespace pc_detail;

QPainterPath ProjectCanvas::penPreviewPath(bool closeToStart) const
{
    if (penPoints_.isEmpty()) {
        return {};
    }
    if (closeToStart && penPoints_.size() >= 3) {
        return buildPenContour(penPoints_).path;
    }
    QVector<PenPoint> previewPoints = penPoints_;
    previewPoints.push_back({penHoverWorld_, PenPointKind::Soft});
    QPainterPath path;
    path.moveTo(previewPoints.front().position);
    int index = 1;
    while (index < previewPoints.size()) {
        const PenPoint &next = previewPoints[index];
        if (next.kind == PenPointKind::Hard) {
            path.lineTo(next.position);
            ++index;
            continue;
        }
        if (index + 1 >= previewPoints.size()) {
            break;
        }
        const PenPoint &after = previewPoints[index + 1];
        const QPointF end = after.kind == PenPointKind::Hard
            ? after.position
            : (next.position + after.position) * 0.5;
        path.quadTo(next.position, end);
        index += after.kind == PenPointKind::Hard ? 2 : 1;
    }
    return path;
}

void ProjectCanvas::drawPenOverlay(QPainter &painter)
{
    if (tool_ != QStringLiteral("pen") && !penFillRunning_) {
        return;
    }
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (penFillRunning_) {
        const QString message = penFillMessage_.isEmpty()
            ? QStringLiteral("Filling Pen path…")
            : penFillMessage_;
        const QFontMetrics metrics(painter.font());
        const QRect textRect = metrics.boundingRect(message).adjusted(-12, -8, 12, 8);
        QRect bubble = textRect;
        bubble.moveCenter(rect().center());
        painter.setPen(QPen(QColor(235, 235, 235), 1));
        painter.setBrush(QColor(25, 27, 31, 220));
        painter.drawRoundedRect(bubble, 6, 6);
        painter.drawText(bubble, Qt::AlignCenter, message);
        painter.restore();
        return;
    }
    if (penPoints_.isEmpty()) {
        painter.restore();
        return;
    }

    const bool nearStart = !penLooped_ && penPoints_.size() >= 3
        && QLineF(worldToScreen(penHoverWorld_), worldToScreen(penPoints_.front().position)).length()
               <= PenCloseRadius;
    const bool closed = penLooped_ || nearStart;
    const QPainterPath worldPath = penPreviewPath(closed);
    const QPainterPath screenPath = worldToScreen_.map(worldPath);
    QColor fill(83, 164, 255, closed ? 50 : 25);
    QPen halo(QColor(15, 17, 20, 230), 4.0);
    halo.setCosmetic(true);
    painter.setPen(halo);
    painter.setBrush(fill);
    painter.drawPath(screenPath);
    QPen pathPen(penError_.isEmpty() ? QColor(83, 164, 255) : QColor(235, 78, 78), 2.0);
    pathPen.setCosmetic(true);
    painter.setPen(pathPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(screenPath);

    if (!closed && !penPoints_.isEmpty()) {
        QPen guide(QColor(190, 195, 205, 180), 1.0, Qt::DashLine);
        guide.setCosmetic(true);
        painter.setPen(guide);
        painter.drawLine(worldToScreen(penPoints_.back().position), worldToScreen(penHoverWorld_));
    }

    for (int i = 0; i < penPoints_.size(); ++i) {
        const PenPoint &point = penPoints_[i];
        const QPointF screen = worldToScreen(point.position);
        const bool hovered = penLooped_ && i == penHoverPoint_;
        const double radius = (i == 0 ? 5.5 : 4.5) + (hovered ? 1.5 : 0.0);
        painter.setPen(QPen(hovered ? QColor(120, 220, 135) : QColor(18, 20, 24),
                            hovered ? 2.5 : 2.0));
        painter.setBrush(point.kind == PenPointKind::Hard
                             ? QColor(232, 72, 72)
                             : QColor(238, 240, 244));
        painter.drawEllipse(screen, radius, radius);
        if (i == 0) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(closed ? QColor(120, 220, 135) : QColor(83, 164, 255), 1.5));
            painter.drawEllipse(screen, PenCloseRadius, PenCloseRadius);
        }
    }
    if (penLooped_ && penHoverPoint_ < 0 && penHoverCurve_.valid()) {
        const QPointF screen = worldToScreen(penHoverCurve_.worldPosition);
        painter.setPen(QPen(QColor(18, 20, 24), 2.0));
        painter.setBrush(QColor(120, 220, 135));
        painter.drawEllipse(screen, 4.0, 4.0);
    }
    painter.setPen(QPen(QColor(245, 65, 65), 2.0));
    for (const QPointF &crossing : penCrossings_) {
        const QPointF screen = worldToScreen(crossing);
        painter.drawLine(screen + QPointF(-6, -6), screen + QPointF(6, 6));
        painter.drawLine(screen + QPointF(-6, 6), screen + QPointF(6, -6));
    }
    if (!penError_.isEmpty()) {
        painter.setPen(QColor(245, 85, 85));
        painter.drawText(QRectF(12, 12, width() - 24, 30), Qt::AlignLeft | Qt::AlignVCenter, penError_);
    }
    painter.restore();
}

QPainterPath ProjectCanvas::liningPreviewPath() const
{
    if (liningPoints_.isEmpty()) {
        return {};
    }
    QVector<PenPoint> points = liningPoints_;
    if (!liningComplete_) {
        points.push_back({liningHoverWorld_, PenPointKind::Hard});
    }
    points.front().kind = PenPointKind::Hard;
    points.back().kind = PenPointKind::Hard;
    return buildLiningPath(points).centerline;
}

void ProjectCanvas::drawLiningOverlay(QPainter &painter)
{
    if (tool_ != QStringLiteral("lining") && !liningFillRunning_) {
        return;
    }
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (liningFillRunning_) {
        const QString message = liningFillMessage_.isEmpty()
            ? QStringLiteral("Filling lining path…")
            : liningFillMessage_;
        const QFontMetrics metrics(painter.font());
        const QRect textRect = metrics.boundingRect(message).adjusted(-12, -8, 12, 8);
        QRect bubble = textRect;
        bubble.moveCenter(rect().center());
        painter.setPen(QPen(QColor(235, 235, 235), 1));
        painter.setBrush(QColor(25, 27, 31, 220));
        painter.drawRoundedRect(bubble, 6, 6);
        painter.drawText(bubble, Qt::AlignCenter, message);
        painter.restore();
        return;
    }
    const QPainterPath centerline = liningPreviewPath();
    if (centerline.isEmpty()) {
        painter.restore();
        return;
    }
    const QPainterPath ribbon = buildLiningRibbon(centerline, liningWidth_);
    const QPainterPath screenRibbon = worldToScreen_.map(ribbon);
    const QPainterPath screenCenterline = worldToScreen_.map(centerline);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 200, 50, liningComplete_ ? 55 : 35));
    painter.drawPath(screenRibbon);

    QPen halo(QColor(15, 17, 20, 230), 4.0);
    halo.setCosmetic(true);
    painter.setPen(halo);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(screenCenterline);
    QPen centerPen(liningError_.isEmpty() ? QColor(255, 200, 50) : QColor(235, 78, 78), 2.0);
    centerPen.setCosmetic(true);
    painter.setPen(centerPen);
    painter.drawPath(screenCenterline);

    for (int i = 0; i < liningPoints_.size(); ++i) {
        const PenPoint &point = liningPoints_[i];
        const QPointF screen = worldToScreen(point.position);
        const bool hovered = liningComplete_ && i == liningHoverPoint_;
        const double radius = 4.5 + (hovered ? 1.5 : 0.0);
        painter.setPen(QPen(hovered ? QColor(120, 220, 135) : QColor(18, 20, 24),
                            hovered ? 2.5 : 2.0));
        painter.setBrush(point.kind == PenPointKind::Hard
                             ? QColor(232, 72, 72)
                             : QColor(238, 240, 244));
        painter.drawEllipse(screen, radius, radius);
    }
    if (liningComplete_ && liningHoverPoint_ < 0 && liningHoverCurve_.valid()) {
        painter.setPen(QPen(QColor(18, 20, 24), 2.0));
        painter.setBrush(QColor(120, 220, 135));
        painter.drawEllipse(worldToScreen(liningHoverCurve_.worldPosition), 4.0, 4.0);
    }
    if (!liningError_.isEmpty()) {
        painter.setPen(QColor(245, 85, 85));
        painter.drawText(QRectF(12, 12, width() - 24, 30),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         liningError_);
    }
    painter.restore();
}

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
const QColor ShapeCountTextColor(255, 255, 255);
const QColor ShapeCountOutlineColor(0, 0, 0);
constexpr double ShapeCountOutlineWidth = 3.0;
constexpr double ShapeCountMargin = 8.0;
constexpr double ShapeCountSupersampling = 2.0;

double rulerMajorStep(double pixelsPerWorldUnit)
{
    const double target = 80.0 / std::max(pixelsPerWorldUnit, 1e-12);
    const double magnitude = std::pow(10.0, std::floor(std::log10(target)));
    const double normalized = target / magnitude;
    if (normalized <= 1.0) {
        return magnitude;
    }
    if (normalized <= 2.0) {
        return magnitude * 2.0;
    }
    if (normalized <= 5.0) {
        return magnitude * 5.0;
    }
    return magnitude * 10.0;
}

QString rulerLabel(double value, double majorStep)
{
    const int decimals = majorStep >= 1.0
        ? 0
        : std::clamp(static_cast<int>(std::ceil(-std::log10(majorStep))), 0, 6);
    if (std::abs(value) < majorStep * 1e-6) {
        value = 0.0;
    }
    return QString::number(value, 'f', decimals);
}

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
    QVector<QStringList> sections;
    QStringList section;
    for (const QString &line : cursorHintLines_) {
        if (line.isEmpty()) {
            if (!section.isEmpty()) {
                sections.push_back(section);
                section.clear();
            }
        } else {
            section.push_back(line);
        }
    }
    if (!section.isEmpty()) {
        sections.push_back(section);
    }
    if (sections.isEmpty()) {
        painter.restore();
        return;
    }

    int textWidth = 0;
    for (const QStringList &lines : sections) {
        for (const QString &line : lines) {
            textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
        }
    }
    const int paddingX = CursorHintPaddingX;
    const int paddingY = CursorHintPaddingY;
    const int lineHeight = metrics.height();
    constexpr int sectionGap = 4;
    int totalHeight = sectionGap * (sections.size() - 1);
    for (const QStringList &lines : sections) {
        totalHeight += lines.size() * lineHeight + paddingY * 2;
    }
    const QSize hintSize(textWidth + paddingX * 2,
                         totalHeight);
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
    hintPainter.setRenderHint(QPainter::TextAntialiasing, true);
    int sectionY = 0;
    for (const QStringList &lines : sections) {
        const int sectionHeight = lines.size() * lineHeight + paddingY * 2;
        hintPainter.setPen(QPen(CursorHintBorderColor, 1));
        hintPainter.setBrush(CursorHintFillColor);
        hintPainter.drawRoundedRect(QRectF(0.0, sectionY, hintSize.width(), sectionHeight),
                                    CursorHintCornerRadius,
                                    CursorHintCornerRadius);
        hintPainter.setPen(CursorHintTextColor);
        int textY = sectionY + paddingY;
        for (const QString &line : lines) {
            hintPainter.drawText(QRectF(paddingX, textY, textWidth, lineHeight),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 line);
            textY += lineHeight;
        }
        sectionY += sectionHeight + sectionGap;
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

void ProjectCanvas::drawRulersAndGuidelines(QPainter &painter)
{
    if (project_ == nullptr) {
        return;
    }

    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, false);

    const QRectF contentRect(RulerExtent, RulerExtent,
                             std::max(0.0, width() - RulerExtent),
                             std::max(0.0, height() - RulerExtent));
    if (guidelinesVisible_) {
        painter.save();
        painter.setClipRect(contentRect);
        QPen guidelinePen(guidelineColor_, 1.0);
        guidelinePen.setCosmetic(true);
        painter.setPen(guidelinePen);
        for (double coordinate : project_->verticalGuidelines) {
            const double x = worldToScreen(QPointF(coordinate, 0.0)).x();
            painter.drawLine(QPointF(x, contentRect.top()), QPointF(x, contentRect.bottom()));
        }
        for (double coordinate : project_->horizontalGuidelines) {
            const double y = worldToScreen(QPointF(0.0, coordinate)).y();
            painter.drawLine(QPointF(contentRect.left(), y), QPointF(contentRect.right(), y));
        }
        painter.restore();
    }

    const bool dark = isDarkTheme(currentUiTheme());
    const QColor rulerBackground = dark ? QColor(38, 40, 44) : QColor(229, 231, 234);
    const QColor rulerDivider = dark ? QColor(83, 87, 94) : QColor(151, 155, 162);
    const QColor rulerText = dark ? QColor(216, 219, 224) : QColor(45, 48, 53);
    painter.fillRect(QRectF(0.0, 0.0, width(), RulerExtent), rulerBackground);
    painter.fillRect(QRectF(0.0, RulerExtent, RulerExtent, height() - RulerExtent), rulerBackground);
    painter.setPen(QPen(rulerDivider, 1.0));
    painter.drawLine(QPointF(RulerExtent - 0.5, 0.0), QPointF(RulerExtent - 0.5, height()));
    painter.drawLine(QPointF(0.0, RulerExtent - 0.5), QPointF(width(), RulerExtent - 0.5));

    QFont rulerFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    rulerFont.setPixelSize(10);
    rulerFont.setHintingPreference(QFont::PreferFullHinting);
    painter.setFont(rulerFont);
    painter.setPen(rulerText);
    const double pixelsPerWorldUnit = std::max(std::abs(worldToScreen_.m11()), 1e-12);
    const double majorStep = rulerMajorStep(pixelsPerWorldUnit);
    const double minorStep = majorStep / 10.0;

    const double leftWorld = screenToWorld(QPointF(contentRect.left(), contentRect.top())).x();
    const double rightWorld = screenToWorld(QPointF(contentRect.right(), contentRect.top())).x();
    const qint64 firstX = static_cast<qint64>(std::ceil(std::min(leftWorld, rightWorld) / minorStep));
    const qint64 lastX = static_cast<qint64>(std::floor(std::max(leftWorld, rightWorld) / minorStep));
    for (qint64 index = firstX; index <= lastX; ++index) {
        const double coordinate = index * minorStep;
        const double x = worldToScreen(QPointF(coordinate, 0.0)).x();
        const int subdivision = static_cast<int>(((index % 10) + 10) % 10);
        const bool major = subdivision == 0;
        const bool half = subdivision == 5;
        const double tick = major ? 11.0 : (half ? 7.0 : 4.0);
        painter.drawLine(QPointF(x, RulerExtent), QPointF(x, RulerExtent - tick));
        if (major) {
            painter.drawText(QRectF(std::round(x) + 3.0, 1.0, 72.0, RulerExtent - 12.0),
                             Qt::AlignLeft | Qt::AlignVCenter, rulerLabel(coordinate, majorStep));
        }
    }

    const double topWorld = screenToWorld(QPointF(contentRect.left(), contentRect.top())).y();
    const double bottomWorld = screenToWorld(QPointF(contentRect.left(), contentRect.bottom())).y();
    const qint64 firstY = static_cast<qint64>(std::ceil(std::min(topWorld, bottomWorld) / minorStep));
    const qint64 lastY = static_cast<qint64>(std::floor(std::max(topWorld, bottomWorld) / minorStep));
    for (qint64 index = firstY; index <= lastY; ++index) {
        const double coordinate = index * minorStep;
        const double y = worldToScreen(QPointF(0.0, coordinate)).y();
        const int subdivision = static_cast<int>(((index % 10) + 10) % 10);
        const bool major = subdivision == 0;
        const bool half = subdivision == 5;
        const double tick = major ? 11.0 : (half ? 7.0 : 4.0);
        painter.drawLine(QPointF(RulerExtent, y), QPointF(RulerExtent - tick, y));
        if (major) {
            painter.save();
            painter.translate(RulerExtent * 0.5 - 1.0, std::round(y));
            painter.rotate(-90.0);
            painter.drawText(QRectF(-36.0, -RulerExtent * 0.5, 72.0, RulerExtent - 12.0),
                             Qt::AlignCenter, rulerLabel(coordinate, majorStep));
            painter.restore();
        }
    }

    if (guidelinesVisible_) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(guidelineColor_);
        for (double coordinate : project_->verticalGuidelines) {
            const double x = worldToScreen(QPointF(coordinate, 0.0)).x();
            if (x >= contentRect.left() && x <= contentRect.right()) {
                painter.drawPolygon(QPolygonF({QPointF(x - 4.0, RulerExtent - 7.0),
                                               QPointF(x + 4.0, RulerExtent - 7.0),
                                               QPointF(x, RulerExtent - 1.0)}));
            }
        }
        for (double coordinate : project_->horizontalGuidelines) {
            const double y = worldToScreen(QPointF(0.0, coordinate)).y();
            if (y >= contentRect.top() && y <= contentRect.bottom()) {
                painter.drawPolygon(QPolygonF({QPointF(RulerExtent - 7.0, y - 4.0),
                                               QPointF(RulerExtent - 7.0, y + 4.0),
                                               QPointF(RulerExtent - 1.0, y)}));
            }
        }
    }
    painter.restore();
}

void ProjectCanvas::drawOverlay(QPainter &painter)
{
    painter.save();
    painter.resetTransform();
    painter.setClipRect(QRectF(RulerExtent, RulerExtent,
                               std::max(0.0, width() - RulerExtent),
                               std::max(0.0, height() - RulerExtent)));
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

    drawRegionOverlay(painter);
    drawBucketOverlay(painter);
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
        if (tool_ == QStringLiteral("transform")
            && (!isTransformDrag() || displayAnchorsDuringTransformDrag_)) {
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

    int loadedCount = 0;
    forEachSceneShape([&](const fh6::scene::Shape &, const QTransform &, int) {
        ++loadedCount;
        return true;
    }, false);
    const int selectedCount = state_ != nullptr ? state_->selectedLayerIds().size() : 0;
    const QStringList countLines = {
        QStringLiteral("Shapes loaded: %1").arg(loadedCount),
        QStringLiteral("Shapes selected: %1").arg(selectedCount),
    };
    QFont countFont = painter.font();
    if (countFont.pointSizeF() > 0.0) {
        countFont.setPointSizeF(countFont.pointSizeF() * 1.5);
    } else {
        countFont.setPixelSize(std::max(1, qRound(countFont.pixelSize() * 1.5)));
    }
    const QFontMetricsF metrics(countFont);
    qreal textWidth = 0.0;
    for (const QString &line : countLines) {
        textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
    }
    const qreal padding = ShapeCountOutlineWidth + 1.0;
    const QSizeF imageSize(textWidth + padding * 2.0,
                           metrics.height() * countLines.size() + padding * 2.0);
    const qreal renderScale = devicePixelRatioF() * ShapeCountSupersampling;
    QImage countImage(QSize(qCeil(imageSize.width() * renderScale),
                            qCeil(imageSize.height() * renderScale)),
                      QImage::Format_ARGB32_Premultiplied);
    countImage.setDevicePixelRatio(renderScale);
    countImage.fill(Qt::transparent);

    QPainter countPainter(&countImage);
    countPainter.setRenderHint(QPainter::Antialiasing, true);
    countPainter.setRenderHint(QPainter::TextAntialiasing, true);
    qreal baseline = padding + metrics.ascent();
    for (const QString &line : countLines) {
        QPainterPath path;
        path.addText(QPointF(padding, baseline), countFont, line);
        countPainter.strokePath(path, QPen(ShapeCountOutlineColor, ShapeCountOutlineWidth,
                                           Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        countPainter.fillPath(path, ShapeCountTextColor);
        baseline += metrics.height();
    }
    countPainter.end();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QPointF(RulerExtent + ShapeCountMargin - padding,
                              RulerExtent + ShapeCountMargin - padding),
                      countImage);

    drawPenOverlay(painter);
    drawLiningOverlay(painter);
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
    return QStringLiteral("%1|%2x%3|%4|%5,%6,%7,%8,%9,%10|%11|%12|%13")
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
        .arg(guideLayersOnTop_ ? 1 : 0)
        .arg(guideLayersVisible_ ? 1 : 0);
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
    if (!guideLayersVisible_ || sceneTree() == nullptr) {
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

bool ProjectCanvas::createRegionsForSelectedGuide(QString *message)
{
    const QVector<fh6::scene::GuideLayer *> guides = selectedGuideLayers();
    if (guides.isEmpty()) {
        if (message != nullptr) {
            *message = QStringLiteral("Select a guide layer first");
        }
        return false;
    }
    if (guides.size() > 1) {
        if (message != nullptr) {
            *message = QStringLiteral("Select a single guide layer");
        }
        return false;
    }
    const fh6::scene::GuideLayer *guide = guides.front();
    const QImage image = guideImage(*guide);
    if (image.isNull()) {
        if (message != nullptr) {
            *message = QStringLiteral("The selected guide layer has no image");
        }
        return false;
    }
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    RegionExtractionParams params;
    if (guide->preprocessColorCount > 0) {
        params.colorCount = guide->preprocessColorCount;
    }
    const RegionExtractionResult regions = extractRegions(image, params);
    QGuiApplication::restoreOverrideCursor();
    if (!regions.valid()) {
        if (message != nullptr) {
            *message = regions.error.isEmpty()
                ? QStringLiteral("Region extraction produced no regions")
                : regions.error;
        }
        return false;
    }
    regionOverlayGuideId_ = guide->id;
    regionOverlay_ = regions;
    regionFills_.clear();
    regionFillSilhouettes_.clear();
    showRegionFills_ = false;
    regionOverlayHidden_ = false;
    update();
    if (message != nullptr) {
        *message = QStringLiteral("Created %1 regions (%2 colour, %3 lineart)")
            .arg(regions.regions.size())
            .arg(regions.colorRegionCount)
            .arg(regions.lineartRegionCount);
    }
    return true;
}

void ProjectCanvas::clearRegionOverlay()
{
    if (regionOverlay_.regions.isEmpty() && regionOverlayGuideId_.isEmpty()) {
        return;
    }
    regionOverlayGuideId_.clear();
    regionOverlay_ = RegionExtractionResult{};
    regionFills_.clear();
    regionFillSilhouettes_.clear();
    showRegionFills_ = false;
    regionOverlayHidden_ = false;
    update();
}

namespace {

// Split a simplified (WindingFill) union path into its outer blob contours,
// discarding hole subpaths (a subpath contained inside another). Each returned
// path is a single closed outer contour, filled solid downstream so that holes
// are overpainted by whatever higher-z region sits in them.
QVector<QPainterPath> outerBlobsFromUnion(const QPainterPath &united)
{
    const QList<QPolygonF> subs = united.toSubpathPolygons();
    QVector<QPolygonF> polys;
    for (const QPolygonF &polygon : subs) {
        if (polygon.size() < 3) {
            continue;
        }
        double twiceArea = 0.0;
        for (int i = 0; i < polygon.size(); ++i) {
            const QPointF &a = polygon[i];
            const QPointF &b = polygon[(i + 1) % polygon.size()];
            twiceArea += a.x() * b.y() - a.y() * b.x();
        }
        if (std::abs(twiceArea) < 1.0) {
            continue;
        }
        polys.push_back(polygon);
    }
    QVector<QPainterPath> blobs;
    for (int i = 0; i < polys.size(); ++i) {
        bool contained = false;
        const QPointF probe = polys[i].first();
        for (int j = 0; j < polys.size(); ++j) {
            if (i == j) {
                continue;
            }
            if (polys[j].containsPoint(probe, Qt::OddEvenFill)) {
                contained = true;
                break;
            }
        }
        if (!contained) {
            QPainterPath path;
            path.addPolygon(polys[i]);
            path.closeSubpath();
            blobs.push_back(path);
        }
    }
    return blobs;
}

} // namespace

bool ProjectCanvas::fillRegionsForOverlay(QString *message)
{
    if (regionOverlay_.regions.isEmpty()) {
        if (message != nullptr) {
            *message = QStringLiteral("Create regions first");
        }
        return false;
    }
    const QVector<PenPrimitive> primitives = penPrimitiveCatalog();
    if (primitives.isEmpty()) {
        if (message != nullptr) {
            *message = QStringLiteral("Pen primitive geometry is unavailable");
        }
        return false;
    }

    QHash<int, QPainterPath> silhouettes;
    for (const PenPrimitive &primitive : primitives) {
        silhouettes.insert(primitive.shapeId, primitive.silhouette);
    }
    // Fit tolerance scales with the image so it stays a small fraction of a pixel
    // of visible error regardless of guide resolution.
    const double tolerance = std::max(1.0,
                                      std::min(regionOverlay_.imageSize.width(),
                                               regionOverlay_.imageSize.height()) * 0.004);

    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QVector<RegionFillLayer> fills;
    int filled = 0;
    int failed = 0;
    int timedOut = 0;
    int placementCount = 0;
    int mergedBlobs = 0;
    int meshFallbacks = 0;
    QHash<QString, int> failureReasons;
    // Bound each fill by wall clock so a pathological outline cannot drive the
    // fitter's boolean-union accumulation into an unbounded quadratic blow-up.
    constexpr qint64 kRegionBudgetMs = 3000;
    // Direct-triangulation fallback sources (Square 101 / Triangle 103 hulls).
    const PolygonMeshSources meshSources = buildPolygonMeshSources(geometry_);
    // RDP corridor for the mesh path: sub-visible, collapses potrace's 8x-
    // oversampled flattening back down so complex/merged blobs are not
    // triangulated at full resolution.
    const double simplifyEpsilon = tolerance;

    const int imageW = regionOverlay_.imageSize.width();
    const int imageH = regionOverlay_.imageSize.height();

    // Build the paint UNITS: same-colour regions are unioned and split into
    // solid outer blobs (adjacent same-colour pieces the segmenter split apart
    // merge into one). Disjoint single regions keep their smooth outline so the
    // Pen fitter's circle caps still engage; merged blobs are polygonal.
    struct FillUnit {
        QColor color;
        QPainterPath outline;   // what to fill / rasterize
        double area = 0.0;
        bool polygonal = false; // came from a merge -> mesh path with occlusion band
    };
    QVector<FillUnit> units;
    {
        struct ColorGroup {
            QColor color;
            double area = 0.0;
            QVector<const ExtractedRegion *> regions;
        };
        QHash<QRgb, int> colorIndex;
        QVector<ColorGroup> groups;
        for (const ExtractedRegion &region : regionOverlay_.regions) {
            if (region.lineart) {
                continue;
            }
            const QRgb key = region.color.rgb();
            int idx = colorIndex.value(key, -1);
            if (idx < 0) {
                idx = groups.size();
                colorIndex.insert(key, idx);
                groups.push_back(ColorGroup{region.color, 0.0, {}});
            }
            groups[idx].area += region.area;
            groups[idx].regions.push_back(&region);
        }
        for (const ColorGroup &group : groups) {
            QPainterPath unionPath;
            unionPath.setFillRule(Qt::WindingFill);
            for (const ExtractedRegion *region : group.regions) {
                unionPath.addPath(region->outline);
            }
            unionPath = unionPath.simplified();
            const QVector<QPainterPath> blobs = outerBlobsFromUnion(unionPath);
            if (blobs.size() < group.regions.size()) {
                for (const QPainterPath &blob : blobs) {
                    const QPolygonF poly = regionOuterContour(blob);
                    double twiceArea = 0.0;
                    for (int k = 0; k < poly.size(); ++k) {
                        const QPointF &p0 = poly[k];
                        const QPointF &p1 = poly[(k + 1) % poly.size()];
                        twiceArea += p0.x() * p1.y() - p0.y() * p1.x();
                    }
                    units.push_back(FillUnit{group.color, blob,
                                             std::abs(twiceArea) * 0.5, true});
                    ++mergedBlobs;
                }
            } else {
                for (const ExtractedRegion *region : group.regions) {
                    units.push_back(FillUnit{group.color, region->outline,
                                             static_cast<double>(region->area), false});
                }
            }
        }
    }

    // z-order: largest area first == bottom of the stack. Planning walks the
    // stack TOP-DOWN, accumulating an `occluded` raster mask of everything above;
    // rendering (fills emitted here) stays largest-first == bottom-up.
    std::sort(units.begin(), units.end(), [](const FillUnit &a, const FillUnit &b) {
        return a.area > b.area;
    });

    QStringList log;
    log << QStringLiteral("Fill Regions diagnostic - image %1x%2, tolerance %3, rdp %4, %5 units")
               .arg(imageW)
               .arg(imageH)
               .arg(tolerance, 0, 'f', 3)
               .arg(simplifyEpsilon, 0, 'f', 3)
               .arg(units.size());

    const auto recordFailure = [&](const PenFillResult &fit) {
        if (fit.cancelled) {
            ++timedOut;
        } else {
            ++failed;
        }
        const QString reason = fit.error.isEmpty()
            ? (fit.cancelled ? QStringLiteral("timed out") : QStringLiteral("empty result"))
            : fit.error;
        failureReasons[reason] += 1;
    };

    const size_t pixelTotal = static_cast<size_t>(imageW) * imageH;
    // Occlusion mask: 1 where some already-planned (higher-z) unit paints. A
    // lower unit may GROW its fill into these pixels because they are repainted
    // from above. `vk`/`comp` are scratch masks reused per unit.
    std::vector<std::uint8_t> occluded(pixelTotal, 0);
    std::vector<std::uint8_t> vk(pixelTotal, 0);
    std::vector<std::uint8_t> comp(pixelTotal, 0);

    // Rasterize a path's fill into `dst` (set to 1), returning the touched rect.
    const auto rasterizePath = [&](const QPainterPath &path, std::vector<std::uint8_t> &dst) -> QRect {
        const QRect b = path.boundingRect().toAlignedRect().intersected(QRect(0, 0, imageW, imageH));
        if (b.isEmpty()) {
            return b;
        }
        QImage tile(b.width(), b.height(), QImage::Format_Grayscale8);
        tile.fill(0);
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.translate(-b.topLeft());
        painter.fillPath(path, Qt::white);
        painter.end();
        for (int y = 0; y < b.height(); ++y) {
            const std::uint8_t *row = tile.constScanLine(y);
            std::uint8_t *out = dst.data() + static_cast<size_t>(b.top() + y) * imageW + b.left();
            for (int x = 0; x < b.width(); ++x) {
                if (row[x]) {
                    out[x] = 1;
                }
            }
        }
        return b;
    };

    // Seed the occlusion mask with all lineart: it renders on top of every
    // colour layer, so every colour region may grow under the strokes -- the
    // universal leeway that lets a region fill across the lines dividing it.
    for (const ExtractedRegion &region : regionOverlay_.regions) {
        if (region.lineart) {
            rasterizePath(region.outline, occluded);
        }
    }

    const RegionExtractionParams traceParams; // defaults are fine for re-tracing
    QVector<RegionFillLayer> unitFills(units.size());
    long grownPixels = 0;
    long seedPixels = 0;
    for (int i = units.size() - 1; i >= 0; --i) {
        const FillUnit &unit = units[i];
        QElapsedTimer clock;
        clock.start();
        const auto cancelled = [&clock]() {
            return clock.elapsed() > kRegionBudgetMs;
        };

        // Rasterize this unit's own footprint (V_R).
        const QRect vkBounds = rasterizePath(unit.outline, vk);

        // Flood-grow across mayCover = occluded | V_R, seeded from V_R. The grown
        // component fills under every higher layer it touches and stops at
        // lower-z (still-unplanned, visible) neighbours -- which remain holes.
        QRect compBounds = vkBounds;
        std::vector<int> stack;
        int unitSeed = 0;
        for (int y = vkBounds.top(); y <= vkBounds.bottom(); ++y) {
            const std::uint8_t *vkRow = vk.data() + static_cast<size_t>(y) * imageW;
            for (int x = vkBounds.left(); x <= vkBounds.right(); ++x) {
                if (vkRow[x] && !comp[static_cast<size_t>(y) * imageW + x]) {
                    comp[static_cast<size_t>(y) * imageW + x] = 1;
                    stack.push_back(y * imageW + x);
                    ++unitSeed;
                }
            }
        }
        while (!stack.empty()) {
            const int idx = stack.back();
            stack.pop_back();
            const int px = idx % imageW;
            const int py = idx / imageW;
            compBounds |= QRect(px, py, 1, 1);
            const int nb[4][2] = {{px - 1, py}, {px + 1, py}, {px, py - 1}, {px, py + 1}};
            for (const auto &n : nb) {
                const int nx = n[0];
                const int ny = n[1];
                if (nx < 0 || ny < 0 || nx >= imageW || ny >= imageH) {
                    continue;
                }
                const size_t nIdx = static_cast<size_t>(ny) * imageW + nx;
                if (comp[nIdx]) {
                    continue;
                }
                if (occluded[nIdx] || vk[nIdx]) {
                    comp[nIdx] = 1;
                    stack.push_back(ny * imageW + nx);
                }
            }
        }

        int compArea = 0;
        for (int y = compBounds.top(); y <= compBounds.bottom(); ++y) {
            const std::uint8_t *row = comp.data() + static_cast<size_t>(y) * imageW;
            for (int x = compBounds.left(); x <= compBounds.right(); ++x) {
                compArea += row[x];
            }
        }
        seedPixels += unitSeed;
        grownPixels += compArea;

        // Re-vectorize the grown component (potrace handles outer + holes), then
        // triangulate each simple fill polygon.
        QString via = QStringLiteral("grown");
        PenFillResult fit;
        const QPainterPath grown = traceMaskToPath(comp, imageW, imageH, compBounds, traceParams);
        if (!grown.isEmpty()) {
            const QList<QPolygonF> fillPolys = grown.toFillPolygons();
            bool anyFail = false;
            for (const QPolygonF &poly : fillPolys) {
                if (poly.size() < 3) {
                    continue;
                }
                QPolygonF trimmed = poly;
                while (trimmed.size() > 1
                       && QLineF(trimmed.front(), trimmed.back()).length() <= 1e-6) {
                    trimmed.removeLast();
                }
                const QPolygonF simplified = simplifyClosedPolygon(trimmed, simplifyEpsilon);
                const PenFillResult part = fillPolygonMesh(simplified, meshSources, cancelled);
                if (part.error.isEmpty() && !part.placements.isEmpty()) {
                    fit.placements += part.placements;
                } else if (part.cancelled) {
                    fit.cancelled = true;
                    anyFail = true;
                    break;
                } else {
                    anyFail = true;
                }
            }
            if (anyFail && fit.placements.isEmpty()) {
                fit.error = QStringLiteral("grown mesh failed");
            }
        }
        // Fallback: solid fill of the original outline (drop holes) if growth failed.
        if (!(fit.error.isEmpty() && !fit.placements.isEmpty()) && !fit.cancelled) {
            const PenFillResult meshFit =
                fillRegionOutlineMesh(unit.outline, meshSources, simplifyEpsilon, cancelled);
            if (meshFit.error.isEmpty() && !meshFit.placements.isEmpty()) {
                ++meshFallbacks;
                fit = meshFit;
                via = QStringLiteral("solid-fallback");
            } else if (fit.placements.isEmpty()) {
                fit = meshFit;
                via = QStringLiteral("failed");
            }
        }

        if (fit.error.isEmpty() && !fit.placements.isEmpty()) {
            RegionFillLayer &layer = unitFills[i];
            layer.color = unit.color;
            layer.area = unit.area;
            layer.placements = fit.placements;
            placementCount += fit.placements.size();
            ++filled;
        } else {
            recordFailure(fit);
        }

        // This unit now sits above every lower unit still to be planned: its
        // grown footprint becomes occlusion leeway for them.
        for (int y = compBounds.top(); y <= compBounds.bottom(); ++y) {
            const std::uint8_t *src = comp.data() + static_cast<size_t>(y) * imageW;
            std::uint8_t *dst = occluded.data() + static_cast<size_t>(y) * imageW;
            for (int x = compBounds.left(); x <= compBounds.right(); ++x) {
                if (src[x]) {
                    dst[x] = 1;
                }
            }
        }
        // Clear the scratch masks within their touched bounds for reuse.
        for (int y = compBounds.top(); y <= compBounds.bottom(); ++y) {
            std::uint8_t *cRow = comp.data() + static_cast<size_t>(y) * imageW;
            std::uint8_t *vRow = vk.data() + static_cast<size_t>(y) * imageW;
            for (int x = compBounds.left(); x <= compBounds.right(); ++x) {
                cRow[x] = 0;
                vRow[x] = 0;
            }
        }

        log << QStringLiteral("unit #%1 %2 %3: seed=%4 grown=%5 -> %6 shapes, %7 ms%8")
                   .arg(i)
                   .arg(unit.color.name())
                   .arg(via)
                   .arg(unitSeed)
                   .arg(compArea)
                   .arg(fit.placements.size())
                   .arg(clock.elapsed())
                   .arg(fit.error.isEmpty() ? QString()
                                            : QStringLiteral(" [error: %1]").arg(fit.error));
    }

    // Emit bottom-up (largest first) so the scene stack matches the plan.
    for (RegionFillLayer &layer : unitFills) {
        if (!layer.placements.isEmpty()) {
            fills.push_back(std::move(layer));
        }
    }
    log << QStringLiteral("Occlusion: grew %1 seed px to %2 filled px (%3x leeway)")
               .arg(seedPixels)
               .arg(grownPixels)
               .arg(seedPixels > 0 ? static_cast<double>(grownPixels) / seedPixels : 0.0, 0, 'f', 2);
    QGuiApplication::restoreOverrideCursor();

    log << QStringLiteral("Summary: %1 fills (%2 merged blobs, %3 mesh fallbacks), "
                          "%4 shapes across %5 colour layers, %6 failed, %7 timed out")
               .arg(filled)
               .arg(mergedBlobs)
               .arg(meshFallbacks)
               .arg(placementCount)
               .arg(fills.size())
               .arg(failed)
               .arg(timedOut);
    const QString logPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/region_fill.log");
    QFile logFile(logPath);
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&logFile);
        stream << log.join(QLatin1Char('\n')) << '\n';
        logFile.close();
    }
    qWarning().noquote() << "Fill Regions log written to" << logPath;

    // Build a "top failure reason" suffix so the cause is visible in-app.
    QString reasonSuffix;
    if (!failureReasons.isEmpty()) {
        QString topReason;
        int topCount = 0;
        for (auto it = failureReasons.constBegin(); it != failureReasons.constEnd(); ++it) {
            if (it.value() > topCount) {
                topCount = it.value();
                topReason = it.key();
            }
        }
        reasonSuffix = QStringLiteral(" - top reason: %1 (x%2)").arg(topReason).arg(topCount);
    }

    if (fills.isEmpty()) {
        if (message != nullptr) {
            *message = QStringLiteral("No regions could be filled%1").arg(reasonSuffix);
        }
        return false;
    }
    // Largest region first so nested (smaller) regions draw on top.
    std::sort(fills.begin(), fills.end(), [](const RegionFillLayer &a, const RegionFillLayer &b) {
        return a.area > b.area;
    });
    regionFills_ = std::move(fills);
    regionFillSilhouettes_ = std::move(silhouettes);
    showRegionFills_ = true;
    update();
    if (message != nullptr) {
        *message = QStringLiteral("Filled %1 colour layers with %2 shapes "
                                  "(%3 merged blobs, %4 failed, %5 timed out)%6")
            .arg(fills.size())
            .arg(placementCount)
            .arg(mergedBlobs)
            .arg(failed)
            .arg(timedOut)
            .arg(reasonSuffix);
    }
    return true;
}

void ProjectCanvas::clearRegionFills()
{
    if (regionFills_.isEmpty() && !showRegionFills_) {
        return;
    }
    regionFills_.clear();
    regionFillSilhouettes_.clear();
    showRegionFills_ = false;
    update();
}

QVector<GeneratedRegionShape> ProjectCanvas::regionFillWorldPlacements()
{
    QVector<GeneratedRegionShape> result;
    if (regionFills_.isEmpty() || regionOverlayGuideId_.isEmpty()) {
        return result;
    }
    const QSize imageSize = regionOverlay_.imageSize;
    if (imageSize.width() < 1 || imageSize.height() < 1) {
        return result;
    }

    // Same image-pixel -> world mapping the overlay uses to draw, minus the
    // world->screen step: image -> guide local -> world.
    QTransform guideWorld;
    QSizeF guideSize;
    bool found = false;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (guide.id != regionOverlayGuideId_ || !isSectionActive(sectionGroupId)) {
            return true;
        }
        guideWorld = world;
        guideSize = sceneNodeSize(guide, geometry_);
        found = true;
        return false;
    }, /*reverse=*/false);
    if (!found || guideSize.width() <= 0.0 || guideSize.height() <= 0.0) {
        return result;
    }
    QTransform imageToLocal;
    imageToLocal.translate(-guideSize.width() * 0.5, -guideSize.height() * 0.5);
    imageToLocal.scale(guideSize.width() / imageSize.width(),
                       guideSize.height() / imageSize.height());
    const QTransform imageToWorld = imageToLocal * guideWorld;

    for (const RegionFillLayer &fill : regionFills_) {
        // Scene shape colour is stored BGRA.
        const std::array<std::uint8_t, 4> color = {
            static_cast<std::uint8_t>(fill.color.blue()),
            static_cast<std::uint8_t>(fill.color.green()),
            static_cast<std::uint8_t>(fill.color.red()),
            255,
        };
        for (const PenPlacement &placement : fill.placements) {
            GeneratedRegionShape shape;
            shape.shapeId = placement.shapeId;
            shape.transform = placement.transform * imageToWorld;
            shape.color = color;
            result.push_back(shape);
        }
    }
    return result;
}

void ProjectCanvas::hideRegionOverlay()
{
    regionOverlayHidden_ = true;
    showRegionFills_ = false;
    update();
}

void ProjectCanvas::drawRegionOverlay(QPainter &painter)
{
    if (regionOverlay_.regions.isEmpty() || regionOverlayGuideId_.isEmpty() || regionOverlayHidden_) {
        return;
    }
    const QSize imageSize = regionOverlay_.imageSize;
    if (imageSize.width() < 1 || imageSize.height() < 1) {
        return;
    }

    // Find the current world placement of the guide the overlay was built from.
    QTransform guideWorld;
    QSizeF guideSize;
    bool found = false;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (guide.id != regionOverlayGuideId_ || !isSectionActive(sectionGroupId)) {
            return true;
        }
        guideWorld = world;
        guideSize = sceneNodeSize(guide, geometry_);
        found = true;
        return false;
    }, /*reverse=*/false);
    if (!found || guideSize.width() <= 0.0 || guideSize.height() <= 0.0) {
        return;
    }

    // Image-pixel space -> guide local space (matches drawImage(localRect, image)).
    QTransform imageToLocal;
    imageToLocal.translate(-guideSize.width() * 0.5, -guideSize.height() * 0.5);
    imageToLocal.scale(guideSize.width() / imageSize.width(),
                       guideSize.height() / imageSize.height());
    const QTransform imageToScreen = imageToLocal * guideWorld * worldToScreen_;

    // Distinct debug palette; the graph coloring guarantees adjacent regions
    // pick different entries, so neighbours never blend together.
    static const QColor kDebugPalette[] = {
        QColor(228, 87, 86), QColor(88, 163, 222), QColor(126, 194, 106),
        QColor(240, 179, 74), QColor(163, 122, 214), QColor(74, 204, 196),
        QColor(232, 130, 197), QColor(150, 158, 170),
    };
    constexpr int kDebugPaletteCount = int(sizeof(kDebugPalette) / sizeof(kDebugPalette[0]));

    painter.save();
    painter.setTransform(imageToScreen, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (showRegionFills_ && !regionFills_.isEmpty()) {
        // Filled preview: each region's primitive placements in the region colour.
        painter.setPen(Qt::NoPen);
        for (const RegionFillLayer &fill : regionFills_) {
            painter.setBrush(fill.color);
            for (const PenPlacement &placement : fill.placements) {
                const auto silhouette = regionFillSilhouettes_.constFind(placement.shapeId);
                if (silhouette != regionFillSilhouettes_.constEnd()) {
                    painter.drawPath(placement.transform.map(silhouette.value()));
                }
            }
        }
    } else {
        // Debug region map: colour regions filled with their graph-colour so each
        // reads as a distinct area.
        QPen outlinePen(QColor(20, 22, 26, 180), 1.0);
        outlinePen.setCosmetic(true);
        for (const ExtractedRegion &region : regionOverlay_.regions) {
            if (region.lineart) {
                continue;
            }
            QColor fill = kDebugPalette[region.debugColor % kDebugPaletteCount];
            fill.setAlpha(120);
            painter.setBrush(fill);
            painter.setPen(outlinePen);
            painter.drawPath(region.outline);
        }
    }

    // Lineart always draws on top, opaque in the opposite colour.
    painter.setPen(Qt::NoPen);
    for (const ExtractedRegion &region : regionOverlay_.regions) {
        if (!region.lineart) {
            continue;
        }
        painter.setBrush(QColor(255 - region.color.red(),
                                255 - region.color.green(),
                                255 - region.color.blue()));
        painter.drawPath(region.outline);
    }
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
            drawRulersAndGuidelines(painter);
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

    drawRulersAndGuidelines(painter);
    drawOverlay(painter);
}

} // namespace gui
