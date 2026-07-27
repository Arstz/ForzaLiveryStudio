#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <optional>
#include <vector>

namespace fh6 {

struct GarageColorLut {
    int dimension = 0;
    float scale = 0.0f;
    std::vector<float> rgba;

    bool valid() const;
    std::array<float, 4> texel(int red, int green, int blue) const;
};

std::optional<GarageColorLut> parseGarageColorLut(
    const QByteArray &bytes, QString *error = nullptr);

std::optional<GarageColorLut> loadGarageColorLut(
    const QString &gameFolder, QString *error = nullptr);

} // namespace fh6
