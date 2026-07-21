#include "scene_codec.h"

#include "core_types.h"
#include "matrix_math.h"

#include <QJsonArray>
#include <QJsonValue>

#include <algorithm>

namespace fh6::scene {
namespace {

QString hex(const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toHex());
}

QByteArray unhex(const QJsonValue &value) {
    return QByteArray::fromHex(value.toString().toLatin1());
}

QString b64(const QByteArray &bytes) {
    return QString::fromLatin1(bytes.toBase64());
}

QByteArray unb64(const QJsonValue &value) {
    return QByteArray::fromBase64(value.toString().toLatin1());
}

QJsonObject transformToJson(const Transform2D &t) {
    QJsonObject o;
    o.insert(QStringLiteral("x"), t.x);
    o.insert(QStringLiteral("y"), t.y);
    o.insert(QStringLiteral("scale_x"), t.scaleX);
    o.insert(QStringLiteral("scale_y"), t.scaleY);
    o.insert(QStringLiteral("rotation"), normalizeRotation(t.rotation));
    o.insert(QStringLiteral("skew"), t.skew);
    return o;
}

Transform2D transformFromJson(const QJsonObject &o) {
    Transform2D t;
    t.x = o.value(QStringLiteral("x")).toDouble(0.0);
    t.y = o.value(QStringLiteral("y")).toDouble(0.0);
    t.scaleX = o.value(QStringLiteral("scale_x")).toDouble(1.0);
    t.scaleY = o.value(QStringLiteral("scale_y")).toDouble(1.0);
    t.rotation = normalizeRotation(o.value(QStringLiteral("rotation")).toDouble(0.0));
    t.skew = o.value(QStringLiteral("skew")).toDouble(0.0);
    return t;
}

void writeBase(QJsonObject &o, const Layer &l) {
    o.insert(QStringLiteral("id"), l.id);
    o.insert(QStringLiteral("name"), l.name);
    o.insert(QStringLiteral("transform"), transformToJson(l.transform));
    o.insert(QStringLiteral("opacity"), l.opacity);
    o.insert(QStringLiteral("visible"), l.visible);
    o.insert(QStringLiteral("locked"), l.locked);
}

void readBase(const QJsonObject &o, Layer &l) {
    l.id = o.value(QStringLiteral("id")).toString();
    l.name = o.value(QStringLiteral("name")).toString(l.name);
    l.transform = transformFromJson(o.value(QStringLiteral("transform")).toObject());
    l.opacity = o.value(QStringLiteral("opacity")).toDouble(l.opacity);
    l.visible = o.value(QStringLiteral("visible")).toBool(l.visible);
    l.locked = o.value(QStringLiteral("locked")).toBool(l.locked);
}

QJsonObject nodeToJson(const Layer &node);

QJsonObject shapeToJson(const Shape &s) {
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("shape"));
    writeBase(o, s);
    o.insert(QStringLiteral("mask"), s.mask);

    QJsonArray color;
    for (int i = 0; i < 4; ++i) {
        color.append(static_cast<int>(s.color[i]));
    }
    o.insert(QStringLiteral("color"), color);

    QJsonObject visual;
    if (s.isRaster()) {
        visual.insert(QStringLiteral("kind"), QStringLiteral("raster"));
        visual.insert(QStringLiteral("raster_id"), static_cast<qint64>(s.rasterId));
        visual.insert(QStringLiteral("width"), s.rasterWidth);
        visual.insert(QStringLiteral("height"), s.rasterHeight);
    } else {
        visual.insert(QStringLiteral("kind"), QStringLiteral("vector"));
        visual.insert(QStringLiteral("shape_id"), static_cast<int>(s.shapeId));
    }
    o.insert(QStringLiteral("visual"), visual);

    QJsonObject debug;
    debug.insert(QStringLiteral("source_shape"), s.sourceShape);
    debug.insert(QStringLiteral("abs_offset"), s.absOffset);
    debug.insert(QStringLiteral("marker"), hex(s.marker));
    debug.insert(QStringLiteral("flags"), s.flags);
    debug.insert(QStringLiteral("source_logo_id"), static_cast<int>(s.sourceLogoId));
    debug.insert(QStringLiteral("has_source_transform"), s.hasSourceTransform);
    if (s.hasSourceTransform) {
        debug.insert(QStringLiteral("source_transform"), transformToJson(s.sourceTransform));
    }
    o.insert(QStringLiteral("debug"), debug);
    return o;
}

