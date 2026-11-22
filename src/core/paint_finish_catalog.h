#pragma once

#include "swatchbin.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <array>
#include <optional>

namespace fls {

// How a livery paint "finish" code composites over a car's paintable region.
enum class PaintFinishCategory {
    Solid,   // dielectric paint tinted by the livery's chosen colour
    TwoTone, // primary/secondary colours blended across the view angle
    Metal,   // livery-tinted metallic finish (brushed/polished/plated)
    Pattern, // self-coloured material (carbon, camo, prismacolor, wood, spyshot)
};

// A single entry of the game's global livery-material enumeration
// (Stripped/StringTables List_LiveryMaterials, keyed by IDS_DisplayName_<code>).
struct PaintFinishInfo {
    int code = 0;
    QString displayName;
    QString materialBase; // painttype .materialbin file stem
    PaintFinishCategory category = PaintFinishCategory::Solid;
};

const QVector<PaintFinishInfo> &paintFinishTable();
const PaintFinishInfo *findPaintFinish(int code);

struct PaintFinishUvMapping {
    std::array<float, 2> tiling = {1.0f, 1.0f};
    int channel = -1;
    float rotationDegrees = 0.0f;
    float rotationPivot = 0.0f;
};

struct PaintFinishMetalMask {
    std::array<float, 2> glossRange = {0.0f, 1.0f};
    std::array<float, 3> alternateColor = {};
    bool variesGloss = false;
    bool variesReflectance = false;
};

// Render parameters resolved from a finish's decoded .materialbin.
struct PaintFinishRender {
    bool valid = false;
    PaintFinishCategory category = PaintFinishCategory::Solid;
    bool usesSecondary = false;
    bool selfColored = false;
    bool hasMaterialColor = false;
    bool reflectanceFromNormalAlpha = false;
    float gloss = 0.45f;
    float metallic = 0.0f;
    float flakeAmount = 0.0f;
    float anisotropy = 0.0f;
    float normalIntensity = 1.0f;
    float roughnessShift = 0.0f;
    float patternCoverage = 1.0f;
    float patternIntensity = 1.0f;
    float clearCoatCoverage = -1.0f;
    float clearCoatRoughness = 0.0f;
    std::array<float, 3> materialColor = {0.5f, 0.5f, 0.5f};
    SwatchImage patternImage;      // BaseColorAlpha colour/pattern
    SwatchImage detailNormalImage; // weave / brushed / flake normal
    SwatchImage roughMetalAoImage; // packed roughness/metal/AO
    SwatchImage weaveMaskImage;
    std::vector<SwatchImage> patternMips;
    std::vector<SwatchImage> detailNormalMips;
    std::vector<SwatchImage> roughMetalAoMips;
    std::vector<SwatchImage> weaveMaskMips;
    std::array<float, 3> weaveColorTintA = {};
    std::array<float, 3> weaveColorTintB = {};
    PaintFinishUvMapping patternUv;
    PaintFinishUvMapping normalUv;
    PaintFinishUvMapping surfaceUv;
    std::optional<PaintFinishMetalMask> metalMask;
    std::optional<std::array<float, 3>> specularReflectance;

    bool hasPattern() const { return patternImage.valid(); }
    bool hasDetailNormal() const { return detailNormalImage.valid(); }
    bool hasRoughMetalAo() const { return roughMetalAoImage.valid(); }
    bool hasWeaveMask() const { return weaveMaskImage.valid(); }
};

// Decodes each painttype .materialbin into render parameters keyed by finish code.
// Given a Forza game install folder, materials are read from the customizable
// paint materials archive; a loose folder of .materialbin files is also accepted.
class PaintFinishLibrary {
public:
    void load(const QString &gameFolder, const QString &game = {});
    void clear();
    void replace(PaintFinishLibrary library);
    bool loaded() const { return loaded_; }
    QString folder() const { return folder_; }
    QString game() const { return game_; }
    // Bumped on every load/clear so cached GPU uploads can detect a reload.
    unsigned generation() const { return generation_; }
    int previewCode(quint32 code, const QString &paintGame) const;
    const PaintFinishRender *find(int code) const;

private:
    QString folder_;
    QString game_;
    bool loaded_ = false;
    unsigned generation_ = 0;
    QHash<int, PaintFinishRender> byCode_;
};

} // namespace fls
