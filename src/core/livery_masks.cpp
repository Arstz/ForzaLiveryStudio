#include "livery_masks.h"

#include <QDir>
#include <QFile>
#include <QXmlStreamReader>

namespace fh6 {
namespace {

struct SideDef {
    const char *maskTag;
    const char *swatchStem;
};
constexpr SideDef kSideDefs[kLiverySideCount] = {
    {"Front", "front"},
    {"Back", "back"},
    {"Top", "top"},
    {"Left", "left"},
    {"Right", "right"},
    {"Wing", "wing"},
    {"Glass_Front", "glass_Front"},
    {"Glass_Back", "glass_Back"},
    {"Glass_Top", "glass_Top"},
    {"Glass_Left", "glass_Left"},
    {"Glass_Right", "glass_Right"},
};

void parseAxis(const QString &token, int &axis, float &sign)
{
    sign = 1.0f;
    int i = 0;
    if (i < token.size() && (token[i] == QLatin1Char('+') || token[i] == QLatin1Char('-'))) {
        sign = token[i] == QLatin1Char('-') ? -1.0f : 1.0f;
        ++i;
    }
    const QChar c = i < token.size() ? token[i].toLower() : QLatin1Char('x');
    axis = c == QLatin1Char('y') ? 1 : (c == QLatin1Char('z') ? 2 : 0);
}

void applyAttributes(const QXmlStreamAttributes &attrs, LiverySide &side)
{
    const auto f = [&](const char *name, float def) {
        return attrs.hasAttribute(QLatin1String(name))
            ? attrs.value(QLatin1String(name)).toFloat()
            : def;
    };
    side.valid = attrs.value(QLatin1String("valid")).toString().compare(
                     QStringLiteral("true"), Qt::CaseInsensitive) == 0;
    side.xOrigin = f("xorigin", 0.0f);
    side.yOrigin = f("yorigin", 0.0f);
    side.top = f("top", 0.0f);
    side.bottom = f("bottom", 0.0f);
    side.left = f("left", 0.0f);
    side.right = f("right", 0.0f);
    side.xScale = f("xScale", 1.0f);
    side.yScale = f("yScale", 1.0f);
    side.rotationDeg = f("rotation", 0.0f);
    parseAxis(attrs.value(QLatin1String("xAxis")).toString(), side.xAxis, side.xSign);
    parseAxis(attrs.value(QLatin1String("yAxis")).toString(), side.yAxis, side.ySign);
}

} // namespace

LiveryMaskSet loadLiveryMasks(const QString &dir, QString *error)
{
    LiveryMaskSet set;
    for (int i = 0; i < kLiverySideCount; ++i) {
        set.sides[i].slot = i;
        set.sides[i].maskName = QString::fromLatin1(kSideDefs[i].maskTag);
    }

    const QDir folder(dir);
    QFile xml(folder.filePath(QStringLiteral("Masks.xml")));
    if (!xml.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(xml.fileName());
        }
        return set;
    }

    QXmlStreamReader reader(&xml);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QString tag = reader.name().toString();
        for (int i = 0; i < kLiverySideCount; ++i) {
            if (tag.compare(QString::fromLatin1(kSideDefs[i].maskTag), Qt::CaseInsensitive) == 0) {
                applyAttributes(reader.attributes(), set.sides[i]);
                break;
            }
        }
    }
    if (reader.hasError()) {
        if (error != nullptr) {
            *error = QStringLiteral("Masks.xml parse error: %1").arg(reader.errorString());
        }
        return set;
    }
    set.loaded = true;

    for (int i = 0; i < kLiverySideCount; ++i) {
        if (kSideDefs[i].swatchStem == nullptr) {
            continue;
        }
        const QString path = folder.filePath(
            QStringLiteral("%1.swatchbin").arg(QString::fromLatin1(kSideDefs[i].swatchStem)));
        if (!QFile::exists(path)) {
            continue;
        }
        SwatchMask mask = loadSwatchMask(path);
        if (mask.valid()) {
            set.sides[i].mask = std::move(mask);
            ++set.loadedMasks;
        }
    }

    return set;
}

} // namespace fh6
