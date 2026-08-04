#pragma once

#include <QtGlobal>

#include <algorithm>
#include <array>

namespace fh6::material_hashes {

template <typename Container, typename Value>
constexpr bool contains(const Container &hashes, Value hash) {
    return std::find(hashes.cbegin(), hashes.cend(), hash) != hashes.cend();
}

namespace binding {

inline constexpr quint64 kBodyPaint = 0xF7DBE8A7C839A675ULL;
inline constexpr quint64 kHoodPaint = 0x6AC1E9D87FE5D953ULL;
inline constexpr quint64 kMirrorPaint = 0x1E5FF0F50C741122ULL;
inline constexpr quint64 kSpoilerPaint = 0xCD48110253EE319AULL;
inline constexpr quint64 kWindowGlass = 0x9582FD1BA2FFF9A4ULL;
inline constexpr quint64 kBrakeCaliper = 0xA5495E0A43DF55B9ULL;

inline constexpr std::array<quint64, 7> kLiveryMaterials = {
    kBodyPaint,
    kHoodPaint,
    kMirrorPaint,
    kSpoilerPaint,
    0xBCEA13C28AA26965ULL,
    0xB963C2391A4EB883ULL,
    0x4A7FF0B38CA8F1A0ULL,
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

} // namespace binding

namespace parameter {

inline constexpr std::array<quint32, 21> kBaseColor = {
    0xEA718FBE, 0x53A946B6, 0x6B242133, 0x63040D89, 0xF51639BE,
    0x57C321A6, 0x73A9E2DF, 0x1F3EB7A9, 0xEF5CCE09, 0x76BEA808,
    0x1F30F777, 0x1925D9BF, 0xD0F0433A, 0xA76D0485, 0xD9826618,
    0x00FC00E4, 0x1F0BBA20, 0x36976C2B, 0x5D1D0449, 0x0940E415,
    0xC0CB2820,
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
inline constexpr quint32 kGlancingFlopColor = 0x682DB27E;
inline constexpr quint32 kFlakeColor = 0x38B56360;
inline constexpr quint32 kGlancingFlopEnabled = 0xB10FD456;
inline constexpr quint32 kGlancingFlopPower = 0x194C4F17;
inline constexpr quint32 kGlancingFlopStrength = 0x9B4EC25B;
inline constexpr quint32 kFlakeRoughness = 0x4E547D2C;
inline constexpr quint32 kFlakeCoverage = 0x604BA06B;
inline constexpr quint32 kGlitterIntensity = 0x4393C2CA;
inline constexpr quint32 kClearcoatCoverage = 0x22CB725E;
inline constexpr quint32 kClearcoatTint = 0x218D90FE;
inline constexpr quint32 kClearcoatOnLivery = 0x443C9A28;
inline constexpr quint32 kNormalIntensity = 0xCD6B029B;
inline constexpr quint32 kNormalMap00UvTiling = 0x9A7DB1FA;
inline constexpr quint32 kNormalMap0UvTiling = 0xFD6BB566;
inline constexpr quint32 kOrangePeelStrength = 0xAB378776;
inline constexpr quint32 kUvOrientation = 0x8B7343AB;
inline constexpr quint32 kWeaveColorTintA = 0xB0338A61;
inline constexpr quint32 kWeaveColorTintB = 0x29D1EC60;
inline constexpr quint32 kWeaveNormalIntensity = 0x4D033DB0;
inline constexpr quint32 kClearCoatNormalUTiling = 0x5968BA4B;
inline constexpr quint32 kClearCoatNormalVTiling = 0xB3EE6729;
inline constexpr std::array<quint32, 2> kTextureTilingU = {
    0x19A7D8F1, 0xB01AEE8E,
};
inline constexpr std::array<quint32, 2> kTextureTilingV = {
    0x4A3D8375, 0x3E95E96D,
};
inline constexpr quint32 kTextureTiling = 0xB99646E7;
inline constexpr quint32 kRoughnessShift = 0x4E282123;

inline constexpr std::array<quint32, 4> kMetallic = {
    0x938926B0, 0xA415641F, 0x1F9B6488, 0x9114636B,
};

inline constexpr std::array<quint32, 2> kFlakeAmount = {
    0x86EF8FB1, 0x604BA06B,
};

inline constexpr quint32 kColorTexture = 0x85E937A9;
inline constexpr quint32 kNormalTexture = 0xF9E8078D;
inline constexpr quint32 kSurfaceTexture = 0x8D9C56EF;
inline constexpr quint32 kTireHeightAoTexture = 0x86AB1BDA;
inline constexpr std::array<quint32, 2> kAoTexture = {
    0x7FDA2F1B, 0x0FEA383B,
};
inline constexpr quint32 kNormalMap00Texture = 0xA0256751;
inline constexpr quint32 kNormalMap0Texture = 0x8FC670E1;
inline constexpr quint32 kOrangePeelNormalTexture = 0x8C7FDE22;
inline constexpr quint32 kWeaveMaskTexture = 0xBAA9FAA5;
inline constexpr quint32 kWeaveNormalTexture = 0xEC13FF23;
inline constexpr quint32 kClearCoatNormalTexture = 0xD8999797;

inline constexpr std::array<quint32, 4> kDetailNormalTexture = {
    0xEC13FF23, 0x87078E77, 0xB59BE3AB, 0xB61760D8,
};

inline constexpr std::array<quint32, 5> kEmissiveTexture = {
    0x4E0D5E89, 0x6161E552, 0x020B22EB, 0x212B4B48, 0x3CB4DFCB,
};

inline constexpr std::array<quint32, 4> kAlphaTexture = {
    0x698CA64F, 0x57D9D49E, 0x66E53F62, 0x2FDCDBF0,
};

} // namespace parameter

} // namespace fh6::material_hashes
