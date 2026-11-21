#pragma once

#include <QtGlobal>

#include <algorithm>
#include <array>

namespace fls::material_hashes {

template <typename Container, typename Value>
constexpr bool contains(const Container &hashes, Value hash) {
    return std::find(hashes.cbegin(), hashes.cend(), hash) != hashes.cend();
}

namespace binding {

inline constexpr quint64 kLegacyLiveryOverlay = 2;
inline constexpr quint64 kBodyPaint = 0xF7DBE8A7C839A675ULL;
inline constexpr quint64 kHoodPaint = 0x6AC1E9D87FE5D953ULL;
inline constexpr quint64 kMirrorPaint = 0x1E5FF0F50C741122ULL;
inline constexpr quint64 kSpoilerPaint = 0xCD48110253EE319AULL;
inline constexpr quint64 kWindowGlass = 0x9582FD1BA2FFF9A4ULL;
inline constexpr quint64 kBrakeCaliper = 0xA5495E0A43DF55B9ULL;
inline constexpr quint64 kRims = 0xDCE6592BFA0DFA78ULL;
inline constexpr quint64 kRims2 = 0x6963CFB9E5C04BBEULL;
inline constexpr quint64 kRims3 = 0x6963D0B9E5C04D71ULL;
inline constexpr quint64 kRimsInner = 0xA11BBB5668440C18ULL;
inline constexpr quint64 kRimsLip = 0xC3F4EEFF7A11DBCDULL;
inline constexpr quint64 kWheel1 = 0xE20AAE21D70536F7ULL;
inline constexpr quint64 kWheel2 = 0xE20AAF21D70538AAULL;
inline constexpr quint64 kBodySecondaryPaint = 0x48E5B27611922B17ULL;
inline constexpr quint64 kHoodSecondaryPaint = 0x4FD95FB5F29A1C11ULL;
inline constexpr quint64 kMirrorSecondaryPaint = 0x115F3B8D0531FAE2ULL;
inline constexpr quint64 kWingSecondaryPaint = 0x06506BFF10D8BBBAULL;
inline constexpr quint64 kBodyTertiaryPaint = 0x496F92FA5A6AAA57ULL;
inline constexpr quint64 kHoodTertiaryPaint = 0x78D434B9676BAFE1ULL;
inline constexpr quint64 kMirrorTertiaryPaint = 0x23E6ED8FEF16F8E0ULL;
inline constexpr quint64 kWingTertiaryPaint = 0x9A883D636B6F3798ULL;
inline constexpr quint64 kTrim1 = 0xE3AEF60D5E234B88ULL;
inline constexpr quint64 kTrim2 = 0xE3AEF90D5E2350A1ULL;
inline constexpr quint64 kOverride1 = 0xBF98CB3AA93337A4ULL;
inline constexpr quint64 kOverride2 = 0xBF98CE3AA9333CBDULL;
inline constexpr quint64 kOverride3 = 0xBF98CD3AA9333B0AULL;
inline constexpr quint64 kOverride4 = 0xBF98C83AA933328BULL;
inline constexpr quint64 kOverride5 = 0xBF98C73AA93330D8ULL;
inline constexpr quint64 kOverride6 = 0xBF98CA3AA93335F1ULL;
inline constexpr quint64 kOverride7 = 0xBF98C93AA933343EULL;
inline constexpr quint64 kOverride8 = 0xBF98C43AA9332BBFULL;
inline constexpr quint64 kOverride9 = 0xBF98C33AA9332A0CULL;
inline constexpr quint64 kWingEndPlates = 0xBCEA13C28AA26965ULL;
inline constexpr quint64 kWingPlane = 0x6E3810972EA4DBF4ULL;
inline constexpr quint64 kWingStruts = 0xB0C163FF8ADE48FDULL;
inline constexpr quint64 kTowHook = 0x471A5FA481625396ULL;

inline constexpr std::array<quint64, 16> kPositionalPaintSlots = {
    kBodyPaint, 0, kHoodPaint, kMirrorPaint, kSpoilerPaint, kBrakeCaliper,
    kRims, kWindowGlass, kBodySecondaryPaint, kHoodSecondaryPaint,
    kMirrorSecondaryPaint, kWingSecondaryPaint, kBodyTertiaryPaint,
    kHoodTertiaryPaint, kMirrorTertiaryPaint, kWingTertiaryPaint,
};

inline constexpr std::array<quint64, 7> kWheelPaintGroups = {
    kRims,
    kRims2,
    kRims3,
    kRimsInner,
    kRimsLip,
    kWheel1,
    kWheel2,
};

inline constexpr std::array<quint64, 15> kBodyPaintGroups = {
    kBodyPaint,
    kBodySecondaryPaint,
    kBodyTertiaryPaint,
    kTrim1,
    kTrim2,
    kOverride1,
    kOverride2,
    kOverride3,
    kOverride4,
    kOverride5,
    kOverride6,
    kOverride7,
    kOverride8,
    kOverride9,
    kTowHook,
};

inline constexpr std::array<quint64, 3> kHoodPaintGroups = {
    kHoodPaint,
    kHoodSecondaryPaint,
    kHoodTertiaryPaint,
};

inline constexpr std::array<quint64, 3> kMirrorPaintGroups = {
    kMirrorPaint,
    kMirrorSecondaryPaint,
    kMirrorTertiaryPaint,
};

inline constexpr std::array<quint64, 6> kWingPaintGroups = {
    kSpoilerPaint,
    kWingSecondaryPaint,
    kWingTertiaryPaint,
    kWingEndPlates,
    kWingPlane,
    kWingStruts,
};

inline constexpr std::array<quint64, 2> kTrimPaintGroups = {
    kTrim1,
    kTrim2,
};

inline constexpr std::array<quint64, 9> kOverridePaintGroups = {
    kOverride1,
    kOverride2,
    kOverride3,
    kOverride4,
    kOverride5,
    kOverride6,
    kOverride7,
    kOverride8,
    kOverride9,
};

// Bindings that follow the vehicle's body color. Wheels, brakes, and glass are
// intentionally excluded because they have independent paint controls.
inline constexpr std::array<quint64, 27> kDefaultCarPaintGroups = {
    kBodyPaint,
    kBodySecondaryPaint,
    kBodyTertiaryPaint,
    kTrim1,
    kTrim2,
    kOverride1,
    kOverride2,
    kOverride3,
    kOverride4,
    kOverride5,
    kOverride6,
    kOverride7,
    kOverride8,
    kOverride9,
    kTowHook,
    kHoodPaint,
    kHoodSecondaryPaint,
    kHoodTertiaryPaint,
    kMirrorPaint,
    kMirrorSecondaryPaint,
    kMirrorTertiaryPaint,
    kSpoilerPaint,
    kWingSecondaryPaint,
    kWingTertiaryPaint,
    kWingEndPlates,
    kWingPlane,
    kWingStruts,
};

// Each axle has five paint channels used by Forza's wheel paint UI. These are
// separate from the generic Rims/Wheel1/Wheel2 material bindings above.
inline constexpr std::array<quint64, 5> kFrontWheelPaint = {
    0xB8925E450764DE78ULL,
    0x15EDB6869EFC7F22ULL,
    0x2407D33BE191E83DULL,
    0x564DF80BF320D318ULL,
    0x818BB1EF6C704F11ULL,
};

inline constexpr std::array<quint64, 5> kRearWheelPaint = {
    0x6613B1E8FA7AE743ULL,
    0x3A3CBDA8CF17E711ULL,
    0xC338EA21477FD950ULL,
    0x0F1580E714EA9063ULL,
    0x874E9585EAB6EF64ULL,
};

inline constexpr std::array<quint64, 46> kLiveryMaterials = {
    kBodyPaint,
    kHoodPaint,
    kMirrorPaint,
    kSpoilerPaint,
    kBrakeCaliper,
    kRims,
    kWindowGlass,
    kBodySecondaryPaint,
    kHoodSecondaryPaint,
    kMirrorSecondaryPaint,
    kWingSecondaryPaint,
    kBodyTertiaryPaint,
    kHoodTertiaryPaint,
    kMirrorTertiaryPaint,
    kWingTertiaryPaint,
    kTrim1,
    kTrim2,
    kWheel1,
    kWheel2,
    kOverride1,
    kOverride2,
    kOverride3,
    kOverride4,
    kOverride5,
    kOverride6,
    kOverride7,
    kOverride8,
    kOverride9,
    kRims2,
    kRims3,
    kRimsInner,
    kRimsLip,
    kWingEndPlates,
    kWingPlane,
    kWingStruts,
    kTowHook,
    kFrontWheelPaint[0],
    kFrontWheelPaint[1],
    kFrontWheelPaint[2],
    kFrontWheelPaint[3],
    kFrontWheelPaint[4],
    kRearWheelPaint[0],
    kRearWheelPaint[1],
    kRearWheelPaint[2],
    kRearWheelPaint[3],
    kRearWheelPaint[4],
};

inline constexpr std::array<quint64, 11> kLiveryPanels = {
    0x4FF3746D9B055F1DULL,
    0xE00E033E6A20B977ULL,
    0xE00E023E6A20B7C4ULL,
    0xE00E053E6A20BCDDULL,
    0xE00E043E6A20BB2AULL,
    0xE00DFF3E6A20B2ABULL,
    0xE00DFE3E6A20B0F8ULL,
    0xE00E013E6A20B611ULL,
    0xE00E003E6A20B45EULL,
    0xE00DFB3E6A20ABDFULL,
    0xE00DFA3E6A20AA2CULL,
};

} // namespace binding

namespace parameter {

inline constexpr std::array<quint32, 20> kBaseColor = {
    0xEA718FBE, 0x53A946B6, 0x6B242133, 0x63040D89, 0xF51639BE,
    0x57C321A6, 0x73A9E2DF, 0x1F3EB7A9, 0xEF5CCE09, 0x76BEA808,
    0x1F30F777, 0x1925D9BF, 0xD0F0433A, 0xA76D0485, 0xD9826618,
    0x00FC00E4, 0x1F0BBA20, 0x36976C2B, 0x5D1D0449, 0x0940E415,
};

inline constexpr std::array<quint32, 8> kEmissiveColor = {
    0x4E0D5E89, 0x6161E552, 0x020B22EB, 0x212B4B48,
    0x3CB4DFCB, 0x21EC1E4D, 0xEFBBC518, 0x1D6AA640,
};

inline constexpr std::array<quint32, 6> kEmissiveIntensity = {
    0x074CCD8C, 0x9421C781, 0xD78943E8, 0x4C6E94DA, 0x22F9702D, 0xE76C20ED,
};

inline constexpr std::array<quint32, 6> kOpacity = {
    0x698CA64F, 0x5D3E6F2D, 0x85E937A9, 0x03ED197F, 0x9C489ADE, 0x40CCF359,
};

inline constexpr std::array<quint32, 10> kGloss = {
    0x5FF94E67, 0x70328B61, 0xF8D6CE36, 0x355CC996, 0xBD820385,
    0xDC5CC796, 0xD2AFDCA3, 0xBA21FEC7, 0x7E88DE7D, 0x52E99DA3,
};

inline constexpr quint32 kClearcoatRoughness = 0x18A539DD;
inline constexpr quint32 kTextureTilingU = 0x19A7D8F1;
inline constexpr quint32 kTextureTilingV = 0x4A3D8375;
inline constexpr quint32 kTextureTiling = 0xB99646E7;

inline constexpr std::array<quint32, 2> kMetallic = {
    0x938926B0, 0xA415641F,
};

inline constexpr std::array<quint32, 2> kFlakeAmount = {
    0x86EF8FB1, 0x604BA06B,
};

inline constexpr quint32 kColorTexture = 0x85E937A9;
inline constexpr quint32 kNormalTexture = 0xF9E8078D;
inline constexpr quint32 kTireNormalHeightTexture = 0x1585B73B;
inline constexpr quint32 kLegacyTireNormalTexture = 0x18EA8FF0;
inline constexpr quint32 kLegacyTireHeightTexture = 0x86AB1BDA;
inline constexpr quint32 kTireSurfaceMasks = 0x6510A127;
inline constexpr quint32 kTireMapTiling = 0x25F1CD0C;
inline constexpr quint32 kTireBaseColor = 0xE795999C;
inline constexpr quint32 kSurfaceTexture = 0x8D9C56EF;
inline constexpr quint32 kWeaveMask = 3131701925;
inline constexpr quint32 kWeaveNormalTexture = 3960733475;
inline constexpr quint32 kWeaveColorTintA = 2956167777;
inline constexpr quint32 kWeaveColorTintB = 701623392;
inline constexpr quint32 kWeaveMetalness = 3750855534;
inline constexpr quint32 kWeaveRoughness = 2690608703;
inline constexpr quint32 kWeaveNormalIntensity = 1292058032;
inline constexpr quint32 kAnisotropy = 1448136802;
inline constexpr quint32 kClearCoatCoverage = 583758430;
inline constexpr quint32 kUserColorSwitch = 3440540967;
inline constexpr quint32 kUserCarColor = 2221386404;
inline constexpr quint32 kUvOrientation = 2339586987;

inline constexpr std::array<quint32, 4> kDetailNormalTexture = {
    kWeaveNormalTexture, 0x87078E77, 0xB59BE3AB, 0xB61760D8,
};

inline constexpr std::array<quint32, 5> kEmissiveTexture = {
    0x4E0D5E89, 0x6161E552, 0x020B22EB, 0x212B4B48, 0x3CB4DFCB,
};
inline constexpr quint32 kEmissiveMaskTexture = 2132370879;
inline constexpr quint32 kEmissionTint = 493528640;
inline constexpr quint32 kEmissionEnabled = 3484217686;
inline constexpr quint32 kEmissionUvOrientation = 3769686720;
inline constexpr quint32 kEmissionTilingU = 2446521101;
inline constexpr quint32 kEmissionTilingV = 2069114479;
inline constexpr quint32 kEmissionMaskTilingU = 2490477472;
inline constexpr quint32 kEmissionMaskTilingV = 2130147010;
inline constexpr quint32 kLegacyEmissionTexture = 1156339763;
inline constexpr quint32 kLegacyEmissionTiling = 763429556;
inline constexpr quint32 kPrimaryUvSelection = 1076596432;
inline constexpr quint32 kGlassBackNormal = 2282876097;
inline constexpr quint32 kGlassBackNormalIntensity = 2224331770;
inline constexpr quint32 kLegacyGlassNormal = 41477758;
inline constexpr quint32 kLegacyGlassNormalTiling = 3498128923;
inline constexpr quint32 kLegacyBulbNormalTiling = 2485952580;
inline constexpr quint32 kLegacyGlassAlphaTiling = 1334899531;
inline constexpr quint32 kGlassColor = 421910975;
inline constexpr quint32 kGlassOpacity = 130281832;
inline constexpr quint32 kLegacyGlassOpacity = 1087173465;
inline constexpr quint32 kSecondaryGlassOpacity = 691601806;
inline constexpr quint32 kGlassDualOpacity = 1136567404;
inline constexpr quint32 kGlassOpacityMaskMin = 1505996630;
inline constexpr quint32 kGlassOpacityMaskMax = 3383803836;
inline constexpr quint32 kAlphaTextureEnabled = 1564372781;
inline constexpr quint32 kFirstGenerationGlassNormal = 3368554958;
inline constexpr quint32 kSecondUvGlassNormal = 3908975388;
inline constexpr quint32 kSecondUvGlassNormalTiling = 2199612831;
inline constexpr quint32 kSecondUvGlassTilingU = 2964886654;
inline constexpr quint32 kSecondUvGlassTilingV = 3436818853;
inline constexpr quint32 kSecondUvGlassRotation = 699108822;
inline constexpr quint32 kGlassTintMaskTexture = 1907067561;
inline constexpr quint32 kGlassTintMaskTiling = 3757793948;
inline constexpr quint32 kGlassTintMaskEnabled = 3570293091;
inline constexpr quint32 kGlassOpacityTintMask = 3745540453;
inline constexpr quint32 kGlassTintMultiplier = 2694311068;
inline constexpr quint32 kLegacyEmissionMaskTexture = 3714532035;
inline constexpr quint32 kLegacyEmissionMaskTiling = 2949269687;
inline constexpr quint32 kLegacyEmissionMaskEnabled = 3589082118;
inline constexpr quint32 kLegacyGlassNormalIntensity = 4005214566;

inline constexpr std::array<quint32, 4> kAlphaTexture = {
    0x698CA64F, 0x57D9D49E, 0x66E53F62, 0x2FDCDBF0,
};

} // namespace parameter

} // namespace fls::material_hashes
