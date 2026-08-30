#include "binary_io.h"
#include "cgroup_codec.h"
#include "vinyl_decoder.h"

#include <QtCore>

#include <array>
#include <cmath>
#include <exception>

namespace {

constexpr quint16 kShapeId = 101;
constexpr quint16 kImpactLowercaseAWireAlias = 3000;
constexpr quint16 kImpactLowercaseA = 3001;
constexpr int kRootChildCount = 2;
constexpr quint8 kGeneration2Marker = 0x02;
constexpr quint8 kLegacyShapeMarker = 0x01;
constexpr quint8 kGeneration2TransformTerminator = 0x02;
constexpr quint8 kGeneration3Marker = 0x03;
constexpr quint8 kGeneration3ShapeMarker = 0x02;
constexpr quint8 kGeneration3TransformTerminator = 0x03;

void appendShapePayload(QByteArray &payload, quint16 shapeId = kShapeId) {
    fls::detail::appendLeU16(payload, shapeId);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append(QByteArray::fromHex("ffffffff"));
}

void appendShape(QByteArray &payload, quint8 marker, bool framed,
                 quint16 shapeId = kShapeId) {
    if (framed) {
        payload.append('\x01');
    }
    payload.append(static_cast<char>(marker));
    appendShapePayload(payload, shapeId);
}

void appendMarkerlessGroupHeader(QByteArray &payload, int count, quint8 bitmap) {
    fls::detail::appendLeU16(payload, static_cast<quint16>(count));
    fls::detail::appendLeU16(payload, 1);
    payload.append(QByteArray(2, '\0'));
    payload.append(static_cast<char>(bitmap));
}

void appendLiveryLockedGroup(QByteArray &payload, quint8 transformMarker,
                             quint8 frameMarker) {
    payload.append(static_cast<char>(transformMarker));
    fls::detail::appendLeFloat(payload, 10.0f);
    fls::detail::appendLeFloat(payload, -20.0f);
    fls::detail::appendLeFloat(payload, -2.0f);
    fls::detail::appendLeFloat(payload, 25.0f);
    payload.append(static_cast<char>(frameMarker));
    payload.append(QByteArray::fromHex("1122334455667788"));
    fls::detail::appendLeFloat(payload, 3.0f);
    appendMarkerlessGroupHeader(payload, kRootChildCount, 0x00);
    appendShape(payload, kGeneration3ShapeMarker, false);
    appendShape(payload, kGeneration3ShapeMarker, false);
}

void appendTransform(QByteArray &payload, quint8 terminator) {
    payload.append('\x01');
    payload.append(static_cast<char>(terminator));
    fls::detail::appendLeFloat(payload, 32.0f);
    fls::detail::appendLeFloat(payload, -16.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
}

void appendGeneration1Transform(QByteArray &payload, float px) {
    payload.append('\x01');
    fls::detail::appendLeFloat(payload, px);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
}

void appendTransformTrailer(QByteArray &payload) {
    payload.append(QByteArray::fromHex("21949fe18af9010900"));
}

void appendNestedShapeGroup(QByteArray &payload, quint8 shapeMarker) {
    payload.append('\x20');
    fls::detail::appendLeU16(payload, 1);
    fls::detail::appendLeU16(payload, 1);
    payload.append(QByteArray(2, '\0'));
    payload.append('\0');
    appendShape(payload, shapeMarker, false);
}

QByteArray makePayload(quint8 generationMarker, quint8 shapeMarker,
                       quint8 transformTerminator, bool framedSecondShape,
                       bool terminalFlag, quint16 firstShapeId = kShapeId) {
    QByteArray payload("gyvl", 4);
    const quint8 childBitmap = framedSecondShape ? 0x00 : 0x02;

    fls::detail::appendLeU32(payload, 1);
    fls::detail::appendLeU32(payload, 0);
    payload.append(static_cast<char>(generationMarker));
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append('\x20');
    fls::detail::appendLeU16(payload, kRootChildCount);
    payload.append('\x01');
    payload.append(QByteArray(3, '\0'));
    payload.append(static_cast<char>(childBitmap));
    appendShape(payload, shapeMarker, false, firstShapeId);
    if (framedSecondShape) {
        appendShape(payload, shapeMarker, true);
    } else {
        appendTransform(payload, transformTerminator);
        appendNestedShapeGroup(payload, shapeMarker);
    }
    if (terminalFlag) {
        payload.append('\x01');
    }

    return payload;
}

QByteArray makeMarkerlessRootPayload() {
    QByteArray payload("gyvl", 4);

    fls::detail::appendLeU32(payload, 1);
    fls::detail::appendLeU32(payload, 0);
    payload.append(static_cast<char>(0x01));
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append('\0');
    fls::detail::appendLeU16(payload, kRootChildCount);
    payload.append('\x01');
    payload.append(QByteArray(3, '\0'));
    payload.append('\0');
    appendShape(payload, kLegacyShapeMarker, false);
    appendShape(payload, kLegacyShapeMarker, false);

    return payload;
}

QByteArray makeGeneration1NestedPayload() {
    QByteArray payload("gyvl", 4);

    fls::detail::appendLeU32(payload, 1);
    fls::detail::appendLeU32(payload, 0);
    payload.append('\x01');
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append('\0');
    fls::detail::appendLeU16(payload, 1);
    payload.append('\x01');
    payload.append(QByteArray(3, '\0'));
    payload.append('\x01');
    appendMarkerlessGroupHeader(payload, kRootChildCount, 0x00);
    appendShape(payload, kLegacyShapeMarker, false);
    appendShape(payload, kLegacyShapeMarker, false);

    return payload;
}

QByteArray makeGeneration1SiblingTransformPayload() {
    QByteArray payload("gyvl", 4);

    fls::detail::appendLeU32(payload, 1);
    fls::detail::appendLeU32(payload, 0);
    payload.append('\x01');
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append('\0');
    fls::detail::appendLeU16(payload, 1);
    payload.append('\x01');
    payload.append(QByteArray(3, '\0'));
    payload.append('\x01');
    appendMarkerlessGroupHeader(payload, kRootChildCount, 0x03);
    appendGeneration1Transform(payload, 10.0f);
    payload.append('\0');
    appendMarkerlessGroupHeader(payload, kRootChildCount, 0x00);
    appendShape(payload, kLegacyShapeMarker, false);
    appendShape(payload, kLegacyShapeMarker, false);
    appendGeneration1Transform(payload, 20.0f);
    appendMarkerlessGroupHeader(payload, kRootChildCount, 0x00);
    appendShape(payload, kLegacyShapeMarker, false);
    appendShape(payload, kLegacyShapeMarker, false);

    return payload;
}

QByteArray makeStandaloneTransformTrailerPayload() {
    QByteArray payload("gyvl", 4);

    fls::detail::appendLeU32(payload, 1);
    fls::detail::appendLeU32(payload, 0);
    payload.append(static_cast<char>(kGeneration3Marker));
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append('\x20');
    fls::detail::appendLeU16(payload, 1);
    payload.append('\x01');
    payload.append(QByteArray(3, '\0'));
    payload.append('\x01');
    appendTransform(payload, kGeneration3TransformTerminator);
    appendTransformTrailer(payload);
    appendMarkerlessGroupHeader(payload, kRootChildCount, 0x00);
    appendShape(payload, kGeneration3ShapeMarker, false);
    appendShape(payload, kGeneration3ShapeMarker, false);

    return payload;
}

void collectShapes(const fls::VinylGroup &group, QVector<const fls::VinylShape *> &shapes) {
    for (const fls::VinylItem &item : group.items) {
        if (item.isShape()) {
            shapes.push_back(&std::get<fls::VinylShape>(item.value));
        } else {
            collectShapes(*std::get<fls::VinylGroupPtr>(item.value), shapes);
        }
    }
}

bool expectMaskState(const QByteArray &payload, const std::array<bool, 2> &expected,
                     const char *failure) {
    const fls::VinylGroup group = fls::decodeGroup(payload, nullptr);
    QVector<const fls::VinylShape *> shapes;
    collectShapes(group, shapes);
    if (shapes.size() != static_cast<int>(expected.size())) {
        qCritical() << failure << "decoded" << shapes.size() << "shapes";
        return false;
    }
    for (int i = 0; i < shapes.size(); ++i) {
        if (shapes[i]->isMask != expected[static_cast<std::size_t>(i)]) {
            qCritical() << failure << "at shape" << i;
            return false;
        }
    }

    return true;
}

bool testGeneration2MaskFraming() {
    const QByteArray transformPayload = makePayload(
        kGeneration2Marker, kLegacyShapeMarker,
        kGeneration2TransformTerminator, false, false);
    const QByteArray shapePayload = makePayload(
        kGeneration2Marker, kLegacyShapeMarker,
        kGeneration2TransformTerminator, true, false);
    const QByteArray terminalPayload = makePayload(
        kGeneration2Marker, kLegacyShapeMarker,
        kGeneration2TransformTerminator, true, true);
    return expectMaskState(transformPayload, {false, false},
                           "generation-2 transform framing set mask state")
        && expectMaskState(shapePayload, {false, false},
                           "generation-2 shape framing set mask state")
        && expectMaskState(terminalPayload, {false, false},
                           "generation-2 terminal framing set mask state");
}

bool testGeneration3TrailingMasks() {
    const QByteArray transformPayload = makePayload(
        kGeneration3Marker, kGeneration3ShapeMarker,
        kGeneration3TransformTerminator, false, false);
    const QByteArray shapePayload = makePayload(
        kGeneration3Marker, kGeneration3ShapeMarker,
        kGeneration3TransformTerminator, true, false);
    const QByteArray terminalPayload = makePayload(
        kGeneration3Marker, kGeneration3ShapeMarker,
        kGeneration3TransformTerminator, true, true);
    return expectMaskState(transformPayload, {true, false},
                           "generation-3 transform mask state was lost")
        && expectMaskState(shapePayload, {true, false},
                           "generation-3 shape mask state was lost")
        && expectMaskState(terminalPayload, {true, true},
                           "generation-3 terminal mask state was lost");
}

bool testShapeIdAliases() {
    const QByteArray payload = makePayload(
        kGeneration3Marker, kGeneration3ShapeMarker,
        kGeneration3TransformTerminator, true, false,
        kImpactLowercaseAWireAlias);

    const fls::VinylGroup group = fls::decodeGroup(payload, nullptr);
    QVector<const fls::VinylShape *> shapes;
    collectShapes(group, shapes);

    return shapes.size() == kRootChildCount
        && shapes.front()->shapeId == kImpactLowercaseA;
}

bool testStandaloneTransformTrailer() {
    const fls::VinylGroup root = fls::decodeGroup(
        makeStandaloneTransformTrailerPayload(), nullptr);
    QVector<const fls::VinylShape *> shapes;
    collectShapes(root, shapes);
    if (root.items.size() != 1 || root.items.front().isShape()) {
        return false;
    }
    const fls::VinylGroup &group = *std::get<fls::VinylGroupPtr>(
        root.items.front().value);

    return root.totalChildren() == 1
        && group.expectedChildren == kRootChildCount
        && group.totalChildren() == kRootChildCount
        && shapes.size() == kRootChildCount
        && std::abs(group.px - 32.0) < 1e-6;
}

bool testGeneration1MarkerlessRoot() {
    const QByteArray payload = makeMarkerlessRootPayload();
    fls::LayerData layerData;
    const fls::VinylGroup group = fls::decodeGroup(payload, &layerData);
    QVector<const fls::VinylShape *> shapes;
    collectShapes(group, shapes);

    const fls::VinylGroup nestedRoot = fls::decodeGroup(
        makeGeneration1NestedPayload(), nullptr);
    QVector<const fls::VinylShape *> nestedShapes;
    collectShapes(nestedRoot, nestedShapes);
    const bool validNestedGroup = nestedRoot.items.size() == 1
        && !nestedRoot.items.front().isShape()
        && std::get<fls::VinylGroupPtr>(nestedRoot.items.front().value)->totalChildren()
            == kRootChildCount;

    const fls::VinylGroup transformedRoot = fls::decodeGroup(
        makeGeneration1SiblingTransformPayload(), nullptr);
    const bool hasTransformParent = transformedRoot.items.size() == 1
        && !transformedRoot.items.front().isShape();
    const fls::VinylGroupPtr transformParent = hasTransformParent
        ? std::get<fls::VinylGroupPtr>(transformedRoot.items.front().value)
        : fls::VinylGroupPtr{};
    const bool validSiblingTransforms = transformParent
        && std::abs(transformParent->px) < 1e-6
        && transformParent->items.size() == kRootChildCount
        && !transformParent->items.front().isShape()
        && !transformParent->items.back().isShape()
        && std::abs(std::get<fls::VinylGroupPtr>(
            transformParent->items.front().value)->px - 10.0) < 1e-6
        && std::abs(std::get<fls::VinylGroupPtr>(
            transformParent->items.back().value)->px - 20.0) < 1e-6;

    return layerData.start == 37
        && group.expectedChildren == kRootChildCount
        && shapes.size() == kRootChildCount
        && validNestedGroup
        && nestedShapes.size() == kRootChildCount
        && validSiblingTransforms;
}

bool testLiveryLockedTransformFrames() {
    QByteArray body;
    appendMarkerlessGroupHeader(body, kRootChildCount, 0x03);
    appendLiveryLockedGroup(body, 0x00, 0x71);
    appendLiveryLockedGroup(body, 0x01, 0x31);

    const QVector<fls::LiverySection> sections = fls::buildLiverySections(
        body, QVector<int>{kRootChildCount * 2});
    if (sections.size() != 11) {
        qCritical() << "livery locked frame test decoded" << sections.size() << "sections";
        return false;
    }
    const fls::VinylGroup &section = sections.front().subtree;
    if (section.items.size() != 1 || section.items.front().isShape()) {
        qCritical() << "livery locked frame root was not decoded";
        return false;
    }
    const fls::VinylGroup &root = *std::get<fls::VinylGroupPtr>(section.items.front().value);
    if (root.items.size() != kRootChildCount
        || root.items.front().isShape() || root.items.back().isShape()) {
        qCritical() << "livery locked frame hierarchy was not decoded";
        return false;
    }
    const fls::VinylGroup &masked = *std::get<fls::VinylGroupPtr>(root.items.front().value);
    const fls::VinylGroup &plain = *std::get<fls::VinylGroupPtr>(root.items.back().value);
    const auto validTransform = [](const fls::VinylGroup &group) {
        return std::abs(group.px - 10.0) < 1e-6
            && std::abs(group.py + 20.0) < 1e-6
            && std::abs(group.sx + 2.0) < 1e-6
            && std::abs(group.sy - 3.0) < 1e-6
            && std::abs(group.rot - 25.0) < 1e-6;
    };

    return masked.items.size() == kRootChildCount
        && plain.items.size() == kRootChildCount
        && masked.isMask && (masked.flags & 0x40)
        && !plain.isMask && !(plain.flags & 0x40)
        && validTransform(masked) && validTransform(plain);
}

bool verifyUnmaskedGroup(const QString &path) {
    const fls::VinylGroup group = fls::decodeGroup(
        fls::readCGroupPayload(path), nullptr);
    QVector<const fls::VinylShape *> shapes;
    collectShapes(group, shapes);
    const QVector<QString> errors = fls::validateTree(group);
    for (const QString &error : errors) {
        qCritical().noquote() << error;
    }
    if (!errors.isEmpty()
        || (group.expectedChildren
            && group.totalChildren() != *group.expectedChildren)) {
        return false;
    }
    for (const fls::VinylShape *shape : shapes) {
        if (shape->isMask) {
            qCritical() << "unexpected mask at payload offset" << shape->absPos;
            return false;
        }
    }
    qInfo() << "decoded unmasked shapes:" << shapes.size();

    return !shapes.isEmpty();
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 2) {
        try {
            return verifyUnmaskedGroup(arguments[1]) ? 0 : 1;
        } catch (const std::exception &error) {
            qCritical() << error.what();
            return 1;
        }
    }
    return testGeneration1MarkerlessRoot()
        && testGeneration2MaskFraming() && testGeneration3TrailingMasks()
        && testShapeIdAliases() && testStandaloneTransformTrailer()
        && testLiveryLockedTransformFrames() ? 0 : 1;
}
