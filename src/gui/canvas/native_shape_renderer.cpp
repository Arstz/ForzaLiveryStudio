#include "native_shape_renderer.h"

#include "layer.h"

#include <algorithm>
#include <cmath>
#include <functional>

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

constexpr char kRasterVertexShader[] = R"(#version 330 core
layout(location = 0) in vec2 vertex;
layout(location = 1) in vec2 uv_in;

uniform vec2 viewport;
uniform vec3 camera_row0;
uniform vec3 camera_row1;
uniform vec3 world_row0;
uniform vec3 world_row1;

out vec2 uv;

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
    uv = uv_in;
}
)";

constexpr char kRasterFragmentShader[] = R"(#version 330 core
in vec2 uv;
uniform sampler2D raster_texture;
uniform vec4 tint;
out vec4 out_color;

void main()
{
    vec4 texel = texture(raster_texture, uv);
    out_color = vec4(texel.rgb * tint.rgb, texel.a * tint.a);
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

QTransform layerTransform(const fh6::scene::Shape &layer) {
    QTransform transform;
    transform.translate(layer.x, layer.y);
    transform.rotate(layer.rotation);
    transform.shear(layer.skew, 0.0);
    transform.scale(layer.scaleX, layer.scaleY);
    return transform;
}

QTransform rendererSceneLocalTransform(const fh6::scene::Layer &node) {
    QTransform transform;
    transform.translate(node.transform.x, node.transform.y);
    transform.rotate(node.transform.rotation);
    transform.shear(node.transform.skew, 0.0);
    transform.scale(node.transform.scaleX, node.transform.scaleY);
    return transform;
}

void appendVertex(QVector<float> &vertices, const QPointF &point, double alpha) {
    vertices.push_back(static_cast<float>(point.x()));
    vertices.push_back(static_cast<float>(point.y()));
    vertices.push_back(static_cast<float>(std::clamp(alpha, 0.0, 1.0)));
}

void appendTriangle(QVector<float> &vertices, const ShapeTriangle &triangle) {
    appendVertex(vertices, triangle.p0, triangle.alpha0);
    appendVertex(vertices, triangle.p1, triangle.alpha1);
    appendVertex(vertices, triangle.p2, triangle.alpha2);
}

void appendRect(QVector<float> &vertices, const QSizeF &size) {
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
    , rasterBuffer_(QOpenGLBuffer::VertexBuffer)
    , compositeBuffer_(QOpenGLBuffer::VertexBuffer) {
}

NativeShapeRenderer::~NativeShapeRenderer() {
    release();
}

void NativeShapeRenderer::initialize() {
    if (initialized_) {
        return;
    }
    if (!program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !program_.link()
        || !rasterProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex, kRasterVertexShader)
        || !rasterProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment, kRasterFragmentShader)
        || !rasterProgram_.link()
        || !compositeProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex, kCompositeVertexShader)
        || !compositeProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment, kCompositeFragmentShader)
        || !compositeProgram_.link()) {
        return;
    }

    vao_.create();
    vertexBuffer_.create();
    rasterVao_.create();
    rasterBuffer_.create();
    compositeVao_.create();
    compositeBuffer_.create();

    viewportLocation_ = program_.uniformLocation("viewport");
    cameraRow0Location_ = program_.uniformLocation("camera_row0");
    cameraRow1Location_ = program_.uniformLocation("camera_row1");
    worldRow0Location_ = program_.uniformLocation("world_row0");
    worldRow1Location_ = program_.uniformLocation("world_row1");
    tintLocation_ = program_.uniformLocation("tint");
    rasterViewportLocation_ = rasterProgram_.uniformLocation("viewport");
    rasterCameraRow0Location_ = rasterProgram_.uniformLocation("camera_row0");
    rasterCameraRow1Location_ = rasterProgram_.uniformLocation("camera_row1");
    rasterWorldRow0Location_ = rasterProgram_.uniformLocation("world_row0");
    rasterWorldRow1Location_ = rasterProgram_.uniformLocation("world_row1");
    rasterTintLocation_ = rasterProgram_.uniformLocation("tint");
    rasterTextureLocation_ = rasterProgram_.uniformLocation("raster_texture");
    compositeTextureLocation_ = compositeProgram_.uniformLocation("scene_texture");

    const float rasterQuad[] = {
        -0.5f, -0.5f, 0.0f, 1.0f,
         0.5f, -0.5f, 1.0f, 1.0f,
         0.5f,  0.5f, 1.0f, 0.0f,
        -0.5f, -0.5f, 0.0f, 1.0f,
         0.5f,  0.5f, 1.0f, 0.0f,
        -0.5f,  0.5f, 0.0f, 0.0f,
    };
    rasterVao_.bind();
    rasterBuffer_.bind();
    rasterBuffer_.allocate(rasterQuad, static_cast<int>(sizeof(rasterQuad)));
    rasterProgram_.bind();
    rasterProgram_.enableAttributeArray(0);
    rasterProgram_.setAttributeBuffer(0, GL_FLOAT, 0, 2, 4 * sizeof(float));
    rasterProgram_.enableAttributeArray(1);
    rasterProgram_.setAttributeBuffer(1, GL_FLOAT, 2 * sizeof(float), 2, 4 * sizeof(float));
    rasterProgram_.release();
    rasterBuffer_.release();
    rasterVao_.release();

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

