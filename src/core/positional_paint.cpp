#include "positional_paint.h"

#include "binary_io.h"
#include "material_hashes.h"
#include "paint_database.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kRecordSize = kPositionalPaintRecordSize;
constexpr int kBlockOverhead = kPositionalPaintBlockOverhead;
constexpr quint32 kCustomColorSelector = 0xffffffffu;
constexpr std::array<int, 3> kRecordCounts = {20, 30, 34};
using namespace material_hashes::binding;
constexpr std::array<quint64, 16> kSlotBindings = {
    kBodyPaint, 0, kHoodPaint, kMirrorPaint, kSpoilerPaint, kBrakeCaliper,
    kRims, kWindowGlass, kBodySecondaryPaint, kHoodSecondaryPaint,
    kMirrorSecondaryPaint, kWingSecondaryPaint, kBodyTertiaryPaint,
    kHoodTertiaryPaint, kMirrorTertiaryPaint, kWingTertiaryPaint,
};

bool validPaintBlock(const QByteArray &block) {
    const int recordCount = (static_cast<int>(block.size()) - kBlockOverhead) / kRecordSize;
    if (block.size() != kBlockOverhead + recordCount * kRecordSize
        || std::find(kRecordCounts.begin(), kRecordCounts.end(), recordCount) == kRecordCounts.end()
        || static_cast<quint8>(block.front()) > 1
        || !block.endsWith(QByteArray("\0\x01", 2))) {
        return false;
    }
    for (int slot = 0; slot < recordCount; ++slot) {
        if (!detail::bytesAt(block, 1 + slot * kRecordSize, {1, 1})) {
            return false;
        }
    }

    return true;
}

LiveryPaintMaterial decodeRecord(const QByteArray &record, quint64 hash) {
    LiveryPaintMaterial material;
    material.materialHash = hash;
    material.primary.enabled = record[2] != 0;
    material.secondary.enabled = record[7] != 0;
    for (int channel = 0; channel < 4; ++channel) {
        material.primary.bgra[channel] = static_cast<quint8>(record[3 + channel]);
        material.secondary.bgra[channel] = static_cast<quint8>(record[8 + channel]);
    }
    material.manufacturerSelector = detail::readLeU32(record, 27);
    material.finish = detail::readLeU32(record, 31);

    return material;
}

QByteArray defaultRecord(int slot) {
    QByteArray record(kRecordSize, '\0');
    QByteArray selector;
    const bool selectable = (slot >= 5 && slot <= 7) || slot >= 12;
    record[0] = '\x01';
    record[1] = '\x01';
    detail::appendLeU32(selector, selectable ? kCustomColorSelector : 0);
    record.replace(27, selector.size(), selector);

    return record;
}

void patchRecord(QByteArray &record, const LiveryPaintMaterial &material,
                 const QString &sourceGame, const QString &targetGame, bool authored, bool edited) {
    QByteArray selectors;
    quint32 finish = exportedPaintFinish(material.finish, sourceGame, targetGame);
    const bool custom = material.manufacturerSelector == kCustomColorSelector;
    if ((authored || edited) && finish == 0 && custom && material.materialHash != kWindowGlass
        && (material.primary.enabled || material.secondary.enabled)) {
        finish = 1;
    }
    record[2] = material.primary.enabled ? '\x01' : '\0';
    record[7] = material.secondary.enabled ? '\x01' : '\0';
    for (int channel = 0; channel < 4; ++channel) {
        record[3 + channel] = static_cast<char>(material.primary.bgra[channel]);
        record[8 + channel] = static_cast<char>(material.secondary.bgra[channel]);
    }
    detail::appendLeU32(selectors, material.manufacturerSelector);
    detail::appendLeU32(selectors, finish);
    record.replace(27, selectors.size(), selectors);
    if (authored && material.primary.enabled && material.materialHash != kWindowGlass) {
        for (const int offset : {12, 17, 22}) {
            record[offset] = '\x01';
        }
    }
}

std::optional<LiveryPaintMaterial> wheelOverride(const LiveryPaintState &state) {
    std::optional<LiveryPaintMaterial> result;
    const LiveryPaintMaterial *base = state.find(kRims);
    for (const auto &hashes : {kFrontWheelPaint, kRearWheelPaint}) {
        for (const quint64 hash : hashes) {
            const LiveryPaintMaterial *material = state.find(hash);
            if (material == nullptr) {
                continue;
            }
            LiveryPaintMaterial normalized = *material;
            normalized.materialHash = kRims;
            if (!result) {
                result = normalized;
            } else if (*result != normalized) {
                throw std::runtime_error("destination requires shared wheel paint");
            }
        }
    }
    if (result) {
        for (const auto &hashes : {kFrontWheelPaint, kRearWheelPaint}) {
            const bool present = std::any_of(hashes.begin(), hashes.end(), [&state](quint64 hash) {
                return state.find(hash) != nullptr;
            });
            if (!present && (base == nullptr || *base != *result)) {
                throw std::runtime_error("destination requires shared wheel paint");
            }
        }
    }

    return result;
}

} // namespace

