#include "swatchbin.h"

#include "binary_io.h"

#include <QFile>

#include <cstring>
#include <optional>

#ifdef Q_OS_WIN
#  include <d3d11.h>
#  include <d3dcompiler.h>
#  include <wrl/client.h>
#endif

namespace fh6 {

using fh6::detail::readLeU16;
using fh6::detail::readLeU32;

namespace {

constexpr quint32 kBundleMagic = 0x47727562;   // "Grub" (on disk "burG")
constexpr quint32 kTagTXCB = 0x54584342;       // "TXCB" (on disk "BCXT") texture blob
constexpr quint32 kTagTXCH = 0x54584348;       // "TXCH" (on disk "HCXT") content header

constexpr int kBlobInfoSize = 0x18;
constexpr int kMetadataInfoSize = 0x08;

struct TextureSurface {
    int width = 0;
    int height = 0;
    int formatEncoded = -1;
    QByteArray pixels;
};

enum class Encoding : int {
    Bc1 = 0, Bc2 = 1, Bc3 = 2, UnsignedBc4 = 3, SignedBc4 = 4,
    UnsignedBc5 = 5, SignedBc5 = 6, UnsignedBc6H = 7, SignedBc6H = 8,
    Bc7 = 9, R32G32B32A32Float = 10, R16G16B16A16 = 11, R16G16B16A16Float = 12,
    R8G8B8A8 = 13, B5G6R5 = 14, B5G5R5A1 = 15, Dct = 16, IntegerDct = 17,
    Procedural = 18, R8 = 19, A8 = 20, R8G8 = 21, Bc7HighQuality = 22,
};

void setError(QString *error, const QString &message)
{
    if (error != nullptr) {
        *error = message;
    }
}

// Decodes one BC4 (single-channel) 8-byte block into a 4x4 tile of bytes.
// Layout: red0, red1, then 16 x 3-bit palette indices packed little-endian.
void decodeBc4Block(const uint8_t *block, uint8_t out[16])
{
    const int r0 = block[0];
    const int r1 = block[1];
    int red[8];
    red[0] = r0;
    red[1] = r1;
    if (r0 > r1) {
        for (int i = 1; i < 7; ++i) {
            red[i + 1] = ((7 - i) * r0 + i * r1) / 7;
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            red[i + 1] = ((5 - i) * r0 + i * r1) / 5;
        }
        red[6] = 0;
        red[7] = 255;
    }

    // 48 bits of indices in block[2..7], 3 bits per texel.
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    }
    for (int i = 0; i < 16; ++i) {
        const int idx = static_cast<int>((bits >> (3 * i)) & 0x7);
        out[i] = static_cast<uint8_t>(red[idx]);
    }
}

SwatchMask decodeBc4Surface(const QByteArray &data, int width, int height, QString *error)
{
    const int blocksWide = (width + 3) / 4;
    const int blocksHigh = (height + 3) / 4;
    const qint64 needed = static_cast<qint64>(blocksWide) * blocksHigh * 8;
    if (data.size() < needed) {
        setError(error, QStringLiteral("swatchbin BC4 data truncated"));
        return {};
    }

    SwatchMask mask;
    mask.width = width;
    mask.height = height;
    mask.coverage.assign(static_cast<size_t>(width) * height, 0);

    const auto *src = reinterpret_cast<const uint8_t *>(data.constData());
    for (int by = 0; by < blocksHigh; ++by) {
        for (int bx = 0; bx < blocksWide; ++bx) {
            uint8_t tile[16];
            decodeBc4Block(src, tile);
            src += 8;
            for (int ty = 0; ty < 4; ++ty) {
                const int py = by * 4 + ty;
                if (py >= height) {
                    continue;
                }
                for (int tx = 0; tx < 4; ++tx) {
                    const int px = bx * 4 + tx;
                    if (px >= width) {
                        continue;
                    }
                    mask.coverage[static_cast<size_t>(py) * width + px] = tile[ty * 4 + tx];
                }
            }
        }
    }
    return mask;
}

SwatchMask decodeR8Surface(const QByteArray &data, int width, int height, QString *error)
{
    const qint64 needed = static_cast<qint64>(width) * height;
    if (data.size() < needed) {
        setError(error, QStringLiteral("swatchbin R8 data truncated"));
        return {};
    }
    SwatchMask mask;
    mask.width = width;
    mask.height = height;
    mask.coverage.resize(static_cast<size_t>(width) * height);
    std::memcpy(mask.coverage.data(), data.constData(), static_cast<size_t>(needed));
    return mask;
}

SwatchImage decodeRgba8Surface(const QByteArray &data, int width, int height, QString *error)
{
    const qint64 needed = static_cast<qint64>(width) * height * 4;
    if (data.size() < needed) {
        setError(error, QStringLiteral("swatchbin RGBA8 data truncated"));
        return {};
    }
    SwatchImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<size_t>(needed));
    std::memcpy(image.rgba.data(), data.constData(), static_cast<size_t>(needed));
    return image;
}

