#pragma once

#include "model_geometry.h"

#include <QHash>
#include <QSet>

namespace gui {

QSet<quint64> carPaintableBindings(const fls::CarModel &model, int lodIndex,
                                 const QHash<int, int> &partSelections);

} // namespace gui