QJsonObject guideToJson(const GuideLayer &g) {
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("guide"));
    writeBase(o, g);
    o.insert(QStringLiteral("source_path"), g.sourcePath);
    if (g.preprocessColorCount > 0) {
        o.insert(QStringLiteral("preprocess_color_count"), g.preprocessColorCount);
    }
    QJsonObject image;
    if (g.image) {
        image.insert(QStringLiteral("format"), g.image->format);
        image.insert(QStringLiteral("width"), g.image->width);
        image.insert(QStringLiteral("height"), g.image->height);
        image.insert(QStringLiteral("image_bytes"), b64(g.image->encoded));
    }
    o.insert(QStringLiteral("image"), image);
    return o;
}

QJsonObject groupToJson(const Group &g) {
    QJsonObject o;
    o.insert(QStringLiteral("kind"), QStringLiteral("group"));
    writeBase(o, g);
    o.insert(QStringLiteral("is_livery_section"), g.isLiverySection);
    o.insert(QStringLiteral("livery_section_slot"), g.liverySectionSlot);

    QJsonObject debug;
    debug.insert(QStringLiteral("source_abs_pos"), g.sourceAbsPos);
    debug.insert(QStringLiteral("pending_transform_marker"), hex(g.pendingTransformMarker));
    debug.insert(QStringLiteral("inline_transform_marker"), hex(g.inlineTransformMarker));
    debug.insert(QStringLiteral("effective_transform_marker"), hex(g.effectiveTransformMarker));
    debug.insert(QStringLiteral("header_control_bytes"), hex(g.headerControlBytes));
    debug.insert(QStringLiteral("flags"), g.flags);
    debug.insert(QStringLiteral("source_parent_id"), g.sourceParentId);
    debug.insert(QStringLiteral("source_previous_sibling_id"), g.sourcePreviousSiblingId);
    debug.insert(QStringLiteral("source_previous_group_depth"), g.sourcePreviousGroupDepth);
    QJsonArray sourceChildrenJson;
    for (const QString &id : g.sourceChildren) {
        sourceChildrenJson.append(id);
    }
    debug.insert(QStringLiteral("source_child_ids"), sourceChildrenJson);
    o.insert(QStringLiteral("debug"), debug);

    QJsonArray children;
    for (const auto &child : g.children) {
        children.append(nodeToJson(*child));
    }
    o.insert(QStringLiteral("children"), children);
    return o;
}

QJsonObject nodeToJson(const Layer &node) {
    switch (node.kind()) {
    case LayerKind::Shape:
        return shapeToJson(static_cast<const Shape &>(node));
    case LayerKind::Guide:
        return guideToJson(static_cast<const GuideLayer &>(node));
    case LayerKind::Group:
        return groupToJson(static_cast<const Group &>(node));
    }
    return {};
}

std::unique_ptr<Layer> nodeFromJson(const QJsonObject &o);

std::unique_ptr<Shape> shapeFromJson(const QJsonObject &o) {
    auto shape = std::make_unique<Shape>();
    readBase(o, *shape);
    shape->mask = o.value(QStringLiteral("mask")).toBool(false);
    const QJsonArray color = o.value(QStringLiteral("color")).toArray();
    if (color.size() == 4) {
        for (int i = 0; i < 4; ++i) {
            shape->color[i] = static_cast<quint8>(std::clamp(color.at(i).toInt(), 0, 255));
        }
    }
    const QJsonObject visual = o.value(QStringLiteral("visual")).toObject();
    if (visual.value(QStringLiteral("kind")).toString() == QLatin1String("raster")) {
        shape->setRasterShape(static_cast<quint32>(visual.value(QStringLiteral("raster_id")).toInteger(0)),
                              visual.value(QStringLiteral("width")).toInt(256),
                              visual.value(QStringLiteral("height")).toInt(256));
    } else {
        shape->setVectorShape(static_cast<quint16>(visual.value(QStringLiteral("shape_id")).toInt(0)));
    }
    const QJsonObject debug = o.value(QStringLiteral("debug")).toObject();
    shape->sourceShape = debug.value(QStringLiteral("source_shape")).toInt(0);
    shape->absOffset = debug.value(QStringLiteral("abs_offset")).toInt(0);
    shape->marker = unhex(debug.value(QStringLiteral("marker")));
    shape->flags = debug.value(QStringLiteral("flags")).toInt(0);
    shape->sourceLogoId = static_cast<quint16>(debug.value(QStringLiteral("source_logo_id")).toInt(0));
    shape->hasSourceTransform = debug.value(QStringLiteral("has_source_transform")).toBool(false);
    if (shape->hasSourceTransform) {
        shape->sourceTransform = transformFromJson(debug.value(QStringLiteral("source_transform")).toObject());
    }
    return shape;
}

