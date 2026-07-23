#include "font_glyphs.h"

#include <iterator>

namespace gui {
namespace fontglyphs {
namespace {

struct FontBlocks {
    const char *name;
    int upper;
    int lower;
};

constexpr FontBlocks kFonts[] = {
    {"Arial", 1901, 2001},
    {"Magneto", 1301, 1401},
    {"Freestyle", 1501, 1601},
    {"Pristina", 1701, 1801},
    {"EnglishMT", 2501, 2601},
    {"BrushMT", 2701, 2801},
    {"Impact", 2901, 3001},
    {"Playbill", 3101, 3201},
    {"TimesNewRoman", 3301, 3401},
    {"Elephant", 3501, 3601},
    {"CenturyGothic", 3701, 3801},
};

constexpr int kBlockSize = 40;

int upperOffset(QChar ch) {
    const ushort u = ch.unicode();
    if (u >= 'A' && u <= 'Z') {
        return u - 'A';
    }
    if (u >= '1' && u <= '9') {
        return 26 + (u - '1');
    }
    switch (u) {
    case '0':
        return 35;
    case '!':
        return 36;
    case '?':
        return 37;
    case '@':
        return 38;
    case '&':
        return 39;
    default:
        return -1;
    }
}

int lowerOffset(QChar ch) {
    const ushort u = ch.unicode();
    if (u >= 'a' && u <= 'z') {
        return u - 'a';
    }
    switch (u) {
    case '$':
        return 26;
    case 0x00A3: // pound sign
        return 27;
    case 0x00A5: // yen sign
        return 28;
    case 0x20AC: // euro sign
        return 29;
    case '(':
        return 30;
    case ')':
        return 31;
    case 0x00A2: // cent sign
        return 32;
    case '*':
        return 33;
    case '#':
        return 34;
    case '+':
        return 35;
    case '%':
        return 36;
    case ';':
        return 37;
    case ':':
        return 38;
    case ',':
        return 39;
    default:
        return -1;
    }
}

} // namespace

QStringList fontNames() {
    QStringList names;
    names.reserve(static_cast<int>(std::size(kFonts)));
    for (const FontBlocks &font : kFonts) {
        names << QString::fromLatin1(font.name);
    }
    return names;
}

QString sectionForShape(int shapeId) {
    for (const FontBlocks &font : kFonts) {
        if ((shapeId >= font.upper && shapeId < font.upper + kBlockSize)
            || (shapeId >= font.lower && shapeId < font.lower + kBlockSize)) {
            return QString::fromLatin1(font.name);
        }
    }
    return QString();
}

int glyphShapeId(const QString &fontName, QChar ch) {
    for (const FontBlocks &font : kFonts) {
        if (fontName != QLatin1String(font.name)) {
            continue;
        }
        const int upper = upperOffset(ch);
        if (upper >= 0) {
            return font.upper + upper;
        }
        const int lower = lowerOffset(ch);
        if (lower >= 0) {
            return font.lower + lower;
        }
        return -1;
    }
    return -1;
}

bool isUpperBlockShape(int shapeId) {
    for (const FontBlocks &font : kFonts) {
        if (shapeId >= font.upper && shapeId < font.upper + kBlockSize) {
            return true;
        }
        if (shapeId >= font.lower && shapeId < font.lower + kBlockSize) {
            return false;
        }
    }
    return false;
}

} // namespace fontglyphs
} // namespace gui
