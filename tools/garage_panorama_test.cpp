#include "garage_environment.h"

#include <QCoreApplication>
#include <QFile>

#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool close(float left, float right, float tolerance = 0.00001f) {
    return std::abs(left - right) <= tolerance;
}

QByteArray metadata(
    const QByteArray &mode = "2", const QByteArray &power = "0.5",
    const QByteArray &rotation = "0", const QByteArray &scale = "1") {
    return QByteArray("<FrameData spherical=\"") + mode
        + "\" sphericalpower=\"" + power
        + "\" rotation=\"" + rotation
        + "\"><Frame scale=\"" + scale + "\"/></FrameData>";
}

fh6::SwatchTexture panoramaTexture() {
    fh6::SwatchTexture texture;
    texture.width = 8;
    texture.height = 4;
    texture.arraySize = 1;
    texture.platform = 0;
    texture.sliceCount = 1;
    texture.mipCount = 1;
    texture.payload = QByteArray(32, '\0');
    fh6::SwatchTextureSlice slice;
    slice.encoding = fh6::SwatchEncoding::UnsignedBc6H;
    slice.mipLevels.push_back({0, 32, 0xFFFFFFFFu});
    texture.slices.push_back(std::move(slice));
    return texture;
}

void testMetadata() {
    fh6::GaragePanoramaResources panorama;
    QString error;
    expect(fh6::parseGaragePanoramaMetadata(metadata(), &panorama, &error),
           "valid panorama metadata parses");
    expect(panorama.sphericalMode == 2 && close(panorama.sphericalPower, 0.5f)
               && close(panorama.rotation, 0.0f) && close(panorama.frameScale, 1.0f),
           "panorama metadata fields are retained");
    expect(!fh6::parseGaragePanoramaMetadata(metadata("1"), &panorama, &error),
           "unsupported panorama projection mode is rejected");
    expect(!fh6::parseGaragePanoramaMetadata(metadata("2", "1.1"), &panorama, &error),
           "out-of-range spherical power is rejected");
    expect(!fh6::parseGaragePanoramaMetadata(metadata("2", "0.5", "1"), &panorama, &error),
           "unsupported panorama rotation is rejected");
    expect(!fh6::parseGaragePanoramaMetadata(metadata("2", "0.5", "0", "0"), &panorama, &error),
           "non-positive panorama frame scale is rejected");
    expect(!fh6::parseGaragePanoramaMetadata("<FrameData>", &panorama, &error),
           "malformed panorama metadata is rejected");
    expect(!fh6::parseGaragePanoramaMetadata(
               metadata().replace("</FrameData>", "<broken></FrameData>"),
               &panorama, &error),
           "malformed metadata after the first frame is rejected");
    expect(!fh6::parseGaragePanoramaMetadata(metadata(), nullptr, &error),
           "null panorama metadata destination is rejected");
}

void testTextureContract() {
    QString error;
    fh6::SwatchTexture texture = panoramaTexture();
    expect(fh6::validateGaragePanoramaTexture(texture, &error),
           "valid dual-hemisphere BC6H texture is accepted");

    fh6::SwatchTexture wrongRatio = texture;
    wrongRatio.width = 4;
    expect(!fh6::validateGaragePanoramaTexture(wrongRatio, &error),
           "non-dual-hemisphere panorama dimensions are rejected");

    fh6::SwatchTexture wrongEncoding = texture;
    wrongEncoding.slices[0].encoding = fh6::SwatchEncoding::Bc7;
    expect(!fh6::validateGaragePanoramaTexture(wrongEncoding, &error),
           "non-BC6H panorama encoding is rejected");

    fh6::SwatchTexture truncated = texture;
    truncated.payload.chop(1);
    expect(!fh6::validateGaragePanoramaTexture(truncated, &error),
           "truncated panorama BC6H payload is rejected");
}

