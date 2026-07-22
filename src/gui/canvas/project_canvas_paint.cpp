#include "project_canvas.h"

#include "project_canvas_internal.h"
#include "region_layer_plan.h"

#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <QThreadPool>

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace gui {

using namespace pc_detail;

QPainterPath ProjectCanvas::penPreviewPath(bool closeToStart) const {
    if (pen_.points.isEmpty()) {
        return {};
    }
    if (closeToStart && pen_.points.size() >= 3) {
        return buildPenContour(pen_.points).path;
    }
    QVector<PenPoint> previewPoints = pen_.points;
    previewPoints.push_back({pen_.hoverWorld, PenPointKind::Soft});
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

void ProjectCanvas::drawPenOverlay(QPainter &painter) {
    if (tool_ != QStringLiteral("pen") && !pen_.fillRunning) {
        return;
    }
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (pen_.fillRunning) {
        const QString message = pen_.fillMessage.isEmpty()
            ? QStringLiteral("Filling Pen path…")
            : pen_.fillMessage;
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
    if (pen_.points.isEmpty()) {
        painter.restore();
        return;
    }

    const bool nearStart = !pen_.closed && pen_.points.size() >= 3
        && QLineF(worldToScreen(pen_.hoverWorld), worldToScreen(pen_.points.front().position)).length()
               <= kPenCloseRadius;
    const bool closed = pen_.closed || nearStart;
    const QPainterPath worldPath = penPreviewPath(closed);
    const QPainterPath screenPath = camera_.matrix().map(worldPath);
    QColor fill(83, 164, 255, closed ? 50 : 25);
    QPen halo(QColor(15, 17, 20, 230), 4.0);
    halo.setCosmetic(true);
    painter.setPen(halo);
    painter.setBrush(fill);
    painter.drawPath(screenPath);
    QPen pathPen(pen_.error.isEmpty() ? QColor(83, 164, 255) : QColor(235, 78, 78), 2.0);
    pathPen.setCosmetic(true);
    painter.setPen(pathPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(screenPath);

    if (!closed && !pen_.points.isEmpty()) {
        QPen guide(QColor(190, 195, 205, 180), 1.0, Qt::DashLine);
        guide.setCosmetic(true);
        painter.setPen(guide);
        painter.drawLine(worldToScreen(pen_.points.back().position), worldToScreen(pen_.hoverWorld));
    }

    for (int i = 0; i < pen_.points.size(); ++i) {
        const PenPoint &point = pen_.points[i];
        const QPointF screen = worldToScreen(point.position);
        const bool hovered = pen_.closed && i == pen_.hoverPoint;
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
            painter.drawEllipse(screen, kPenCloseRadius, kPenCloseRadius);
        }
    }
    if (pen_.closed && pen_.hoverPoint < 0 && pen_.hoverCurve.valid()) {
        const QPointF screen = worldToScreen(pen_.hoverCurve.worldPosition);
        painter.setPen(QPen(QColor(18, 20, 24), 2.0));
        painter.setBrush(QColor(120, 220, 135));
        painter.drawEllipse(screen, 4.0, 4.0);
    }
    painter.setPen(QPen(QColor(245, 65, 65), 2.0));
    for (const QPointF &crossing : pen_.crossings) {
        const QPointF screen = worldToScreen(crossing);
        painter.drawLine(screen + QPointF(-6, -6), screen + QPointF(6, 6));
        painter.drawLine(screen + QPointF(-6, 6), screen + QPointF(6, -6));
    }
    if (!pen_.error.isEmpty()) {
        painter.setPen(QColor(245, 85, 85));
        painter.drawText(QRectF(12, 12, width() - 24, 30), Qt::AlignLeft | Qt::AlignVCenter, pen_.error);
    }
    painter.restore();
}

QPainterPath ProjectCanvas::liningPreviewPath() const {
    if (lining_.points.isEmpty()) {
        return {};
    }
    QVector<PenPoint> points = lining_.points;
    if (!lining_.closed) {
        points.push_back({lining_.hoverWorld, PenPointKind::Hard});
    }
    points.front().kind = PenPointKind::Hard;
    points.back().kind = PenPointKind::Hard;
    return buildLiningPath(points).centerline;
}

void ProjectCanvas::drawLiningOverlay(QPainter &painter) {
    if (tool_ != QStringLiteral("lining") && !lining_.fillRunning) {
        return;
    }
    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (lining_.fillRunning) {
        const QString message = lining_.fillMessage.isEmpty()
            ? QStringLiteral("Filling lining path…")
            : lining_.fillMessage;
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
    const QPainterPath screenRibbon = camera_.matrix().map(ribbon);
    const QPainterPath screenCenterline = camera_.matrix().map(centerline);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 200, 50, lining_.closed ? 55 : 35));
    painter.drawPath(screenRibbon);

    QPen halo(QColor(15, 17, 20, 230), 4.0);
    halo.setCosmetic(true);
    painter.setPen(halo);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(screenCenterline);
    QPen centerPen(lining_.error.isEmpty() ? QColor(255, 200, 50) : QColor(235, 78, 78), 2.0);
    centerPen.setCosmetic(true);
    painter.setPen(centerPen);
    painter.drawPath(screenCenterline);

    for (int i = 0; i < lining_.points.size(); ++i) {
        const PenPoint &point = lining_.points[i];
        const QPointF screen = worldToScreen(point.position);
        const bool hovered = lining_.closed && i == lining_.hoverPoint;
        const double radius = 4.5 + (hovered ? 1.5 : 0.0);
        painter.setPen(QPen(hovered ? QColor(120, 220, 135) : QColor(18, 20, 24),
                            hovered ? 2.5 : 2.0));
        painter.setBrush(point.kind == PenPointKind::Hard
                             ? QColor(232, 72, 72)
                             : QColor(238, 240, 244));
        painter.drawEllipse(screen, radius, radius);
    }
    if (lining_.closed && lining_.hoverPoint < 0 && lining_.hoverCurve.valid()) {
        painter.setPen(QPen(QColor(18, 20, 24), 2.0));
        painter.setBrush(QColor(120, 220, 135));
        painter.drawEllipse(worldToScreen(lining_.hoverCurve.worldPosition), 4.0, 4.0);
    }
    if (!lining_.error.isEmpty()) {
        painter.setPen(QColor(245, 85, 85));
        painter.drawText(QRectF(12, 12, width() - 24, 30),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         lining_.error);
    }
    painter.restore();
}

namespace {

const QColor kSelectionAccentColor(255, 200, 50);
const QColor kOverlayHaloColor(0, 0, 0);
const QColor kSelectionFrameColor(255, 255, 255);
constexpr double kHoverHaloWidth = 4.0;
constexpr double kHoverAccentWidth = 2.0;
constexpr double kSelectionFrameHaloWidth = 3.0;
constexpr double kSelectionFrameLineWidth = 1.0;
constexpr double kHandleBorderWidth = 2.0;
constexpr int kMarqueeFillAlpha = 32;
const QColor kVisibilityViewportColor(70, 170, 230, 190);
const QColor kVisibilityLooseColor(255, 210, 80, 160);
constexpr double kVisibilityBorderHaloWidth = 3.0;
constexpr double kVisibilityBorderLineWidth = 1.0;
constexpr double kPositionLimitHalfWidth1080p = 1024.0;
constexpr double kPositionLimitHalfHeight1080p = 604.0;

const QColor kCursorHintBorderColor(0, 0, 0, 180);
const QColor kCursorHintFillColor(20, 20, 22, 210);
const QColor kCursorHintTextColor(245, 246, 248);
constexpr double kCursorHintCornerRadius = 4.0;
constexpr int kCursorHintPaddingX = 8;
constexpr int kCursorHintPaddingY = 6;
constexpr double kCursorHintCursorOffset = 18.0;
constexpr double kCursorHintScreenMargin = 4.0;

const QColor kEmptyCanvasTextColor(190, 194, 201);
const QColor kShapeCountTextColor(255, 255, 255);
const QColor kShapeCountOutlineColor(0, 0, 0);
constexpr double kShapeCountOutlineWidth = 3.0;
constexpr double kShapeCountMargin = 8.0;
constexpr double kShapeCountSupersampling = 2.0;

double rulerMajorStep(double pixelsPerWorldUnit) {
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

QString rulerLabel(double value, double majorStep) {
    const int decimals = majorStep >= 1.0
        ? 0
        : std::clamp(static_cast<int>(std::ceil(-std::log10(majorStep))), 0, 6);
    if (std::abs(value) < majorStep * 1e-6) {
        value = 0.0;
    }
    return QString::number(value, 'f', decimals);
}

} // namespace


void ProjectCanvas::clearCursorHint() {
    cursorHintLines_.clear();
}

void ProjectCanvas::setCursorHint(const QPointF &point, const QStringList &lines) {
    cursorHintPoint_ = point;
    cursorHintLines_ = lines;
}

void ProjectCanvas::drawCursorHint(QPainter &painter) {
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
    const int paddingX = kCursorHintPaddingX;
    const int paddingY = kCursorHintPaddingY;
    const int lineHeight = metrics.height();
    constexpr int sectionGap = 4;
    int totalHeight = sectionGap * (sections.size() - 1);
    for (const QStringList &lines : sections) {
        totalHeight += lines.size() * lineHeight + paddingY * 2;
    }
    const QSize hintSize(textWidth + paddingX * 2,
                         totalHeight);
    QPointF topLeft = cursorHintPoint_ + QPointF(kCursorHintCursorOffset, kCursorHintCursorOffset);
    topLeft.setX(std::min(topLeft.x(), static_cast<double>(width() - hintSize.width() - kCursorHintScreenMargin)));
    topLeft.setY(std::min(topLeft.y(), static_cast<double>(height() - hintSize.height() - kCursorHintScreenMargin)));
    topLeft.setX(std::max(topLeft.x(), kCursorHintScreenMargin));
    topLeft.setY(std::max(topLeft.y(), kCursorHintScreenMargin));
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
        hintPainter.setPen(QPen(kCursorHintBorderColor, 1));
        hintPainter.setBrush(kCursorHintFillColor);
        hintPainter.drawRoundedRect(QRectF(0.0, sectionY, hintSize.width(), sectionHeight),
                                    kCursorHintCornerRadius,
                                    kCursorHintCornerRadius);
        hintPainter.setPen(kCursorHintTextColor);
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

void ProjectCanvas::drawVisibilityBorders(QPainter &painter) {
    if (project_ == nullptr || (!options_.visibilityBordersEnabled && !options_.positionLimitBorderEnabled)) {
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
        painter.setPen(QPen(kOverlayHaloColor, kVisibilityBorderHaloWidth, style));
        painter.drawPolygon(polygon);
        painter.setPen(QPen(color, kVisibilityBorderLineWidth, style));
        painter.drawPolygon(polygon);
    };

    const QRectF viewportRect(-options_.borderResolution.width() * 0.5,
                              -options_.borderResolution.height() * 0.5,
                              options_.borderResolution.width(),
                              options_.borderResolution.height());
    if (options_.visibilityBordersEnabled) {
        drawWorldRect(viewportRect, kVisibilityViewportColor, Qt::SolidLine);
    }
    if (options_.positionLimitBorderEnabled) {
        const double scale = static_cast<double>(options_.borderResolution.height()) / 1080.0;
        const QRectF positionLimitRect(-kPositionLimitHalfWidth1080p * scale,
                                       -kPositionLimitHalfHeight1080p * scale,
                                       kPositionLimitHalfWidth1080p * 2.0 * scale,
                                       kPositionLimitHalfHeight1080p * 2.0 * scale);
        drawWorldRect(positionLimitRect, kVisibilityLooseColor, Qt::DashLine);
    }
}

void ProjectCanvas::drawRulersAndGuidelines(QPainter &painter) {
    if (project_ == nullptr) {
        return;
    }

    painter.save();
    painter.resetTransform();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setRenderHint(QPainter::TextAntialiasing, false);

    const QRectF contentRect(kRulerExtent, kRulerExtent,
                             std::max(0.0, width() - kRulerExtent),
                             std::max(0.0, height() - kRulerExtent));
    if (guidelines_.visible) {
        painter.save();
        painter.setClipRect(contentRect);
        QPen guidelinePen(guidelines_.color, 1.0);
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
    painter.fillRect(QRectF(0.0, 0.0, width(), kRulerExtent), rulerBackground);
    painter.fillRect(QRectF(0.0, kRulerExtent, kRulerExtent, height() - kRulerExtent), rulerBackground);
    painter.setPen(QPen(rulerDivider, 1.0));
    painter.drawLine(QPointF(kRulerExtent - 0.5, 0.0), QPointF(kRulerExtent - 0.5, height()));
    painter.drawLine(QPointF(0.0, kRulerExtent - 0.5), QPointF(width(), kRulerExtent - 0.5));

    QFont rulerFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    rulerFont.setPixelSize(10);
    rulerFont.setHintingPreference(QFont::PreferFullHinting);
    painter.setFont(rulerFont);
    painter.setPen(rulerText);
    const double pixelsPerWorldUnit = std::max(std::abs(camera_.matrix().m11()), 1e-12);
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
        painter.drawLine(QPointF(x, kRulerExtent), QPointF(x, kRulerExtent - tick));
        if (major) {
            painter.drawText(QRectF(std::round(x) + 3.0, 1.0, 72.0, kRulerExtent - 12.0),
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
        painter.drawLine(QPointF(kRulerExtent, y), QPointF(kRulerExtent - tick, y));
        if (major) {
            painter.save();
            painter.translate(kRulerExtent * 0.5 - 1.0, std::round(y));
            painter.rotate(-90.0);
            painter.drawText(QRectF(-36.0, -kRulerExtent * 0.5, 72.0, kRulerExtent - 12.0),
                             Qt::AlignCenter, rulerLabel(coordinate, majorStep));
            painter.restore();
        }
    }

    if (guidelines_.visible) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(guidelines_.color);
        for (double coordinate : project_->verticalGuidelines) {
            const double x = worldToScreen(QPointF(coordinate, 0.0)).x();
            if (x >= contentRect.left() && x <= contentRect.right()) {
                painter.drawPolygon(QPolygonF({QPointF(x - 4.0, kRulerExtent - 7.0),
                                               QPointF(x + 4.0, kRulerExtent - 7.0),
                                               QPointF(x, kRulerExtent - 1.0)}));
            }
        }
        for (double coordinate : project_->horizontalGuidelines) {
            const double y = worldToScreen(QPointF(0.0, coordinate)).y();
            if (y >= contentRect.top() && y <= contentRect.bottom()) {
                painter.drawPolygon(QPolygonF({QPointF(kRulerExtent - 7.0, y - 4.0),
                                               QPointF(kRulerExtent - 7.0, y + 4.0),
                                               QPointF(kRulerExtent - 1.0, y)}));
            }
        }
    }
    painter.restore();
}

void ProjectCanvas::drawOverlay(QPainter &painter) {
    painter.save();
    painter.resetTransform();
    painter.setClipRect(QRectF(kRulerExtent, kRulerExtent,
                               std::max(0.0, width() - kRulerExtent),
                               std::max(0.0, height() - kRulerExtent)));
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (carUnwrapVisible_ && !carUnwrapOverlay_.isNull()) {
        const QTransform imageToWorld(1.0, 0.0, 0.0, 1.0, -1024.0, -512.0);
        painter.save();
        painter.setTransform(imageToWorld * camera_.matrix(), false);
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
        painter.setPen(QPen(kOverlayHaloColor, kHoverHaloWidth));
        painter.drawPolygon(hoverPolygon_);
        painter.setPen(QPen(kSelectionAccentColor, kHoverAccentWidth));
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
            painter.setPen(QPen(kOverlayHaloColor, kSelectionFrameHaloWidth));
            painter.drawPolygon(boxPolygon);
            painter.setPen(QPen(kSelectionFrameColor, kSelectionFrameLineWidth));
            painter.drawPolygon(boxPolygon);
        }
        if (tool_ == QStringLiteral("transform")
            && (!isTransformDrag() || options_.displayAnchorsDuringTransformDrag)) {
            QVector<QPointF> handles = {
                topLeft, topRight, bottomLeft, bottomRight,
                toScreen.map(QPointF(lr.left(), lr.center().y())),
                toScreen.map(QPointF(lr.right(), lr.center().y())),
                toScreen.map(QPointF(lr.center().x(), lr.top())),
                toScreen.map(QPointF(lr.center().x(), lr.bottom())),
            }; {
                const QPointF topCenter = toScreen.map(QPointF(lr.center().x(), lr.top()));
                const QPointF inward = toScreen.map(QPointF(lr.center().x(), lr.top() + 1.0));
                QPointF up = topCenter - inward;
                const double upLen = std::hypot(up.x(), up.y());
                if (upLen > 1e-9) {
                    up /= upLen;
                    handles.push_back(topCenter + up * kSkewHandleOffset);
                }
            }
            for (const QPointF &handle : handles) {
                QRectF rect(handle.x() - kHandleHalf, handle.y() - kHandleHalf, kHandleHalf * 2.0, kHandleHalf * 2.0);
                painter.fillRect(rect, kSelectionFrameColor);
                painter.setPen(QPen(kOverlayHaloColor, kHandleBorderWidth));
                painter.drawRect(rect);
            }
        }
    }

    if (drag_.mode == DragMode::Marquee && drag_.marqueeRect.isValid()) {
        QColor marqueeFill = kSelectionAccentColor;
        marqueeFill.setAlpha(kMarqueeFillAlpha);
        painter.setBrush(marqueeFill);
        painter.setPen(QPen(kSelectionAccentColor, 1, Qt::DashLine));
        painter.drawRect(drag_.marqueeRect);
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
    const qreal padding = kShapeCountOutlineWidth + 1.0;
    const QSizeF imageSize(textWidth + padding * 2.0,
                           metrics.height() * countLines.size() + padding * 2.0);
    const qreal renderScale = devicePixelRatioF() * kShapeCountSupersampling;
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
        countPainter.strokePath(path, QPen(kShapeCountOutlineColor, kShapeCountOutlineWidth,
                                           Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        countPainter.fillPath(path, kShapeCountTextColor);
        baseline += metrics.height();
    }
    countPainter.end();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QPointF(kRulerExtent + kShapeCountMargin - padding,
                              kRulerExtent + kShapeCountMargin - padding),
                      countImage);

    drawPenOverlay(painter);
    drawLiningOverlay(painter);
    drawCursorHint(painter);
    painter.restore();
}

void ProjectCanvas::updateSelectionFlashState() {
    if (!flash_.enabled) {
        setFlashingLayerIds({});
        return;
    }
    QSet<QString> selected;
    if (state_ != nullptr) {
        selected = state_->selectedLayerIds();
    }
    setFlashingLayerIds(selected);
}

void ProjectCanvas::setFlashingLayerIds(const QSet<QString> &ids) {
    const QSet<QString> selected = flash_.enabled ? ids : QSet<QString>{};
    const bool selectionChanged = selected != flash_.layerIds;
    if (selectionChanged) {
        flash_.layerIds = selected;
        flash_.clock.restart();
    }
    if (flash_.layerIds.isEmpty()) {
        flash_.timer.stop();
    } else if (selectionChanged || !flash_.timer.isActive()) {
        scheduleSelectionFlashTimer();
    }
}

void ProjectCanvas::scheduleSelectionFlashTimer() {
    if (flash_.layerIds.isEmpty()) {
        flash_.timer.stop();
        return;
    }
    const qint64 elapsed = flash_.clock.elapsed() % kSelectionFlashPeriodMs;
    if (elapsed < kSelectionFlashDurationMs) {
        flash_.timer.start(kSelectionFlashFrameMs);
        return;
    }
    flash_.timer.start(static_cast<int>(kSelectionFlashPeriodMs - elapsed));
}

std::optional<double> ProjectCanvas::selectionFlashProgress() const {
    if (flash_.layerIds.isEmpty() || !flash_.clock.isValid()) {
        return std::nullopt;
    }
    const qint64 elapsed = flash_.clock.elapsed() % kSelectionFlashPeriodMs;
    if (elapsed >= kSelectionFlashDurationMs) {
        return std::nullopt;
    }
    return static_cast<double>(elapsed) / static_cast<double>(kSelectionFlashDurationMs);
}

double ProjectCanvas::selectionFlashHue() const {
    return selectionFlashProgress().value_or(-1.0);
}

double ProjectCanvas::selectionFlashStrength() const {
    const std::optional<double> progress = selectionFlashProgress();
    return progress.has_value() ? 0.18 + 0.72 * std::sin(*progress * kPi) : 0.0;
}

QImage ProjectCanvas::guideImage(const fh6::scene::GuideLayer &guide) const {
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

QString ProjectCanvas::sectionCanvasCacheKey() const {
    if (project_ == nullptr || state_ == nullptr || !project_->isLivery || state_->activeSectionId_.isEmpty() || size().isEmpty()) {
        return {};
    }
    return QStringLiteral("%1|%2x%3|%4|%5,%6,%7,%8,%9,%10|%11|%12|%13")
        .arg(state_->activeSectionId_)
        .arg(width())
        .arg(height())
        .arg(QString::number(devicePixelRatioF(), 'g', 12))
        .arg(QString::number(camera_.matrix().m11(), 'g', 17))
        .arg(QString::number(camera_.matrix().m12(), 'g', 17))
        .arg(QString::number(camera_.matrix().m21(), 'g', 17))
        .arg(QString::number(camera_.matrix().m22(), 'g', 17))
        .arg(QString::number(camera_.matrix().dx(), 'g', 17))
        .arg(QString::number(camera_.matrix().dy(), 'g', 17))
        .arg(options_.canvasColor.rgba())
        .arg(options_.guideLayersOnTop ? 1 : 0)
        .arg(options_.guideLayersVisible ? 1 : 0);
}

void ProjectCanvas::storeSectionCanvasCache(const QString &key) {
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

void ProjectCanvas::drawGuideLayers(QPainter &painter) {
    if (!options_.guideLayersVisible || sceneTree() == nullptr) {
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
        if (!(world * camera_.matrix()).mapRect(localRect).intersects(QRectF(rect()).adjusted(-1.0, -1.0, 1.0, 1.0))) {
            return true;
        }
        const QImage image = guideImage(guide);
        if (image.isNull()) {
            return true;
        }
        painter.save();
        painter.setOpacity(std::clamp(guide.opacity, 0.0, 1.0));
        painter.setTransform(world * camera_.matrix(), true);
        painter.drawImage(localRect, image);
        painter.restore();
        return true;
    }, /*reverse=*/false);
    painter.restore();
}

bool ProjectCanvas::createRegionsForSelectedGuide(int smallRegionMergeArea,
                                                  QString *message) {
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
    params.smallRegionMergeArea = std::max(0, smallRegionMergeArea);
    if (guide->preprocessColorCount > 0) {
        params.maxColorCount = guide->preprocessColorCount;
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
    region_.guideId = guide->id;
    region_.overlay = regions;
    ++region_.generation;
    region_.fills.clear();
    region_.fillSilhouettes.clear();
    region_.showFills = false;
    region_.hidden = false;
    update();
    if (message != nullptr) {
        *message = QStringLiteral("Created %1 regions (%2 colour, %3 lineart, %4 small merged)")
            .arg(regions.regions.size())
            .arg(regions.colorRegionCount)
            .arg(regions.lineartRegionCount)
            .arg(regions.mergedSmallRegionCount);
    }
    return true;
}

void ProjectCanvas::clearRegionOverlay() {
    if (region_.overlay.regions.isEmpty() && region_.guideId.isEmpty()) {
        return;
    }
    region_.guideId.clear();
    region_.overlay = RegionExtractionResult{};
    ++region_.generation;
    region_.fills.clear();
    region_.fillSilhouettes.clear();
    region_.showFills = false;
    region_.hidden = false;
    update();
}

bool ProjectCanvas::prepareRegionFillBatch(RegionFillBatchRequest *request,
                                           QString *message) const {
    if (request == nullptr) {
        if (message != nullptr) {
            *message = QStringLiteral("Region fill request is unavailable");
        }
        return false;
    }
    if (region_.overlay.regions.isEmpty()) {
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

    *request = RegionFillBatchRequest{};
    request->regions = region_.overlay;
    request->primitives = primitives;
    request->meshSources = buildPolygonMeshSources(geometry_);
    request->overlayGuideId = region_.guideId;
    request->overlayGeneration = region_.generation;
    return true;
}

namespace {

constexpr QRgb kSafeOnlyDifferenceColor = qRgba(255, 55, 55, 230);
constexpr QRgb kDangerousOnlyDifferenceColor = qRgba(30, 220, 255, 230);
constexpr QRgb kChangedColorDifferenceColor = qRgba(255, 215, 35, 230);

QImage renderRegionFillVariant(
    const QSize &imageSize,
    const QVector<RegionFillLayer> &fills,
    const QHash<int, QPainterPath> &silhouettes,
    RegionFillVariant variant) {
    QImage image(imageSize, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (const RegionFillLayer &fill : fills) {
        if (fill.variant != variant) {
            continue;
        }
        painter.setBrush(fill.color);
        for (const PenPlacement &placement : fill.placements) {
            const auto silhouette = silhouettes.constFind(placement.shapeId);
            if (silhouette != silhouettes.constEnd()) {
                painter.drawPath(placement.transform.map(silhouette.value()));
            }
        }
    }
    painter.end();

    return image;
}

QImage regionFillDifferenceHeatmap(
    const QImage &safe,
    const QImage &dangerous,
    int *differencePixelCount) {
    if (differencePixelCount != nullptr) {
        *differencePixelCount = 0;
    }
    if (safe.isNull() || dangerous.isNull() || safe.size() != dangerous.size()) {
        return {};
    }
    QImage heatmap(safe.size(), QImage::Format_ARGB32);
    heatmap.fill(Qt::transparent);
    int differences = 0;
    for (int y = 0; y < safe.height(); ++y) {
        const QRgb *safeRow =
            reinterpret_cast<const QRgb *>(safe.constScanLine(y));
        const QRgb *dangerousRow =
            reinterpret_cast<const QRgb *>(dangerous.constScanLine(y));
        QRgb *heatmapRow = reinterpret_cast<QRgb *>(heatmap.scanLine(y));
        for (int x = 0; x < safe.width(); ++x) {
            if (safeRow[x] == dangerousRow[x]) {
                continue;
            }
            ++differences;
            const bool safeVisible = qAlpha(safeRow[x]) > 0;
            const bool dangerousVisible = qAlpha(dangerousRow[x]) > 0;
            if (safeVisible && !dangerousVisible) {
                heatmapRow[x] = kSafeOnlyDifferenceColor;
            } else if (!safeVisible && dangerousVisible) {
                heatmapRow[x] = kDangerousOnlyDifferenceColor;
            } else {
                heatmapRow[x] = kChangedColorDifferenceColor;
            }
        }
    }
    if (differencePixelCount != nullptr) {
        *differencePixelCount = differences;
    }

    return heatmap;
}

} // namespace

RegionFillBatchResult computeRegionFills(
    const RegionFillBatchRequest &request,
    const std::function<void(const QString &, int, int)> &progress,
    const std::function<bool()> &cancelled) {
    RegionFillBatchResult result;
    result.overlayGuideId = request.overlayGuideId;
    result.overlayGeneration = request.overlayGeneration;
    const RegionExtractionResult &regionOverlay = request.regions;
    const QVector<PenPrimitive> &primitives = request.primitives;
    if (regionOverlay.regions.isEmpty() || primitives.isEmpty()) {
        result.error = QStringLiteral("Region fill input is empty");
        return result;
    }

    QHash<int, QPainterPath> silhouettes;
    for (const PenPrimitive &primitive : primitives) {
        silhouettes.insert(primitive.shapeId, primitive.silhouette);
    }
    const double tolerance = std::max(1.0,
                                      std::min(regionOverlay.imageSize.width(),
                                               regionOverlay.imageSize.height()) * 0.004);

    QVector<RegionFillLayer> fills;
    int filled = 0;
    int failed = 0;
    int timedOut = 0;
    int placementCount = 0;
    int meshFallbacks = 0;
    int sourceOutlineFallbacks = 0;
    int softRunRetries = 0;
    int baselineRetries = 0;
    int removedSoftPoints = 0;
    int coreEllipseCount = 0;
    int safeRdpFills = 0;
    int dangerousRdpFills = 0;
    QHash<QString, int> failureReasons;
    constexpr qint64 kRegionBudgetMs = 3000;
    const PolygonMeshSources &meshSources = request.meshSources;
    constexpr double kFallbackSimplifyEpsilon = 0.45;
    constexpr double kCyclicRdpEpsilon = 1.9;
    constexpr int kCyclicRdpCurveSamples = 32;
    const auto globallyCancelled = [&cancelled]() {
        return cancelled && cancelled();
    };
    struct FillUnit {
        QColor color;
        QPainterPath outline;
        QVector<int> sourceIndices;
        QVector<int> absorbedIndices;
        double area = 0.0;
        int drawOrder = -1;
        RegionFillVariant variant = RegionFillVariant::Safe;
    };
    QElapsedTimer planningClock;
    planningClock.start();
    const RegionLayerPlanVariants layerPlans =
        buildRegionLayerPlanVariants(regionOverlay, progress, globallyCancelled);
    const qint64 planningElapsedMs = planningClock.elapsed();
    if (layerPlans.safe.cancelled || layerPlans.dangerous.cancelled
        || globallyCancelled()) {
        result.cancelled = true;
        return result;
    }
    QVector<FillUnit> units;
    units.reserve(layerPlans.safe.units.size() + layerPlans.dangerous.units.size());
    const auto appendPlanUnits = [&units](const RegionLayerPlan &plan,
                                          RegionFillVariant variant) {
        for (int drawOrder = 0; drawOrder < plan.units.size(); ++drawOrder) {
            const RegionLayerUnit &unit = plan.units[drawOrder];
            units.push_back(FillUnit{unit.color, unit.outline,
                                     unit.sourceRegionIndices,
                                     unit.absorbedRegionIndices, unit.area,
                                     drawOrder, variant});
        }
    };
    appendPlanUnits(layerPlans.safe, RegionFillVariant::Safe);
    appendPlanUnits(layerPlans.dangerous, RegionFillVariant::Dangerous);
    QStringList log;
    log << QStringLiteral("Fill Regions diagnostic - image %1x%2, tolerance %3, "
                          "%4 safe + %5 dangerous planned units, planning %6 ms")
               .arg(regionOverlay.imageSize.width())
               .arg(regionOverlay.imageSize.height())
               .arg(tolerance, 0, 'f', 3)
               .arg(layerPlans.safe.units.size())
               .arg(layerPlans.dangerous.units.size())
               .arg(planningElapsedMs);
    log << QStringLiteral("Safe/Dangerous contour mode: cyclic closed RDP epsilon=%1, "
                          "%2 curve samples, direct polygon mesh; Pen fitting is "
                          "retained as recovery")
               .arg(kCyclicRdpEpsilon, 0, 'f', 1)
               .arg(kCyclicRdpCurveSamples);
    const QString layerLogPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/region_layer.log");
    QFile layerLogFile(layerLogPath);
    if (layerLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&layerLogFile);
        const auto writePlan = [&stream](const char *name,
                                         const RegionLayerPlan &plan) {
            stream << '[' << name << "]\n"
                   << "input_regions=" << plan.inputRegionCount << '\n'
                   << "planned_units=" << plan.units.size() << '\n'
                   << "same_color_merges=" << plan.sameColorMergeCount << '\n'
                   << "nearby_same_color_merges="
                   << plan.nearbySameColorMergeCount << '\n'
                   << "edge_component_merges="
                   << plan.edgeComponentMergeCount << '\n'
                   << "hierarchical_contour_merges="
                   << plan.hierarchicalContourMergeCount << '\n'
                   << "nearby_conflict_rejections="
                   << plan.nearbyConflictRejectCount << '\n'
                   << "nearby_foreign_owner_rejections="
                   << plan.nearbyForeignOwnerRejectCount << '\n'
                   << "adjacent_conflict_rejections="
                   << plan.adjacentConflictRejectCount << '\n'
                   << "containment_conflict_rejections="
                   << plan.containmentConflictRejectCount << '\n'
                   << "absorption_conflict_rejections="
                   << plan.absorptionConflictRejectCount << '\n'
                   << "suppressed_operations="
                   << plan.suppressedOperationCount << '\n'
                   << "dependency_cycle_rebuilds="
                   << plan.dependencyCycleRebuildCount << '\n'
                   << "conflict_isolated_sources="
                   << plan.conflictIsolatedSourceCount << '\n'
                   << "absorbed_regions=" << plan.absorbedRegionCount << '\n'
                   << "large_contained_absorptions="
                   << plan.largeContainedAbsorptionCount << '\n'
                   << "morphological_closings="
                   << plan.morphologicalClosingCount << '\n'
                   << "convex_simplifications="
                   << plan.convexSimplificationCount << '\n'
                   << "geometry_point_reductions="
                   << plan.geometryPointReductionCount << '\n'
                   << "dangerous_cycle_breaks="
                   << plan.dangerousCycleBreakCount << '\n'
                   << "ordering_edges=" << plan.orderingEdgeCount << '\n'
                   << "validation_mismatch_pixels="
                   << plan.validationMismatchPixels << '\n'
                   << "fallback=" << (plan.fallback ? "yes" : "no") << '\n';
            if (!plan.fallbackReason.isEmpty()) {
                stream << "fallback_reason=" << plan.fallbackReason << '\n';
            }
            stream << '\n' << plan.diagnostics.join(QLatin1Char('\n')) << "\n\n";
        };
        stream << "Region layer plan variants diagnostic\n\n";
        writePlan("safe", layerPlans.safe);
        writePlan("dangerous", layerPlans.dangerous);
        layerLogFile.close();
        qWarning().noquote() << "Region layer log written to" << layerLogPath;
    } else {
        qWarning().noquote() << "Could not write region layer log to" << layerLogPath;
    }
    const auto unitSourcesText = [&regionOverlay](const FillUnit &unit) {
        QStringList values;
        values.reserve(unit.sourceIndices.size());
        for (const int sourceIndex : unit.sourceIndices) {
            if (sourceIndex >= 0 && sourceIndex < regionOverlay.regions.size()) {
                values.push_back(QStringLiteral("%1/label-%2")
                                     .arg(sourceIndex)
                                     .arg(regionOverlay.regions[sourceIndex].id));
            }
        }

        return values.join(QLatin1Char(','));
    };
    const auto absorbedSourcesText = [&regionOverlay](const FillUnit &unit) {
        QStringList values;
        values.reserve(unit.absorbedIndices.size());
        for (const int sourceIndex : unit.absorbedIndices) {
            if (sourceIndex >= 0 && sourceIndex < regionOverlay.regions.size()) {
                values.push_back(QStringLiteral("%1/label-%2")
                                     .arg(sourceIndex)
                                     .arg(regionOverlay.regions[sourceIndex].id));
            }
        }

        return values.join(QLatin1Char(','));
    };
    for (int i = 0; i < units.size(); ++i) {
        const QString variantName = units[i].variant == RegionFillVariant::Safe
            ? QStringLiteral("safe") : QStringLiteral("dangerous");
        log << QStringLiteral("%1 plan #%2 color=%3 sources=%4 absorbed=%5 area=%6")
                   .arg(variantName)
                   .arg(units[i].drawOrder)
                   .arg(units[i].color.name(QColor::HexRgb))
                   .arg(unitSourcesText(units[i]))
                   .arg(absorbedSourcesText(units[i]))
                   .arg(units[i].area, 0, 'f', 0);
    }
    result.totalRegions = units.size();
    if (progress) {
        progress(QStringLiteral("Filling regions"), 0, result.totalRegions);
    }
    if (units.isEmpty()) {
        result.error = QStringLiteral("No colour regions are available to fill");
        return result;
    }
    int biggestUnitIndex = 0;
    for (int i = 1; i < units.size(); ++i) {
        if (units[i].area > units[biggestUnitIndex].area) {
            biggestUnitIndex = i;
        }
    }

    struct UnitWorkResult {
        PenFillResult fit;
        QString via = QStringLiteral("failed");
        QString cyclicRdpError;
        QString penError;
        RegionFillContourStats contourStats;
        QVector<PenPoint> optimizedPenPoints;
        QPolygonF optimizedContour;
        qint64 elapsedMs = 0;
        int fallbackInputPoints = 0;
        int fallbackMeshPoints = 0;
        int cyclicRdpInputPoints = 0;
        int cyclicRdpOutputPoints = 0;
        bool timedOut = false;
        bool meshFallback = false;
    };
    std::vector<UnitWorkResult> workResults(static_cast<size_t>(units.size()));
    const auto fitSucceeded = [](const PenFillResult &fit) {
        return fit.error.isEmpty() && !fit.placements.isEmpty() && !fit.cancelled;
    };
    std::atomic<int> nextUnit{0};
    std::atomic<int> completedUnits{0};
    const int availableThreads = std::max(1, QThread::idealThreadCount());
    const int requestedWorkers = std::max(1, availableThreads / 2);
    const int workerCount = std::min(requestedWorkers, static_cast<int>(units.size()));
    log << QStringLiteral("Workers: %1 of %2 available CPU threads")
               .arg(workerCount)
               .arg(availableThreads);

    QThreadPool fillPool;
    fillPool.setMaxThreadCount(workerCount);
    for (int worker = 0; worker < workerCount; ++worker) {
        fillPool.start([&, worker]() {
            Q_UNUSED(worker);
            while (true) {
                const int i = nextUnit.fetch_add(1, std::memory_order_relaxed);
                if (i >= units.size()) {
                    return;
                }
                UnitWorkResult &work = workResults[static_cast<size_t>(i)];
                if (!globallyCancelled()) {
                    const FillUnit &unit = units[i];
                    QElapsedTimer clock;
                    clock.start();
                    const auto unitCancelled = [&clock, &globallyCancelled]() {
                        return globallyCancelled() || clock.elapsed() > kRegionBudgetMs;
                    };
                    QPolygonF optimizedContour;
                    const QPolygonF sourceContour = regionOuterContour(
                        unit.outline, kCyclicRdpCurveSamples);
                    const QPolygonF simplifiedContour = simplifyClosedPolygonCyclic(
                        sourceContour, kCyclicRdpEpsilon);
                    work.cyclicRdpInputPoints = sourceContour.size();
                    work.cyclicRdpOutputPoints = simplifiedContour.size();
                    work.contourStats.originalPointCount =
                        regionOutlinePenPointCount(unit.outline);
                    work.contourStats.optimizedPointCount = simplifiedContour.size();
                    work.contourStats.flattenedPointCount = simplifiedContour.size();
                    work.contourStats.dssim =
                        std::numeric_limits<double>::quiet_NaN();
                    work.via = unit.variant == RegionFillVariant::Safe
                        ? QStringLiteral("safe-rdp-mesh")
                        : QStringLiteral("dangerous-rdp-mesh");
                    work.fit = fillPolygonMesh(simplifiedContour, meshSources,
                                               unitCancelled);
                    if (i == biggestUnitIndex) {
                        work.optimizedContour = simplifiedContour;
                        work.optimizedPenPoints.reserve(simplifiedContour.size());
                        for (const QPointF &point : simplifiedContour) {
                            work.optimizedPenPoints.push_back(
                                PenPoint{point, PenPointKind::Hard});
                        }
                    }
                    if (!fitSucceeded(work.fit)) {
                        work.cyclicRdpError = work.fit.error;
                    }
                    if (!fitSucceeded(work.fit) && !globallyCancelled()) {
                        work.via = unit.variant == RegionFillVariant::Dangerous
                            ? QStringLiteral("dangerous-rdp-pen-fallback")
                            : QStringLiteral("safe-rdp-pen-fallback");
                        work.fit = fillRegionOutline(
                            unit.outline, primitives, tolerance, unitCancelled,
                            &optimizedContour, &work.contourStats,
                            i == biggestUnitIndex ? &work.optimizedPenPoints : nullptr,
                            regionOverlay.imageSize);
                        if (i == biggestUnitIndex) {
                            work.optimizedContour = optimizedContour;
                        }
                        if (work.contourStats.softRunRetry && fitSucceeded(work.fit)) {
                            work.via = QStringLiteral("hard-only-pen-retry");
                        } else if (work.contourStats.baselineRetry
                                   && fitSucceeded(work.fit)) {
                            work.via = QStringLiteral("baseline-pen-retry");
                        }
                    }
                    if (work.fit.cancelled && !globallyCancelled()) {
                        work.timedOut = true;
                    }

                    if (!fitSucceeded(work.fit) && !globallyCancelled()) {
                        work.penError = work.fit.error;
                        work.meshFallback = true;
                        work.via = work.timedOut
                            ? QStringLiteral("optimized-mesh-timeout")
                            : QStringLiteral("optimized-mesh-fallback");
                        QPolygonF fallbackContour = optimizedContour.size() >= 3
                            ? optimizedContour : regionOuterContour(unit.outline);
                        if (fallbackContour.size() >= 3) {
                            if (optimizedContour.size() < 3) {
                                work.via = QStringLiteral("source-outline-mesh-fallback");
                            }
                            work.fallbackInputPoints = fallbackContour.size();
                            QPolygonF meshContour = simplifyClosedPolygon(
                                fallbackContour, kFallbackSimplifyEpsilon);
                            if (!buildPolygonContour(meshContour).valid()) {
                                meshContour = fallbackContour;
                            }
                            work.fallbackMeshPoints = meshContour.size();
                            work.fit = fillPolygonMesh(meshContour, meshSources,
                                                       globallyCancelled);
                            if (!fitSucceeded(work.fit)
                                && meshContour.size() != fallbackContour.size()
                                && !globallyCancelled()) {
                                work.fallbackMeshPoints = fallbackContour.size();
                                work.fit = fillPolygonMesh(fallbackContour, meshSources,
                                                            globallyCancelled);
                            }
                        } else if (work.fit.error.isEmpty()) {
                            work.fit = PenFillResult{};
                            work.fit.error = QStringLiteral("Optimized contour is unavailable");
                        }
                    }
                    if (!fitSucceeded(work.fit)) {
                        work.via = QStringLiteral("failed");
                    }
                    work.elapsedMs = clock.elapsed();
                }
                const int done = completedUnits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (progress) {
                    progress(QStringLiteral("Filling regions"),
                             done, result.totalRegions);
                }
            }
        });
    }
    fillPool.waitForDone();
    result.completedRegions = completedUnits.load(std::memory_order_relaxed);
    if (globallyCancelled()) {
        result.cancelled = true;
        return result;
    }

    const FillUnit &biggestUnit = units[biggestUnitIndex];
    const UnitWorkResult &biggestWork =
        workResults[static_cast<size_t>(biggestUnitIndex)];
    log << QStringLiteral("Biggest region points: region #%1 (sources %2) %3 area=%4, "
                          "original=%5 optimized=%6 flattened=%7 removed-hard=%8, "
                          "removed-soft=%9, hard-only-retry=%10, "
                          "optimization-skipped=%11, DSSIM=%12")
               .arg(biggestUnitIndex)
               .arg(unitSourcesText(biggestUnit))
               .arg(biggestUnit.color.name())
               .arg(biggestUnit.area, 0, 'f', 0)
               .arg(biggestWork.contourStats.originalPointCount)
               .arg(biggestWork.contourStats.optimizedPointCount)
               .arg(biggestWork.contourStats.flattenedPointCount)
               .arg(biggestWork.contourStats.removedHardPoints)
               .arg(biggestWork.contourStats.removedSoftPoints)
               .arg(biggestWork.contourStats.softRunRetry
                        ? QStringLiteral("yes") : QStringLiteral("no"))
               .arg(biggestWork.contourStats.optimizationSkipped
                        ? QStringLiteral("yes") : QStringLiteral("no"))
               .arg(biggestWork.contourStats.dssim, 0, 'g', 8);

    const QString pointsLogPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/region_points.log");
    QFile pointsLogFile(pointsLogPath);
    if (pointsLogFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        QTextStream stream(&pointsLogFile);
        stream.setRealNumberNotation(QTextStream::SmartNotation);
        stream.setRealNumberPrecision(17);
        stream << "Largest Fill Region path points\n"
               << "region_index=" << biggestUnitIndex << '\n'
               << "source_indices=" << unitSourcesText(biggestUnit) << '\n'
               << "color=" << biggestUnit.color.name() << '\n'
               << "area=" << biggestUnit.area << '\n'
               << "original_pen_point_count="
               << biggestWork.contourStats.originalPointCount << '\n'
               << "optimized_pen_point_count="
               << biggestWork.contourStats.optimizedPointCount << '\n'
               << "removed_hard_point_count="
               << biggestWork.contourStats.removedHardPoints << '\n'
               << "removed_soft_point_count="
               << biggestWork.contourStats.removedSoftPoints << '\n'
               << "hard_only_retry="
               << (biggestWork.contourStats.softRunRetry ? "yes" : "no") << '\n'
               << "flattened_point_count="
               << biggestWork.contourStats.flattenedPointCount << '\n'
               << "dssim=" << biggestWork.contourStats.dssim << "\n\n";

        const auto elementTypeName = [](QPainterPath::ElementType type) {
            switch (type) {
            case QPainterPath::MoveToElement:
                return QStringLiteral("move");
            case QPainterPath::LineToElement:
                return QStringLiteral("line");
            case QPainterPath::CurveToElement:
                return QStringLiteral("curve-control-1");
            case QPainterPath::CurveToDataElement:
                return QStringLiteral("curve-data");
            }
            return QStringLiteral("unknown");
        };

        stream << "[source_qpainter_path]\n"
               << "count=" << biggestUnit.outline.elementCount() << '\n'
               << "index,type,x,y\n";
        for (int i = 0; i < biggestUnit.outline.elementCount(); ++i) {
            const QPainterPath::Element element = biggestUnit.outline.elementAt(i);
            stream << i << ',' << elementTypeName(element.type) << ','
                   << element.x << ',' << element.y << '\n';
        }

        stream << "\n[optimized_pen_points]\n"
               << "count=" << biggestWork.optimizedPenPoints.size() << '\n'
               << "index,kind,x,y\n";
        for (int i = 0; i < biggestWork.optimizedPenPoints.size(); ++i) {
            const PenPoint &point = biggestWork.optimizedPenPoints[i];
            stream << i << ','
                   << (point.kind == PenPointKind::Hard ? "hard" : "soft") << ','
                   << point.position.x() << ',' << point.position.y() << '\n';
        }

        stream << "\n[flattened_optimized_contour]\n"
               << "count=" << biggestWork.optimizedContour.size() << '\n'
               << "index,x,y\n";
        for (int i = 0; i < biggestWork.optimizedContour.size(); ++i) {
            const QPointF &point = biggestWork.optimizedContour[i];
            stream << i << ',' << point.x() << ',' << point.y() << '\n';
        }
        pointsLogFile.close();
        qWarning().noquote() << "Largest region points log written to" << pointsLogPath;
    } else {
        qWarning().noquote() << "Could not write largest region points log to"
                             << pointsLogPath;
    }

    QVector<RegionFillLayer> unitFills(units.size());
    for (int i = 0; i < units.size(); ++i) {
        const FillUnit &unit = units[i];
        UnitWorkResult &work = workResults[static_cast<size_t>(i)];
        const int unitCoreEllipseCount = static_cast<int>(std::count_if(
            work.fit.placements.cbegin(), work.fit.placements.cend(),
            [](const PenPlacement &placement) {
                return placement.coreEllipse;
            }));
        coreEllipseCount += unitCoreEllipseCount;
        if (work.timedOut) {
            ++timedOut;
        }
        if (work.meshFallback && fitSucceeded(work.fit)) {
            ++meshFallbacks;
            if (work.via == QStringLiteral("source-outline-mesh-fallback")) {
                ++sourceOutlineFallbacks;
            }
        }
        if (work.contourStats.softRunRetry && fitSucceeded(work.fit)) {
            ++softRunRetries;
        }
        if (work.contourStats.baselineRetry && fitSucceeded(work.fit)) {
            ++baselineRetries;
        }
        if (fitSucceeded(work.fit)) {
            if (work.via == QStringLiteral("safe-rdp-mesh")) {
                ++safeRdpFills;
            } else if (work.via == QStringLiteral("dangerous-rdp-mesh")) {
                ++dangerousRdpFills;
            }
        }
        if (fitSucceeded(work.fit)) {
            RegionFillLayer &layer = unitFills[i];
            layer.color = unit.color;
            layer.area = unit.area;
            layer.placements = std::move(work.fit.placements);
            layer.drawOrder = unit.drawOrder;
            layer.variant = unit.variant;
            placementCount += layer.placements.size();
            removedSoftPoints += work.contourStats.removedSoftPoints;
            ++filled;
        } else {
            ++failed;
            const QString reason = work.fit.error.isEmpty()
                ? QStringLiteral("empty result") : work.fit.error;
            failureReasons[reason] += 1;
        }
        QString detail;
        if (!work.penError.isEmpty()) {
            detail += QStringLiteral(" [Pen: %1]").arg(work.penError);
        }
        if (!work.cyclicRdpError.isEmpty()) {
            detail += QStringLiteral(" [cyclic RDP: %1]")
                          .arg(work.cyclicRdpError);
        }
        if (!work.fit.error.isEmpty() && work.fit.error != work.penError) {
            detail += QStringLiteral(" [error: %1]").arg(work.fit.error);
        }
        if (work.meshFallback && work.fallbackInputPoints > 0) {
            detail += QStringLiteral(" [mesh points: %1 -> %2]")
                          .arg(work.fallbackInputPoints)
                          .arg(work.fallbackMeshPoints);
        }
        if (work.cyclicRdpInputPoints > 0) {
            detail += QStringLiteral(" [cyclic RDP %1: %2 -> %3]")
                          .arg(kCyclicRdpEpsilon, 0, 'f', 1)
                          .arg(work.cyclicRdpInputPoints)
                          .arg(work.cyclicRdpOutputPoints);
        }
        if (unitCoreEllipseCount > 0) {
            detail += QStringLiteral(" [core ellipses: %1]").arg(unitCoreEllipseCount);
        }
        if (work.via == QStringLiteral("safe-rdp-mesh")
            || work.via == QStringLiteral("dangerous-rdp-mesh")) {
            detail += QStringLiteral(" [points: %1 Pen controls -> %2 polygon vertices, "
                                     "DSSIM not evaluated during fill]")
                          .arg(work.contourStats.originalPointCount)
                          .arg(work.contourStats.optimizedPointCount);
        } else {
            detail += QStringLiteral(
                          " [points: %1 -> %2, hard -%3, soft -%4, DSSIM %5]")
                          .arg(work.contourStats.originalPointCount)
                          .arg(work.contourStats.optimizedPointCount)
                          .arg(work.contourStats.removedHardPoints)
                          .arg(work.contourStats.removedSoftPoints)
                          .arg(work.contourStats.dssim, 0, 'g', 8);
        }
        const QString variantName = unit.variant == RegionFillVariant::Safe
            ? QStringLiteral("safe") : QStringLiteral("dangerous");
        log << QStringLiteral("%1 region #%2 (sources %3) %4 %5: "
                              "area=%6 -> %7 shapes, %8 ms%9")
                   .arg(variantName)
                   .arg(unit.drawOrder)
                   .arg(unitSourcesText(unit))
                   .arg(unit.color.name())
                   .arg(work.via)
                   .arg(unit.area, 0, 'f', 0)
                   .arg(unitFills[i].placements.size())
                   .arg(work.elapsedMs)
                   .arg(detail);
    }

    for (RegionFillLayer &layer : unitFills) {
        if (!layer.placements.isEmpty()) {
            fills.push_back(std::move(layer));
        }
    }
    sortRegionFillLayersByDrawOrder(&fills);
    const bool drawOrderPreserved = std::is_sorted(
        fills.cbegin(), fills.cend(),
        [](const RegionFillLayer &left, const RegionFillLayer &right) {
            if (left.variant != right.variant) {
                return left.variant == RegionFillVariant::Safe;
            }
            return left.drawOrder < right.drawOrder;
        });
    log << QStringLiteral("Parallel draw order preserved: %1")
               .arg(drawOrderPreserved ? QStringLiteral("yes") : QStringLiteral("no"));
    const QImage safeRendered = renderRegionFillVariant(
        regionOverlay.imageSize, fills, silhouettes, RegionFillVariant::Safe);
    const QImage dangerousRendered = renderRegionFillVariant(
        regionOverlay.imageSize, fills, silhouettes, RegionFillVariant::Dangerous);
    result.differenceHeatmap = regionFillDifferenceHeatmap(
        safeRendered, dangerousRendered, &result.differencePixelCount);
    log << QStringLiteral("Safe/Dangerous heatmap difference pixels: %1")
               .arg(result.differencePixelCount);
    log << QStringLiteral("Summary: %1 planned units filled (%2 Safe and %3 Dangerous "
                          "cyclic-RDP meshes, %4 hard-only Pen retries, %5 baseline "
                          "Pen retries, %6 mesh fallbacks including %7 source-outline "
                          "recoveries), %8 shapes including %9 core ellipses, %10 soft "
                          "controls removed, %11 failed, %12 timed out")
               .arg(filled)
               .arg(safeRdpFills)
               .arg(dangerousRdpFills)
               .arg(softRunRetries)
               .arg(baselineRetries)
               .arg(meshFallbacks)
               .arg(sourceOutlineFallbacks)
               .arg(placementCount)
               .arg(coreEllipseCount)
               .arg(removedSoftPoints)
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
        result.error = QStringLiteral("No regions could be filled%1").arg(reasonSuffix);
        return result;
    }
    const int safeFillCount = static_cast<int>(std::count_if(
        fills.cbegin(), fills.cend(), [](const RegionFillLayer &fill) {
            return fill.variant == RegionFillVariant::Safe;
        }));
    const int dangerousFillCount = fills.size() - safeFillCount;
    result.summary = QStringLiteral("Filled %1 safe and %2 dangerous layer units "
                                    "with %3 shapes (%4 failed, %5 timed out)%6")
        .arg(safeFillCount)
        .arg(dangerousFillCount)
        .arg(placementCount)
        .arg(failed)
        .arg(timedOut)
        .arg(reasonSuffix);
    result.fills = std::move(fills);
    result.silhouettes = std::move(silhouettes);
    return result;
}

bool ProjectCanvas::applyRegionFillBatch(RegionFillBatchResult result,
                                         QString *message) {
    if (result.cancelled) {
        if (message != nullptr) {
            *message = QStringLiteral("Region Fill cancelled");
        }
        return false;
    }
    if (result.overlayGeneration != region_.generation
        || result.overlayGuideId != region_.guideId) {
        if (message != nullptr) {
            *message = QStringLiteral("Region Fill result is stale; create regions again");
        }
        return false;
    }
    if (!result.error.isEmpty() || result.fills.isEmpty()) {
        if (message != nullptr) {
            *message = result.error.isEmpty()
                ? QStringLiteral("Region Fill produced no layers") : result.error;
        }
        return false;
    }
    region_.fills = std::move(result.fills);
    region_.fillSilhouettes = std::move(result.silhouettes);
    region_.showFills = true;
    update();
    if (message != nullptr) {
        *message = result.summary;
    }
    return true;
}

void ProjectCanvas::clearRegionFills() {
    if (region_.fills.isEmpty() && !region_.showFills) {
        return;
    }
    region_.fills.clear();
    region_.fillSilhouettes.clear();
    region_.showFills = false;
    update();
}

QVector<GeneratedRegionVariant> ProjectCanvas::regionFillWorldVariants() {
    QVector<GeneratedRegionVariant> result;
    if (region_.fills.isEmpty() || region_.guideId.isEmpty()) {
        return result;
    }
    const QSize imageSize = region_.overlay.imageSize;
    if (imageSize.width() < 1 || imageSize.height() < 1) {
        return result;
    }

    // Same image-pixel -> world mapping the overlay uses to draw, minus the
    // world->screen step: image -> guide local -> world.
    QTransform guideWorld;
    QSizeF guideSize;
    bool found = false;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (guide.id != region_.guideId || !isSectionActive(sectionGroupId)) {
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
    result.push_back({QStringLiteral("Safe"), {}, true});
    result.push_back({QStringLiteral("Dangerous"), {}, false});

    for (const RegionFillLayer &fill : region_.fills) {
        GeneratedRegionVariant &variant = fill.variant == RegionFillVariant::Safe
            ? result[0] : result[1];
        GeneratedRegionGroup group;
        group.shapes.reserve(fill.placements.size());
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
            group.shapes.push_back(shape);
        }
        if (!group.shapes.isEmpty()) {
            variant.regions.push_back(std::move(group));
        }
    }
    return result;
}

void ProjectCanvas::hideRegionOverlay() {
    region_.hidden = true;
    region_.showFills = false;
    update();
}

void ProjectCanvas::drawRegionOverlay(QPainter &painter) {
    if (region_.overlay.regions.isEmpty() || region_.guideId.isEmpty() || region_.hidden) {
        return;
    }
    const QSize imageSize = region_.overlay.imageSize;
    if (imageSize.width() < 1 || imageSize.height() < 1) {
        return;
    }

    QTransform guideWorld;
    QSizeF guideSize;
    bool found = false;
    forEachSceneGuide([&](const fh6::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
        if (guide.id != region_.guideId || !isSectionActive(sectionGroupId)) {
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
    const QTransform imageToScreen = imageToLocal * guideWorld * camera_.matrix();

    static const QColor kDebugPalette[] = {
        QColor(228, 87, 86), QColor(88, 163, 222), QColor(126, 194, 106),
        QColor(240, 179, 74), QColor(163, 122, 214), QColor(74, 204, 196),
        QColor(232, 130, 197), QColor(150, 158, 170),
    };
    constexpr int kDebugPaletteCount = int(sizeof(kDebugPalette) / sizeof(kDebugPalette[0]));

    painter.save();
    painter.setTransform(imageToScreen, true);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (region_.showFills && !region_.fills.isEmpty()) {
        painter.setPen(Qt::NoPen);
        for (const RegionFillLayer &fill : region_.fills) {
            if (fill.variant != RegionFillVariant::Safe) {
                continue;
            }
            painter.setBrush(fill.color);
            for (const PenPlacement &placement : fill.placements) {
                const auto silhouette = region_.fillSilhouettes.constFind(placement.shapeId);
                if (silhouette != region_.fillSilhouettes.constEnd()) {
                    painter.drawPath(placement.transform.map(silhouette.value()));
                }
            }
        }
    } else {
        QPen outlinePen(QColor(20, 22, 26, 180), 1.0);
        outlinePen.setCosmetic(true);
        for (const ExtractedRegion &region : region_.overlay.regions) {
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

    painter.setPen(Qt::NoPen);
    for (const ExtractedRegion &region : region_.overlay.regions) {
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

void ProjectCanvas::initializeGL() {
    renderer_.initialize();
    rendererGeometryDirty_ = true;
}

void ProjectCanvas::paintGL() {
    if (project_ == nullptr) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), options_.canvasColor);
        painter.setPen(kEmptyCanvasTextColor);
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
    const bool flashActive = flashProgress.has_value() && !flash_.layerIds.isEmpty();
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
    painter.fillRect(rect(), options_.canvasColor);
    if (!options_.guideLayersOnTop) {
        drawGuideLayers(painter);
    }

    const qreal dpr = devicePixelRatioF();
    const QSize deviceSize(std::lround(width() * dpr), std::lround(height() * dpr));
    QTransform deviceCamera = camera_.matrix();
    deviceCamera *= QTransform::fromScale(dpr, dpr);

    painter.beginNativePainting();
    if (state_ != nullptr) {
        renderer_.render(state_->renderEntries(), geometry_, deviceCamera, deviceSize,
                         flash_.layerIds, selectionFlashHue(), selectionFlashStrength(), false);
    } else {
        renderer_.render(*project_, geometry_, deviceCamera, deviceSize,
                         flash_.layerIds, selectionFlashHue(), selectionFlashStrength(), false);
    }
    painter.endNativePainting();

    if (options_.guideLayersOnTop) {
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