std::unique_ptr<GuideLayer> guideFromJson(const QJsonObject &o) {
    auto guide = std::make_unique<GuideLayer>();
    readBase(o, *guide);
    guide->sourcePath = o.value(QStringLiteral("source_path")).toString();
    guide->preprocessColorCount = std::clamp(
        o.value(QStringLiteral("preprocess_color_count")).toInt(0), 0, 256);
    auto raster = std::make_unique<RasterContainer>();
    const QJsonObject image = o.value(QStringLiteral("image")).toObject();
    raster->rasterId = 0;
    raster->format = image.value(QStringLiteral("format")).toString();
    raster->width = image.value(QStringLiteral("width")).toInt(0);
    raster->height = image.value(QStringLiteral("height")).toInt(0);
    raster->encoded = unb64(image.value(QStringLiteral("image_bytes")));
    guide->image = std::move(raster);
    return guide;
}

std::unique_ptr<Group> groupFromJson(const QJsonObject &o) {
    auto group = std::make_unique<Group>();
    readBase(o, *group);
    group->isLiverySection = o.value(QStringLiteral("is_livery_section")).toBool(false);
    group->liverySectionSlot = o.value(QStringLiteral("livery_section_slot")).toInt(-1);
    const QJsonObject debug = o.value(QStringLiteral("debug")).toObject();
    group->sourceAbsPos = debug.value(QStringLiteral("source_abs_pos")).toInt(0);
    group->pendingTransformMarker = unhex(debug.value(QStringLiteral("pending_transform_marker")));
    group->inlineTransformMarker = unhex(debug.value(QStringLiteral("inline_transform_marker")));
    group->effectiveTransformMarker = unhex(debug.value(QStringLiteral("effective_transform_marker")));
    group->headerControlBytes = unhex(debug.value(QStringLiteral("header_control_bytes")));
    group->flags = debug.value(QStringLiteral("flags")).toInt(0);
    group->sourceParentId = debug.value(QStringLiteral("source_parent_id")).toString();
    group->sourcePreviousSiblingId = debug.value(QStringLiteral("source_previous_sibling_id")).toString();
    group->sourcePreviousGroupDepth = debug.value(QStringLiteral("source_previous_group_depth")).toInt(0);
    const QJsonArray sourceChildrenJson = debug.value(QStringLiteral("source_child_ids")).toArray();
    for (const QJsonValue &value : sourceChildrenJson) {
        group->sourceChildren.push_back(value.toString());
    }
    const QJsonArray children = o.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &value : children) {
        if (value.isObject()) {
            group->append(nodeFromJson(value.toObject()));
        }
    }
    return group;
}

std::unique_ptr<Layer> nodeFromJson(const QJsonObject &o) {
    const QString kind = o.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("group")) {
        return groupFromJson(o);
    }
    if (kind == QLatin1String("guide")) {
        return guideFromJson(o);
    }
    return shapeFromJson(o);
}

} // namespace

void ensureProjectSceneRoot(fh6::Project &project) {
    if (!project.root) {
        project.root = std::make_unique<Group>();
    }
    if (project.root->id.isEmpty()) {
        project.root->id = QStringLiteral("__root__");
    }
    if (project.root->name.isEmpty()) {
        project.root->name = QStringLiteral("Project");
    }
}

QJsonObject sceneTreeToJson(const Group &root) {
    QJsonObject object;
    QJsonArray children;
    for (const auto &child : root.children) {
        children.append(nodeToJson(*child));
    }
    object.insert(QStringLiteral("children"), children);
    return object;
}

std::unique_ptr<Group> sceneTreeFromJson(const QJsonObject &object) {
    auto root = std::make_unique<Group>();
    root->id = QStringLiteral("__root__");
    root->name = QStringLiteral("Project");
    const QJsonArray children = object.value(QStringLiteral("children")).toArray();
    for (const QJsonValue &value : children) {
        if (value.isObject()) {
            root->append(nodeFromJson(value.toObject()));
        }
    }
    return root;
}

} // namespace fh6::scene