QByteArray positionalPaintBlock(const QByteArray &payload) {
    const int artwork = payload.indexOf(QByteArray("gyvl", 4));
    if (artwork < 4) {
        return {};
    }
    const qint64 stats = static_cast<qint64>(artwork) + detail::readLeU32(payload, artwork - 4);
    if (stats < artwork || stats + 28 > payload.size()) {
        return {};
    }
    const bool tagged = payload.mid(stats, 4) == QByteArray("yrvl", 4);
    const qint64 start = stats + (tagged ? 36 : 28);
    if (tagged && payload.mid(start - 4, 4) != QByteArray("yrvl", 4)) {
        return {};
    }
    for (const int count : kRecordCounts) {
        const int size = kBlockOverhead + count * kRecordSize;
        const qint64 end = start + size;
        if (end > payload.size()
            || (tagged ? payload.mid(end, 4) != QByteArray("yrvl", 4) : end != payload.size())) {
            continue;
        }
        const QByteArray block = payload.mid(start, size);
        if (validPaintBlock(block)) {
            return block;
        }
    }

    return {};
}

QString positionalPaintGame(const QByteArray &payload, int recordCount) {
    const bool motorsport = payload.mid(41, 4) == QByteArray("yrvl", 4)
        && detail::readLeU32(payload, 4) == 2;
    const quint32 version = detail::readLeU32(payload, motorsport ? 45 : 29);
    if (motorsport) {
        return QStringLiteral("FM2023");
    }
    if (recordCount == 20) {
        return version == 2 ? QStringLiteral("FM5") : QStringLiteral("FH2");
    }
    if (recordCount == 30) {
        return version == 5 ? QStringLiteral("FM6")
            : version == 6 ? QStringLiteral("FH3") : QStringLiteral("FM7");
    }

    return version >= 18 ? QStringLiteral("FH5") : QStringLiteral("FH4");
}

LiveryPaintState decodePositionalPaint(const QByteArray &block, const QString &game) {
    LiveryPaintState state;
    if (!validPaintBlock(block)) {
        throw std::runtime_error("invalid positional paint block");
    }
    state.positionalSource = block;
    state.game = game;
    for (int slot = 0; slot < static_cast<int>(kSlotBindings.size()); ++slot) {
        const quint64 hash = kSlotBindings[slot];
        if (hash != 0) {
            state.materials.push_back(decodeRecord(block.mid(1 + slot * kRecordSize, kRecordSize), hash));
        }
    }

    return state;
}

QByteArray encodePositionalPaint(const Project &project, int recordCount, const QString &game) {
    const LiveryPaintState &state = project.liveryPaint;
    const QByteArray source = validPaintBlock(state.positionalSource)
        ? state.positionalSource : positionalPaintBlock(project.liverySource);
    const int sourceCount = source.isEmpty() ? 0
        : (static_cast<int>(source.size()) - kBlockOverhead) / kRecordSize;
    const QString sourceGame = state.game.isEmpty() && sourceCount > 0
        ? positionalPaintGame(project.liverySource, sourceCount) : state.game;
    const std::optional<LiveryPaintMaterial> wheels = wheelOverride(state);
    QByteArray block(1, source.isEmpty() ? '\0' : source.front());
    bool changedWheels = false;
    if (std::find(kRecordCounts.begin(), kRecordCounts.end(), recordCount) == kRecordCounts.end()) {
        throw std::runtime_error("invalid destination paint record count");
    }
    for (int slot = 0; slot < recordCount; ++slot) {
        const quint64 hash = slot < static_cast<int>(kSlotBindings.size()) ? kSlotBindings[slot] : 0;
        const bool retained = slot < sourceCount;
        QByteArray record = retained ? source.mid(1 + slot * kRecordSize, kRecordSize) : defaultRecord(slot);
        const LiveryPaintMaterial *material = hash == kRims && wheels ? &*wheels : state.find(hash);
        if (hash != 0 && material != nullptr) {
            const LiveryPaintMaterial previous = decodeRecord(record, hash);
            const bool changed = !retained || previous != *material;
            if (changed || sourceGame != game) {
                patchRecord(record, *material, sourceGame, game, !retained, changed);
            }
            if (hash == kRims) {
                changedWheels = changed;
            }
        } else if (retained && sourceGame != game) {
            QByteArray finish;
            detail::appendLeU32(finish, exportedPaintFinish(detail::readLeU32(record, 31), sourceGame, game));
            record.replace(31, finish.size(), finish);
        }
        if (slot >= 30 && (changedWheels || !retained)) {
            const LiveryPaintMaterial *rim = wheels ? &*wheels : state.find(kRims);
            if (rim != nullptr) {
                patchRecord(record, *rim, sourceGame, game, !retained, true);
            }
        }
        block.append(record);
    }
    block.append(source.isEmpty() ? QByteArray("\0\x01", 2) : source.right(2));

    return block;
}

} // namespace fls
