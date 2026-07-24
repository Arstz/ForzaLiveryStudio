#include "manufacturer_colors.h"

#include "binary_io.h"
#include "model_bundle.h"

#include <stdexcept>

namespace fh6 {
namespace {

class Cursor {
public:
    explicit Cursor(const QByteArray &bytes)
        : bytes_(bytes) {}

    quint8 u8() {
        require(1);
        return static_cast<quint8>(bytes_[position_++]);
    }

    quint16 u16() {
        require(2);
        const quint16 value = detail::readLeU16(bytes_, position_);
        position_ += 2;
        return value;
    }

    quint32 u32() {
        require(4);
        const quint32 value = detail::readLeU32(bytes_, position_);
        position_ += 4;
        return value;
    }

    float f32() {
        require(4);
        const float value = detail::readLeFloat(bytes_, position_);
        position_ += 4;
        return value;
    }

    QString string8() {
        const int size = u8();
        require(size);
        const QString value = QString::fromLatin1(bytes_.constData() + position_, size);
        position_ += size;
        return value;
    }

private:
    void require(int size) const {
        if (size < 0 || position_ < 0 || position_ + size > bytes_.size()) {
            throw std::runtime_error("ManufacturerColors data is truncated");
        }
    }

    const QByteArray &bytes_;
    int position_ = 0;
};

std::array<float, 3> readColor(Cursor &cursor) {
    return {cursor.f32(), cursor.f32(), cursor.f32()};
}

ManufacturerColorPalette decodePaletteBlob(const QByteArray &data) {
    constexpr quint32 kMaxMaterialSlots = 256;

    Cursor cursor(data);
    const quint32 count = cursor.u8();

    ManufacturerColorPalette palette;
    palette.colors.reserve(static_cast<qsizetype>(count));
    for (quint32 index = 0; index < count; ++index) {
        ManufacturerColor color;
        color.enabled = cursor.u8() != 0;
        const quint32 materialSlotCount = cursor.u32();
        if (materialSlotCount > kMaxMaterialSlots) {
            throw std::runtime_error("ManufacturerColors material-slot list is too large");
        }
        color.materialSlots.reserve(static_cast<qsizetype>(materialSlotCount));
        for (quint32 slot = 0; slot < materialSlotCount; ++slot) {
            color.materialSlots.push_back(cursor.string8());
        }
        color.primary = readColor(cursor);
        color.materialPath = cursor.string8();
        color.secondaryEnabled = cursor.u8() != 0;
        color.secondary = readColor(cursor);
        cursor.u16();
        palette.colors.push_back(std::move(color));
    }

    return palette;
}

} // namespace

const ManufacturerColor *ManufacturerColorPalette::find(quint32 selector) const {
    if (selector >= static_cast<quint32>(colors.size()) || !colors[selector].enabled) {
        return nullptr;
    }

    return &colors[selector];
}

ManufacturerColorPalette decodeManufacturerColors(const QByteArray &bytes) {
    const ModelBundle bundle = parseModelBundle(bytes);
    const std::vector<const BundleBlobRecord *> blobs =
        bundle.blobsWithTag(bundle_tags::ManufacturerColors);
    if (blobs.empty()) {
        throw std::runtime_error("bundle has no ManufacturerColors data");
    }

    return decodePaletteBlob(blobs.front()->data);
}

} // namespace fh6
