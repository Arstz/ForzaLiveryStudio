#pragma once

#include <QVector2D>
#include <QVector3D>

#include <algorithm>

namespace gui {

enum class LightDirectionCandidate {
    OppositeXmlDirection,
    XmlDirection,
    BothDirections,
};

namespace garage_render_defaults {

inline const QVector3D kRawXmlLightDirection(-0.197420f, -0.302618f, 0.932442f);
inline const QVector3D kLightChromaticity(0.900001f, 0.900000f, 0.750001f);
inline const QVector3D kAmbientColor(0.142109f, 0.175171f, 0.197850f);
inline const QVector3D kEnvironmentLowColor(0.025f, 0.028f, 0.035f);
inline const QVector3D kEnvironmentHighColor(0.58f, 0.64f, 0.72f);
inline const QVector3D kBackgroundColor(0.09f, 0.10f, 0.12f);
inline const QVector3D kGroundColor(0.115f, 0.125f, 0.145f);
inline constexpr bool kHdrEnabled = true;
inline constexpr bool kGroundEnabled = true;
inline constexpr float kDirectDiffuseScale = 3.0f;
inline constexpr float kDirectSpecularScale = 2.0f;
inline constexpr float kAmbientScale = 0.2f;
inline constexpr float kEnvironmentDiffuseScale = 0.15f;
inline constexpr float kEnvironmentSpecularLowScale = 0.15f;
inline constexpr float kEnvironmentSpecularHighScale = 0.95f;
inline constexpr float kExposure = -0.4f;
inline constexpr float kFilmicWhite = 1.5f;
inline constexpr float kShadowOpacity = 0.35f;
inline constexpr float kGroundPlaneSizeScale = 4.0f;
inline constexpr float kGroundVerticalOffsetScale = 0.003f;
inline constexpr float kShadowWidthScale = 0.92f;
inline constexpr float kShadowLengthScale = 0.84f;
inline constexpr float kCameraFovDegrees = 45.0f;
inline constexpr float kCameraYawRadians = 0.6f;
inline constexpr float kCameraPitchRadians = 0.25f;
inline constexpr float kCameraInitialDistance = 4.0f;
inline constexpr float kCameraDistanceRadiusScale = 2.6f;

} // namespace garage_render_defaults

struct GarageRenderSettings {
    struct Lighting {
        QVector3D rawXmlDirection = garage_render_defaults::kRawXmlLightDirection;
        QVector3D chromaticity = garage_render_defaults::kLightChromaticity;
        QVector3D ambientColor = garage_render_defaults::kAmbientColor;
        LightDirectionCandidate directionCandidate = LightDirectionCandidate::BothDirections;
        float directDiffuseScale = garage_render_defaults::kDirectDiffuseScale;
        float directSpecularScale = garage_render_defaults::kDirectSpecularScale;
        float ambientScale = garage_render_defaults::kAmbientScale;

        QVector3D xmlRendererDirection() const {
            return QVector3D(
                -rawXmlDirection.x(), rawXmlDirection.y(), rawXmlDirection.z()).normalized();
        }

        QVector3D primaryRendererDirection() const {
            if (directionCandidate == LightDirectionCandidate::XmlDirection) {
                return xmlRendererDirection();
            }

            return -xmlRendererDirection();
        }

        QVector3D secondaryRendererDirection() const {
            return xmlRendererDirection();
        }

        QVector2D directLightWeights() const {
            return directionCandidate == LightDirectionCandidate::BothDirections
                ? QVector2D(0.5f, 0.5f)
                : QVector2D(1.0f, 0.0f);
        }

        QVector3D normalizedChromaticity() const {
            const float maximum = std::max({chromaticity.x(), chromaticity.y(), chromaticity.z()});

            return maximum > 0.0f ? chromaticity / maximum : QVector3D(1.0f, 1.0f, 1.0f);
        }
    };

    struct Environment {
        QVector3D lowColor = garage_render_defaults::kEnvironmentLowColor;
        QVector3D highColor = garage_render_defaults::kEnvironmentHighColor;
        QVector3D backgroundColor = garage_render_defaults::kBackgroundColor;
        float diffuseScale = garage_render_defaults::kEnvironmentDiffuseScale;
        float specularLowScale = garage_render_defaults::kEnvironmentSpecularLowScale;
        float specularHighScale = garage_render_defaults::kEnvironmentSpecularHighScale;
    };

    struct PostProcessing {
        bool hdrEnabled = garage_render_defaults::kHdrEnabled;
        float exposure = garage_render_defaults::kExposure;
        float filmicWhite = garage_render_defaults::kFilmicWhite;
    };

    struct Ground {
        bool enabled = garage_render_defaults::kGroundEnabled;
        QVector3D color = garage_render_defaults::kGroundColor;
        float shadowOpacity = garage_render_defaults::kShadowOpacity;
        float planeSizeScale = garage_render_defaults::kGroundPlaneSizeScale;
        float verticalOffsetScale = garage_render_defaults::kGroundVerticalOffsetScale;
        float shadowWidthScale = garage_render_defaults::kShadowWidthScale;
        float shadowLengthScale = garage_render_defaults::kShadowLengthScale;
    };

    struct Camera {
        float fovDegrees = garage_render_defaults::kCameraFovDegrees;
        float yawRadians = garage_render_defaults::kCameraYawRadians;
        float pitchRadians = garage_render_defaults::kCameraPitchRadians;
        float initialDistance = garage_render_defaults::kCameraInitialDistance;
        float distanceRadiusScale = garage_render_defaults::kCameraDistanceRadiusScale;
    };

    Lighting lighting;
    Environment environment;
    PostProcessing postProcessing;
    Ground ground;
    Camera camera;
};

} // namespace gui
