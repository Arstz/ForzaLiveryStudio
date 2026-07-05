#include "native_shape_renderer.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObjectFormat>
#include <QColor>
#include <QSizeF>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr char kVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 vertex;
layout(location = 1) in float vertex_alpha;

uniform vec2 viewport;
uniform vec3 camera_row0;
uniform vec3 camera_row1;
uniform vec3 world_row0;
uniform vec3 world_row1;
uniform vec4 tint;

out vec4 frag_color;

void main()
{
    vec2 world = vec2(
        dot(world_row0.xy, vertex) + world_row0.z,
        dot(world_row1.xy, vertex) + world_row1.z
    );
    vec2 pixel = vec2(
        dot(camera_row0.xy, world) + camera_row0.z,
        dot(camera_row1.xy, world) + camera_row1.z
    );
    vec2 ndc = vec2(
        pixel.x * 2.0 / viewport.x - 1.0,
        1.0 - pixel.y * 2.0 / viewport.y
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_color = vec4(tint.rgb, tint.a * vertex_alpha);
}
)";

constexpr char kFragmentShader[] = R"(#version 330 core
in vec4 frag_color;
out vec4 out_color;

void main()
{
    out_color = frag_color;
}
)";

constexpr char kCompositeVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 vertex;
layout(location = 1) in vec2 uv_in;

out vec2 uv;

void main()
{
    gl_Position = vec4(vertex, 0.0, 1.0);
    uv = uv_in;
}
)";

constexpr char kCompositeFragmentShader[] = R"(#version 330 core
in vec2 uv;
uniform sampler2D scene_texture;
out vec4 out_color;

void main()
{
    out_color = texture(scene_texture, uv);
}
)";

QTransform layerTransform(const fh6::ShapeLayer &layer)
{
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

void appendVertex(QVector<float> &vertices, const QPointF &point, double alpha)
{
    vertices.push_back(static_cast<float>(point.x()));
    vertices.push_back(static_cast<float>(point.y()));
    vertices.push_back(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
}

void appendTriangle(QVector<float> &vertices, const ShapeTriangle &triangle)
{
    appendVertex(vertices, triangle.p0, triangle.alpha0);
    appendVertex(vertices, triangle.p1, triangle.alpha1);
    appendVertex(vertices, triangle.p2, triangle.alpha2);
}

void appendRect(QVector<float> &vertices, const QSizeF &size)
{
    const double hw = size.width() * 0.5;
    const double hh = size.height() * 0.5;
    appendVertex(vertices, QPointF(-hw, -hh), 0.75);
    appendVertex(vertices, QPointF(hw, -hh), 0.75);
    appendVertex(vertices, QPointF(hw, hh), 0.75);
    appendVertex(vertices, QPointF(-hw, -hh), 0.75);
    appendVertex(vertices, QPointF(hw, hh), 0.75);
    appendVertex(vertices, QPointF(-hw, hh), 0.75);
}

} // namespace

NativeShapeRenderer::NativeShapeRenderer()
    : vertexBuffer_(QOpenGLBuffer::VertexBuffer)
    , compositeBuffer_(QOpenGLBuffer::VertexBuffer)
{
}

NativeShapeRenderer::~NativeShapeRenderer()
{
    release();
}

void NativeShapeRenderer::initialize()
{
    if (initialized_) {
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !program_.link()
        || !compositeProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex, kCompositeVertexShader)
        || !compositeProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment, kCompositeFragmentShader)
        || !compositeProgram_.link()) {
        return;
    }

    vao_.create();
    vertexBuffer_.create();
    compositeVao_.create();
    compositeBuffer_.create();

    viewportLocation_ = program_.uniformLocation("viewport");
    cameraRow0Location_ = program_.uniformLocation("camera_row0");
    cameraRow1Location_ = program_.uniformLocation("camera_row1");
    worldRow0Location_ = program_.uniformLocation("world_row0");
    worldRow1Location_ = program_.uniformLocation("world_row1");
    tintLocation_ = program_.uniformLocation("tint");
    compositeTextureLocation_ = compositeProgram_.uniformLocation("scene_texture");

    const float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
    };
    compositeVao_.bind();
    compositeBuffer_.bind();
    compositeBuffer_.allocate(quad, static_cast<int>(sizeof(quad)));
    compositeProgram_.bind();
    compositeProgram_.enableAttributeArray(0);
    compositeProgram_.setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(float));
    compositeProgram_.enableAttributeArray(1);
    compositeProgram_.setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    compositeProgram_.release();
    compositeBuffer_.release();
    compositeVao_.release();

    initialized_ = true;
    geometryUploaded_ = false;
}