void NativeShapeRenderer::release() {
    if (!rasterTextures_.isEmpty()) {
        if (QOpenGLContext *context = QOpenGLContext::currentContext()) {
            QVector<GLuint> textures;
            textures.reserve(rasterTextures_.size());
            for (auto it = rasterTextures_.cbegin(); it != rasterTextures_.cend(); ++it) {
                textures.push_back(it.value());
            }
            context->functions()->glDeleteTextures(textures.size(), textures.constData());
        }
        rasterTextures_.clear();
        rasterTextureSizes_.clear();
    }
    if (vertexBuffer_.isCreated()) {
        vertexBuffer_.destroy();
    }
    if (vao_.isCreated()) {
        vao_.destroy();
    }
    if (rasterBuffer_.isCreated()) {
        rasterBuffer_.destroy();
    }
    if (rasterVao_.isCreated()) {
        rasterVao_.destroy();
    }
    if (compositeBuffer_.isCreated()) {
        compositeBuffer_.destroy();
    }
    if (compositeVao_.isCreated()) {
        compositeVao_.destroy();
    }
    program_.removeAllShaders();
    rasterProgram_.removeAllShaders();
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
    rasterViewportLocation_ = -1;
    rasterCameraRow0Location_ = -1;
    rasterCameraRow1Location_ = -1;
    rasterWorldRow0Location_ = -1;
    rasterWorldRow1Location_ = -1;
    rasterTintLocation_ = -1;
    rasterTextureLocation_ = -1;
    compositeTextureLocation_ = -1;
    initialized_ = false;
    geometryUploaded_ = false;
}

bool NativeShapeRenderer::isInitialized() const {
    return initialized_;
}

