#pragma once

#include "core_types.h"

#include <QVector>

namespace fh6 {

class VinylFlattener {
public:
    QVector<FlattenedLayer> flattenGroup(const VinylGroup &root) const;
};

QVector<FlattenedLayer> flattenGroup(const VinylGroup &root);

} // namespace fh6
