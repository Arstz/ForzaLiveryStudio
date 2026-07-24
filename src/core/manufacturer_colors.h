#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <memory>

namespace fh6 {

struct ModelMaterial;

struct ManufacturerColor {
    QStringList materialSlots;
    QString materialPath;
    std::shared_ptr<ModelMaterial> material;
    std::array<float, 3> primary = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> secondary = {0.0f, 0.0f, 0.0f};
    bool enabled = false;
    bool secondaryEnabled = false;
};

struct ManufacturerColorPalette {
    QVector<ManufacturerColor> colors;

    const ManufacturerColor *find(quint32 selector) const;
};

ManufacturerColorPalette decodeManufacturerColors(const QByteArray &bytes);

} // namespace fh6