void NativeShapeRenderer::uploadGeometry(const ShapeGeometryStore &geometry) {
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
    bool clearBackground) {
    QOpenGLFunctions *functions = QOpenGLContext::currentContext()->functions();
    functions->glViewport(0, 0, std::max(size.width(), 1), std::max(size.height(), 1));
    functions->glDisable(GL_DEPTH_TEST);
    if (clearBackground) {
        functions->glClearColor(24.0f / 255.0f, 25.0f / 255.0f, 28.0f / 255.0f, 1.0f);
        functions->glClear(GL_COLOR_BUFFER_BIT);
    }

    if (!drawSceneToFbo(functions, project, geometry, worldToScreen, size)) {
        return;
    }

    compositeScene(functions, size, clearBackground);

    if (flashHue >= 0.0 && flashStrength > 0.0 && !flashingLayerIds.isEmpty()) {
        vao_.bind();
        program_.bind();
        program_.setUniformValue(viewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        setUniformRows(cameraRow0Location_, cameraRow1Location_, worldToScreen);
        functions->glEnable(GL_BLEND);
        functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        const QColor color = QColor::fromHsvF(std::clamp(flashHue, 0.0, 1.0), 1.0, 1.0);
        if (project.root) {
            std::function<void(const fh6::scene::Layer &, const QTransform &)> flashWalk =
                [&](const fh6::scene::Layer &node, const QTransform &parentWorld) {
                    const QTransform world = rendererSceneLocalTransform(node) * parentWorld;
                    if (node.kind() == fh6::scene::LayerKind::Group) {
                        for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                            flashWalk(*child, world);
                        }
                        return;
                    }
                    if (node.kind() != fh6::scene::LayerKind::Shape) {
                        return;
                    }
                    const auto &layer = static_cast<const fh6::scene::Shape &>(node);
                    if (!flashingLayerIds.contains(layer.id) || layer.raster) {
                        return;
                    }
                    ShapeRange range = ranges_.value(layer.shapeId);
                    if (range.vertexCount <= 0) {
                        range = fallbackRange(layer.shapeId, geometry);
                    }
                    if (range.vertexCount <= 0) {
                        return;
                    }
                    const float baseAlpha = std::max(static_cast<float>(layer.color[3]) / 255.0f, 0.75f);
                    setUniformRows(worldRow0Location_, worldRow1Location_, world);
                    program_.setUniformValue(tintLocation_,
                                             static_cast<float>(color.redF()),
                                             static_cast<float>(color.greenF()),
                                             static_cast<float>(color.blueF()),
                                             baseAlpha * static_cast<float>(flashStrength));
                    functions->glDrawArrays(GL_TRIANGLES, range.firstVertex, range.vertexCount);
                };
            for (const auto &child : project.root->children) {
                flashWalk(*child, QTransform());
            }
        }
        functions->glDisable(GL_BLEND);
        program_.release();
        vao_.release();
    }
}

