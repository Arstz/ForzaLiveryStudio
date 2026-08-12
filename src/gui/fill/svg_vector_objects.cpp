#include "svg_vector_objects.h"

#include <QtSvg/QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

QString unsupportedSvgFeature(const QByteArray &svg) {
    QXmlStreamReader reader(svg);
    const QSet<QString> unsupportedElements = {
        QStringLiteral("clippath"),
        QStringLiteral("filter"),
        QStringLiteral("foreignobject"),
        QStringLiteral("image"),
        QStringLiteral("lineargradient"),
        QStringLiteral("mask"),
        QStringLiteral("pattern"),
        QStringLiteral("radialgradient"),
        QStringLiteral("style"),
        QStringLiteral("textpath"),
    };
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }
        const QString element = reader.name().toString().toLower();
        if (unsupportedElements.contains(element)) {
            return element;
        }
        for (const QXmlStreamAttribute &attribute : reader.attributes()) {
            const QString name = attribute.name().toString().toLower();
            const QString value = attribute.value().toString().toLower();
            if (name == QLatin1String("clip-path")
                || name == QLatin1String("filter")
                || name == QLatin1String("mask")
                || value.contains(QStringLiteral("url("))
                || value.contains(QStringLiteral("mix-blend-mode"))) {
                return name;
            }
        }
    }
    return reader.hasError() ? QStringLiteral("invalid XML") : QString();
}

class SvgCaptureEngine final : public QPaintEngine {
public:
    SvgCaptureEngine()
        : QPaintEngine(QPaintEngine::AllFeatures) {
    }

    bool begin(QPaintDevice *) override {
        setActive(true);
        return true;
    }

    bool end() override {
        setActive(false);
        return true;
    }

    Type type() const override {
        return QPaintEngine::User;
    }

    void updateState(const QPaintEngineState &) override {
    }

    void drawPath(const QPainterPath &path) override {
        if (state == nullptr || path.isEmpty()) {
            return;
        }
        if (painter() != nullptr && painter()->hasClipping()) {
            markUnsupported(QStringLiteral("clip paths"));
            return;
        }

        const qreal opacity = state->opacity();
        if (!std::isfinite(opacity) || opacity <= 0.0) {
            return;
        }
        const QTransform transform = state->transform();
        const QBrush brush = state->brush();
        if (brush.style() != Qt::NoBrush) {
            if (brush.style() != Qt::SolidPattern) {
                markUnsupported(QStringLiteral("gradients, patterns, or textured fills"));
                return;
            }
            QColor color = brush.color();
            color.setAlphaF(std::clamp(color.alphaF() * opacity, 0.0, 1.0));
            if (color.alpha() > 0) {
                objects.push_back({transform.map(path), color});
            }
            return;
        }

        const QPen pen = state->pen();
        if (pen.style() == Qt::NoPen || pen.color().alpha() == 0) {
            return;
        }
        if (pen.brush().style() != Qt::SolidPattern) {
            markUnsupported(QStringLiteral("gradient or textured strokes"));
            return;
        }
        QPainterPathStroker stroker(pen);
        QPainterPath stroke = stroker.createStroke(path);
        if (stroke.isEmpty()) {
            return;
        }
        QColor color = pen.color();
        color.setAlphaF(std::clamp(color.alphaF() * opacity, 0.0, 1.0));
        objects.push_back({transform.map(stroke), color});
    }

    void drawPixmap(const QRectF &, const QPixmap &, const QRectF &) override {
        markUnsupported(QStringLiteral("embedded raster images, masks, or patterns"));
    }

    void drawImage(const QRectF &, const QImage &, const QRectF &,
                   Qt::ImageConversionFlags) override {
        markUnsupported(QStringLiteral("embedded raster images, masks, or patterns"));
    }