#ifdef Q_OS_WIN
QString hresultString(const char *step, HRESULT hr)
{
    return QStringLiteral("%1 failed (HRESULT 0x%2)")
        .arg(QString::fromLatin1(step))
        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

SwatchImage decodeBc7Surface(const QByteArray &data, int width, int height, QString *error)
{
    const int blocksWide = (width + 3) / 4;
    const int blocksHigh = (height + 3) / 4;
    const qint64 needed = static_cast<qint64>(blocksWide) * blocksHigh * 16;
    if (data.size() < needed) {
        setError(error, QStringLiteral("swatchbin BC7 data truncated"));
        return {};
    }

    using Microsoft::WRL::ComPtr;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &device, &selectedLevel, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &device, &selectedLevel, &context);
    }
    if (FAILED(hr)) {
        setError(error, hresultString("D3D11CreateDevice", hr));
        return {};
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcDesc.Width = static_cast<UINT>(width);
    srcDesc.Height = static_cast<UINT>(height);
    srcDesc.MipLevels = 1;
    srcDesc.ArraySize = 1;
    srcDesc.Format = DXGI_FORMAT_BC7_UNORM;
    srcDesc.SampleDesc.Count = 1;
    srcDesc.Usage = D3D11_USAGE_IMMUTABLE;
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srcData{};
    srcData.pSysMem = data.constData();
    srcData.SysMemPitch = static_cast<UINT>(blocksWide * 16);
    srcData.SysMemSlicePitch = static_cast<UINT>(needed);
    ComPtr<ID3D11Texture2D> srcTexture;
    hr = device->CreateTexture2D(&srcDesc, &srcData, &srcTexture);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateTexture2D(BC7)", hr));
        return {};
    }

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(srcTexture.Get(), nullptr, &srv);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateShaderResourceView", hr));
        return {};
    }

    D3D11_TEXTURE2D_DESC dstDesc{};
    dstDesc.Width = static_cast<UINT>(width);
    dstDesc.Height = static_cast<UINT>(height);
    dstDesc.MipLevels = 1;
    dstDesc.ArraySize = 1;
    dstDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstDesc.SampleDesc.Count = 1;
    dstDesc.Usage = D3D11_USAGE_DEFAULT;
    dstDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> renderTexture;
    hr = device->CreateTexture2D(&dstDesc, nullptr, &renderTexture);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateTexture2D(RGBA)", hr));
        return {};
    }

    ComPtr<ID3D11RenderTargetView> rtv;
    hr = device->CreateRenderTargetView(renderTexture.Get(), nullptr, &rtv);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateRenderTargetView", hr));
        return {};
    }

    static constexpr char kVs[] = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    float2 uv[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    VSOut o;
    o.pos = float4(pos[id], 0.0, 1.0);
    o.uv = uv[id];
    return o;
}
)";
    static constexpr char kPs[] = R"(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
    return tex0.SampleLevel(samp0, uv, 0.0);
}
)";

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    ComPtr<ID3DBlob> shaderError;
    hr = D3DCompile(kVs, std::strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_4_0",
                    0, 0, &vsBlob, &shaderError);
    if (FAILED(hr)) {
        setError(error, hresultString("D3DCompile(VS)", hr));
        return {};
    }
    hr = D3DCompile(kPs, std::strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_4_0",
                    0, 0, &psBlob, &shaderError);
    if (FAILED(hr)) {
        setError(error, hresultString("D3DCompile(PS)", hr));
        return {};
    }

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateVertexShader", hr));
        return {};
    }
    hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr)) {
        setError(error, hresultString("CreatePixelShader", hr));
        return {};
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    hr = device->CreateSamplerState(&samplerDesc, &sampler);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateSamplerState", hr));
        return {};
    }

    const float clear[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(rtv.Get(), clear);
    ID3D11RenderTargetView *rtvs[] = {rtv.Get()};
    context->OMSetRenderTargets(1, rtvs, nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView *srvs[] = {srv.Get()};
    ID3D11SamplerState *samplers[] = {sampler.Get()};
    context->PSSetShaderResources(0, 1, srvs);
    context->PSSetSamplers(0, 1, samplers);
    context->Draw(3, 0);

    D3D11_TEXTURE2D_DESC stagingDesc = dstDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stagingTexture;
    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        setError(error, hresultString("CreateTexture2D(staging)", hr));
        return {};
    }
    context->CopyResource(stagingTexture.Get(), renderTexture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        setError(error, hresultString("Map(staging)", hr));
        return {};
    }

    SwatchImage image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<size_t>(width) * height * 4);
    auto *dst = image.rgba.data();
    const auto *src = static_cast<const uint8_t *>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    rowBytes);
    }
    context->Unmap(stagingTexture.Get(), 0);
    return image;
}
#else
SwatchImage decodeBc7Surface(const QByteArray &, int, int, QString *error)
{
    setError(error, QStringLiteral("BC7 swatchbin decode is only supported on Windows"));
    return {};
}
#endif

