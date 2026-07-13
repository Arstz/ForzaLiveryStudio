#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace fh6 {

struct SwatchMask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> coverage;

    bool valid() const { return width > 0 && height > 0 && !coverage.empty(); }
    uint8_t at(int x, int y) const { return coverage[static_cast<size_t>(y) * width + x]; }
};

struct SwatchImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && rgba.size() == static_cast<size_t>(width) * height * 4; }
};

SwatchMask loadSwatchMask(const QString &path, QString *error = nullptr);

SwatchMask decodeSwatchMask(const QByteArray &bytes, QString *error = nullptr);

SwatchImage loadSwatchImage(const QString &path, QString *error = nullptr);

SwatchImage decodeSwatchImage(const QByteArray &bytes, QString *error = nullptr);

} // namespace fh6