void NativeShapeRenderer::release()
{
    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.destroy();
    }
    if (vao_.isCreated()) {
        vao_.destroy();
    }
    if (compositeBuffer_.isCreated()) {
        compositeBuffer_.destroy();
    }
    if (compositeVao_.isCreated()) {
        compositeVao_.destroy();
    }
    program_.removeAllShaders();
    compositeProgram_.removeAllShaders();
    sceneFbo_.reset();
    sceneFboSize_ = {};
    ranges_.clear();
    vertices_.clear();
    viewportLocation_ = -1;
    cameraRow0Location_ = -1;
    cameraRow1Location_ = -1;
    worldRow0Location_ = -1;
    worldRow1Location_ = -1;
    tintLocation_ = -1;
    compositeTextureLocation_ = -1;
    initialized_ = false;
    geometryUploaded_ = false;
}

bool NativeShapeRenderer::isInitialized() const
{
    return initialized_;
}

void NativeShapeRenderer::uploadGeometry(const ShapeGeometryStore &geometry)
{
    if (!initialized_) {
        return;
    }

    vertices_.clear();
    ranges_.clear();
    for (int shapeId : geometry.shapeIds()) {
        const ShapeGeometry *shape = geometry.shape(shapeId);
        if (shape == nullptr) {
            continue;
        }
        ShapeRange range;
        range.firstVertex = vertices_.size() / 3;
        if (shape->triangles.isEmpty()) {
            appendRect(vertices_, QSizeF(shape->width, shape->height));
        } else {
            for (const ShapeTriangle &triangle : shape->triangles) {
                appendTriangle(vertices_, triangle);
            }
        }
        range.vertexCount = vertices_.size() / 3 - range.firstVertex;
        ranges_.insert(shapeId, range);
    }

    vao_.bind();
    vertexBuffer_.bind();
    vertexBuffer_.allocate(vertices_.constData(), vertices_.size() * static_cast<int>(sizeof(float)));
    program_.bind();
    program_.enableAttributeArray(0);
    program_.setAttributeBuffer(0, GL_FLOAT, 0, 2, 3 * sizeof(float));
    program_.enableAttributeArray(1);
    program_.setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 1, 3 * sizeof(float));
    program_.release();
    vertexBuffer_.release();
    vao_.release();

    geometryUploaded_ = true;
}