std::optional<TextureSurface> readTextureSurface(const QByteArray &bytes, QString *error)
{
    if (bytes.size() < 8 || readLeU32(bytes, 0) != kBundleMagic) {
        setError(error, QStringLiteral("not a Grub bundle (bad magic)"));
        return std::nullopt;
    }

    const quint8 verMajor = static_cast<quint8>(bytes[4]);
    const quint8 verMinor = static_cast<quint8>(bytes[5]);

    int pos = 6;
    quint32 blobCount = 0;
    if (verMajor > 1 || (verMajor == 1 && verMinor >= 1)) {
        pos += 2 + 4 + 4; // int16 padding, u32 headerSize, u32 totalSize
        blobCount = readLeU32(bytes, pos);
        pos += 4;
    } else {
        blobCount = readLeU16(bytes, pos);
        pos += 2 + 8;
    }

    const int blobHeadersStart = pos;
    for (quint32 i = 0; i < blobCount; ++i) {
        const int header = blobHeadersStart + static_cast<int>(i) * kBlobInfoSize;
        if (header + kBlobInfoSize > bytes.size()) {
            break;
        }
        if (readLeU32(bytes, header) != kTagTXCB) {
            continue;
        }

        const quint16 metadataCount = readLeU16(bytes, header + 6);
        const quint32 metadataOffset = readLeU32(bytes, header + 8);
        const quint32 dataOffset = readLeU32(bytes, header + 12);
        const quint32 compressedSize = readLeU32(bytes, header + 16);
        const quint32 uncompressedSize = readLeU32(bytes, header + 20);
        const quint32 sizeToRead = uncompressedSize > 0 ? uncompressedSize : compressedSize;

        int txchData = -1;
        for (quint32 m = 0; m < metadataCount; ++m) {
            const int recordBase = static_cast<int>(metadataOffset) + static_cast<int>(m) * kMetadataInfoSize;
            if (recordBase + kMetadataInfoSize > bytes.size()) {
                break;
            }
            if (readLeU32(bytes, recordBase) == kTagTXCH) {
                txchData = recordBase + readLeU16(bytes, recordBase + 6);
                break;
            }
        }
        if (txchData < 0 || txchData + 64 > bytes.size()) {
            setError(error, QStringLiteral("swatchbin missing TXCH header"));
            return std::nullopt;
        }

        const int width = static_cast<int>(readLeU32(bytes, txchData + 0x18));
        const int height = static_cast<int>(readLeU32(bytes, txchData + 0x1C));
        const quint16 packed = readLeU16(bytes, txchData + 0x24);
        const int platform = packed >> 14;
        const qint32 transcoding = static_cast<qint32>(readLeU32(bytes, txchData + 0x28));
        const quint32 slicesOffset = readLeU32(bytes, txchData + 0x38);

        if (platform != 0) {
            setError(error, QStringLiteral("swatchbin is Xbox/Durango format (tiled); not supported"));
            return std::nullopt;
        }
        if (width <= 0 || height <= 0 || slicesOffset == 0) {
            setError(error, QStringLiteral("swatchbin has no decodable slice"));
            return std::nullopt;
        }

        const int sliceEnc = static_cast<int>(readLeU32(bytes, txchData + static_cast<int>(slicesOffset)));
        int formatEncoded = sliceEnc;
        if (transcoding > 1) {
            formatEncoded = transcoding - 2;
        }

        if (static_cast<int>(dataOffset) < 0 || static_cast<qint64>(dataOffset) + sizeToRead > bytes.size()) {
            setError(error, QStringLiteral("swatchbin pixel data out of range"));
            return std::nullopt;
        }

        TextureSurface surface;
        surface.width = width;
        surface.height = height;
        surface.formatEncoded = formatEncoded;
        surface.pixels = bytes.mid(static_cast<int>(dataOffset), static_cast<int>(sizeToRead));
        return surface;
    }

    setError(error, QStringLiteral("no TXCB texture blob found in swatchbin"));
    return std::nullopt;
}

} // namespace

