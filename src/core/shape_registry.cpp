#include "shape_registry.h"

namespace {

struct ShapeRange {
    quint16 start;
    quint16 end;
    const char *prefix;
};

// Keep this table aligned with the keys in assets/vector/shape_names.json.
// Each shipped shape family contains 40 consecutive IDs.
constexpr ShapeRange kShapeRanges[] = {
    {0x0065, 0x008c, "Primitives"},
    {0x00c9, 0x00f0, "Gradient_Shapes"},
    {0x012d, 0x0154, "Stripes"},
    {0x0191, 0x01b8, "Tears"},
    {0x01f5, 0x021c, "Racing_Icons"},
    {0x0259, 0x0280, "Flames"},
    {0x02bd, 0x02e4, "Paint_Splats"},
    {0x0321, 0x0348, "Tribal"},
    {0x0385, 0x03ac, "Nature"},
    {0x0515, 0x053c, "Upper_Letters_2"},
    {0x0579, 0x05a0, "Lower_Letters_2"},
    {0x05dd, 0x0604, "Upper_Letters_3"},
    {0x0641, 0x0668, "Lower_Letters_3"},
    {0x06a5, 0x06cc, "Upper_Letters_4"},
    {0x0709, 0x0730, "Lower_Letters_4"},
    {0x076d, 0x0794, "Upper_Letters_1"},
    {0x07d1, 0x07f8, "Lower_Letters_1"},
    {0x0835, 0x085c, "Community_Vinyls_1"},
    {0x0899, 0x08c0, "Community_Vinyls_2"},
    {0x08fd, 0x0924, "Community_Vinyls_3"},
    {0x0961, 0x0988, "Community_Vinyls_4"},
    {0x09c5, 0x09ec, "Upper_Letters_5"},
    {0x0a29, 0x0a50, "Lower_Letters_5"},
    {0x0a8d, 0x0ab4, "Upper_Letters_6"},
    {0x0af1, 0x0b18, "Lower_Letters_6"},
    {0x0b55, 0x0b7c, "Upper_Letters_7"},
    {0x0bb9, 0x0be0, "Lower_Letters_7"},
    {0x0c1d, 0x0c44, "Upper_Letters_8"},
    {0x0c81, 0x0ca8, "Lower_Letters_8"},
    {0x0ce5, 0x0d0c, "Upper_Letters_9"},
    {0x0d49, 0x0d70, "Lower_Letters_9"},
    {0x0dad, 0x0dd4, "Upper_Letters_10"},
    {0x0e11, 0x0e38, "Lower_Letters_10"},
    {0x0e75, 0x0e9c, "Upper_Letters_11"},
    {0x0ed9, 0x0f00, "Lower_Letters_11"},
};

const ShapeRange *rangeForShape(quint16 shapeId)
{
    for (const ShapeRange &range : kShapeRanges) {
        if (shapeId >= range.start && shapeId <= range.end) {
            return &range;
        }
    }
    return nullptr;
}

} // namespace

namespace fh6::detail {

quint16 canonicalShapeId(quint16 encodedShapeId)
{
    // Captured C_group and C_livery streams encode Arial lowercase `a` one
    // below its logical registry ID. Keep the wire alias at the decode boundary
    // so every runtime/editor shape continues to use the ID present in
    // shape_names.json and shape_geometry.json.gz.
    return encodedShapeId == 0x07d0 ? 0x07d1 : encodedShapeId;
}

bool isKnownShapeId(quint16 shapeId)
{
    return rangeForShape(shapeId) != nullptr;
}

QString shapeName(quint16 shapeId)
{
    if (const ShapeRange *range = rangeForShape(shapeId)) {
        return QStringLiteral("%1_0x%2")
            .arg(QString::fromLatin1(range->prefix))
            .arg(shapeId, 4, 16, QLatin1Char('0'));
    }
    return QStringLiteral("0x%1").arg(shapeId, 0, 16);
}

} // namespace fh6::detail
