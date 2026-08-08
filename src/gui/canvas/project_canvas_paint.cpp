#include "project_canvas.h"

#include "project_canvas_internal.h"

#include <cstdint>

namespace gui {

using namespace pc_detail;

QPainterPath ProjectCanvas::penPreviewPath(bool closeToStart) const {
    if (pen_.points.isEmpty()) {
        return {};
    }
    if (pen_.closed && pen_.cutoutClosed) {
        return penGeometryCache().worldPath;
    }
    const bool drawingCutout = pen_.closed && !pen_.cutoutClosed;
    const QVector<PenPoint> &activePoints = drawingCutout
        ? pen_.cutouts[pen_.activeCutout]
        : pen_.points;
    if (closeToStart && activePoints.size() >= 3) {
        return penGeometryCache().worldPath;
    }
    const int activeLoopIndex = drawingCutout ? pen_.activeCutout : -1;
    QPainterPath path;
    for (const CachedPenLoop &loop : penGeometryCache().loops) {
        if (loop.loopIndex == activeLoopIndex) {
            path = loop.openPath;
            break;
        }
    }
    if (!activePoints.isEmpty()
        && activePoints.back().kind == PenPointKind::Soft) {
        path.quadTo(activePoints.back().position,
                    (activePoints.back().position + pen_.hoverWorld) * 0.5);
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

    const bool drawingCutout = pen_.closed && !pen_.cutoutClosed;
    const QVector<PenPoint> &activePoints = drawingCutout
        ? pen_.cutouts[pen_.activeCutout]
        : pen_.points;
    const bool nearStart = (!pen_.closed || drawingCutout) && activePoints.size() >= 3
        && QLineF(worldToScreen(pen_.hoverWorld), worldToScreen(activePoints.front().position)).length()
               <= kPenCloseRadius;
    const bool closed = (pen_.closed && pen_.cutoutClosed) || nearStart;
    const QPainterPath worldPath = penPreviewPath(closed);
    const QPainterPath screenPath = closed
        ? penScreenPath()
        : camera_.matrix().map(worldPath);
    QColor fill(83, 164, 255, closed ? 50 : 25);
    QPen halo(QColor(15, 17, 20, 230), 4.0);
    halo.setCosmetic(true);
    QPen pathPen(pen_.error.isEmpty() ? QColor(83, 164, 255) : QColor(235, 78, 78), 2.0);
    pathPen.setCosmetic(true);
    const auto drawFilledPath = [&](const QPainterPath &path) {
        painter.setPen(halo);
        painter.setBrush(fill);
        painter.drawPath(path);
        painter.setPen(pathPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    };
    const auto drawOpenPath = [&](const QPainterPath &path) {
        painter.strokePath(path, halo);
        painter.strokePath(path, pathPen);
    };
    if (drawingCutout && !closed) {
        drawFilledPath(penCompletedScreenPath());
        drawOpenPath(screenPath);
    } else {
        drawFilledPath(screenPath);
    }

    if (!closed && !activePoints.isEmpty()) {
        QPen guide(QColor(190, 195, 205, 180), 1.0, Qt::DashLine);
        guide.setCosmetic(true);
        painter.setPen(guide);
        painter.drawLine(worldToScreen(activePoints.back().position), worldToScreen(pen_.hoverWorld));
    }

    const auto drawLoopPoints = [&](const QVector<PenPoint> &points, int loopIndex) {
        for (int i = 0; i < points.size(); ++i) {
            const PenPoint &point = points[i];
            const QPointF screen = worldToScreen(point.position);
            const bool hovered = pen_.cutoutClosed
                && loopIndex == pen_.hoverLoop && i == pen_.hoverPoint;
            const double radius = (i == 0 ? 5.5 : 4.5) + (hovered ? 1.5 : 0.0);
            painter.setPen(QPen(hovered ? QColor(120, 220, 135) : QColor(18, 20, 24),
                                hovered ? 2.5 : 2.0));
            painter.setBrush(point.kind == PenPointKind::Hard
                                 ? QColor(232, 72, 72)
                                 : QColor(238, 240, 244));
            painter.drawEllipse(screen, radius, radius);
            const bool activeStart = i == 0
                && ((!pen_.closed && loopIndex < 0)
                    || (drawingCutout && loopIndex == pen_.activeCutout));
            if (activeStart) {
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(nearStart ? QColor(120, 220, 135) : QColor(83, 164, 255), 1.5));
                painter.drawEllipse(screen, kPenCloseRadius, kPenCloseRadius);
            }
        }
    };
    drawLoopPoints(pen_.points, -1);
    for (int loopIndex = 0; loopIndex < pen_.cutouts.size(); ++loopIndex) {
        drawLoopPoints(pen_.cutouts[loopIndex], loopIndex);
    }
    if (pen_.closed && pen_.cutoutClosed && pen_.hoverPoint < 0 && pen_.hoverCurve.valid()) {
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
constexpr double kVisibilityBorderHaloWidth = 3.0;
constexpr double kVisibilityBorderLineWidth = 1.0;

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
    if (project_ == nullptr || !options_.visibilityBordersEnabled) {
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
    drawWorldRect(viewportRect, kVisibilityViewportColor, Qt::SolidLine);
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

    if (carUnwrapVisible_ && !carUnwrapOverlay_.empty()) {
        static const QColor kSideColors[fls::kLiverySideCount] = {
            QColor(230, 60, 60),
            QColor(60, 200, 60),
            QColor(70, 120, 240),
            QColor(230, 220, 60),
            QColor(220, 80, 220),
            QColor(60, 210, 210),
            QColor(255, 130, 60),
            QColor(120, 255, 140),
            QColor(120, 190, 255),
            QColor(255, 235, 120),
            QColor(255, 140, 255),
        };
        painter.save();
        painter.setTransform(camera_.matrix(), false);
        painter.setPen(Qt::NoPen);
        painter.setOpacity(0.45);
        for (int sideIndex = 0; sideIndex < fls::kLiverySideCount; ++sideIndex) {
            const CarUnwrapSide &side = carUnwrapOverlay_.sides[sideIndex];
            if (!side.valid()) {
                continue;
            }
            painter.setBrush(kSideColors[sideIndex]);
            painter.drawPath(side.path);
        }
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
            };
            if (!options_.separateOpacityAndSkewTools) {
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
    forEachSceneShape([&](const fls::scene::Shape &, const QTransform &, int) {
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

QImage ProjectCanvas::guideImage(const fls::scene::GuideLayer &guide) const {
    const fls::scene::RasterContainer *img = guide.image.get();
    const int width = img != nullptr ? img->width : 0;
    const int height = img != nullptr ? img->height : 0;
    const QString format = img != nullptr ? img->format : QString();
    const QByteArray &pixelBytes = img != nullptr ? img->pixels : QByteArray();
    const QByteArray &encodedBytes = img != nullptr ? img->encoded : QByteArray();
    const QString cacheKey = QStringLiteral("%1|%2|%3|%4|%5|%6|%7")
        .arg(guide.id)
        .arg(width)
        .arg(height)
        .arg(format)
        .arg(pixelBytes.isEmpty() ? encodedBytes.size() : pixelBytes.size())
        .arg(QString::number(reinterpret_cast<quintptr>(img), 16))
        .arg(guide.imageTopDown ? 1 : 0);
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
        if (!guide.imageTopDown) {
            image = image.mirrored(false, true);
        }
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
    forEachSceneGuide([&](const fls::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
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
        painter.setTransform(pc_detail::guideImageToLocal(image.size(), size)
                                 * world * camera_.matrix(),
                             false);
        painter.drawImage(QPointF(), image);
        painter.restore();
        return true;
    }, /*reverse=*/false);
    painter.restore();
}

bool ProjectCanvas::createRegionsForSelectedGuide(int smallRegionMergeArea,
                                                  QString *message) {
    const QVector<fls::scene::GuideLayer *> guides = selectedGuideLayers();
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
    const fls::scene::GuideLayer *guide = guides.front();
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

    QTransform guideWorld;
    QSizeF guideSize;
    bool found = false;
    forEachSceneGuide([&](const fls::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
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
    const QTransform imageToWorld = pc_detail::guideImageToLocal(imageSize, guideSize)
        * guideWorld;
    result.push_back({QStringLiteral("Safe"), {}, true});
    result.push_back({QStringLiteral("Dangerous"), {}, false});

    for (const RegionFillLayer &fill : region_.fills) {
        int variantIndex = 0;
        if (fill.variant == RegionFillVariant::Dangerous) {
            variantIndex = 1;
        }
        GeneratedRegionVariant &variant = result[variantIndex];
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
    forEachSceneGuide([&](const fls::scene::GuideLayer &guide, const QTransform &world, const QString &sectionGroupId) {
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

    const QTransform imageToScreen = pc_detail::guideImageToLocal(imageSize, guideSize)
        * guideWorld * camera_.matrix();

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
