#pragma once

#include <QString>

#include <array>

namespace fh6 {

// Automotive paint parameters retain their presence separately from their value so
// decoded material data can take precedence without turning zero/false into "missing".
struct AutomotivePaintParameters {
    bool hasGlancingFlopColor = false;
    std::array<float, 4> glancingFlopColor = {0.0f, 0.0f, 0.0f, 1.0f};
    bool hasGlancingFlopPower = false;
    float glancingFlopPower = 2.0f;
    bool hasGlancingFlopEnabled = false;
    bool glancingFlopEnabled = false;
    bool hasGlancingFlopStrength = false;
    float glancingFlopStrength = 0.0f;

    bool hasFlakeColor = false;
    std::array<float, 4> flakeColor = {1.0f, 1.0f, 1.0f, 1.0f};
    bool hasFlakeCoverage = false;
    float flakeCoverage = 0.0f;
    bool hasFlakeRoughness = false;
    float flakeRoughness = 0.4f;
    bool hasGlitterIntensity = false;
    float glitterIntensity = 0.0f;

    bool hasClearCoatCoverage = false;
    float clearCoatCoverage = 0.0f;
    bool hasClearCoatRoughness = false;
    float clearCoatRoughness = 0.1f;
    bool hasClearCoatTint = false;
    std::array<float, 4> clearCoatTint = {1.0f, 1.0f, 1.0f, 1.0f};
    bool hasClearCoatOnLivery = false;
    bool clearCoatOnLivery = true;

    bool hasNormalIntensity = false;
    float normalIntensity = 1.0f;
    bool hasNormalMap00UvTiling = false;
    std::array<float, 4> normalMap00UvTiling = {1.0f, 1.0f, 0.0f, 0.0f};
    bool hasNormalMap0UvTiling = false;
    std::array<float, 4> normalMap0UvTiling = {1.0f, 1.0f, 0.0f, 0.0f};

    bool hasOrangePeelStrength = false;
    float orangePeelStrength = 0.0f;
    QString normalMap00Texture;
    QString normalMap0Texture;
    QString orangePeelNormalTexture;
};

} // namespace fh6