void NativeShapeRenderer::render(
    const fh6::Project &project,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size,
    const QSet<QString> &flashingLayerIds,
    double flashHue,
    double flashStrength,
    bool clearBackground)
{
    QOpenGLFunctions *functions = QOpenGLContext::currentContext()->functions();
    functions->glViewport(0, 0, std::max(size.width(), 1), std::max(size.height(), 1));
    functions->glDisable(GL_DEPTH_TEST);
    if (clearBackground) {
        functions->glClearColor(24.0f / 255.0f, 25.0f / 255.0f, 28.0f / 255.0f, 1.0f);
        functions->glClear(GL_COLOR_BUFFER_BIT);
    }

    if (!initialized_ || !geometryUploaded_ || vertices_.isEmpty() || size.isEmpty()) {
        return;
    }

    ensureSceneFramebuffer(size);
    if (sceneFbo_ == nullptr || !sceneFbo_->isValid()) {
        return;
    }

    sceneFbo_->bind();
    functions->glViewport(0, 0, std::max(size.width(), 1), std::max(size.height(), 1));
    functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    functions->glClear(GL_COLOR_BUFFER_BIT);

    vao_.bind();
    program_.bind();
    program_.setUniformValue(viewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
    setUniformRows(cameraRow0Location_, cameraRow1Location_, worldToScreen);
    functions->glEnable(GL_BLEND);

    bool haveMaskMode = false;
    bool currentMaskMode = false;
    for (const fh6::ShapeLayer &layer : project.layers) {
        if (!layer.visible) {
            continue;
        }
        ShapeRange range = ranges_.value(layer.shapeId);
        if (range.vertexCount <= 0) {
            range = fallbackRange(layer.shapeId, geometry);
        }
        if (range.vertexCount <= 0) {
            continue;
        }

        if (!haveMaskMode || currentMaskMode != layer.mask) {
            haveMaskMode = true;
            currentMaskMode = layer.mask;
            if (layer.mask) {
                functions->glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            }
        }

        setUniformRows(worldRow0Location_, worldRow1Location_, layerTransform(layer));
        if (layer.mask) {
            program_.setUniformValue(tintLocation_, 0.0f, 0.0f, 0.0f, static_cast<float>(layer.color[3]) / 255.0f);
        } else {
            program_.setUniformValue(tintLocation_,
                                     static_cast<float>(layer.color[2]) / 255.0f,
                                     static_cast<float>(layer.color[1]) / 255.0f,
                                     static_cast<float>(layer.color[0]) / 255.0f,
                                     static_cast<float>(layer.color[3]) / 255.0f);
        }
        functions->glDrawArrays(GL_TRIANGLES, range.firstVertex, range.vertexCount);
    }

    functions->glDisable(GL_BLEND);
    program_.release();
    vao_.release();
    sceneFbo_->release();

    compositeScene(functions, size, clearBackground);

    if (flashHue >= 0.0 && flashStrength > 0.0 && !flashingLayerIds.isEmpty()) {
        vao_.bind();
        program_.bind();
        program_.setUniformValue(viewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        setUniformRows(cameraRow0Location_, cameraRow1Location_, worldToScreen);
        functions->glEnable(GL_BLEND);
        functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        const QColor color = QColor::fromHsvF(std::clamp(flashHue, 0.0, 1.0), 1.0, 1.0);
        for (const fh6::ShapeLayer &layer : project.layers) {
            if (!flashingLayerIds.contains(layer.id)) {
                continue;
            }
            ShapeRange range = ranges_.value(layer.shapeId);
            if (range.vertexCount <= 0) {
                range = fallbackRange(layer.shapeId, geometry);
            }
            if (range.vertexCount <= 0) {
                continue;
            }
            const float baseAlpha = std::max(static_cast<float>(layer.color[3]) / 255.0f, 0.75f);
            setUniformRows(worldRow0Location_, worldRow1Location_, layerTransform(layer));
            program_.setUniformValue(tintLocation_,
                                     static_cast<float>(color.redF()),
                                     static_cast<float>(color.greenF()),
                                     static_cast<float>(color.blueF()),
                                     baseAlpha * static_cast<float>(flashStrength));
            functions->glDrawArrays(GL_TRIANGLES, range.firstVertex, range.vertexCount);
        }
        functions->glDisable(GL_BLEND);
        program_.release();
        vao_.release();
    }
}

void NativeShapeRenderer::ensureSceneFramebuffer(const QSize &size)
{
    const QSize fboSize(std::max(size.width(), 1), std::max(size.height(), 1));
    if (sceneFbo_ != nullptr && sceneFboSize_ == fboSize && sceneFbo_->isValid()) {
        return;
    }
    QOpenGLFramebufferObjectFormat format;
    format.setInternalTextureFormat(GL_RGBA8);
    format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    sceneFbo_ = std::make_unique<QOpenGLFramebufferObject>(fboSize, format);
    sceneFboSize_ = fboSize;
}

void NativeShapeRenderer::compositeScene(QOpenGLFunctions *functions, const QSize &size, bool clearBackground)
{
    functions->glViewport(0, 0, std::max(size.width(), 1), std::max(size.height(), 1));
    if (clearBackground) {
        functions->glClearColor(24.0f / 255.0f, 25.0f / 255.0f, 28.0f / 255.0f, 1.0f);
        functions->glClear(GL_COLOR_BUFFER_BIT);
    }
    functions->glEnable(GL_BLEND);
    functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    compositeVao_.bind();
    compositeProgram_.bind();
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, sceneFbo_->texture());
    compositeProgram_.setUniformValue(compositeTextureLocation_, 0);
    functions->glDrawArrays(GL_TRIANGLES, 0, 6);
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    compositeProgram_.release();
    compositeVao_.release();
    functions->glDisable(GL_BLEND);
}

void NativeShapeRenderer::setUniformRows(int row0Location, int row1Location, const QTransform &transform)
{
    program_.setUniformValue(row0Location,
                             static_cast<float>(transform.m11()),
                             static_cast<float>(transform.m21()),
                             static_cast<float>(transform.dx()));
    program_.setUniformValue(row1Location,
                             static_cast<float>(transform.m12()),
                             static_cast<float>(transform.m22()),
                             static_cast<float>(transform.dy()));
}

NativeShapeRenderer::ShapeRange NativeShapeRenderer::fallbackRange(int shapeId, const ShapeGeometryStore &geometry)
{
    if (ranges_.contains(shapeId)) {
        return ranges_.value(shapeId);
    }
    const ShapeRange range{static_cast<int>(vertices_.size() / 3), 6};
    appendRect(vertices_, geometry.shapeSize(shapeId));

    vao_.bind();
    vertexBuffer_.bind();
    vertexBuffer_.allocate(vertices_.constData(), vertices_.size() * static_cast<int>(sizeof(float)));
    vertexBuffer_.release();
    vao_.release();

    ranges_.insert(shapeId, range);
    return range;
}

} // namespace gui
