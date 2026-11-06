#pragma once

#include "core_types.h"
#include "model_geometry.h"
#include "paint_color_popup.h"

#include <QHash>

namespace gui {

struct CarPaintSlot {
    QString name;
    QVector<quint64> fallbackHashes;
    quint64 hash = 0;
    bool wheel = false;
    bool window = false;
};

struct CarPaintFinish {
    QString name;
    quint32 code = 0;
    int colorCount = 1;
    bool wheelsAllowed = true;
    bool special = false;
};

QVector<CarPaintSlot> carPaintSlots(const fls::CarModel &model, const fls::LiveryPaintState &paint,
                                  int lodIndex = 0, const QHash<int, int> &partSelections = {});
QVector<CarPaintFinish> carPaintFinishes(const QString &game);
QVector<PaintColorPreset> carManufacturerColors(const QString &game, int carId, quint64 slot, bool secondary);

} // namespace gui