    void drawTextItem(const QPointF &position, const QTextItem &textItem) override {
        if (state == nullptr || textItem.text().isEmpty()) {
            return;
        }
        if (painter() != nullptr && painter()->hasClipping()) {
            markUnsupported(QStringLiteral("clipped text"));
            return;
        }
        const qreal opacity = state->opacity();
        if (!std::isfinite(opacity) || opacity <= 0.0) {
            return;
        }
        const QPen pen = state->pen();
        if (pen.style() == Qt::NoPen || pen.brush().style() != Qt::SolidPattern) {
            markUnsupported(QStringLiteral("gradient or patterned text"));
            return;
        }

        QColor color = pen.color();
        color.setAlphaF(std::clamp(color.alphaF() * opacity, 0.0, 1.0));
        if (color.alpha() == 0) {
            return;
        }

        const QString text = textItem.text();
        const QFont font = textItem.font();
        const QFontMetricsF metrics(font);
        const QTransform transform = state->transform();
        const bool separable = !(textItem.renderFlags() & QTextItem::RightToLeft)
            && std::all_of(text.cbegin(), text.cend(), [](QChar character) {
                   return !character.isHighSurrogate()
                       && !character.isLowSurrogate()
                       && character.combiningClass() == 0;
               });
        if (separable) {
            QString prefix;
            prefix.reserve(text.size());
            for (const QChar character : text) {
                const qreal offset = metrics.horizontalAdvance(prefix);
                QPainterPath glyph;
                glyph.addText(position + QPointF(offset, 0.0), font, QString(character));
                if (!glyph.isEmpty()) {
                    objects.push_back({transform.map(glyph), color});
                }
                prefix.append(character);
            }
            if (!objects.isEmpty()) {
                return;
            }
        }

        QPainterPath glyphs;
        glyphs.addText(position, font, text);
        if (glyphs.isEmpty()) {
            markUnsupported(QStringLiteral("a font without vector glyph outlines"));
        } else {
            objects.push_back({transform.map(glyphs), color});
        }
    }

    QVector<SvgVectorObject> objects;
    QString fallbackReason;

private:
    void markUnsupported(const QString &feature) {
        if (fallbackReason.isEmpty()) {
            fallbackReason = QStringLiteral("SVG object selection does not support %1")
                                 .arg(feature);
        }
    }
};

class SvgCaptureDevice final : public QPaintDevice {
public:
    explicit SvgCaptureDevice(const QSize &size)
        : size_(size) {
    }

    QPaintEngine *paintEngine() const override {
        return const_cast<SvgCaptureEngine *>(&engine);
    }

    SvgCaptureEngine engine;

protected:
    int metric(PaintDeviceMetric metric) const override {
        switch (metric) {
        case PdmWidth:
            return size_.width();
        case PdmHeight:
            return size_.height();
        case PdmWidthMM:
            return qRound(size_.width() * 25.4 / 96.0);
        case PdmHeightMM:
            return qRound(size_.height() * 25.4 / 96.0);
        case PdmDpiX:
        case PdmPhysicalDpiX:
            return 96;
        case PdmDpiY:
        case PdmPhysicalDpiY:
            return 96;
        case PdmDepth:
            return 32;
        case PdmNumColors:
            return 16'777'216;
        case PdmDevicePixelRatio:
            return 1;
        case PdmDevicePixelRatioScaled:
            return 65'536;
        default:
            return 0;
        }
    }

private:
    QSize size_;
};

} // namespace

SvgVectorDocument extractSvgVectorObjects(const QByteArray &svg,
                                          const QSize &logicalSize) {
    SvgVectorDocument result;
    if (svg.isEmpty() || logicalSize.isEmpty()) {
        result.fallbackReason = QStringLiteral("SVG source or dimensions are empty");
        return result;
    }
    const QString unsupported = unsupportedSvgFeature(svg);
    if (!unsupported.isEmpty()) {
        result.fallbackReason = QStringLiteral("SVG object selection does not support %1")
                                    .arg(unsupported);
        return result;
    }
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) {
        result.fallbackReason = QStringLiteral("SVG source could not be parsed");
        return result;
    }

    SvgCaptureDevice device(logicalSize);
    QPainter painter(&device);
    renderer.render(&painter, QRectF(QPointF(), QSizeF(logicalSize)));
    painter.end();
    result.objects = std::move(device.engine.objects);
    result.fallbackReason = device.engine.fallbackReason;
    if (result.fallbackReason.isEmpty() && result.objects.isEmpty()) {
        result.fallbackReason = QStringLiteral("SVG contains no selectable filled objects");
    }
    return result;
}

SvgVectorObjectHit svgVectorObjectAt(const SvgVectorDocument &document,
                                     const QPointF &point) {
    SvgVectorObjectHit result;
    if (!document.supportsObjectSelection()) {
        return result;
    }
    for (int index = document.objects.size() - 1; index >= 0; --index) {
        const SvgVectorObject &object = document.objects[index];
        if (object.path.controlPointRect().contains(point)
            && object.path.contains(point)) {
            result.path = object.path;
            result.color = object.color;
            result.objectIndex = index;
            return result;
        }
    }
    return result;
}

} // namespace gui
