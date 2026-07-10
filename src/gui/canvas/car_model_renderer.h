#pragma once

// OpenGL renderer for a decoded car model. A sibling to NativeShapeRenderer: it
// owns its own shader/VBOs and draws the model's meshes with a perspective,
// depth-tested pass. Each body-paint fragment applies the livery by runtime
// planar projection: the fragment's world position is projected onto each
// livery side's paint canvas (per the car's Masks.xml axes/region) and the
// per-side coverage swatchbin clips each projection. The owning side is chosen by
// a per-mesh allowed-sides mask (which sides the part may take, so the livery crops
// at panel seams) plus a facing-angle gate, then the side the fragment faces most
// directly wins. The rasterized vinyl canvas is sampled there and blended over a
// flat base paint colour with cheap directional lighting. Only each part's highest
// LOD is drawn; materials/textures/PBR are out of scope.

#include "livery_masks.h"
#include "model_geometry.h"

#include <QtGui>
#include <QtOpenGL>

#include <memory>
#include <vector>

namespace gui {

class CarModelRenderer {
public:
    CarModelRenderer();
    ~CarModelRenderer();

    void initialize();
    void release();
    bool isInitialized() const;
    bool hasModel() const;

    // (Re)uploads the model's meshes. Safe to call repeatedly; replaces any prior model.
    void uploadModel(const fh6::CarModel &model);
    void clearModel();

    // Uploads the car's per-side livery masks (Masks.xml projection params + the
    // BC4 coverage swatchbins) so body-paint fragments can be projected onto the
    // paint canvas. The model is needed to measure each panel's real world extent
    // (per-side, from the body geometry) so the region fit is not skewed by the
    // full car bounds. Safe to call with an empty/invalid set (livery then renders
    // as flat paint). Requires an initialized renderer + current GL context.
    void setLivery(const fh6::CarModel &model, const fh6::LiveryMaskSet &masks);
    void clearLivery();

    // World-canvas scale (canvas units per world metre) used by the projection.
    // Tunable calibration knob; larger = decal covers less of the panel.
    void setLiveryScale(float unitsPerMetre);
    float liveryScale() const { return liveryScale_; }

    // Minimum dot(side facing, fragment normal) for a side to claim a fragment,
    // i.e. cos(max deviation angle). Higher = a side only paints surfaces that
    // face it more directly, discarding projection overflow onto angled panels.
    void setFacingMin(float cosThreshold);
    float facingMin() const { return facingMin_; }

    // Debug visualization for calibrating the projection: 0 = normal render,
    // 1 = projected paint-canvas UV as red/green, 2 = owning side as a flat colour.
    void setDebugMode(int mode) { debugMode_ = mode; }
    int debugMode() const { return debugMode_; }

    // Draws the uploaded meshes. liveryTexture is the GL id of the rasterized vinyl
    // paint canvas (0 = none, meshes render in flat base paint). Expects a current
    // GL context with a depth buffer.
    void render(const QMatrix4x4 &view,
                const QMatrix4x4 &projection,
                GLuint liveryTexture,
                const QColor &basePaint);

private:
    struct MeshBuffers {
        QOpenGLVertexArrayObject vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int indexCount = 0;
        bool applyLivery = false; // true only for body paint/window-glass meshes
        int allowedSides = 0;     // bitmask of livery sides this part may take
        bool translucent = false;
        float alpha = 1.0f;
        QVector3D center; // world-space mesh centre, used for transparent sorting
        QMatrix4x4 model; // bone world transform, in column-vector (M*v) convention
    };

    // Number of livery projection slots (body paint + glass), matching the
    // C_livery/Masks.xml side order.
    static constexpr int kLiverySideCount = fh6::kLiverySideCount;

    QOpenGLShaderProgram program_;
    std::vector<std::unique_ptr<MeshBuffers>> meshes_;
    bool initialized_ = false;

    // Livery projection state.
    GLuint sideMaskArray_ = 0;   // GL_TEXTURE_2D_ARRAY of the per-side swatchbins
    int sideCount_ = 0;          // sides actually uploaded (0 = no livery masks)
    // Global fine-tune multiplier on the per-side region fit (1.0 = exact region).
    // Adjustable at runtime ('[' / ']').
    float liveryScale_ = 1.0f;
    // cos(max deviation) gate: a side is discarded for a fragment whose normal
    // deviates from the side's target facing by more than this. 0.17 ~= 80 degrees
    // (loose, to reach curved nooks); the facing-vs-Back selection prevents the
    // rear overflow that a loose gate used to cause.
    float facingMin_ = 0.17f;
    int debugMode_ = 0;
    // Per-side projection params (kLiverySideCount entries), flattened for uniforms.
    // Uses the Masks.xml axes directly (validated offline against the swatchbin
    // coverage): project world pos onto the two signed axes, normalise to the
    // panel's measured extent, and map left->right / top->bottom into the region.
    // A ±90 rotation becomes an axis swap; N gates which fragments belong.
    QVector<QVector4D> sideAxis_;      // (xAxisIdx, yAxisIdx, xSignScale, ySignScale)
    QVector<QVector2D> sideEMin_;      // min projected (ax, ay) over the panel
    QVector<QVector2D> sideEMax_;      // max projected (ax, ay) over the panel
    QVector<QVector4D> sideRegion_;    // (left, right, top, bottom) canvas units
    QVector<QVector3D> sideFacing_;    // outward panel normal
    QVector<float> sideSwap_;          // 1 when the region is rotated 90 degrees

    int mvpLocation_ = -1;
    int modelLocation_ = -1;
    int liveryTexLocation_ = -1;
    int basePaintLocation_ = -1;
    int hasLiveryLocation_ = -1;
    int sideMasksLocation_ = -1;
    int sideCountLocation_ = -1;
    int sideAxisLocation_ = -1;
    int sideEMinLocation_ = -1;
    int sideEMaxLocation_ = -1;
    int sideRegionLocation_ = -1;
    int sideFacingLocation_ = -1;
    int sideSwapLocation_ = -1;
    int liveryScaleLocation_ = -1;
    int debugModeLocation_ = -1;
    int allowedSidesLocation_ = -1;
    int facingMinLocation_ = -1;
    int materialAlphaLocation_ = -1;
};

} // namespace gui