SwatchMask decodeSwatchMask(const QByteArray &bytes, QString *error)
{
    const auto surface = readTextureSurface(bytes, error);
    if (!surface) {
        return {};
    }

    switch (static_cast<Encoding>(surface->formatEncoded)) {
    case Encoding::UnsignedBc4:
    case Encoding::SignedBc4:
        return decodeBc4Surface(surface->pixels, surface->width, surface->height, error);
    case Encoding::R8:
    case Encoding::A8:
        return decodeR8Surface(surface->pixels, surface->width, surface->height, error);
    default:
        setError(error, QStringLiteral("swatchbin encoding %1 not supported (only BC4/R8 masks)")
                            .arg(surface->formatEncoded));
        return {};
    }
}

SwatchImage decodeSwatchImage(const QByteArray &bytes, QString *error)
{
    const auto surface = readTextureSurface(bytes, error);
    if (!surface) {
        return {};
    }

    switch (static_cast<Encoding>(surface->formatEncoded)) {
    case Encoding::Bc7:
    case Encoding::Bc7HighQuality:
        return decodeBc7Surface(surface->pixels, surface->width, surface->height, error);
    case Encoding::R8G8B8A8:
        return decodeRgba8Surface(surface->pixels, surface->width, surface->height, error);
    default:
        setError(error, QStringLiteral("swatchbin encoding %1 not supported as a color image")
                            .arg(surface->formatEncoded));
        return {};
    }
}

SwatchMask loadSwatchMask(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open %1").arg(path));
        return {};
    }
    return decodeSwatchMask(file.readAll(), error);
}

SwatchImage loadSwatchImage(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("cannot open %1").arg(path));
        return {};
    }
    return decodeSwatchImage(file.readAll(), error);
}

} // namespace fh6
