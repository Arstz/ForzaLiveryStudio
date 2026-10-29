#include "contour_leeway.h"
#include "pen_fill.h"

namespace gui {
namespace {

QTransform worldTransform(const fls::scene::Layer &node) {
    const auto matrix = node.worldMatrix();

    return QTransform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1], matrix.m[1][1],
        matrix.m[0][2], matrix.m[1][2]);
}

bool appendLeeway(const fls::scene::Layer &node, const ShapeGeometryStore &geometry,
                   const QString &excludedContourId, QPainterPath *result, QString *error) {
    using namespace fls::scene;
    bool opaque = true;

    if (node.kind() == LayerKind::Guide)
        return true;
    for (const Layer *ancestor = &node; ancestor != nullptr; ancestor = ancestor->parent()) {
        if (!ancestor->visible)
            return true;
        opaque = opaque && ancestor->opacity >= 1.0;
    }
    if (!opaque) {
        *error = QStringLiteral("Leeway requires fully opaque shapes and saved paths");
        return false;
    }
    if (node.kind() == LayerKind::Group) {
        for (const auto &child : static_cast<const Group &>(node).children)
            if (!appendLeeway(*child, geometry, excludedContourId, result, error))
                return false;

        return true;
    }
    QPainterPath path;
    if (node.kind() == LayerKind::Shape) {
        const auto &shape = static_cast<const Shape &>(node);
        const auto *shapeGeometry = geometry.shape(shape.shapeId);

        if (shape.isRaster() || shape.mask || shape.color[3] != 255) {
            *error = QStringLiteral("Leeway requires opaque vector shapes without masks");
            return false;
        }
        if (shapeGeometry == nullptr) {
            *error = QStringLiteral("Leeway shape geometry is unavailable");
            return false;
        }
        path = buildPenPrimitive(shape.shapeId, *shapeGeometry).silhouette;
    } else if (node.kind() == LayerKind::Contour) {
        const auto &figure = static_cast<const ContourFigure &>(node);
        QVector<PenLoop> loops{{figure.data.points, PenLoopKind::Outer}};

        if (!excludedContourId.isEmpty() && node.id == excludedContourId) {
            *error = QStringLiteral("The active contour cannot be used as its own leeway");
            return false;
        }
        if (figure.data.pathKind != ContourPathKind::Pen || !figure.data.closed
            || (!figure.data.cutouts.isEmpty() && !figure.data.cutoutClosed) || figure.data.fillMask) {
            *error = QStringLiteral("Leeway requires closed Pen paths without masks");
            return false;
        }
        for (const auto &cutout : figure.data.cutouts)
            loops.push_back({cutout, PenLoopKind::Cutout});
        const auto contour = buildPenContour(loops);
        if (!contour.valid()) {
            *error = contour.error;
            return false;
        }
        path = contour.path;
    } else {
        return true;
    }
    if (path.isEmpty()) {
        *error = QStringLiteral("Leeway geometry is unavailable");
        return false;
    }
    *result = result->united(worldTransform(node).map(path));

    return true;
}

} // namespace

QPainterPath buildContourLeeway(const fls::scene::Layer &node, const ShapeGeometryStore &geometry,
                                const QString &excludedContourId, QString *error) {
    QPainterPath result;
    QString failure;

    if (!appendLeeway(node, geometry, excludedContourId, &result, &failure))
        result = {};
    if (result.isEmpty() && failure.isEmpty())
        failure = QStringLiteral("Leeway has no visible filled geometry");
    if (error != nullptr)
        *error = failure;

    return result;
}

} // namespace gui