void expectUv(
    const std::array<float, 3> &direction, float power,
    float expectedU, float expectedV, const char *message) {
    const std::array<float, 2> uv = fh6::garagePanoramaUv(direction, power);
    expect(close(uv[0], expectedU) && close(uv[1], expectedV), message);
}

void testProjection() {
    expectUv({0.0f, 1.0f, 0.0f}, 0.5f, 0.25f, 0.5f,
             "upper pole maps to the left disk center");
    expectUv({0.0f, -1.0f, 0.0f}, 0.5f, 0.75f, 0.5f,
             "lower pole maps to the right disk center");
    expectUv({0.0f, 0.0f, 1.0f}, 0.5f, 0.5f, 0.5f,
             "forward equator maps to the left disk edge");
    expectUv({0.0f, 0.0f, -1.0f}, 0.5f, 0.0f, 0.5f,
             "back equator maps to the opposite left disk edge");
    expectUv({1.0f, 0.0f, 0.0f}, 0.5f, 0.25f, 0.0f,
             "right equator maps to the top of the left disk");
    expectUv({0.0f, 1.0f, 1.0f}, 0.5f, 0.34375f, 0.5f,
             "spherical power applies the recovered radial warp");
    expectUv({0.0f, -1.0f, 1.0f}, 0.5f, 0.84375f, 0.5f,
             "lower hemisphere applies the recovered disk offset and flip");

    bool coordinatesInRange = true;
    bool hemispheresSeparated = true;
    for (int y = -8; y <= 8; ++y) {
        for (int x = -8; x <= 8; ++x) {
            for (int z = -8; z <= 8; ++z) {
                if (x == 0 && y == 0 && z == 0) {
                    continue;
                }
                const std::array<float, 2> uv = fh6::garagePanoramaUv(
                    {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)},
                    0.5f);
                coordinatesInRange = coordinatesInRange
                    && uv[0] >= 0.0f && uv[0] <= 1.0f
                    && uv[1] >= 0.0f && uv[1] <= 1.0f;
                hemispheresSeparated = hemispheresSeparated
                    && (y < 0 ? uv[0] >= 0.5f : uv[0] <= 0.5f);
            }
        }
    }
    expect(coordinatesInRange,
           "projected panorama coordinates stay inside the texture");
    expect(hemispheresSeparated,
           "projected direction remains in its assigned hemisphere disk");
}

void testGameEvidence(const QString &metadataPath, const QString &texturePath) {
    QFile metadataFile(metadataPath);
    expect(metadataFile.open(QIODevice::ReadOnly), "game panorama metadata opens");
    fh6::GaragePanoramaResources panorama;
    QString error;
    if (metadataFile.isOpen()) {
        expect(fh6::parseGaragePanoramaMetadata(
                   metadataFile.readAll(), &panorama, &error),
               "game panorama metadata parses");
        expect(panorama.sphericalMode == 2 && close(panorama.sphericalPower, 0.5f)
                   && close(panorama.rotation, 0.0f) && close(panorama.frameScale, 1.0f),
               "game panorama metadata matches verified evidence");
    }

    QFile textureFile(texturePath);
    expect(textureFile.open(QIODevice::ReadOnly), "game panorama texture opens");
    if (textureFile.isOpen()) {
        const auto texture = fh6::parseSwatchTexture(textureFile.readAll(), &error);
        expect(texture.has_value(), "game panorama texture parses");
        if (texture) {
            expect(fh6::validateGaragePanoramaTexture(*texture, &error),
                   "game panorama texture satisfies the runtime contract");
            expect(texture->width == 8192 && texture->height == 4096,
                   "game panorama dimensions match verified evidence");
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testMetadata();
    testTextureContract();
    testProjection();
    if (app.arguments().size() == 3) {
        testGameEvidence(app.arguments()[1], app.arguments()[2]);
    }
    if (failures == 0) {
        std::puts("garage panorama tests passed");
    }
    return failures == 0 ? 0 : 1;
}
