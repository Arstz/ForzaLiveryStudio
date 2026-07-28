#include "garage_panorama_renderer.h"

#include "swatchbin.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <algorithm>

namespace gui {
namespace {

constexpr char kPanoramaVertexShader[] = R"(#version 330 core
out vec2 ndc_position;

void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2(3.0, -1.0),
        vec2(-1.0, 3.0));
    ndc_position = positions[gl_VertexID];
    gl_Position = vec4(ndc_position, 1.0, 1.0);
}
)";

constexpr char kPanoramaFragmentShader[] = R"(#version 330 core
in vec2 ndc_position;

uniform sampler2D panorama_texture;
uniform mat4 inverse_view_projection;
uniform float spherical_power;
uniform float frame_scale;

out vec4 out_color;

void main()
{
    vec3 direction = normalize(
        (inverse_view_projection * vec4(ndc_position, 1.0, 1.0)).xyz);
    float theta = acos(clamp(abs(direction.y), 0.0, 1.0));
    float polar = theta * (2.0 / 3.14159265358979323846);
    float radius = theta * (1.0 / 3.14159265358979323846)
        * mix(1.0, polar, spherical_power);
    vec2 horizontal = vec2(direction.z, -direction.x);
    float horizontalLength = length(horizontal);
    vec2 disk = horizontalLength > 0.000001
        ? horizontal * (radius / horizontalLength)
        : vec2(0.0);
    vec2 uv = vec2((disk.x + 0.5) * 0.5, disk.y + 0.5);
    if (direction.y < 0.0) {
        uv.x += 0.5;
        uv.y = 1.0 - uv.y;
    }
    vec3 color = textureLod(panorama_texture, uv, 0.0).rgb
        / max(frame_scale, 0.000001);
    out_color = vec4(color, 1.0);
}
)";

bool validPanoramaTexture(const fh6::SwatchTexture &texture) {
    return texture.valid() && texture.platform == 0 && texture.arraySize == 1
        && texture.sliceCount == 1 && texture.mipCount == 1
        && texture.width == texture.height * 2 && texture.slices.size() == 1
        && texture.slices[0].encoding == fh6::SwatchEncoding::UnsignedBc6H
        && texture.slices[0].mipLevels.size() == 1;
}

} // namespace

QString GaragePanoramaRenderer::shaderSelfTest() {
    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, kPanoramaVertexShader)
        || !program.addShaderFromSourceCode(
            QOpenGLShader::Fragment, kPanoramaFragmentShader)
        || !program.link()) {
        return program.log();
    }
    return {};
}

void GaragePanoramaRenderer::initialize() {
    if (initialized_) {
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kPanoramaVertexShader)
        || !program_.addShaderFromSourceCode(
            QOpenGLShader::Fragment, kPanoramaFragmentShader)
        || !program_.link()) {
        qWarning().noquote() << "Garage panorama shader failed to build:" << program_.log();
        return;
    }
    if (!vao_.create()) {
        qWarning() << "Garage panorama geometry initialization failed";
        program_.removeAllShaders();
        return;
    }
    panoramaLocation_ = program_.uniformLocation("panorama_texture");
    inverseViewProjectionLocation_ = program_.uniformLocation("inverse_view_projection");
    sphericalPowerLocation_ = program_.uniformLocation("spherical_power");
    frameScaleLocation_ = program_.uniformLocation("frame_scale");
    initialized_ = true;
}

void GaragePanoramaRenderer::release() {
    clearPanorama();
    if (vao_.isCreated()) {
        vao_.destroy();
    }
    program_.removeAllShaders();
    panoramaLocation_ = -1;
    inverseViewProjectionLocation_ = -1;
    sphericalPowerLocation_ = -1;
    frameScaleLocation_ = -1;
    initialized_ = false;
}

void GaragePanoramaRenderer::clearPanorama() {
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (panoramaTexture_ != 0 && context != nullptr) {
        context->functions()->glDeleteTextures(1, &panoramaTexture_);
    }
    panoramaTexture_ = 0;
}

