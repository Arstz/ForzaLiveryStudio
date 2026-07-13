#pragma once

#include <QChar>
#include <QString>
#include <QStringList>

namespace gui {
namespace fontglyphs {

QStringList fontNames();

QString sectionForShape(int shapeId);

int glyphShapeId(const QString &fontName, QChar ch);

bool isUpperBlockShape(int shapeId);

} // namespace fontglyphs
} // namespace gui