void NativeShapeRenderer::render(
    const QVector<SceneRenderEntry> &entries,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size,
    const QSet<QString> &flashingLayerIds,
    double flashHue,
    double flashStrength,
    bool clearBackground) {
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
    functions->glDisable(GL_DEPTH_TEST);
    functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    functions->glClear(GL_COLOR_BUFFER_BIT);
    const bool drew = drawRenderEntries(functions, entries, geometry, worldToScreen, size);
    sceneFbo_->release();
    if (!drew) {
        return;
    }

    compositeScene(functions, size, clearBackground);

    if (flashHue >= 0.0 && flashStrength > 0.0 && !flashingLayerIds.isEmpty()) {
        vao_.bind();
        program_.bind();
        program_.setUniformValue(viewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        setUniformRows(cameraRow0Location_, cameraRow1Location_, worldToScreen);
        functions->glEnable(GL_BLEND);
        functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        const QColor color = QColor::fromHsvF(std::clamp(flashHue, 0.0, 1.0), 1.0, 1.0);
        for (const SceneRenderEntry &entry : entries) {
            const fh6::scene::Shape *layer = entry.shape;
            if (layer == nullptr || !flashingLayerIds.contains(layer->id) || layer->raster) {
                continue;
            }
            ShapeRange range = ranges_.value(layer->shapeId);
            if (range.vertexCount <= 0) {
                range = fallbackRange(layer->shapeId, geometry);
            }
            if (range.vertexCount <= 0) {
                continue;
            }
            const float baseAlpha = std::max(static_cast<float>(layer->color[3]) / 255.0f, 0.75f);
            setUniformRows(worldRow0Location_, worldRow1Location_, entry.worldTransform);
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

bool NativeShapeRenderer::drawSceneToFbo(
    QOpenGLFunctions *functions,
    const fh6::Project &project,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size) {
    if (!initialized_ || !geometryUploaded_ || vertices_.isEmpty() || size.isEmpty()) {
        return false;
    }

    ensureSceneFramebuffer(size);
    if (sceneFbo_ == nullptr || !sceneFbo_->isValid()) {
        return false;
    }

    sceneFbo_->bind();
    functions->glViewport(0, 0, std::max(size.width(), 1), std::max(size.height(), 1));
    functions->glDisable(GL_DEPTH_TEST);
    functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    functions->glClear(GL_COLOR_BUFFER_BIT);

    const bool drew = drawProjectLayers(functions, project, geometry, worldToScreen, size);
    sceneFbo_->release();
    return drew;
}

bool NativeShapeRenderer::drawRenderEntries(
    QOpenGLFunctions *functions,
    const QVector<SceneRenderEntry> &entries,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size) {
    if (entries.isEmpty()) {
        return false;
    }

    functions->glEnable(GL_BLEND);

    bool haveMaskMode = false;
    bool currentMaskMode = false;
    bool drew = false;
    enum class ActiveProgram {
        None,
        Vector,
        Raster,
    };
    ActiveProgram active = ActiveProgram::None;

    auto activateVector = [&]() {
        if (active == ActiveProgram::Vector) {
            return;
        }
        if (active == ActiveProgram::Raster) {
            rasterProgram_.release();
            rasterVao_.release();
        }
        vao_.bind();
        program_.bind();
        program_.setUniformValue(viewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        setUniformRows(cameraRow0Location_, cameraRow1Location_, worldToScreen);
        active = ActiveProgram::Vector;
    };

    auto activateRaster = [&]() {
        if (active == ActiveProgram::Raster) {
            return;
        }
        if (active == ActiveProgram::Vector) {
            program_.release();
            vao_.release();
        }
        rasterVao_.bind();
        rasterProgram_.bind();
        rasterProgram_.setUniformValue(rasterViewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        rasterProgram_.setUniformValue(rasterCameraRow0Location_,
                                       static_cast<float>(worldToScreen.m11()),
                                       static_cast<float>(worldToScreen.m21()),
                                       static_cast<float>(worldToScreen.dx()));
        rasterProgram_.setUniformValue(rasterCameraRow1Location_,
                                       static_cast<float>(worldToScreen.m12()),
                                       static_cast<float>(worldToScreen.m22()),
                                       static_cast<float>(worldToScreen.dy()));
        active = ActiveProgram::Raster;
    };

    for (const SceneRenderEntry &entry : entries) {
        const fh6::scene::Shape *shape = entry.shape;
        if (shape == nullptr || !shape->visible) {
            continue;
        }

        if (!haveMaskMode || currentMaskMode != shape->mask) {
            haveMaskMode = true;
            currentMaskMode = shape->mask;
            if (shape->mask) {
                functions->glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            } else {
                functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            }
        }

        const bool isRaster = shape->visual && shape->visual->kind() == fh6::scene::VisualKind::Raster;
        if (isRaster) {
            const auto *raster = static_cast<const fh6::scene::RasterContainer *>(shape->visual.get());
            activateRaster();
            drew = drawRasterLayer(functions, raster->rasterId, shape->color, shape->mask,
                                   QSize(raster->width, raster->height), entry.worldTransform)
                   || drew;
            continue;
        }

        quint16 shapeId = 0;
        if (shape->visual && shape->visual->kind() == fh6::scene::VisualKind::Vector) {
            shapeId = static_cast<const fh6::scene::VectorContainer *>(shape->visual.get())->shapeId;
        }
        ShapeRange range = ranges_.value(shapeId);
        if (range.vertexCount <= 0) {
            range = fallbackRange(shapeId, geometry);
        }
        if (range.vertexCount <= 0) {
            continue;
        }
        activateVector();
        setUniformRows(worldRow0Location_, worldRow1Location_, entry.worldTransform);
        if (shape->mask) {
            program_.setUniformValue(tintLocation_, 0.0f, 0.0f, 0.0f, static_cast<float>(shape->color[3]) / 255.0f);
        } else {
            program_.setUniformValue(tintLocation_,
                                     static_cast<float>(shape->color[2]) / 255.0f,
                                     static_cast<float>(shape->color[1]) / 255.0f,
                                     static_cast<float>(shape->color[0]) / 255.0f,
                                     static_cast<float>(shape->color[3]) / 255.0f);
        }
        functions->glDrawArrays(GL_TRIANGLES, range.firstVertex, range.vertexCount);
        drew = true;
    }

    if (active == ActiveProgram::Vector) {
        program_.release();
        vao_.release();
    } else if (active == ActiveProgram::Raster) {
        rasterProgram_.release();
        rasterVao_.release();
    }
    functions->glDisable(GL_BLEND);
    return drew;
}

bool NativeShapeRenderer::drawProjectLayers(
    QOpenGLFunctions *functions,
    const fh6::Project &project,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size) {
    if (!project.root) {
        return false;
    }
    const fh6::scene::Group &root = *project.root;

    functions->glEnable(GL_BLEND);

    bool haveMaskMode = false;
    bool currentMaskMode = false;
    bool drew = false;
    enum class ActiveProgram {
        None,
        Vector,
        Raster,
    };
    ActiveProgram active = ActiveProgram::None;

    auto activateVector = [&]() {
        if (active == ActiveProgram::Vector) {
            return;
        }
        if (active == ActiveProgram::Raster) {
            rasterProgram_.release();
            rasterVao_.release();
        }
        vao_.bind();
        program_.bind();
        program_.setUniformValue(viewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        setUniformRows(cameraRow0Location_, cameraRow1Location_, worldToScreen);
        active = ActiveProgram::Vector;
    };

    auto activateRaster = [&]() {
        if (active == ActiveProgram::Raster) {
            return;
        }
        if (active == ActiveProgram::Vector) {
            program_.release();
            vao_.release();
        }
        rasterVao_.bind();
        rasterProgram_.bind();
        rasterProgram_.setUniformValue(rasterViewportLocation_, static_cast<float>(size.width()), static_cast<float>(size.height()));
        rasterProgram_.setUniformValue(rasterCameraRow0Location_,
                                       static_cast<float>(worldToScreen.m11()),
                                       static_cast<float>(worldToScreen.m21()),
                                       static_cast<float>(worldToScreen.dx()));
        rasterProgram_.setUniformValue(rasterCameraRow1Location_,
                                       static_cast<float>(worldToScreen.m12()),
                                       static_cast<float>(worldToScreen.m22()),
                                       static_cast<float>(worldToScreen.dy()));
        active = ActiveProgram::Raster;
    };

    std::function<void(const fh6::scene::Layer &, const QTransform &)> walk =
        [&](const fh6::scene::Layer &node, const QTransform &parentWorld) {
            const QTransform world = rendererSceneLocalTransform(node) * parentWorld;
            if (node.kind() == fh6::scene::LayerKind::Group) {
                const auto &group = static_cast<const fh6::scene::Group &>(node);
                for (const auto &child : group.children) {
                    walk(*child, world);
                }
                return;
            }
            if (node.kind() != fh6::scene::LayerKind::Shape) {
                return;
            }
            const auto &shape = static_cast<const fh6::scene::Shape &>(node);
            if (!shape.visible) {
                return;
            }

            if (!haveMaskMode || currentMaskMode != shape.mask) {
                haveMaskMode = true;
                currentMaskMode = shape.mask;
                if (shape.mask) {
                    functions->glBlendFuncSeparate(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
                } else {
                    functions->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                }
            }

            const bool isRaster = shape.visual && shape.visual->kind() == fh6::scene::VisualKind::Raster;
            if (isRaster) {
                const auto *raster = static_cast<const fh6::scene::RasterContainer *>(shape.visual.get());
                activateRaster();
                drew = drawRasterLayer(functions, raster->rasterId, shape.color, shape.mask,
                                       QSize(raster->width, raster->height), world)
                       || drew;
                return;
            }

            quint16 shapeId = 0;
            if (shape.visual && shape.visual->kind() == fh6::scene::VisualKind::Vector) {
                shapeId = static_cast<const fh6::scene::VectorContainer *>(shape.visual.get())->shapeId;
            }
            ShapeRange range = ranges_.value(shapeId);
            if (range.vertexCount <= 0) {
                range = fallbackRange(shapeId, geometry);
            }
            if (range.vertexCount <= 0) {
                return;
            }
            activateVector();
            setUniformRows(worldRow0Location_, worldRow1Location_, world);
            if (shape.mask) {
                program_.setUniformValue(tintLocation_, 0.0f, 0.0f, 0.0f, static_cast<float>(shape.color[3]) / 255.0f);
            } else {
                program_.setUniformValue(tintLocation_,
                                         static_cast<float>(shape.color[2]) / 255.0f,
                                         static_cast<float>(shape.color[1]) / 255.0f,
                                         static_cast<float>(shape.color[0]) / 255.0f,
                                         static_cast<float>(shape.color[3]) / 255.0f);
            }
            functions->glDrawArrays(GL_TRIANGLES, range.firstVertex, range.vertexCount);
            drew = true;
        };

    for (const auto &child : root.children) {
        walk(*child, QTransform());
    }

    if (active == ActiveProgram::Vector) {
        program_.release();
        vao_.release();
    } else if (active == ActiveProgram::Raster) {
        rasterProgram_.release();
        rasterVao_.release();
    }
    functions->glDisable(GL_BLEND);
    return drew;
}

bool NativeShapeRenderer::drawRasterLayer(QOpenGLFunctions *functions,
                                          quint32 rasterId,
                                          const std::array<quint8, 4> &color,
                                          bool mask,
                                          const QSize &fallbackSize,
                                          const QTransform &world) {
    const GLuint texture = ensureRasterTexture(functions, rasterId);
    if (texture == 0) {
        return false;
    }
    const QSize decalSize = rasterTextureSizes_.value(rasterId, fallbackSize);
    QTransform transform = world;
    transform.scale(std::max(decalSize.width(), 1), std::max(decalSize.height(), 1));
    rasterProgram_.setUniformValue(rasterWorldRow0Location_,
                                   static_cast<float>(transform.m11()),
                                   static_cast<float>(transform.m21()),
                                   static_cast<float>(transform.dx()));
    rasterProgram_.setUniformValue(rasterWorldRow1Location_,
                                   static_cast<float>(transform.m12()),
                                   static_cast<float>(transform.m22()),
                                   static_cast<float>(transform.dy()));
    if (mask) {
        rasterProgram_.setUniformValue(rasterTintLocation_, 0.0f, 0.0f, 0.0f, static_cast<float>(color[3]) / 255.0f);
    } else {
        rasterProgram_.setUniformValue(rasterTintLocation_, 1.0f, 1.0f, 1.0f, static_cast<float>(color[3]) / 255.0f);
    }
    functions->glActiveTexture(GL_TEXTURE0);
    functions->glBindTexture(GL_TEXTURE_2D, texture);
    rasterProgram_.setUniformValue(rasterTextureLocation_, 0);
    functions->glDrawArrays(GL_TRIANGLES, 0, 6);
    functions->glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

bool NativeShapeRenderer::ensureRasterPackLoaded() {
    if (rasterPack_.isLoaded()) {
        return true;
    }
    if (!rasterPackError_.isEmpty()) {
        return false;
    }

    const QString appPath = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appPath).filePath(QStringLiteral("assets/raster/decal_textures.bin")),
        QDir(QDir::currentPath()).filePath(QStringLiteral("assets/raster/decal_textures.bin")),
    };
    for (const QString &path : candidates) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        QString error;
        if (rasterPack_.load(path, &error)) {
            return true;
        }
        rasterPackError_ = error;
        return false;
    }
    rasterPackError_ = QStringLiteral("assets/raster/decal_textures.bin was not found");
    return false;
}

GLuint NativeShapeRenderer::ensureRasterTexture(QOpenGLFunctions *functions, quint32 rasterId) {
    if (rasterId == 0) {
        return 0;
    }
    if (const auto it = rasterTextures_.constFind(rasterId); it != rasterTextures_.constEnd()) {
        return it.value();
    }
    if (!ensureRasterPackLoaded()) {
        return 0;
    }

    const fh6::RasterDecal decal = rasterPack_.decal(rasterId);
    if (!decal.valid()) {
        return 0;
    }

    GLuint texture = 0;
    functions->glGenTextures(1, &texture);
    functions->glBindTexture(GL_TEXTURE_2D, texture);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    functions->glTexImage2D(GL_TEXTURE_2D,
                            0,
                            GL_RGBA8,
                            decal.width,
                            decal.height,
                            0,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            decal.rgba.constData());
    functions->glBindTexture(GL_TEXTURE_2D, 0);

    rasterTextures_.insert(rasterId, texture);
    rasterTextureSizes_.insert(rasterId, QSize(decal.width, decal.height));
    return texture;
}

GLuint NativeShapeRenderer::renderSceneToTexture(
    const fh6::Project &project,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size) {
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr) {
        return 0;
    }
    if (!drawSceneToFbo(context->functions(), project, geometry, worldToScreen, size)) {
        return 0;
    }
    return sceneFbo_->texture();
}

GLuint NativeShapeRenderer::renderScenesToTexture(
    const QVector<fh6::Project> &projects,
    const QVector<QRect> &clipRects,
    const ShapeGeometryStore &geometry,
    const QTransform &worldToScreen,
    const QSize &size,
    bool preserveExisting) {
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (context == nullptr || !initialized_ || !geometryUploaded_ || vertices_.isEmpty() || size.isEmpty()) {
        return 0;
    }

    QOpenGLFunctions *functions = context->functions();
    ensureSceneFramebuffer(size);
    if (sceneFbo_ == nullptr || !sceneFbo_->isValid()) {
        return 0;
    }

    sceneFbo_->bind();
    functions->glViewport(0, 0, std::max(size.width(), 1), std::max(size.height(), 1));
    functions->glDisable(GL_DEPTH_TEST);
    if (!preserveExisting) {
        functions->glDisable(GL_SCISSOR_TEST);
        functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        functions->glClear(GL_COLOR_BUFFER_BIT);
    } else {
        functions->glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        for (int i = 0; i < projects.size(); ++i) {
            const QRect clip = i < clipRects.size() ? clipRects[i] : QRect();
            const QRect bounded = clip.intersected(QRect(QPoint(0, 0), size));
            if (bounded.isEmpty()) {
                continue;
            }
            functions->glEnable(GL_SCISSOR_TEST);
            functions->glScissor(bounded.x(),
                                 size.height() - bounded.y() - bounded.height(),
                                 bounded.width(),
                                 bounded.height());
            functions->glClear(GL_COLOR_BUFFER_BIT);
        }
    }

    bool drew = false;
    for (int i = 0; i < projects.size(); ++i) {
        const QRect clip = i < clipRects.size() ? clipRects[i] : QRect();
        if (!clip.isEmpty()) {
            const QRect bounded = clip.intersected(QRect(QPoint(0, 0), size));
            if (bounded.isEmpty()) {
                continue;
            }
            functions->glEnable(GL_SCISSOR_TEST);
            functions->glScissor(bounded.x(),
                                 size.height() - bounded.y() - bounded.height(),
                                 bounded.width(),
                                 bounded.height());
        } else if (preserveExisting) {
            continue;
        } else {
            functions->glDisable(GL_SCISSOR_TEST);
        }
        drew = drawProjectLayers(functions, projects[i], geometry, worldToScreen, size) || drew;
    }

    functions->glDisable(GL_SCISSOR_TEST);
    sceneFbo_->release();
    return (preserveExisting || drew) ? sceneFbo_->texture() : 0;
}

void NativeShapeRenderer::ensureSceneFramebuffer(const QSize &size) {
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

void NativeShapeRenderer::compositeScene(QOpenGLFunctions *functions, const QSize &size, bool clearBackground) {
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

void NativeShapeRenderer::setUniformRows(int row0Location, int row1Location, const QTransform &transform) {
    program_.setUniformValue(row0Location,
                             static_cast<float>(transform.m11()),
                             static_cast<float>(transform.m21()),
                             static_cast<float>(transform.dx()));
    program_.setUniformValue(row1Location,
                             static_cast<float>(transform.m12()),
                             static_cast<float>(transform.m22()),
                             static_cast<float>(transform.dy()));
}

NativeShapeRenderer::ShapeRange NativeShapeRenderer::fallbackRange(int shapeId, const ShapeGeometryStore &geometry) {
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
