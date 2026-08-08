#include "binary_io.h"
#include "cgroup_codec.h"
#include "vinyl_decoder.h"

#include <QtCore>

#include <array>
#include <exception>

namespace {

constexpr quint16 kShapeId = 101;
constexpr int kRootChildCount = 2;
constexpr quint8 kGeneration2Marker = 0x02;
constexpr quint8 kGeneration2ShapeMarker = 0x01;
constexpr quint8 kGeneration2TransformTerminator = 0x02;
constexpr quint8 kGeneration3Marker = 0x03;
constexpr quint8 kGeneration3ShapeMarker = 0x02;
constexpr quint8 kGeneration3TransformTerminator = 0x03;

void appendShapePayload(QByteArray &payload) {
    fls::detail::appendLeU16(payload, kShapeId);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
    payload.append(QByteArray::fromHex("ffffffff"));
}

void appendShape(QByteArray &payload, quint8 marker, bool framed) {
    if (framed) {
        payload.append('\x01');
    }
    payload.append(static_cast<char>(marker));
    appendShapePayload(payload);
}

void appendTransform(QByteArray &payload, quint8 terminator) {
    payload.append('\x01');
    payload.append(static_cast<char>(terminator));
    fls::detail::appendLeFloat(payload, 32.0f);
    fls::detail::appendLeFloat(payload, -16.0f);
    fls::detail::appendLeFloat(payload, 1.0f);
    fls::detail::appendLeFloat(payload, 0.0f);
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
                       bool terminalFlag) {
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
    appendShape(payload, shapeMarker, false);
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
        kGeneration2Marker, kGeneration2ShapeMarker,
        kGeneration2TransformTerminator, false, false);
    const QByteArray shapePayload = makePayload(
        kGeneration2Marker, kGeneration2ShapeMarker,
        kGeneration2TransformTerminator, true, false);
    const QByteArray terminalPayload = makePayload(
        kGeneration2Marker, kGeneration2ShapeMarker,
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

bool verifyUnmaskedGroup(const QString &path) {
    const fls::VinylGroup group = fls::decodeGroup(fls::readCGroupPayload(path), nullptr);
    QVector<const fls::VinylShape *> shapes;
    collectShapes(group, shapes);
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
    return testGeneration2MaskFraming() && testGeneration3TrailingMasks() ? 0 : 1;
}
