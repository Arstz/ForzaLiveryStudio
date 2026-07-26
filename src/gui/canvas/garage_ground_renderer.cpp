#include "garage_ground_renderer.h"

#include "color_space.h"

#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#include <algorithm>
#include <array>

namespace gui {
namespace {

constexpr char kGroundVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 in_position;

uniform mat4 mvp;
uniform vec3 ground_center;
uniform float plane_half_size;

out vec2 world_xz;

void main()
{
    vec3 world = vec3(
        ground_center.x + in_position.x * plane_half_size,
        ground_center.y,
        ground_center.z + in_position.y * plane_half_size);
    world_xz = world.xz;
    gl_Position = mvp * vec4(world, 1.0);
}
)";

constexpr char kGroundFragmentShader[] = R"(#version 330 core
in vec2 world_xz;

uniform vec2 shadow_center;
uniform vec2 shadow_radii;
uniform vec3 ground_color;
uniform vec3 background_color;
uniform float plane_half_size;
uniform float shadow_opacity;

out vec4 out_color;

void main()
{
    vec2 fromShadow = world_xz - shadow_center;
    vec2 ellipse = fromShadow / max(shadow_radii, vec2(0.0001));
    float ellipseDistance = dot(ellipse, ellipse);
    float broadShadow = 1.0 - smoothstep(0.08, 1.0, ellipseDistance);
    float contactShadow = 1.0 - smoothstep(0.0, 1.0, ellipseDistance * 2.4);
    float shadow = shadow_opacity * (0.72 * broadShadow + 0.28 * contactShadow);

    float planeDistance = length(fromShadow) / max(plane_half_size, 0.0001);
    float planeWeight = 1.0 - smoothstep(0.62, 0.98, planeDistance);
    vec3 floorColor = mix(background_color, ground_color, planeWeight);
    out_color = vec4(floorColor * (1.0 - shadow), 1.0);
}
)";

} // namespace

QString GarageGroundRenderer::shaderSelfTest() {
    QOpenGLShaderProgram program;
    if (!program.addShaderFromSourceCode(QOpenGLShader::Vertex, kGroundVertexShader)
        || !program.addShaderFromSourceCode(QOpenGLShader::Fragment, kGroundFragmentShader)
        || !program.link()) {
        return program.log();
    }
    return {};
}

void GarageGroundRenderer::initialize() {
    if (initialized_) {
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kGroundVertexShader)
        || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kGroundFragmentShader)
        || !program_.link()) {
        qWarning().noquote() << "Garage ground shader failed to build:" << program_.log();
        return;
    }

    static constexpr std::array<float, 8> vertices = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    if (!vao_.create() || !vertexBuffer_.create()) {
        qWarning() << "Garage ground geometry initialization failed";
        release();
        return;
    }
    vao_.bind();
    vertexBuffer_.bind();
    vertexBuffer_.allocate(vertices.data(), static_cast<int>(vertices.size() * sizeof(float)));
    program_.bind();
    program_.enableAttributeArray(0);
    program_.setAttributeBuffer(0, GL_FLOAT, 0, 2, 2 * sizeof(float));
    program_.release();
    vertexBuffer_.release();
    vao_.release();

    mvpLocation_ = program_.uniformLocation("mvp");
    groundCenterLocation_ = program_.uniformLocation("ground_center");
    planeHalfSizeLocation_ = program_.uniformLocation("plane_half_size");
    shadowCenterLocation_ = program_.uniformLocation("shadow_center");
    shadowRadiiLocation_ = program_.uniformLocation("shadow_radii");
    groundColorLocation_ = program_.uniformLocation("ground_color");
    backgroundColorLocation_ = program_.uniformLocation("background_color");
    shadowOpacityLocation_ = program_.uniformLocation("shadow_opacity");
    initialized_ = true;
}

void GarageGroundRenderer::release() {
    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.destroy();
    }
    if (vao_.isCreated()) {
        vao_.destroy();
    }
    program_.removeAllShaders();
    initialized_ = false;
}

