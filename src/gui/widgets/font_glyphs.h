#pragma once

#include <QChar>
#include <QString>
#include <QStringList>

// Shared mapping between the vector shape "font" letter blocks and the
// characters they represent. The registry stores each font as two 40-shape
// blocks (an Upper block: A-Z 1-9 0 ! ? @ &, and a Lower block:
// a-z $ pound yen euro ( ) cent * # + % ; : ,). Both the shape browser (which
// merges the two blocks into one named section) and the Text place tool (which
// turns a typed string into a row of glyph shapes) derive from this one table
// so the two never drift apart.
namespace gui {
namespace fontglyphs {

// Font display names, in the order the fonts are presented to the user.
QStringList fontNames();

// Merged section name for a letter-block shape id, or an empty string when the
// id is not part of any font's Upper/Lower block.
QString sectionForShape(int shapeId);

// Shape id for a character rendered in the named font, or -1 when the font has
// no glyph for that character. Case sensitive: 'A' and 'a' resolve to the
// Upper and Lower blocks respectively. Space is not a glyph and returns -1;
// callers treat it as a blank advance.
int glyphShapeId(const QString &fontName, QChar ch);

// True when the shape id sits in some font's Upper block (A-Z, digits, ! ? @ &)
// rather than its Lower block. Lets callers pick a per-case monospace advance,
// since Upper-block glyphs are wider than Lower-block ones.
bool isUpperBlockShape(int shapeId);

} // namespace fontglyphs
} // namespace gui
