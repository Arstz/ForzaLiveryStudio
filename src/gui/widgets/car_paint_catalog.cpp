#include "car_paint_catalog.h"
#include "car_paint_geometry.h"

#include "material_hashes.h"
#include "paint_database.h"
#include "paint_finish_catalog.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int kPositionalBindingCount =
    static_cast<int>(fls::material_hashes::binding::kPositionalPaintSlots.size()) - 1;

QString displayName(QString name) {
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    name.replace(QRegularExpression(QStringLiteral("([a-z])([A-Z])")), QStringLiteral("\\1 \\2"));

    return name;
}

QColor nativeColor(const QJsonArray &rgb) {
    const auto channel = [](double linear) {
        linear = std::clamp(linear, 0.0, 1.0);
        return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    };

    return QColor::fromRgbF(channel(rgb[0].toDouble()), channel(rgb[1].toDouble()), channel(rgb[2].toDouble()));
}

QString slotToken(quint64 hash) {
    using namespace fls::material_hashes::binding;
    if (hash == kHoodPaint) return QStringLiteral("Hood");
    if (hash == kMirrorPaint) return QStringLiteral("Mirror");
    if (hash == kSpoilerPaint) return QStringLiteral("Wing");
    if (hash == kBodySecondaryPaint) return QStringLiteral("Body_SecondaryColor");
    if (hash == kHoodSecondaryPaint) return QStringLiteral("Hood_SecondaryColor");
    if (hash == kMirrorSecondaryPaint) return QStringLiteral("Mirror_SecondaryColor");
    if (hash == kWingSecondaryPaint) return QStringLiteral("Wing_SecondaryColor");
    if (hash == kBodyTertiaryPaint) return QStringLiteral("Body_TertiaryColor");
    if (hash == kHoodTertiaryPaint) return QStringLiteral("Hood_TertiaryColor");
    if (hash == kMirrorTertiaryPaint) return QStringLiteral("Mirror_TertiaryColor");
    if (hash == kWingTertiaryPaint) return QStringLiteral("Wing_TertiaryColor");
    if (hash == kTrim1) return QStringLiteral("Trim1");
    if (hash == kTrim2) return QStringLiteral("Trim2");

    return QStringLiteral("Body");
}

} // namespace

QVector<CarPaintSlot> carPaintSlots(const fls::CarModel &model, const fls::LiveryPaintState &paint,
                                  int lodIndex, const QHash<int, int> &partSelections) {
    using namespace fls::material_hashes::binding;
    const QStringList names = {
        QStringLiteral("Body"), QStringLiteral("Hood"), QStringLiteral("Mirrors"), QStringLiteral("Spoiler"),
        QStringLiteral("Brakes"), QStringLiteral("Rims"), QStringLiteral("Windows"),
        QStringLiteral("Body 2"), QStringLiteral("Hood 2"), QStringLiteral("Mirrors 2"), QStringLiteral("Spoiler 2"),
        QStringLiteral("Body 3"), QStringLiteral("Hood 3"), QStringLiteral("Mirrors 3"), QStringLiteral("Spoiler 3"),
        QStringLiteral("Trim 1"), QStringLiteral("Trim 2"), QStringLiteral("Wheel detail 1"), QStringLiteral("Wheel detail 2"),
        QStringLiteral("Override 1"), QStringLiteral("Override 2"), QStringLiteral("Override 3"),
        QStringLiteral("Override 4"), QStringLiteral("Override 5"), QStringLiteral("Override 6"),
        QStringLiteral("Override 7"), QStringLiteral("Override 8"), QStringLiteral("Override 9"),
        QStringLiteral("Rims 2"), QStringLiteral("Rims 3"), QStringLiteral("Rim barrel"), QStringLiteral("Rim lip"),
        QStringLiteral("Spoiler end plates"), QStringLiteral("Spoiler plane"), QStringLiteral("Spoiler struts"),
        QStringLiteral("Tow hook"),
        QStringLiteral("Front rims"), QStringLiteral("Front rim group 2"), QStringLiteral("Front rim barrel"),
        QStringLiteral("Front hub"), QStringLiteral("Front wheel bolts"),
        QStringLiteral("Rear rims"), QStringLiteral("Rear rim group 2"), QStringLiteral("Rear rim barrel"),
        QStringLiteral("Rear hub"), QStringLiteral("Rear wheel bolts"),
    };
    QVector<CarPaintSlot> paintSlots;
    QSet<quint64> present = carPaintableBindings(model, lodIndex, partSelections);
    const bool positional = !paint.game.isEmpty() && paint.game != QStringLiteral("FH6");
    if (positional) {
        const auto bindings = present.values();
        for (quint64 hash : bindings) {
            if (fls::material_hashes::contains(kWheelPaintGroups, hash)
                || fls::material_hashes::contains(kFrontWheelPaint, hash)
                || fls::material_hashes::contains(kRearWheelPaint, hash)) {
                present.remove(hash);
                present.insert(kRims);
            }
        }
    }
    for (size_t index = 0; index < kLiveryMaterials.size(); ++index) {
        const quint64 hash = kLiveryMaterials[index];
        if (!present.contains(hash) || (positional && index >= kPositionalBindingCount)) continue;
        CarPaintSlot slot;
        slot.name = names[static_cast<qsizetype>(index)];
        slot.hash = hash;
        slot.window = hash == kWindowGlass;
        slot.wheel = fls::material_hashes::contains(kWheelPaintGroups, hash)
            || fls::material_hashes::contains(kFrontWheelPaint, hash)
            || fls::material_hashes::contains(kRearWheelPaint, hash);
        if (slot.wheel && hash != kRims) slot.fallbackHashes.push_back(kRims);
        if (fls::material_hashes::contains(kDefaultCarPaintGroups, hash) && hash != kBodyPaint)
            slot.fallbackHashes.push_back(kBodyPaint);
        paintSlots.push_back(std::move(slot));
    }
    for (const auto &material : paint.materials) {
        if (material.materialHash != 0 && present.contains(material.materialHash)
            && !fls::material_hashes::contains(kLiveryMaterials, material.materialHash)) {
            paintSlots.push_back({QStringLiteral("Paint slot %1").arg(paintSlots.size() + 1), {}, material.materialHash});
        }
    }

    return paintSlots;
}