void GarageGroundRenderer::render(
    const QMatrix4x4 &view,
    const QMatrix4x4 &projection,
    const fh6::ModelVec3 &boundsMin,
    const fh6::ModelVec3 &boundsMax,
    const GarageRenderSettings::Ground &settings,
    const QVector3D &backgroundColor,
    bool linearOutput) {
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!initialized_ || !settings.enabled || context == nullptr) {
        return;
    }

    const float halfWidth = std::max(0.001f, (boundsMax.x - boundsMin.x) * 0.5f);
    const float halfLength = std::max(0.001f, (boundsMax.z - boundsMin.z) * 0.5f);
    const float height = std::max(0.001f, boundsMax.y - boundsMin.y);
    const float centerX = -(boundsMin.x + boundsMax.x) * 0.5f;
    const float centerZ = (boundsMin.z + boundsMax.z) * 0.5f;
    const float groundY = boundsMin.y - std::max(0.002f, height * settings.verticalOffsetScale);
    const float planeHalfSize = std::max(halfWidth, halfLength) * settings.planeSizeScale;
    const QVector3D groundCenter(centerX, groundY, centerZ);
    const QVector2D shadowCenter(centerX, centerZ);
    const QVector2D shadowRadii(
        halfWidth * settings.shadowWidthScale,
        halfLength * settings.shadowLengthScale);
    const QVector3D outputGroundColor = linearOutput
        ? srgbToLinear(settings.color)
        : settings.color;
    const QVector3D outputBackgroundColor = linearOutput
        ? srgbToLinear(backgroundColor)
        : backgroundColor;

    QOpenGLExtraFunctions *functions = context->extraFunctions();
    const GLboolean depthEnabled = functions->glIsEnabled(GL_DEPTH_TEST);
    const GLboolean blendEnabled = functions->glIsEnabled(GL_BLEND);
    const GLboolean cullEnabled = functions->glIsEnabled(GL_CULL_FACE);
    GLboolean depthWriteEnabled = GL_TRUE;
    GLint depthFunction = GL_LESS;
    GLint currentProgram = 0;
    GLint vertexArray = 0;
    functions->glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteEnabled);
    functions->glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
    functions->glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    functions->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);

    functions->glEnable(GL_DEPTH_TEST);
    functions->glDepthFunc(GL_LEQUAL);
    functions->glDepthMask(GL_TRUE);
    functions->glDisable(GL_BLEND);
    functions->glDisable(GL_CULL_FACE);
    program_.bind();
    program_.setUniformValue(mvpLocation_, projection * view);
    program_.setUniformValue(groundCenterLocation_, groundCenter);
    program_.setUniformValue(planeHalfSizeLocation_, planeHalfSize);
    program_.setUniformValue(shadowCenterLocation_, shadowCenter);
    program_.setUniformValue(shadowRadiiLocation_, shadowRadii);
    program_.setUniformValue(groundColorLocation_, outputGroundColor);
    program_.setUniformValue(backgroundColorLocation_, outputBackgroundColor);
    program_.setUniformValue(shadowOpacityLocation_, std::clamp(settings.shadowOpacity, 0.0f, 1.0f));
    vao_.bind();
    functions->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    functions->glBindVertexArray(static_cast<GLuint>(vertexArray));
    functions->glUseProgram(static_cast<GLuint>(currentProgram));
    functions->glDepthFunc(static_cast<GLenum>(depthFunction));
    functions->glDepthMask(depthWriteEnabled);
    if (depthEnabled == GL_TRUE) {
        functions->glEnable(GL_DEPTH_TEST);
    } else {
        functions->glDisable(GL_DEPTH_TEST);
    }
    if (blendEnabled == GL_TRUE) {
        functions->glEnable(GL_BLEND);
    } else {
        functions->glDisable(GL_BLEND);
    }
    if (cullEnabled == GL_TRUE) {
        functions->glEnable(GL_CULL_FACE);
    } else {
        functions->glDisable(GL_CULL_FACE);
    }
}

} // namespace gui
