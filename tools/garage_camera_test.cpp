#include "garage_camera_presets.h"

#include <QCoreApplication>
#include <QFile>
#include <QHash>
#include <QXmlStreamReader>

#include <cmath>
#include <cstdio>

namespace {

struct RawCameraValues {
    QVector3D position;
    QVector3D forward;
    float fovDegrees = 0.0f;
};

bool nearlyEqual(float actual, float expected, float tolerance = 1.0e-5f) {
    return std::abs(actual - expected) <= tolerance;
}

bool vectorsNearlyEqual(
    const QVector3D &actual, const QVector3D &expected, float tolerance = 1.0e-5f) {
    return (actual - expected).length() <= tolerance;
}

int fail(const char *message) {
    std::fprintf(stderr, "%s\n", message);

    return 1;
}

bool attributeFloat(
    const QXmlStreamAttributes &attributes, QAnyStringView name, float *value) {
    bool converted = false;
    const float parsed = attributes.value(name).toFloat(&converted);
    if (converted) {
        *value = parsed;
    }

    return converted;
}

bool loadEvidence(
    const QString &path, QHash<QString, RawCameraValues> *values, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != u"Preset") {
            continue;
        }
        const QXmlStreamAttributes attributes = xml.attributes();
        const QString name = attributes.value(u"name").toString();
        RawCameraValues raw;
        float posX = 0.0f;
        float posY = 0.0f;
        float posZ = 0.0f;
        float faceX = 0.0f;
        float faceY = 0.0f;
        float faceZ = 0.0f;
        if (!attributeFloat(attributes, u"posx", &posX)
            || !attributeFloat(attributes, u"posy", &posY)
            || !attributeFloat(attributes, u"posz", &posZ)
            || !attributeFloat(attributes, u"facex", &faceX)
            || !attributeFloat(attributes, u"facey", &faceY)
            || !attributeFloat(attributes, u"facez", &faceZ)
            || !attributeFloat(attributes, u"fov", &raw.fovDegrees)) {
            *error = QStringLiteral("Invalid numeric camera attributes for %1").arg(name);
            return false;
        }
        raw.position = QVector3D(posX, posY, posZ);
        raw.forward = QVector3D(faceX, faceY, faceZ);
        values->insert(name, raw);
    }
    if (xml.hasError()) {
        *error = xml.errorString();
        return false;
    }

    return true;
}

int verifyMath() {
    const auto &definitions = gui::garageCameraDefinitions();
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        const auto preset = static_cast<gui::GarageCameraPreset>(index);
        const gui::GarageCameraDefinition &definition = definitions[index];
        const gui::GarageCameraFrame frame = gui::garageCameraFrame(preset);
        const QVector3D expectedPosition(
            -definition.gamePosition.x(),
            definition.gamePosition.y(),
            definition.gamePosition.z());
        const QVector3D expectedForward(
            -definition.gameForward.x(),
            definition.gameForward.y(),
            definition.gameForward.z());
        if (!nearlyEqual(definition.gameForward.length(), 1.0f, 2.0e-5f)) {
            return fail("Camera source direction is not normalized");
        }
        if (!vectorsNearlyEqual(frame.position, expectedPosition)) {
            return fail("Camera position did not pass through the X-mirror contract");
        }
        if (!vectorsNearlyEqual(
                gui::garageCameraForward(frame), expectedForward.normalized())) {
            return fail("Camera orientation does not preserve the source face direction");
        }
        if (!nearlyEqual(
                QVector3D::dotProduct(
                    gui::garageCameraForward(frame), gui::garageCameraUp(frame)),
                0.0f)) {
            return fail("Camera forward and up axes are not orthogonal");
        }
    }

    const gui::GarageCameraFrame from =
        gui::garageCameraFrame(gui::GarageCameraPreset::PaintColor);
    const gui::GarageCameraFrame to =
        gui::garageCameraFrame(gui::GarageCameraPreset::LiveryBack);
    const gui::GarageCameraFrame before = gui::interpolateCameraFrame(from, to, -1.0f);
    const gui::GarageCameraFrame after = gui::interpolateCameraFrame(from, to, 2.0f);
    const gui::GarageCameraFrame middle = gui::interpolateCameraFrame(from, to, 0.5f);
    if (!vectorsNearlyEqual(before.position, from.position)
        || !vectorsNearlyEqual(after.position, to.position)
        || !nearlyEqual(before.fovDegrees, from.fovDegrees)
        || !nearlyEqual(after.fovDegrees, to.fovDegrees)) {
        return fail("Camera frame interpolation is not bounded");
    }
    if (!nearlyEqual(middle.orientation.length(), 1.0f)) {
        return fail("Camera quaternion interpolation is not normalized");
    }

    float previous = gui::boundedCameraEase(-1.0f);
    if (!nearlyEqual(previous, 0.0f)
        || !nearlyEqual(gui::boundedCameraEase(2.0f), 1.0f)) {
        return fail("Camera easing endpoints are not bounded");
    }
    for (int step = 1; step <= 100; ++step) {
        const float amount = static_cast<float>(step) / 100.0f;
        const float eased = gui::boundedCameraEase(amount);
        if (eased < previous || eased < 0.0f || eased > 1.0f) {
            return fail("Camera easing is not monotonic and bounded");
        }
        previous = eased;
    }

    return 0;
}

int verifyEvidence(const QString &path) {
    QHash<QString, RawCameraValues> values;
    QString error;
    if (!loadEvidence(path, &values, &error)) {
        std::fprintf(stderr, "%s\n", qPrintable(error));
        return 1;
    }
    for (const gui::GarageCameraDefinition &definition : gui::garageCameraDefinitions()) {
        const QString name = QString::fromLatin1(definition.sourceName);
        const auto found = values.constFind(name);
        if (found == values.constEnd()) {
            std::fprintf(stderr, "Missing camera evidence for %s\n", qPrintable(name));
            return 1;
        }
        if (!vectorsNearlyEqual(found->position, definition.gamePosition, 1.0e-6f)
            || !vectorsNearlyEqual(found->forward, definition.gameForward, 1.0e-6f)
            || !nearlyEqual(found->fovDegrees, definition.fovDegrees, 1.0e-6f)) {
            std::fprintf(stderr, "Camera evidence differs for %s\n", qPrintable(name));
            return 1;
        }
    }

    return 0;
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const int mathResult = verifyMath();
    if (mathResult != 0 || argc < 2) {
        return mathResult;
    }

    return verifyEvidence(QString::fromLocal8Bit(argv[1]));
}