QVector<CarPaintFinish> carPaintFinishes(const QString &game) {
    const QJsonArray finishes = fls::gamePaintDatabase(game).value(QStringLiteral("finishes")).toArray();
    QVector<CarPaintFinish> result;
    for (const QJsonValue &value : finishes) {
        const QJsonObject entry = value.toObject();
        CarPaintFinish finish;
        finish.code = static_cast<quint32>(entry.value(QStringLiteral("ID")).toInt());
        finish.colorCount = entry.value(QStringLiteral("NumInputColors")).toInt(1);
        finish.wheelsAllowed = entry.value(QStringLiteral("WheelsOK")).toInt(1) != 0;
        const int previewCode = fls::matchingPaintFinish(finish.code, game, QStringLiteral("FH6"));
        const fls::PaintFinishInfo *info = fls::findPaintFinish(previewCode);
        finish.special = finish.colorCount == 0 || (info != nullptr
            && (info->category == fls::PaintFinishCategory::Pattern || info->category == fls::PaintFinishCategory::Metal));
        finish.name = info != nullptr ? info->displayName
            : displayName(entry.value(QStringLiteral("Material")).toString());
        if (finish.name.isEmpty()) finish.name = QStringLiteral("Material %1").arg(finish.code);
        result.push_back(std::move(finish));
    }
    if (result.isEmpty()) {
        for (const auto &info : fls::paintFinishTable()) {
            result.push_back({info.displayName, static_cast<quint32>(info.code),
                              info.category == fls::PaintFinishCategory::TwoTone ? 2 : 1, true,
                              info.category == fls::PaintFinishCategory::Pattern || info.category == fls::PaintFinishCategory::Metal});
        }
    }

    return result;
}

QVector<PaintColorPreset> carManufacturerColors(const QString &game, int carId, quint64 slot, bool secondary) {
    const QJsonObject database = fls::gamePaintDatabase(game);
    const QJsonObject palette = database.value(QStringLiteral("manufacturer_palettes")).toObject()
        .value(QString::number(carId)).toObject();
    const QJsonArray colors = palette.value(QStringLiteral("colors")).toArray();
    const QString token = slotToken(slot);
    const auto &bindings = fls::material_hashes::binding::kPositionalPaintSlots;
    const auto legacyBinding = std::find(bindings.cbegin(), bindings.cend(), slot);
    const quint64 mask = slot == 0 || legacyBinding == bindings.cend()
        ? 0 : quint64(1) << (legacyBinding - bindings.cbegin());
    QVector<PaintColorPreset> result;
    for (const QJsonValue &value : colors) {
        const QJsonObject color = value.toObject();
        if (!color.value(QStringLiteral("enabled")).toBool()) continue;
        const QJsonArray entries = color.value(QStringLiteral("entries")).toArray();
        QJsonObject chosen;
        for (const QJsonValue &entryValue : entries) {
            const QJsonObject entry = entryValue.toObject();
            if (entry.value(QStringLiteral("rgb")).toArray().size() != 3) continue;
            if (chosen.isEmpty()) chosen = entry;
            if (entry.value(QStringLiteral("material_slots")).toArray().contains(token)
                || (static_cast<quint64>(entry.value(QStringLiteral("material_mask")).toDouble()) & mask) != 0) {
                chosen = entry;
                break;
            }
        }
        QJsonArray rgb = chosen.value(QStringLiteral("rgb")).toArray();
        const QJsonArray overrides = color.value(QStringLiteral("color_overrides")).toArray();
        const int channel = secondary ? 1 : 0;
        if (channel < overrides.size()) {
            const QJsonObject overrideColor = overrides[channel].toObject();
            if (overrideColor.value(QStringLiteral("enabled")).toBool()
                && overrideColor.value(QStringLiteral("rgb")).toArray().size() == 3)
                rgb = overrideColor.value(QStringLiteral("rgb")).toArray();
        }
        if (rgb.size() != 3) continue;
        const int index = color.value(QStringLiteral("index")).toInt();
        QString name = displayName(chosen.value(QStringLiteral("name")).toString());
        if (name.isEmpty()) name = QStringLiteral("Manufacturer %1").arg(index + 1);
        result.push_back({name, nativeColor(rgb), index});
    }

    return result;
}

} // namespace gui
