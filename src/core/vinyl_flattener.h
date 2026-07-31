#pragma once

#include "core_types.h"

#include <QVector>

namespace fls {

class VinylFlattener {
public:
    QVector<FlattenedLayer> flattenGroup(const VinylGroup &root) const;
};

QVector<FlattenedLayer> flattenGroup(const VinylGroup &root);

} // namespace fls