bool GaragePanoramaRenderer::setPanorama(
    const fh6::SwatchTexture &texture, QString *error) {
    const auto fail = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!initialized_ || context == nullptr) {
        return fail(QStringLiteral("panorama upload requires an initialized current GL context"));
    }
    if (!validPanoramaTexture(texture)) {
        return fail(QStringLiteral("panorama texture topology is unsupported"));
    }
    if (!context->hasExtension(QByteArrayLiteral("GL_ARB_texture_compression_bptc"))) {
        return fail(QStringLiteral("GL_ARB_texture_compression_bptc is unavailable"));
    }

    QOpenGLFunctions *functions = context->functions();
    GLint maximumTextureSize = 0;
    functions->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    if (texture.width > maximumTextureSize || texture.height > maximumTextureSize) {
        return fail(QStringLiteral("panorama exceeds GL_MAX_TEXTURE_SIZE"));
    }

    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    while (functions->glGetError() != GL_NO_ERROR) {
    }

    GLuint uploaded = 0;
    functions->glGenTextures(1, &uploaded);
    functions->glBindTexture(GL_TEXTURE_2D, uploaded);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    const QByteArray data = texture.mipBytes(0, 0);
    functions->glCompressedTexImage2D(
        GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,
        texture.width, texture.height, 0, data.size(), data.constData());
    const GLenum uploadError = functions->glGetError();
    functions->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    functions->glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    if (uploadError != GL_NO_ERROR) {
        functions->glDeleteTextures(1, &uploaded);
        return fail(QStringLiteral("panorama upload failed with GL error 0x%1")
                        .arg(uploadError, 0, 16));
    }

    clearPanorama();
    panoramaTexture_ = uploaded;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void GaragePanoramaRenderer::render(
    const QMatrix4x4 &view,
    const QMatrix4x4 &projection,
    float sphericalPower,
    float frameScale) {
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!initialized_ || panoramaTexture_ == 0 || context == nullptr) {
        return;
    }

    QMatrix4x4 rotationView = view;
    rotationView(0, 3) = 0.0f;
    rotationView(1, 3) = 0.0f;
    rotationView(2, 3) = 0.0f;
    bool invertible = false;
    const QMatrix4x4 inverseViewProjection = (projection * rotationView).inverted(&invertible);
    if (!invertible) {
        return;
    }

    QOpenGLExtraFunctions *functions = context->extraFunctions();
    const GLboolean depthEnabled = functions->glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendEnabled = functions->glIsEnabled(GL_BLEND);
    const GLboolean cullEnabled = functions->glIsEnabled(GL_CULL_FACE);
    GLboolean depthWriteEnabled = GL_TRUE;
    GLint previousActiveTexture = GL_TEXTURE0;
    GLint previousTexture = 0;
    GLint currentProgram = 0;
    GLint vertexArray = 0;
    functions->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteEnabled);
    functions->glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    functions->glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    functions->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);

    functions->glDisable(GL_DEPTH_TEST);
    functions->glDepthMask(GL_FALSE);
    functions->glDisable(GL_BLEND);
    functions->glDisable(GL_CULL_FACE);
    functions->glBindTexture(GL_TEXTURE_2D, panoramaTexture_);
    program_.bind();
    program_.setUniformValue(panoramaLocation_, 0);
    program_.setUniformValue(inverseViewProjectionLocation_, inverseViewProjection);
    program_.setUniformValue(
        sphericalPowerLocation_, std::clamp(sphericalPower, 0.0f, 1.0f));
    program_.setUniformValue(frameScaleLocation_, std::max(frameScale, 0.000001f));
    vao_.bind();
    functions->glDrawArrays(GL_TRIANGLES, 0, 3);

    functions->glBindVertexArray(static_cast<GLuint>(vertexArray));
    functions->glUseProgram(static_cast<GLuint>(currentProgram));
    functions->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    functions->glActiveTexture(static_cast<GLenum>(previousActiveTexture));
    functions->glDepthMask(depthWriteEnabled);
    if (depthEnabled == GL_TRUE) {
        functions->glEnable(GL_DEPTH_TEST);
    }
    if (blendEnabled == GL_TRUE) {
        functions->glEnable(GL_BLEND);
    }
    if (cullEnabled == GL_TRUE) {
        functions->glEnable(GL_CULL_FACE);
    }
}

} // namespace gui
