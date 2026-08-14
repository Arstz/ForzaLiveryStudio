#pragma once

#include "core_types.h"
#include "car_unwrap_overlay.h"
#include "livery_masks.h"
#include "manufacturer_colors.h"
#include "model_geometry.h"
#include "paint_finish_catalog.h"

#include <QtGui>
#include <QtOpenGL>

#include <memory>
#include <vector>

namespace fls {
struct ModelMaterialTexture;
}

namespace gui {

CarUnwrapOverlay buildCarUnwrapOverlay(const fls::CarModel &model,
                                       const fls::LiveryMaskSet &masks,
                                       int lodIndex = 0);
CarUnwrapOverlaySet buildCarUnwrapOverlaySet(
    const fls::CarModel &model,
    const fls::LiveryMaskSet &masks,
    const QSet<int> &selectablePartTypes,
    int lodIndex = 0);

class CarModelRenderer {
public:
    CarModelRenderer();
    ~CarModelRenderer();

    void initialize();
    void release();
    bool isInitialized() const;
    static QString shaderSelfTest();
    bool hasModel() const;

    void uploadModel(const fls::CarModel &model, int lodIndex = 0);
    void clearModel();
    void setPartSelections(const QHash<int, int> &selections) { partSelections_ = selections; }

    void setLivery(const fls::CarModel &model, const fls::LiveryMaskSet &masks);
    void setPaintTextureRegions(const QVector<QVector4D> &regions);
    void clearLivery();

    void setDebugMode(int mode) { debugMode_ = mode; }
    int debugMode() const { return debugMode_; }
    void setWireframeVisible(bool visible) { wireframeVisible_ = visible; }
    bool wireframeVisible() const { return wireframeVisible_; }
    void setWireframeSide(int side) { wireframeSide_ = side; }

    void render(const QMatrix4x4 &view,
                const QMatrix4x4 &projection,
                GLuint liveryTexture,
                const QColor &basePaint,
                const fls::LiveryPaintState *paintState,
                const fls::ManufacturerColorPalette *manufacturerColors,
                const fls::PaintFinishLibrary *paintFinishes);

private:
    struct MeshBuffers {
        QOpenGLVertexArrayObject vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int indexCount = 0;
        bool applyLivery = false;
        bool hasDirectLiveryUv = false;
        bool bodyPaint = false;
        int allowedSides = 0;
        bool translucent = false;
        bool doubleSided = false;
        float alpha = 1.0f;
        std::vector<quint64> paintMaterialHashes;
        bool hasMaterialColor = false;
        QVector3D materialColor;
        QVector3D emissiveColor;
        float emissiveIntensity = 0.0f;
        float gloss = 0.45f;
        float metallic = 0.0f;
        QString name;
        QString materialName;
        int carPartType = -1;
        bool stockPart = true;
        std::vector<int> partOptionIds;
        GLuint diffuseTexture = 0;
        GLuint alphaTexture = 0;
        GLuint normalTexture = 0;
        GLuint surfaceTexture = 0;
        GLuint emissiveTexture = 0;
        QVector3D center;
        QMatrix4x4 model;
    };

    struct MaterialTextureCacheEntry {
        std::shared_ptr<const fls::ModelMaterialTexture> source;
        GLuint id = 0;
        qsizetype bytes = 0;
    };

    struct WireframeBuffers {
        QOpenGLVertexArrayObject vao;
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int indexCount = 0;
        bool hasDirectLiveryUv = false;
        int allowedSides = 0;
        int carPartType = -1;
        bool stockPart = true;
        std::vector<int> partOptionIds;
        QMatrix4x4 model;
    };

    struct FinishTextureEntry {
        GLuint pattern = 0;
        GLuint normal = 0;
        GLuint surface = 0;
    };

    GLuint uploadSwatchTexture(const fls::SwatchImage &image);
    const FinishTextureEntry *ensurePaintFinishTextures(int code, const fls::PaintFinishRender &render);
    void clearPaintFinishTextures();

    static constexpr int kLiverySideCount = fls::kLiverySideCount;

    QOpenGLShaderProgram program_;
    std::vector<std::unique_ptr<MeshBuffers>> meshes_;
    std::vector<std::unique_ptr<WireframeBuffers>> wireframeMeshes_;
    QHash<QString, MaterialTextureCacheEntry> materialTextureCache_;
    qsizetype materialTextureCacheBytes_ = 0;
    QHash<int, FinishTextureEntry> finishTextureCache_;
    unsigned paintFinishGeneration_ = 0;
    bool paintFinishTracked_ = false;
    bool paintDiagnosticsLogged_ = false;
    bool initialized_ = false;
    bool wireframeVisible_ = false;
    int wireframeSide_ = -1;

    GLuint sideMaskArray_ = 0;
    int sideCount_ = 0;
    int debugMode_ = 0;
    QHash<int, int> partSelections_;
    QVector<QVector4D> sideAxis_;
    QVector<QVector2D> sideEMin_;
    QVector<QVector2D> sideEMax_;
    QVector<QVector4D> sideRegion_;
    QVector<QVector4D> defaultSidePaintRegion_;
    QVector<QVector4D> sidePaintRegion_;
    QVector<QVector3D> sideFacing_;

    int mvpLocation_ = -1;
    int modelLocation_ = -1;
    int liveryTexLocation_ = -1;
    int basePaintLocation_ = -1;
    int hasLiveryLocation_ = -1;
    int useDirectUvLocation_ = -1;
    int sideMasksLocation_ = -1;
    int sideCountLocation_ = -1;
    int sideAxisLocation_ = -1;
    int sideEMinLocation_ = -1;
    int sideEMaxLocation_ = -1;
    int sideRegionLocation_ = -1;
    int sidePaintRegionLocation_ = -1;
    int sideFacingLocation_ = -1;
    int debugModeLocation_ = -1;
    int wireframeOverlayLocation_ = -1;
    int allowedSidesLocation_ = -1;
    int materialAlphaLocation_ = -1;
    int secondaryPaintLocation_ = -1;
    int secondaryMixLocation_ = -1;
    int glossLocation_ = -1;
    int metallicLocation_ = -1;
    int emissiveLocation_ = -1;
    int eyePositionLocation_ = -1;
    int nativeDiffuseLocation_ = -1;
    int nativeAlphaLocation_ = -1;
    int nativeNormalLocation_ = -1;
    int nativeSurfaceLocation_ = -1;
    int nativeEmissiveLocation_ = -1;
    int hasNativeDiffuseLocation_ = -1;
    int hasNativeAlphaLocation_ = -1;
    int hasNativeNormalLocation_ = -1;
    int hasNativeSurfaceLocation_ = -1;
    int hasNativeEmissiveLocation_ = -1;
    int finishPatternLocation_ = -1;
    int finishNormalLocation_ = -1;
    int finishSurfaceLocation_ = -1;
    int hasFinishPatternLocation_ = -1;
    int hasFinishNormalLocation_ = -1;
    int hasFinishSurfaceLocation_ = -1;
    int finishSelfColoredLocation_ = -1;
    int finishTilingLocation_ = -1;
    int finishFlakeLocation_ = -1;
    int clearCoatRoughnessLocation_ = -1;
    int clearCoatIntensityLocation_ = -1;
};

} // namespace gui
