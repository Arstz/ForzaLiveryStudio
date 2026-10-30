#include "main_window.h"

#include "main_window_internal.h"
#include "car_preview_widget.h"
#include "selection_geometry.h"

#include <QSaveFile>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <stdexcept>

namespace gui {
namespace {

constexpr int kNumberPrecision = 15;
constexpr double kColorRange = 255.0;
const QString kSvgNamespace = QStringLiteral("http://www.w3.org/2000/svg");

struct SvgShape {
    QMap<QString, QString> attributes;
    QString sectionId;
    bool mask = false;
    bool visible = true;
};

QHash<QString, QTransform> svgSectionTransforms(const EditorState &state, const CarPreviewWidget *carPreview) {
    QHash<QString, QTransform> transforms;

    if (!state.project_.isLivery) {
        return transforms;
    }
    for (const auto &entry : state.renderEntries()) {
        if (entry.shape == nullptr || entry.shape->isRaster() || transforms.contains(entry.sectionGroupId)) {
            continue;
        }
        const auto *section = dynamic_cast<const fls::scene::Group *>(state.sceneNode(entry.sectionGroupId));
        if (section == nullptr || !section->isLiverySection) {
            throw std::runtime_error("Place livery shapes in a section before exporting.");
        }
        const auto transform = carPreview != nullptr
            ? carPreview->liverySectionToAtlasTransform(section->liverySectionSlot) : std::nullopt;
        if (!transform) {
            throw std::runtime_error(QStringLiteral("Load a car model with a UV layout for section \"%1\" before exporting.")
                .arg(section->name).toStdString());
        }
        transforms.insert(entry.sectionGroupId, *transform);
    }

    return transforms;
}

QString svgNumber(double value) {
    return QString::number(value, 'g', kNumberPrecision);
}

QString svgPoint(const QPointF &point) {
    return svgNumber(point.x()) + QLatin1Char(' ') + svgNumber(point.y());
}

QString svgRect(const QRectF &rect) {
    return svgPoint(rect.topLeft()) + QLatin1Char(' ')
        + svgNumber(rect.width()) + QLatin1Char(' ') + svgNumber(rect.height());
}

QString svgPath(const QPainterPath &path) {
    QString result;

    for (int index = 0; index < path.elementCount(); ++index) {
        const auto element = path.elementAt(index);
        if (element.isMoveTo()) {
            result += QStringLiteral("M") + svgPoint(element);
        } else if (element.isLineTo()) {
            result += QStringLiteral("L") + svgPoint(element);
        } else if (element.isCurveTo()) {
            result += QStringLiteral("C") + svgPoint(element) + QLatin1Char(' ')
                + svgPoint(path.elementAt(index + 1)) + QLatin1Char(' ')
                + svgPoint(path.elementAt(index + 2));
            index += 2;
        }
    }

    return result;
}

QString svgTransform(const QTransform &transform) {
    return QStringLiteral("matrix(%1 %2 %3 %4 %5 %6)")
        .arg(svgNumber(transform.m11()), svgNumber(transform.m12()),
             svgNumber(transform.m21()), svgNumber(transform.m22()),
             svgNumber(transform.dx()), svgNumber(transform.dy()));
}

QString svgReference(const QString &id) {
    return QStringLiteral("url(#%1)").arg(id);
}

void writeSvgShape(QXmlStreamWriter &xml, const SvgShape &shape,
                   const QString &id, const QString &maskId = {}, bool maskContent = false) {
    xml.writeStartElement(QStringLiteral("path"));
    xml.writeAttribute(QStringLiteral("id"), id);
    for (auto attribute = shape.attributes.cbegin(); attribute != shape.attributes.cend(); ++attribute) {
        xml.writeAttribute(attribute.key(), attribute.value());
    }
    if (!maskId.isEmpty()) {
        xml.writeAttribute(QStringLiteral("mask"), svgReference(maskId));
    }
    if (maskContent) {
        xml.writeAttribute(QStringLiteral("filter"), svgReference(QStringLiteral("mask-black")));
    } else if (!shape.visible || shape.mask) {
        xml.writeAttribute(QStringLiteral("display"), QStringLiteral("none"));
    }
    xml.writeEndElement();
}

QString writeMeshPaint(QXmlStreamWriter &xml, const ShapeGeometry &geometry,
                       const QString &id, const QString &color, const QTransform &transform) {
    const QRectF bounds = vectorSelectionBody(geometry).boundingRect();

    xml.writeStartElement(QStringLiteral("pattern"));
    xml.writeAttribute(QStringLiteral("id"), id);
    xml.writeAttribute(QStringLiteral("patternUnits"), QStringLiteral("userSpaceOnUse"));
    xml.writeAttribute(QStringLiteral("patternTransform"), svgTransform(transform));
    xml.writeAttribute(QStringLiteral("x"), svgNumber(bounds.x()));
    xml.writeAttribute(QStringLiteral("y"), svgNumber(bounds.y()));
    xml.writeAttribute(QStringLiteral("width"), svgNumber(bounds.width()));
    xml.writeAttribute(QStringLiteral("height"), svgNumber(bounds.height()));
    int index = 0;
    for (const auto &triangle : geometry.triangles) {
        const QPointF first = triangle.p1 - triangle.p0;
        const QPointF second = triangle.p2 - triangle.p0;
        const double determinant = first.x() * second.y() - first.y() * second.x();
        const double minimum = std::min({triangle.alpha0, triangle.alpha1, triangle.alpha2});
        const double maximum = std::max({triangle.alpha0, triangle.alpha1, triangle.alpha2});
        QString fill = color;

        if (determinant == 0.0) {
            continue;
        }
        if (minimum != maximum) {
            const QString gradientId = id + QStringLiteral("-gradient-%1").arg(index++);
            const double firstAlpha = triangle.alpha1 - triangle.alpha0;
            const double secondAlpha = triangle.alpha2 - triangle.alpha0;
            const QPointF gradient((firstAlpha * second.y() - secondAlpha * first.y()) / determinant,
                                   (first.x() * secondAlpha - second.x() * firstAlpha) / determinant);
            const double lengthSquared = QPointF::dotProduct(gradient, gradient);
            const QPointF start = triangle.p0 + gradient * ((minimum - triangle.alpha0) / lengthSquared);
            const QPointF end = triangle.p0 + gradient * ((maximum - triangle.alpha0) / lengthSquared);

            xml.writeStartElement(QStringLiteral("linearGradient"));
            xml.writeAttribute(QStringLiteral("id"), gradientId);
            xml.writeAttribute(QStringLiteral("gradientUnits"), QStringLiteral("userSpaceOnUse"));
            xml.writeAttribute(QStringLiteral("x1"), svgNumber(start.x()));
            xml.writeAttribute(QStringLiteral("y1"), svgNumber(start.y()));
            xml.writeAttribute(QStringLiteral("x2"), svgNumber(end.x()));
            xml.writeAttribute(QStringLiteral("y2"), svgNumber(end.y()));
            for (const auto &[offset, alpha] : {std::pair{0, minimum}, std::pair{1, maximum}}) {
                xml.writeStartElement(QStringLiteral("stop"));
                xml.writeAttribute(QStringLiteral("offset"), QString::number(offset));
                xml.writeAttribute(QStringLiteral("stop-color"), color);
                xml.writeAttribute(QStringLiteral("stop-opacity"), svgNumber(alpha));
                xml.writeEndElement();
            }
            xml.writeEndElement();
            fill = svgReference(gradientId);
        }
        xml.writeStartElement(QStringLiteral("path"));
        xml.writeAttribute(QStringLiteral("d"), QStringLiteral("M%1L%2L%3Z")
            .arg(svgPoint(triangle.p0), svgPoint(triangle.p1), svgPoint(triangle.p2)));
        xml.writeAttribute(QStringLiteral("fill"), fill);
        if (minimum == maximum) {
            xml.writeAttribute(QStringLiteral("fill-opacity"), svgNumber(minimum));
        }
        xml.writeEndElement();
    }
    xml.writeEndElement();

    return svgReference(id);
}

SvgShape svgShape(QXmlStreamWriter &xml, const SceneRenderEntry &entry,
                  const ShapeGeometryStore &geometry, QHash<int, QPainterPath> &paths,
                  BoundsAccumulator &bounds, int index, const QTransform &worldToSvg) {
    const auto &shape = *entry.shape;
    const QTransform transform = entry.worldTransform * worldToSvg;
    const auto *vector = dynamic_cast<const fls::scene::VectorContainer *>(shape.visual.get());
    const int shapeId = vector != nullptr ? vector->shapeId : shape.shapeId;
    const auto *source = geometry.shape(shapeId);
    SvgShape result;

    if (source == nullptr) {
        throw std::runtime_error(QStringLiteral("Could not load shape %1.").arg(shapeId).toStdString());
    }
    if (!paths.contains(shapeId)) {
        paths.insert(shapeId, vectorSelectionBody(*source));
    }
    const auto &path = paths[shapeId];
    const QString color = QColor(shape.color[2], shape.color[1], shape.color[0]).name();
    const bool varyingAlpha = std::any_of(source->triangles.cbegin(), source->triangles.cend(),
        [](const ShapeTriangle &triangle) {
            return triangle.alpha0 != 1.0 || triangle.alpha1 != 1.0 || triangle.alpha2 != 1.0;
        });

    result.sectionId = entry.sectionGroupId;
    result.mask = shape.mask;
    result.visible = shape.visible;
    result.attributes.insert(QStringLiteral("opacity"), svgNumber(shape.color[3] / kColorRange));
    result.attributes.insert(QStringLiteral("data-name"), shape.name);
    bounds.add(transform, path.boundingRect());
    result.attributes.insert(QStringLiteral("d"), svgPath(transform.map(path)));
    result.attributes.insert(QStringLiteral("fill-rule"), path.fillRule() == Qt::OddEvenFill
        ? QStringLiteral("evenodd") : QStringLiteral("nonzero"));
    result.attributes.insert(QStringLiteral("fill"), varyingAlpha
        ? writeMeshPaint(xml, *source, QStringLiteral("paint-%1").arg(index), color, transform) : color);

    return result;
}

void writeSvgProject(QIODevice &device, const QVector<SceneRenderEntry> &entries,
                     const QHash<QString, QTransform> &sectionTransforms, bool livery) {
    ShapeGeometryStore geometry;
    QHash<int, QPainterPath> paths;
    BoundsAccumulator bounds;
    QVector<SvgShape> shapes;
    QByteArray definitions;
    QXmlStreamWriter definitionXml(&definitions);
    QXmlStreamWriter xml(&device);
    QString error;

    if (!geometry.loadDefault(&error)) {
        throw std::runtime_error(error.toStdString());
    }
    definitionXml.writeStartElement(QStringLiteral("defs"));
    definitionXml.writeDefaultNamespace(kSvgNamespace);
    for (const auto &entry : entries) {
        if (entry.shape != nullptr && !entry.shape->isRaster()) {
            const QTransform worldToSvg = livery
                ? sectionTransforms.value(entry.sectionGroupId) : QTransform::fromScale(1.0, -1.0);
            shapes.push_back(svgShape(definitionXml, entry, geometry, paths, bounds, shapes.size(), worldToSvg));
        }
    }
    definitionXml.writeEndElement();
    const QRectF viewBox = livery
        ? QRectF(0.0, 0.0, 2.0 * fls::kLiveryCanvasHalfWidth, 2.0 * fls::kLiveryCanvasHalfHeight)
        : bounds.hasBounds() && bounds.bounds().isValid() ? bounds.bounds() : QRectF(0.0, 0.0, 1.0, 1.0);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement(QStringLiteral("svg"));
    xml.writeDefaultNamespace(kSvgNamespace);
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.1"));
    xml.writeAttribute(QStringLiteral("viewBox"), svgRect(viewBox));
    xml.writeAttribute(QStringLiteral("width"), svgNumber(viewBox.width()));
    xml.writeAttribute(QStringLiteral("height"), svgNumber(viewBox.height()));
    xml.writeStartElement(QStringLiteral("defs"));
    QXmlStreamReader reader(definitions);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartDocument() && !reader.isEndDocument()
            && reader.name() != QStringLiteral("defs")) {
            xml.writeCurrentToken(reader);
        }
    }
    if (reader.hasError()) {
        throw std::runtime_error(reader.errorString().toStdString());
    }
    xml.writeStartElement(QStringLiteral("filter"));
    xml.writeAttribute(QStringLiteral("id"), QStringLiteral("mask-black"));
    xml.writeStartElement(QStringLiteral("feColorMatrix"));
    xml.writeAttribute(QStringLiteral("type"), QStringLiteral("matrix"));
    xml.writeAttribute(QStringLiteral("values"), QStringLiteral("0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1 0"));
    xml.writeEndElement();
    xml.writeEndElement();
    QHash<QString, QString> laterMasks;
    QVector<QString> shapeMasks(shapes.size());
    for (qsizetype index = shapes.size(); index-- > 0;) {
        const auto &shape = shapes[index];
        const QString laterMask = laterMasks.value(shape.sectionId);
        shapeMasks[index] = laterMask;
        if (!shape.mask || !shape.visible) {
            continue;
        }
        const QString id = QStringLiteral("mask-%1").arg(index);
        xml.writeStartElement(QStringLiteral("mask"));
        xml.writeAttribute(QStringLiteral("id"), id);
        xml.writeAttribute(QStringLiteral("maskUnits"), QStringLiteral("userSpaceOnUse"));
        xml.writeAttribute(QStringLiteral("x"), svgNumber(viewBox.x()));
        xml.writeAttribute(QStringLiteral("y"), svgNumber(viewBox.y()));
        xml.writeAttribute(QStringLiteral("width"), svgNumber(viewBox.width()));
        xml.writeAttribute(QStringLiteral("height"), svgNumber(viewBox.height()));
        xml.writeStartElement(QStringLiteral("rect"));
        xml.writeAttribute(QStringLiteral("x"), svgNumber(viewBox.x()));
        xml.writeAttribute(QStringLiteral("y"), svgNumber(viewBox.y()));
        xml.writeAttribute(QStringLiteral("width"), svgNumber(viewBox.width()));
        xml.writeAttribute(QStringLiteral("height"), svgNumber(viewBox.height()));
        xml.writeAttribute(QStringLiteral("fill"), QStringLiteral("white"));
        if (!laterMask.isEmpty()) {
            xml.writeAttribute(QStringLiteral("mask"), svgReference(laterMask));
        }
        xml.writeEndElement();
        writeSvgShape(xml, shape, QStringLiteral("mask-shape-%1").arg(index), {}, true);
        xml.writeEndElement();
        laterMasks.insert(shape.sectionId, id);
    }
    xml.writeEndElement();
    for (qsizetype index = 0; index < shapes.size(); ++index) {
        writeSvgShape(xml, shapes[index], QStringLiteral("shape-%1").arg(index), shapeMasks[index]);
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    if (xml.hasError()) {
        throw std::runtime_error("Could not write the SVG document.");
    }
}

} // namespace

void MainWindow::exportProjectSvgDialog() {
    if (!state_->hasProject()) {
        QMessageBox::information(this, QStringLiteral("Export project to SVG"),
                                 QStringLiteral("Open a project before exporting."));
        return;
    }
    const QString suggestedPath = QDir(importDialogStartDirectory(this, QStringLiteral("exportSvg")))
        .filePath(mw_detail::safeGroupName(state_->project_.name) + QStringLiteral(".svg"));
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export project to SVG"),
                                               suggestedPath, QStringLiteral("SVG image (*.svg)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".svg");
    }
    try {
        const auto sectionTransforms = svgSectionTransforms(*state_, carPreview_);
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            throw std::runtime_error(file.errorString().toStdString());
        }
        writeSvgProject(file, state_->renderEntries(), sectionTransforms, state_->project_.isLivery);
        if (!file.commit()) {
            throw std::runtime_error(file.errorString().toStdString());
        }
        rememberImportDirectory(path, QStringLiteral("exportSvg"));
        statusBar()->showMessage(QStringLiteral("Exported %1").arg(path), 5000);
    } catch (const std::exception &exception) {
        QMessageBox::critical(this, QStringLiteral("SVG export failed"), QString::fromUtf8(exception.what()));
    }
}

} // namespace gui
