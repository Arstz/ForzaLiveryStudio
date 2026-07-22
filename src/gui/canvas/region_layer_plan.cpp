#include "region_layer_plan.h"

#include "region_fill.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

namespace gui {
namespace {

constexpr double kMaximumAbsorbedAreaFraction = 0.1;
constexpr int kNearbyMergeMaximumGap = 6;
constexpr int kNearbyMergeMaximumDetour = 2;
constexpr int kNearbyMergeMaximumForeignOwners = 1;
constexpr std::array<int, 5> kDangerousBridgeDetourAllowances = {0, 2, 4, 8, 16};
constexpr std::array<int, 4> kDangerousClosingRadii = {1, 2, 4, 8};
constexpr std::array<double, 5> kDangerousStraightBridgeWidths = {1.0, 3.0, 5.0,
                                                                  9.0, 17.0};
constexpr int kEstimatedBoundaryShapesPerPoint = 2;
constexpr int kEstimatedPixelsPerCoreShape = 128;
constexpr int kMaximumStraightAttachmentContacts = 192;
constexpr int kMaximumStraightAttachmentsPerPair = 8;
constexpr int kMinimumStraightAttachmentSpacing = 8;
constexpr double kOuterBoundaryStrokeWidth = 3.0;

enum class PlanOperationKind {
    None,
    GeometrySimplification,
    NearbyMerge,
    AdjacentMerge,
    ContainmentMerge,
    Absorption,
};

struct PlannerSuppressions {
    QSet<quint64> nearbyPairs;
    QSet<quint64> adjacentPairs;
    QSet<quint64> containmentPairs;
    QSet<quint64> absorptionPairs;
    QSet<int> geometrySources;
    int geometryOperationCount = 0;
};

struct PlannerOptions {
    bool hierarchicalContourMerges = false;
    bool absorbAllContainedRegions = false;
    bool simplifyAllowedGeometry = false;
    bool acceptAnyOutput = false;
    int maximumForeignOwners = kNearbyMergeMaximumForeignOwners;
};

struct PlanOperation {
    PlanOperationKind kind = PlanOperationKind::None;
    quint64 key = 0;
    int leftSourceIndex = -1;
    int rightSourceIndex = -1;
    int gap = 0;
    int foreignOwnerCount = 0;
    QVector<int> relatedSourceIndices;
};

struct RecoveryAction {
    PlanOperation operation;
    QString diagnostic;

    bool empty() const {
        return operation.kind == PlanOperationKind::None;
    }
};

class DisjointSet {
public:
    explicit DisjointSet(int size)
        : parents_(static_cast<size_t>(size)), ranks_(static_cast<size_t>(size), 0) {
        std::iota(parents_.begin(), parents_.end(), 0);
    }

    int root(int value) {
        int &parent = parents_[static_cast<size_t>(value)];
        if (parent != value) {
            parent = root(parent);
        }

        return parent;
    }

    bool unite(int left, int right) {
        left = root(left);
        right = root(right);
        if (left == right) {
            return false;
        }
        if (ranks_[static_cast<size_t>(left)] < ranks_[static_cast<size_t>(right)]) {
            std::swap(left, right);
        }
        parents_[static_cast<size_t>(right)] = left;
        if (ranks_[static_cast<size_t>(left)] == ranks_[static_cast<size_t>(right)]) {
            ++ranks_[static_cast<size_t>(left)];
        }

        return true;
    }

private:
    std::vector<int> parents_;
    std::vector<std::uint8_t> ranks_;
};

struct SourceRegion {
    QPainterPath outerPath;
    QVector<int> pixels;
    int resultIndex = -1;
    int label = -1;
};

struct LayerGroup {
    QColor color;
    QPainterPath outline;
    QPainterPath outerPath;
    QVector<int> sourceRegionIndices;
    QVector<int> pixels;
    QVector<int> absorbedRegionIndices;
    int firstSourceIndex = -1;
    int sourceArea = 0;
    int fillArea = 0;
};

struct SimplifiedGeometry {
    QVector<int> pixels;
    QPainterPath outline;
    QString kind = QStringLiteral("base");
    int radius = 0;
    int pointCount = 0;
};

struct NearbyMergeCandidate {
    QVector<int> bridgePixels;
    int leftSource = -1;
    int rightSource = -1;
    int leftPixel = -1;
    int rightPixel = -1;
    int gap = 0;
    int edgeClearance = 0;
    int bridgeDetourAllowance = 0;
    int bridgePointCount = 0;
    int bridgeEstimatedShapeCount = 0;
    int separatePointCount = 0;
    int separateEstimatedShapeCount = 0;
    int hierarchyLevel = 0;
    int straightAttachmentCount = 0;
    double bridgeWidth = 0.0;
    bool edgeComponentBridge = false;
    bool hierarchicalContourBridge = false;
};

struct PlannerCandidateCache {
    QVector<NearbyMergeCandidate> candidates;
    int hierarchicalCandidateCount = 0;
    bool ready = false;
};

struct NearbyMergeBridge {
    QVector<int> pixels;
    int leftSource = -1;
};

bool planningCancelled(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

RegionLayerPlan cancelledPlan(RegionLayerPlan plan) {
    plan.cancelled = true;
    plan.diagnostics << QStringLiteral("planning_cancelled=yes");

    return plan;
}

QPainterPath outerPath(const QPainterPath &outline) {
    const QPolygonF contour = regionOuterContour(outline);
    if (contour.size() < 3) {
        return {};
    }
    QPainterPath path;
    path.moveTo(contour.front());
    for (int i = 1; i < contour.size(); ++i) {
        path.lineTo(contour[i]);
    }
    path.closeSubpath();

    return path;
}

bool sameColor(const QColor &left, const QColor &right) {
    return left.rgba() == right.rgba();
}

bool strictlyContains(const QPainterPath &parent, const QPainterPath &child) {
    const QRectF parentBounds = parent.boundingRect();
    const QRectF childBounds = child.boundingRect();

    return !parent.isEmpty() && !child.isEmpty()
        && parentBounds != childBounds
        && parentBounds.contains(childBounds)
        && parent.contains(child);
}

double enclosureSize(const QPainterPath &path) {
    const QRectF bounds = path.boundingRect();

    return bounds.width() * bounds.height();
}

QRect pixelBounds(const QVector<int> &pixels, int width) {
    if (pixels.isEmpty() || width <= 0) {
        return {};
    }
    int minX = width;
    int minY = std::numeric_limits<int>::max();
    int maxX = -1;
    int maxY = -1;
    for (const int pixel : pixels) {
        const int x = pixel % width;
        const int y = pixel / width;
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
    }

    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

QPainterPath tracePixels(const QVector<int> &pixels,
                         const QSize &imageSize,
                         const RegionExtractionParams &params) {
    const int imageWidth = imageSize.width();
    const QRect bounds = pixelBounds(pixels, imageWidth)
        .intersected(QRect(QPoint(0, 0), imageSize));
    if (bounds.isEmpty()) {
        return {};
    }
    const int width = bounds.width();
    const int height = bounds.height();
    std::vector<std::uint8_t> mask(static_cast<size_t>(width) * height, 0);
    for (const int pixel : pixels) {
        const int x = pixel % imageWidth - bounds.left();
        const int y = pixel / imageWidth - bounds.top();
        if (x >= 0 && y >= 0 && x < width && y < height) {
            mask[static_cast<size_t>(y) * width + x] = 1;
        }
    }
    QPainterPath path = traceMaskToPath(
        mask, width, height, QRect(0, 0, width, height), params);
    path.translate(bounds.left(), bounds.top());

    return path;
}

int pathPointCount(const QPainterPath &path) {
    return path.elementCount();
}

int estimatedShapeCount(int pixelCount, int pointCount) {
    const int coreShapes =
        (pixelCount + kEstimatedPixelsPerCoreShape - 1) / kEstimatedPixelsPerCoreShape;

    return pointCount * kEstimatedBoundaryShapesPerPoint + coreShapes;
}

std::vector<std::uint8_t> squareBinaryFilter(
    const std::vector<std::uint8_t> &source,
    int width,
    int height,
    int radius,
    bool requireFullWindow) {
    std::vector<std::uint8_t> horizontal(source.size(), 0);
    std::vector<std::uint8_t> result(source.size(), 0);
    QVector<int> prefix(std::max(width, height) + 1, 0);
    const int windowSize = radius * 2 + 1;
    for (int y = 0; y < height; ++y) {
        prefix[0] = 0;
        for (int x = 0; x < width; ++x) {
            prefix[x + 1] = prefix[x]
                + source[static_cast<size_t>(y) * width + x];
        }
        for (int x = 0; x < width; ++x) {
            const int left = x - radius;
            const int right = x + radius;
            const int clippedLeft = std::max(0, left);
            const int clippedRight = std::min(width - 1, right);
            const int count = prefix[clippedRight + 1] - prefix[clippedLeft];
            const bool enabled = requireFullWindow
                ? left >= 0 && right < width && count == windowSize
                : count > 0;
            horizontal[static_cast<size_t>(y) * width + x] = enabled ? 1 : 0;
        }
    }
    for (int x = 0; x < width; ++x) {
        prefix[0] = 0;
        for (int y = 0; y < height; ++y) {
            prefix[y + 1] = prefix[y]
                + horizontal[static_cast<size_t>(y) * width + x];
        }
        for (int y = 0; y < height; ++y) {
            const int top = y - radius;
            const int bottom = y + radius;
            const int clippedTop = std::max(0, top);
            const int clippedBottom = std::min(height - 1, bottom);
            const int count = prefix[clippedBottom + 1] - prefix[clippedTop];
            const bool enabled = requireFullWindow
                ? top >= 0 && bottom < height && count == windowSize
                : count > 0;
            result[static_cast<size_t>(y) * width + x] = enabled ? 1 : 0;
        }
    }

    return result;
}

QVector<int> morphologicallyClosedPixels(const QVector<int> &pixels,
                                         const QSize &imageSize,
                                         int radius) {
    const int imageWidth = imageSize.width();
    const QRect sourceBounds = pixelBounds(pixels, imageWidth);
    const QRect bounds = sourceBounds.adjusted(-radius * 2, -radius * 2,
                                                radius * 2, radius * 2)
                             .intersected(QRect(QPoint(0, 0), imageSize));
    if (bounds.isEmpty()) {
        return {};
    }
    const int width = bounds.width();
    const int height = bounds.height();
    std::vector<std::uint8_t> mask(static_cast<size_t>(width) * height, 0);
    for (const int pixel : pixels) {
        const int x = pixel % imageWidth - bounds.left();
        const int y = pixel / imageWidth - bounds.top();
        if (x >= 0 && y >= 0 && x < width && y < height) {
            mask[static_cast<size_t>(y) * width + x] = 1;
        }
    }
    const std::vector<std::uint8_t> dilated =
        squareBinaryFilter(mask, width, height, radius, false);
    const std::vector<std::uint8_t> closed =
        squareBinaryFilter(dilated, width, height, radius, true);
    QVector<int> result;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (closed[static_cast<size_t>(y) * width + x] != 0) {
                result.push_back((bounds.top() + y) * imageWidth + bounds.left() + x);
            }
        }
    }

    return result;
}

double hullCross(const QPointF &origin, const QPointF &left, const QPointF &right) {
    return (left.x() - origin.x()) * (right.y() - origin.y())
        - (left.y() - origin.y()) * (right.x() - origin.x());
}

QPolygonF convexHull(QPolygonF points) {
    std::sort(points.begin(), points.end(), [](const QPointF &left, const QPointF &right) {
        return left.x() < right.x()
            || (left.x() == right.x() && left.y() < right.y());
    });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) {
        return points;
    }
    QPolygonF lower;
    for (const QPointF &point : points) {
        while (lower.size() >= 2
               && hullCross(lower[lower.size() - 2], lower.back(), point) <= 0.0) {
            lower.pop_back();
        }
        lower.push_back(point);
    }
    QPolygonF upper;
    for (int i = points.size() - 1; i >= 0; --i) {
        const QPointF &point = points[i];
        while (upper.size() >= 2
               && hullCross(upper[upper.size() - 2], upper.back(), point) <= 0.0) {
            upper.pop_back();
        }
        upper.push_back(point);
    }
    lower.pop_back();
    upper.pop_back();
    lower += upper;

    return lower;
}

QVector<int> convexPixels(const QVector<int> &pixels,
                         const QPainterPath &outline,
                         const QSize &imageSize) {
    const QPolygonF hull = convexHull(regionOuterContour(outline));
    if (hull.size() < 3) {
        return {};
    }
    QPainterPath hullPath;
    hullPath.moveTo(hull.front());
    for (int i = 1; i < hull.size(); ++i) {
        hullPath.lineTo(hull[i]);
    }
    hullPath.closeSubpath();
    QImage mask(imageSize, QImage::Format_Grayscale8);
    if (mask.isNull()) {
        return {};
    }
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.fillPath(hullPath, Qt::white);
    painter.end();
    QVector<int> result = pixels;
    const QRect bounds = hullPath.boundingRect().toAlignedRect()
        .intersected(QRect(QPoint(0, 0), imageSize));
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            if (row[x] != 0) {
                result.push_back(y * imageSize.width() + x);
            }
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());

    return result;
}

bool pixelsUseOnlyAllowedGeometry(
    const QVector<int> &pixels,
    const QColor &color,
    const RegionExtractionResult &regions,
    const QVector<SourceRegion> &sources,
    const std::vector<int> &sourceDataAtPixel) {
    const std::vector<std::uint8_t> &lineart = regions.raster->lineart;
    const std::vector<std::uint8_t> &foreground = regions.raster->foreground;
    for (const int pixel : pixels) {
        if (pixel < 0 || pixel >= static_cast<int>(sourceDataAtPixel.size())) {
            return false;
        }
        if (lineart.size() == sourceDataAtPixel.size()
            && foreground.size() == sourceDataAtPixel.size()
            && lineart[static_cast<size_t>(pixel)] != 0
            && foreground[static_cast<size_t>(pixel)] != 0) {
            continue;
        }
        const int source = sourceDataAtPixel[static_cast<size_t>(pixel)];
        if (source < 0
            || !sameColor(color,
                          regions.regions[sources[source].resultIndex].color)) {
            return false;
        }
    }

    return true;
}

bool containsAllPixels(const QVector<int> &candidate, const QVector<int> &required) {
    return std::includes(candidate.cbegin(), candidate.cend(),
                         required.cbegin(), required.cend());
}

SimplifiedGeometry simplifyAllowedGeometry(
    const QVector<int> &pixels,
    const QPainterPath &baseOutline,
    const QColor &color,
    const RegionExtractionResult &regions,
    const QVector<SourceRegion> &sources,
    const std::vector<int> &sourceDataAtPixel) {
    SimplifiedGeometry best;
    best.pixels = pixels;
    best.outline = baseOutline;
    best.pointCount = pathPointCount(best.outline);
    const auto consider = [&](QVector<int> candidatePixels,
                              const QString &kind,
                              int radius,
                              SimplifiedGeometry *selected) {
        if (candidatePixels.isEmpty()) {
            return;
        }
        std::sort(candidatePixels.begin(), candidatePixels.end());
        candidatePixels.erase(
            std::unique(candidatePixels.begin(), candidatePixels.end()),
            candidatePixels.end());
        if (!containsAllPixels(candidatePixels, pixels)
            || !pixelsUseOnlyAllowedGeometry(
                candidatePixels, color, regions, sources, sourceDataAtPixel)) {
            return;
        }
        QPainterPath candidateOutline = tracePixels(
            candidatePixels, regions.imageSize, regions.raster->traceParams);
        const int candidatePointCount = pathPointCount(candidateOutline);
        if (candidateOutline.isEmpty()
            || candidatePointCount > selected->pointCount
            || (candidatePointCount == selected->pointCount
                && candidatePixels.size() >= selected->pixels.size())) {
            return;
        }
        selected->pixels = std::move(candidatePixels);
        selected->outline = std::move(candidateOutline);
        selected->kind = kind;
        selected->radius = radius;
        selected->pointCount = candidatePointCount;
    };
    for (const int radius : kDangerousClosingRadii) {
        consider(morphologicallyClosedPixels(pixels, regions.imageSize, radius),
                 QStringLiteral("morphological-close"), radius, &best);
    }
    consider(convexPixels(pixels, best.outline, regions.imageSize),
             QStringLiteral("convex"), 0, &best);

    return best;
}

quint64 sourcePairKey(int left, int right) {
    if (left > right) {
        std::swap(left, right);
    }

    return (static_cast<quint64>(static_cast<quint32>(left)) << 32)
        | static_cast<quint32>(right);
}

quint64 directedSourcePairKey(int parent, int child) {
    return (static_cast<quint64>(static_cast<quint32>(parent)) << 32)
        | static_cast<quint32>(child);
}

QString operationKindText(PlanOperationKind kind) {
    switch (kind) {
    case PlanOperationKind::GeometrySimplification:
        return QStringLiteral("geometry-simplification");
    case PlanOperationKind::NearbyMerge:
        return QStringLiteral("nearby-merge");
    case PlanOperationKind::AdjacentMerge:
        return QStringLiteral("adjacent-merge");
    case PlanOperationKind::ContainmentMerge:
        return QStringLiteral("containment-merge");
    case PlanOperationKind::Absorption:
        return QStringLiteral("absorption");
    case PlanOperationKind::None:
        return QStringLiteral("none");
    }

    return QStringLiteral("none");
}

int operationRollbackPriority(PlanOperationKind kind) {
    switch (kind) {
    case PlanOperationKind::GeometrySimplification:
        return 4;
    case PlanOperationKind::NearbyMerge:
        return 3;
    case PlanOperationKind::Absorption:
        return 2;
    case PlanOperationKind::ContainmentMerge:
        return 1;
    case PlanOperationKind::AdjacentMerge:
    case PlanOperationKind::None:
        return 0;
    }

    return 0;
}

QVector<NearbyMergeCandidate> findNearbyMergeCandidates(
    const RegionExtractionResult &regions,
    const QVector<SourceRegion> &sources,
    const std::vector<int> &sourceDataAtPixel,
    const std::function<bool()> &cancelled) {
    QHash<quint64, NearbyMergeCandidate> candidateByPair;
    const int width = regions.imageSize.width();
    const int height = regions.imageSize.height();
    const int maximumPixelDistance = kNearbyMergeMaximumGap + 1;

    for (int pixel = 0; pixel < static_cast<int>(sourceDataAtPixel.size()); ++pixel) {
        if ((pixel & 4095) == 0 && planningCancelled(cancelled)) {
            return {};
        }
        const int source = sourceDataAtPixel[static_cast<size_t>(pixel)];
        if (source < 0) {
            continue;
        }
        const int x = pixel % width;
        const int y = pixel / width;
        const int directNeighbors[] = {
            x > 0 ? pixel - 1 : -1,
            x + 1 < width ? pixel + 1 : -1,
            y > 0 ? pixel - width : -1,
            y + 1 < height ? pixel + width : -1,
        };
        const bool boundary = std::any_of(
            std::begin(directNeighbors), std::end(directNeighbors),
            [&](int neighbor) {
                return neighbor < 0
                    || sourceDataAtPixel[static_cast<size_t>(neighbor)] != source;
            });
        if (!boundary) {
            continue;
        }
        for (int offsetY = -maximumPixelDistance;
             offsetY <= maximumPixelDistance; ++offsetY) {
            const int targetY = y + offsetY;
            if (targetY < 0 || targetY >= height) {
                continue;
            }
            const int horizontalLimit = maximumPixelDistance - std::abs(offsetY);
            for (int offsetX = -horizontalLimit; offsetX <= horizontalLimit; ++offsetX) {
                const int distance = std::abs(offsetX) + std::abs(offsetY);
                if (distance < 2) {
                    continue;
                }
                const int targetX = x + offsetX;
                if (targetX < 0 || targetX >= width) {
                    continue;
                }
                const int targetPixel = targetY * width + targetX;
                const int targetSource =
                    sourceDataAtPixel[static_cast<size_t>(targetPixel)];
                if (targetSource <= source
                    || !sameColor(regions.regions[sources[source].resultIndex].color,
                                  regions.regions[sources[targetSource].resultIndex].color)) {
                    continue;
                }
                const quint64 key = sourcePairKey(source, targetSource);
                const int gap = distance - 1;
                const int edgeClearance = std::min({
                    x,
                    y,
                    width - x - 1,
                    height - y - 1,
                    targetX,
                    targetY,
                    width - targetX - 1,
                    height - targetY - 1,
                });
                auto candidate = candidateByPair.find(key);
                if (candidate == candidateByPair.end()
                    || gap < candidate->gap
                    || (gap == candidate->gap
                        && edgeClearance > candidate->edgeClearance)
                    || (gap == candidate->gap
                        && edgeClearance == candidate->edgeClearance
                        && pixel < candidate->leftPixel)
                    || (gap == candidate->gap
                        && edgeClearance == candidate->edgeClearance
                        && pixel == candidate->leftPixel
                        && targetPixel < candidate->rightPixel)) {
                    NearbyMergeCandidate value;
                    value.leftSource = source;
                    value.rightSource = targetSource;
                    value.leftPixel = pixel;
                    value.rightPixel = targetPixel;
                    value.gap = gap;
                    value.edgeClearance = edgeClearance;
                    candidateByPair.insert(key, value);
                }
            }
        }
    }
    QVector<NearbyMergeCandidate> result = candidateByPair.values();
    std::sort(result.begin(), result.end(),
              [](const NearbyMergeCandidate &left, const NearbyMergeCandidate &right) {
                  if (left.gap != right.gap) {
                      return left.gap < right.gap;
                  }
                  if (left.leftSource != right.leftSource) {
                      return left.leftSource < right.leftSource;
                  }
                  return left.rightSource < right.rightSource;
              });

    return result;
}

QVector<int> foregroundBridge(int start,
                              int target,
                              const QSize &imageSize,
                              const std::vector<std::uint8_t> &foreground) {
    const int width = imageSize.width();
    const int height = imageSize.height();
    const QPoint startPoint(start % width, start / width);
    const QPoint targetPoint(target % width, target / width);
    const int directDistance = std::abs(startPoint.x() - targetPoint.x())
        + std::abs(startPoint.y() - targetPoint.y());
    const int maximumSteps = directDistance + kNearbyMergeMaximumDetour;
    const QRect searchBounds = QRect(startPoint, targetPoint).normalized()
        .adjusted(-kNearbyMergeMaximumDetour, -kNearbyMergeMaximumDetour,
                  kNearbyMergeMaximumDetour, kNearbyMergeMaximumDetour)
        .intersected(QRect(0, 0, width, height));
    const int localWidth = searchBounds.width();
    const int localHeight = searchBounds.height();
    QVector<int> parents(localWidth * localHeight, -2);
    QVector<int> distances(localWidth * localHeight, -1);
    QVector<int> queue;
    const auto localIndex = [&](int pixel) {
        const int x = pixel % width - searchBounds.left();
        const int y = pixel / width - searchBounds.top();
        return y * localWidth + x;
    };
    const int startLocal = localIndex(start);
    parents[startLocal] = -1;
    distances[startLocal] = 0;
    queue.push_back(start);
    int queueIndex = 0;
    while (queueIndex < queue.size()) {
        const int pixel = queue[queueIndex++];
        if (pixel == target) {
            break;
        }
        const int distance = distances[localIndex(pixel)];
        if (distance >= maximumSteps) {
            continue;
        }
        const int x = pixel % width;
        const int y = pixel / width;
        const int neighbors[] = {
            x > searchBounds.left() ? pixel - 1 : -1,
            x < searchBounds.right() ? pixel + 1 : -1,
            y > searchBounds.top() ? pixel - width : -1,
            y < searchBounds.bottom() ? pixel + width : -1,
        };
        for (const int neighbor : neighbors) {
            if (neighbor < 0 || foreground[static_cast<size_t>(neighbor)] == 0) {
                continue;
            }
            const int neighborLocal = localIndex(neighbor);
            if (parents[neighborLocal] != -2) {
                continue;
            }
            parents[neighborLocal] = pixel;
            distances[neighborLocal] = distance + 1;
            queue.push_back(neighbor);
        }
    }
    const int targetLocal = localIndex(target);
    if (parents[targetLocal] == -2 || distances[targetLocal] > maximumSteps) {
        return {};
    }
    QVector<int> result;
    for (int pixel = target; pixel >= 0; pixel = parents[localIndex(pixel)]) {
        result.push_back(pixel);
        if (pixel == start) {
            break;
        }
    }
    std::reverse(result.begin(), result.end());

    return result;
}

QVector<int> rasterizedConnectorPixels(const QPainterPath &connector,
                                       const QSize &imageSize);

QVector<NearbyMergeCandidate> findHierarchicalContourMergeCandidates(
    const RegionExtractionResult &regions,
    const QVector<SourceRegion> &sources,
    const std::vector<int> &sourceDataAtPixel,
    const std::vector<std::uint8_t> &lineart,
    const RegionLayerPlanProgress &progress,
    const std::function<bool()> &cancelled) {
    struct EdgeComponent {
        QVector<int> pixels;
        QHash<int, QHash<int, int>> sourcePixelByContactPixel;
    };

    const int width = regions.imageSize.width();
    const int height = regions.imageSize.height();
    const int pixelCount = width * height;
    const std::vector<std::uint8_t> &foreground = regions.raster->foreground;
    if (lineart.size() != static_cast<size_t>(pixelCount)
        || foreground.size() != static_cast<size_t>(pixelCount)) {
        return {};
    }
    QVector<QSet<int>> outerBoundaryPixels(sources.size());
    QPainterPathStroker boundaryStroker;
    boundaryStroker.setWidth(kOuterBoundaryStrokeWidth);
    boundaryStroker.setCapStyle(Qt::RoundCap);
    boundaryStroker.setJoinStyle(Qt::RoundJoin);
    for (int sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const QPainterPath boundary = boundaryStroker.createStroke(
            sources[sourceIndex].outerPath);
        const QVector<int> pixels = rasterizedConnectorPixels(
            boundary, regions.imageSize);
        outerBoundaryPixels[sourceIndex].reserve(pixels.size());
        for (const int pixel : pixels) {
            outerBoundaryPixels[sourceIndex].insert(pixel);
        }
    }
    QVector<int> componentByPixel(pixelCount, -1);
    QVector<EdgeComponent> components;
    QVector<int> queue;
    for (int start = 0; start < pixelCount; ++start) {
        if ((start & 4095) == 0 && planningCancelled(cancelled)) {
            return {};
        }
        if (componentByPixel[start] >= 0
            || lineart[static_cast<size_t>(start)] == 0
            || foreground[static_cast<size_t>(start)] == 0
            || sourceDataAtPixel[static_cast<size_t>(start)] >= 0) {
            continue;
        }
        const int componentIndex = components.size();
        components.push_back(EdgeComponent{});
        EdgeComponent &component = components.back();
        queue.clear();
        queue.push_back(start);
        componentByPixel[start] = componentIndex;
        for (int queueIndex = 0; queueIndex < queue.size(); ++queueIndex) {
            if ((queueIndex & 4095) == 0 && planningCancelled(cancelled)) {
                return {};
            }
            const int pixel = queue[queueIndex];
            component.pixels.push_back(pixel);
            const int x = pixel % width;
            const int y = pixel / width;
            const int neighbors[] = {
                x > 0 ? pixel - 1 : -1,
                x + 1 < width ? pixel + 1 : -1,
                y > 0 ? pixel - width : -1,
                y + 1 < height ? pixel + width : -1,
            };
            for (const int neighbor : neighbors) {
                if (neighbor < 0) {
                    continue;
                }
                const int neighborSource =
                    sourceDataAtPixel[static_cast<size_t>(neighbor)];
                if (neighborSource >= 0) {
                    if (!outerBoundaryPixels[neighborSource].contains(neighbor)) {
                        continue;
                    }
                    QHash<int, int> &sourcePixels =
                        component.sourcePixelByContactPixel[neighborSource];
                    const int existingSourcePixel = sourcePixels.value(pixel, -1);
                    if (existingSourcePixel < 0 || neighbor < existingSourcePixel) {
                        sourcePixels.insert(pixel, neighbor);
                    }
                    continue;
                }
                if (lineart[static_cast<size_t>(neighbor)] != 0
                    && foreground[static_cast<size_t>(neighbor)] != 0
                    && componentByPixel[neighbor] < 0) {
                    componentByPixel[neighbor] = componentIndex;
                    queue.push_back(neighbor);
                }
            }
        }
    }

    QHash<quint64, NearbyMergeCandidate> candidateByPair;
    QVector<int> visitGeneration(pixelCount, 0);
    QVector<int> distances(pixelCount, -1);
    QVector<int> originSourcePixels(pixelCount, -1);
    QVector<int> widthVisitGeneration(pixelCount, 0);
    QVector<int> widthDistances(pixelCount, -1);
    int generation = 0;
    int widthGeneration = 0;
    int processedContactSources = 0;
    int totalContactSources = 0;
    for (const EdgeComponent &component : components) {
        totalContactSources += component.sourcePixelByContactPixel.size();
    }
    if (progress) {
        progress(QStringLiteral("Planning Dangerous contour hierarchy"),
                 0, std::max(1, totalContactSources));
    }
    const auto advanceContactProgress = [&]() {
        ++processedContactSources;
        if (progress) {
            progress(QStringLiteral("Planning Dangerous contour hierarchy"),
                     processedContactSources, std::max(1, totalContactSources));
        }
    };
    for (int componentIndex = 0; componentIndex < components.size(); ++componentIndex) {
        const EdgeComponent &component = components[componentIndex];
        QVector<int> contactSources = component.sourcePixelByContactPixel.keys();
        std::sort(contactSources.begin(), contactSources.end());
        for (int leftPosition = 0; leftPosition < contactSources.size(); ++leftPosition) {
            if (planningCancelled(cancelled)) {
                return {};
            }
            const int leftSource = contactSources[leftPosition];
            bool hasMatchingSource = false;
            for (int rightPosition = leftPosition + 1;
                 rightPosition < contactSources.size(); ++rightPosition) {
                const int rightSource = contactSources[rightPosition];
                if (sameColor(regions.regions[sources[leftSource].resultIndex].color,
                              regions.regions[sources[rightSource].resultIndex].color)) {
                    hasMatchingSource = true;
                    break;
                }
            }
            if (!hasMatchingSource) {
                advanceContactProgress();
                continue;
            }

            ++generation;
            queue.clear();
            const auto leftContact =
                component.sourcePixelByContactPixel.constFind(leftSource);
            const QHash<int, int> &leftContacts = leftContact.value();
            QVector<int> leftContactPixels = leftContacts.keys();
            std::sort(leftContactPixels.begin(), leftContactPixels.end());
            for (const int pixel : leftContactPixels) {
                visitGeneration[pixel] = generation;
                distances[pixel] = 0;
                originSourcePixels[pixel] = leftContacts.value(pixel);
                queue.push_back(pixel);
            }
            for (int queueIndex = 0; queueIndex < queue.size(); ++queueIndex) {
                const int pixel = queue[queueIndex];
                const int x = pixel % width;
                const int y = pixel / width;
                const int neighbors[] = {
                    x > 0 ? pixel - 1 : -1,
                    x + 1 < width ? pixel + 1 : -1,
                    y > 0 ? pixel - width : -1,
                    y + 1 < height ? pixel + width : -1,
                };
                for (const int neighbor : neighbors) {
                    if (neighbor < 0
                        || componentByPixel[neighbor] != componentIndex
                        || visitGeneration[neighbor] == generation) {
                        continue;
                    }
                    visitGeneration[neighbor] = generation;
                    distances[neighbor] = distances[pixel] + 1;
                    originSourcePixels[neighbor] = originSourcePixels[pixel];
                    queue.push_back(neighbor);
                }
            }

            for (int rightPosition = leftPosition + 1;
                 rightPosition < contactSources.size(); ++rightPosition) {
                const int rightSource = contactSources[rightPosition];
                if (!sameColor(regions.regions[sources[leftSource].resultIndex].color,
                               regions.regions[sources[rightSource].resultIndex].color)) {
                    continue;
                }
                const auto rightContact =
                    component.sourcePixelByContactPixel.constFind(rightSource);
                const QHash<int, int> &rightContacts = rightContact.value();
                QVector<int> rightContactPixels = rightContacts.keys();
                std::sort(rightContactPixels.begin(), rightContactPixels.end());
                int targetContactPixel = -1;
                int targetSourcePixel = -1;
                int bestDistance = std::numeric_limits<int>::max();
                for (const int pixel : rightContactPixels) {
                    if (visitGeneration[pixel] == generation
                        && distances[pixel] < bestDistance) {
                        targetContactPixel = pixel;
                        targetSourcePixel = rightContacts.value(pixel);
                        bestDistance = distances[pixel];
                    }
                }
                if (targetContactPixel < 0) {
                    continue;
                }
                ++widthGeneration;
                queue.clear();
                for (const int pixel : rightContactPixels) {
                    widthVisitGeneration[pixel] = widthGeneration;
                    widthDistances[pixel] = 0;
                    queue.push_back(pixel);
                }
                for (int queueIndex = 0; queueIndex < queue.size(); ++queueIndex) {
                    const int pixel = queue[queueIndex];
                    const int x = pixel % width;
                    const int y = pixel / width;
                    const int neighbors[] = {
                        x > 0 ? pixel - 1 : -1,
                        x + 1 < width ? pixel + 1 : -1,
                        y > 0 ? pixel - width : -1,
                        y + 1 < height ? pixel + width : -1,
                    };
                    for (const int neighbor : neighbors) {
                        if (neighbor < 0
                            || componentByPixel[neighbor] != componentIndex
                            || widthVisitGeneration[neighbor] == widthGeneration) {
                            continue;
                        }
                        widthVisitGeneration[neighbor] = widthGeneration;
                        widthDistances[neighbor] = widthDistances[pixel] + 1;
                        queue.push_back(neighbor);
                    }
                }
                NearbyMergeCandidate candidate;
                candidate.leftSource = leftSource;
                candidate.rightSource = rightSource;
                candidate.leftPixel = originSourcePixels[targetContactPixel];
                candidate.rightPixel = targetSourcePixel;
                candidate.gap = bestDistance + 1;
                candidate.hierarchicalContourBridge = true;
                const int separatePixelCount = sources[leftSource].pixels.size()
                    + sources[rightSource].pixels.size();
                const int separatePointCount = pathPointCount(sources[leftSource].outerPath)
                    + pathPointCount(sources[rightSource].outerPath);
                candidate.separateEstimatedShapeCount =
                    estimatedShapeCount(separatePixelCount, separatePointCount);
                const auto considerCorridor = [&](int detourAllowance,
                                                  bool entireComponent) {
                    if (planningCancelled(cancelled)) {
                        return;
                    }
                    QVector<int> corridor;
                    corridor.reserve(component.pixels.size());
                    for (const int pixel : component.pixels) {
                        const bool betweenBothSides =
                            visitGeneration[pixel] == generation
                            && widthVisitGeneration[pixel] == widthGeneration
                            && distances[pixel] + widthDistances[pixel]
                                <= bestDistance + detourAllowance;
                        if (entireComponent || betweenBothSides) {
                            corridor.push_back(pixel);
                        }
                    }
                    QVector<int> mergedPixels = sources[leftSource].pixels;
                    mergedPixels += sources[rightSource].pixels;
                    mergedPixels += corridor;
                    std::sort(mergedPixels.begin(), mergedPixels.end());
                    mergedPixels.erase(
                        std::unique(mergedPixels.begin(), mergedPixels.end()),
                        mergedPixels.end());
                    const QPainterPath outline = tracePixels(
                        mergedPixels, regions.imageSize, regions.raster->traceParams);
                    const int pointCount = pathPointCount(outline);
                    const int shapeCount =
                        estimatedShapeCount(mergedPixels.size(), pointCount);
                    if (outline.isEmpty()
                        || shapeCount >= candidate.separateEstimatedShapeCount
                        || (candidate.bridgeEstimatedShapeCount > 0
                            && shapeCount > candidate.bridgeEstimatedShapeCount)
                        || (candidate.bridgeEstimatedShapeCount > 0
                            && shapeCount == candidate.bridgeEstimatedShapeCount
                            && pointCount > candidate.bridgePointCount)
                        || (candidate.bridgeEstimatedShapeCount > 0
                            && shapeCount == candidate.bridgeEstimatedShapeCount
                            && pointCount == candidate.bridgePointCount
                            && corridor.size() <= candidate.bridgePixels.size() - 2)) {
                        return;
                    }
                    candidate.bridgePixels.clear();
                    candidate.bridgePixels.reserve(corridor.size() + 2);
                    candidate.bridgePixels.push_back(candidate.leftPixel);
                    candidate.bridgePixels += corridor;
                    candidate.bridgePixels.push_back(candidate.rightPixel);
                    candidate.bridgeDetourAllowance =
                        entireComponent ? -1 : detourAllowance;
                    candidate.bridgePointCount = pointCount;
                    candidate.bridgeEstimatedShapeCount = shapeCount;
                    candidate.bridgeWidth = bestDistance > 0
                        ? static_cast<double>(corridor.size()) / bestDistance
                        : static_cast<double>(corridor.size());
                };
                for (const int detourAllowance : kDangerousBridgeDetourAllowances) {
                    considerCorridor(detourAllowance, false);
                }
                considerCorridor(0, true);
                if (candidate.bridgePixels.isEmpty()) {
                    continue;
                }
                const quint64 key = sourcePairKey(leftSource, rightSource);
                auto existing = candidateByPair.find(key);
                if (existing == candidateByPair.end()
                    || candidate.bridgeEstimatedShapeCount
                        < existing->bridgeEstimatedShapeCount
                    || (candidate.bridgeEstimatedShapeCount
                            == existing->bridgeEstimatedShapeCount
                        && candidate.bridgePointCount < existing->bridgePointCount)
                    || (candidate.bridgeEstimatedShapeCount
                            == existing->bridgeEstimatedShapeCount
                        && candidate.bridgePointCount == existing->bridgePointCount
                        && candidate.bridgePixels.size()
                            > existing->bridgePixels.size())) {
                    candidateByPair.insert(key, std::move(candidate));
                }
            }
            advanceContactProgress();
        }
    }
    QVector<NearbyMergeCandidate> candidates = candidateByPair.values();
    std::sort(candidates.begin(), candidates.end(),
              [](const NearbyMergeCandidate &left,
                 const NearbyMergeCandidate &right) {
                  if (left.bridgeEstimatedShapeCount
                      != right.bridgeEstimatedShapeCount) {
                      return left.bridgeEstimatedShapeCount
                          < right.bridgeEstimatedShapeCount;
                  }
                  if (left.bridgePointCount != right.bridgePointCount) {
                      return left.bridgePointCount < right.bridgePointCount;
                  }
                  if (left.gap != right.gap) {
                      return left.gap < right.gap;
                  }
                  if (left.leftSource != right.leftSource) {
                      return left.leftSource < right.leftSource;
                  }
                  return left.rightSource < right.rightSource;
              });
    DisjointSet hierarchy(sources.size());
    QVector<int> levelByRoot(sources.size(), 0);
    QVector<NearbyMergeCandidate> result;
    for (NearbyMergeCandidate &candidate : candidates) {
        const int leftRoot = hierarchy.root(candidate.leftSource);
        const int rightRoot = hierarchy.root(candidate.rightSource);
        if (leftRoot == rightRoot) {
            continue;
        }
        candidate.hierarchyLevel =
            std::max(levelByRoot[leftRoot], levelByRoot[rightRoot]) + 1;
        hierarchy.unite(leftRoot, rightRoot);
        levelByRoot[hierarchy.root(leftRoot)] = candidate.hierarchyLevel;
        result.push_back(std::move(candidate));
    }
    std::sort(result.begin(), result.end(),
              [](const NearbyMergeCandidate &left,
                 const NearbyMergeCandidate &right) {
                  if (left.hierarchyLevel != right.hierarchyLevel) {
                      return left.hierarchyLevel < right.hierarchyLevel;
                  }
                  if (left.bridgePointCount != right.bridgePointCount) {
                      return left.bridgePointCount < right.bridgePointCount;
                  }
                  if (left.bridgeEstimatedShapeCount != right.bridgeEstimatedShapeCount) {
                      return left.bridgeEstimatedShapeCount < right.bridgeEstimatedShapeCount;
                  }
                  return sourcePairKey(left.leftSource, left.rightSource)
                      < sourcePairKey(right.leftSource, right.rightSource);
              });

    return result;
}

QVector<int> rasterizedConnectorPixels(const QPainterPath &connector,
                                       const QSize &imageSize) {
    const QRect imageBounds(QPoint(0, 0), imageSize);
    const QRect bounds = connector.boundingRect().toAlignedRect().adjusted(-1, -1, 1, 1)
        .intersected(imageBounds);
    if (bounds.isEmpty()) {
        return {};
    }
    QImage mask(bounds.size(), QImage::Format_Grayscale8);
    mask.fill(0);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing, false);
    QTransform local;
    local.translate(-bounds.left(), -bounds.top());
    painter.fillPath(local.map(connector), Qt::white);
    painter.end();
    QVector<int> result;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (row[x] != 0) {
                result.push_back((bounds.top() + y) * imageSize.width()
                                 + bounds.left() + x);
            }
        }
    }

    return result;
}

QVector<int> sampledAttachmentContacts(const QHash<int, int> &contacts) {
    QVector<int> pixels = contacts.keys();
    std::sort(pixels.begin(), pixels.end());
    if (pixels.size() <= kMaximumStraightAttachmentContacts) {
        return pixels;
    }
    QVector<int> result;
    result.reserve(kMaximumStraightAttachmentContacts);
    for (int sample = 0; sample < kMaximumStraightAttachmentContacts; ++sample) {
        const int index = sample * (pixels.size() - 1)
            / (kMaximumStraightAttachmentContacts - 1);
        result.push_back(pixels[index]);
    }

    return result;
}

int pixelDistanceSquared(int left, int right, int width) {
    const int horizontal = left % width - right % width;
    const int vertical = left / width - right / width;

    return horizontal * horizontal + vertical * vertical;
}

QPainterPath straightConnectorPath(int leftPixel, int rightPixel, int width) {
    QPainterPath result(QPointF(leftPixel % width + 0.5,
                               leftPixel / width + 0.5));
    result.lineTo(QPointF(rightPixel % width + 0.5,
                          rightPixel / width + 0.5));

    return result;
}

QVector<NearbyMergeCandidate> findStraightHierarchicalContourMergeCandidates(
    const RegionExtractionResult &regions,
    const QVector<SourceRegion> &sources,
    const std::vector<int> &sourceDataAtPixel,
    const std::vector<std::uint8_t> &lineart,
    const RegionLayerPlanProgress &progress,
    const std::function<bool()> &cancelled) {
    struct EdgeComponent {
        QVector<int> pixels;
        QHash<int, QHash<int, int>> sourcePixelByContactPixel;
    };
    struct AttachmentProposal {
        int leftContactPixel = -1;
        int rightContactPixel = -1;
        int distanceSquared = 0;
    };

    const int width = regions.imageSize.width();
    const int height = regions.imageSize.height();
    const int pixelCount = width * height;
    const std::vector<std::uint8_t> &foreground = regions.raster->foreground;

    if (lineart.size() != static_cast<size_t>(pixelCount)
        || foreground.size() != static_cast<size_t>(pixelCount)) {
        return {};
    }
    QVector<QSet<int>> outerBoundaryPixels(sources.size());
    QPainterPathStroker boundaryStroker;
    boundaryStroker.setWidth(kOuterBoundaryStrokeWidth);
    boundaryStroker.setCapStyle(Qt::RoundCap);
    boundaryStroker.setJoinStyle(Qt::RoundJoin);
    for (int sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
        const QVector<int> pixels = rasterizedConnectorPixels(
            boundaryStroker.createStroke(sources[sourceIndex].outerPath),
            regions.imageSize);
        outerBoundaryPixels[sourceIndex].reserve(pixels.size());
        for (const int pixel : pixels) {
            outerBoundaryPixels[sourceIndex].insert(pixel);
        }
    }
    QVector<int> componentByPixel(pixelCount, -1);
    QVector<EdgeComponent> components;
    QVector<int> queue;
    for (int start = 0; start < pixelCount; ++start) {
        if ((start & 4095) == 0 && planningCancelled(cancelled)) {
            return {};
        }
        if (componentByPixel[start] >= 0
            || lineart[static_cast<size_t>(start)] == 0
            || foreground[static_cast<size_t>(start)] == 0
            || sourceDataAtPixel[static_cast<size_t>(start)] >= 0) {
            continue;
        }
        const int componentIndex = components.size();
        components.push_back(EdgeComponent{});
        EdgeComponent &component = components.back();
        queue.clear();
        queue.push_back(start);
        componentByPixel[start] = componentIndex;
        for (int queueIndex = 0; queueIndex < queue.size(); ++queueIndex) {
            const int pixel = queue[queueIndex];
            component.pixels.push_back(pixel);
            const int x = pixel % width;
            const int y = pixel / width;
            const int neighbors[] = {
                x > 0 ? pixel - 1 : -1,
                x + 1 < width ? pixel + 1 : -1,
                y > 0 ? pixel - width : -1,
                y + 1 < height ? pixel + width : -1,
            };
            for (const int neighbor : neighbors) {
                if (neighbor < 0) {
                    continue;
                }
                const int neighborSource =
                    sourceDataAtPixel[static_cast<size_t>(neighbor)];
                if (neighborSource >= 0) {
                    if (!outerBoundaryPixels[neighborSource].contains(neighbor)) {
                        continue;
                    }
                    QHash<int, int> &contacts =
                        component.sourcePixelByContactPixel[neighborSource];
                    const int existing = contacts.value(pixel, -1);
                    if (existing < 0 || neighbor < existing) {
                        contacts.insert(pixel, neighbor);
                    }
                    continue;
                }
                if (lineart[static_cast<size_t>(neighbor)] != 0
                    && foreground[static_cast<size_t>(neighbor)] != 0
                    && componentByPixel[neighbor] < 0) {
                    componentByPixel[neighbor] = componentIndex;
                    queue.push_back(neighbor);
                }
            }
        }
    }
    QVector<NearbyMergeCandidate> candidates;
    int totalContactPairs = 0;
    for (const EdgeComponent &component : components) {
        const QVector<int> contactSources = component.sourcePixelByContactPixel.keys();
        for (int leftPosition = 0; leftPosition < contactSources.size(); ++leftPosition) {
            for (int rightPosition = leftPosition + 1;
                 rightPosition < contactSources.size(); ++rightPosition) {
                if (sameColor(
                        regions.regions[sources[contactSources[leftPosition]].resultIndex].color,
                        regions.regions[sources[contactSources[rightPosition]].resultIndex].color)) {
                    ++totalContactPairs;
                }
            }
        }
    }
    int processedContactPairs = 0;
    if (progress) {
        progress(QStringLiteral("Planning Dangerous contour hierarchy"),
                 0, std::max(1, totalContactPairs));
    }
    for (int componentIndex = 0; componentIndex < components.size(); ++componentIndex) {
        if (planningCancelled(cancelled)) {
            return {};
        }
        const EdgeComponent &component = components[componentIndex];
        QVector<int> contactSources = component.sourcePixelByContactPixel.keys();
        std::sort(contactSources.begin(), contactSources.end());
        for (int leftPosition = 0; leftPosition < contactSources.size(); ++leftPosition) {
            const int leftSource = contactSources[leftPosition];
            const QHash<int, int> &leftContacts =
                component.sourcePixelByContactPixel.value(leftSource);
            const QVector<int> sampledLeft = sampledAttachmentContacts(leftContacts);
            for (int rightPosition = leftPosition + 1;
                 rightPosition < contactSources.size(); ++rightPosition) {
                const int rightSource = contactSources[rightPosition];
                if (!sameColor(regions.regions[sources[leftSource].resultIndex].color,
                               regions.regions[sources[rightSource].resultIndex].color)) {
                    continue;
                }
                const QHash<int, int> &rightContacts =
                    component.sourcePixelByContactPixel.value(rightSource);
                const QVector<int> sampledRight = sampledAttachmentContacts(rightContacts);
                QHash<quint64, AttachmentProposal> proposalByContacts;
                const auto addNearestProposals = [&](const QVector<int> &from,
                                                     const QVector<int> &to,
                                                     bool reverse) {
                    for (const int fromPixel : from) {
                        int nearestPixel = -1;
                        int nearestDistance = std::numeric_limits<int>::max();
                        for (const int toPixel : to) {
                            const int distance = pixelDistanceSquared(
                                fromPixel, toPixel, width);
                            if (distance < nearestDistance) {
                                nearestDistance = distance;
                                nearestPixel = toPixel;
                            }
                        }
                        const int leftPixel = reverse ? nearestPixel : fromPixel;
                        const int rightPixel = reverse ? fromPixel : nearestPixel;
                        if (leftPixel >= 0 && rightPixel >= 0) {
                            proposalByContacts.insert(
                                sourcePairKey(leftPixel, rightPixel),
                                {leftPixel, rightPixel, nearestDistance});
                        }
                    }
                };
                addNearestProposals(sampledLeft, sampledRight, false);
                addNearestProposals(sampledRight, sampledLeft, true);
                QVector<AttachmentProposal> proposals = proposalByContacts.values();
                std::sort(proposals.begin(), proposals.end(),
                          [](const AttachmentProposal &left,
                             const AttachmentProposal &right) {
                              if (left.distanceSquared != right.distanceSquared) {
                                  return left.distanceSquared < right.distanceSquared;
                              }
                              if (left.leftContactPixel != right.leftContactPixel) {
                                  return left.leftContactPixel < right.leftContactPixel;
                              }
                              return left.rightContactPixel < right.rightContactPixel;
                          });
                QVector<NearbyMergeCandidate> selected;
                QVector<QVector<int>> selectedWideCorridors;
                for (const AttachmentProposal &proposal : proposals) {
                    if (planningCancelled(cancelled)) {
                        return {};
                    }
                    if (selected.size() >= kMaximumStraightAttachmentsPerPair) {
                        break;
                    }
                    const bool duplicate = std::any_of(
                        selected.cbegin(), selected.cend(),
                        [&](const NearbyMergeCandidate &candidate) {
                            return pixelDistanceSquared(
                                       proposal.leftContactPixel,
                                       candidate.leftPixel, width)
                                    < kMinimumStraightAttachmentSpacing
                                        * kMinimumStraightAttachmentSpacing
                                && pixelDistanceSquared(
                                       proposal.rightContactPixel,
                                       candidate.rightPixel, width)
                                    < kMinimumStraightAttachmentSpacing
                                        * kMinimumStraightAttachmentSpacing;
                        });
                    if (duplicate) {
                        continue;
                    }
                    const QPainterPath centerline = straightConnectorPath(
                        proposal.leftContactPixel, proposal.rightContactPixel, width);
                    QPainterPathStroker lineStroker;
                    lineStroker.setWidth(1.0);
                    lineStroker.setCapStyle(Qt::RoundCap);
                    const QVector<int> linePixels = rasterizedConnectorPixels(
                        lineStroker.createStroke(centerline), regions.imageSize);
                    const bool straightLineAllowed = !linePixels.isEmpty()
                        && std::all_of(
                            linePixels.cbegin(), linePixels.cend(),
                            [&](int pixel) {
                                return componentByPixel[pixel] == componentIndex
                                    && lineart[static_cast<size_t>(pixel)] != 0
                                    && foreground[static_cast<size_t>(pixel)] != 0
                                    && sourceDataAtPixel[static_cast<size_t>(pixel)] < 0;
                            });
                    if (!straightLineAllowed) {
                        continue;
                    }
                    NearbyMergeCandidate candidate;
                    candidate.leftSource = leftSource;
                    candidate.rightSource = rightSource;
                    candidate.leftPixel = leftContacts.value(proposal.leftContactPixel);
                    candidate.rightPixel = rightContacts.value(proposal.rightContactPixel);
                    candidate.gap = std::max(1, qRound(std::sqrt(
                        static_cast<double>(proposal.distanceSquared))));
                    candidate.hierarchicalContourBridge = true;
                    candidate.straightAttachmentCount = 1;
                    const int separatePixelCount = sources[leftSource].pixels.size()
                        + sources[rightSource].pixels.size();
                    const int separatePointCount = pathPointCount(
                        sources[leftSource].outerPath)
                        + pathPointCount(sources[rightSource].outerPath);
                    candidate.separatePointCount = separatePointCount;
                    candidate.separateEstimatedShapeCount = estimatedShapeCount(
                        separatePixelCount, separatePointCount);
                    QVector<int> widestCorridor;
                    for (const double bridgeWidth : kDangerousStraightBridgeWidths) {
                        if (planningCancelled(cancelled)) {
                            return {};
                        }
                        QPainterPathStroker corridorStroker;
                        corridorStroker.setWidth(bridgeWidth);
                        corridorStroker.setCapStyle(Qt::RoundCap);
                        corridorStroker.setJoinStyle(Qt::RoundJoin);
                        const QVector<int> rasterized = rasterizedConnectorPixels(
                            corridorStroker.createStroke(centerline), regions.imageSize);
                        QVector<int> corridor;
                        corridor.reserve(rasterized.size() + 2);
                        corridor.push_back(candidate.leftPixel);
                        for (const int pixel : rasterized) {
                            if (componentByPixel[pixel] == componentIndex
                                && lineart[static_cast<size_t>(pixel)] != 0
                                && foreground[static_cast<size_t>(pixel)] != 0
                                && sourceDataAtPixel[static_cast<size_t>(pixel)] < 0) {
                                corridor.push_back(pixel);
                            }
                        }
                        corridor.push_back(candidate.rightPixel);
                        if (bridgeWidth == kDangerousStraightBridgeWidths.back()) {
                            widestCorridor = corridor;
                        }
                        QVector<int> mergedPixels = sources[leftSource].pixels;
                        mergedPixels += sources[rightSource].pixels;
                        mergedPixels += corridor;
                        std::sort(mergedPixels.begin(), mergedPixels.end());
                        mergedPixels.erase(
                            std::unique(mergedPixels.begin(), mergedPixels.end()),
                            mergedPixels.end());
                        const QPainterPath outline = tracePixels(
                            mergedPixels, regions.imageSize, regions.raster->traceParams);
                        const int pointCount = pathPointCount(outline);
                        const int shapeCount = estimatedShapeCount(
                            mergedPixels.size(), pointCount);
                        const bool improvesSeparate =
                            pointCount < candidate.separatePointCount
                            || (pointCount == candidate.separatePointCount
                                && shapeCount < candidate.separateEstimatedShapeCount);
                        const bool improvesSelected =
                            candidate.bridgePointCount == 0
                            || pointCount < candidate.bridgePointCount
                            || (pointCount == candidate.bridgePointCount
                                && shapeCount < candidate.bridgeEstimatedShapeCount);
                        if (outline.isEmpty() || !improvesSeparate
                            || !improvesSelected) {
                            continue;
                        }
                        candidate.bridgePixels = std::move(corridor);
                        candidate.bridgePointCount = pointCount;
                        candidate.bridgeEstimatedShapeCount = shapeCount;
                        candidate.bridgeWidth = bridgeWidth;
                    }
                    if (!candidate.bridgePixels.isEmpty()) {
                        selected.push_back(candidate);
                        selectedWideCorridors.push_back(
                            widestCorridor.isEmpty() ? candidate.bridgePixels
                                                     : std::move(widestCorridor));
                        candidates.push_back(std::move(candidate));
                    }
                }
                QVector<int> cumulativeBridge;
                for (int attachmentIndex = 0;
                     attachmentIndex < selected.size(); ++attachmentIndex) {
                    cumulativeBridge += selectedWideCorridors[attachmentIndex];
                    std::sort(cumulativeBridge.begin(), cumulativeBridge.end());
                    cumulativeBridge.erase(
                        std::unique(cumulativeBridge.begin(), cumulativeBridge.end()),
                        cumulativeBridge.end());
                    if (attachmentIndex == 0) {
                        continue;
                    }
                    QVector<int> mergedPixels = sources[leftSource].pixels;
                    mergedPixels += sources[rightSource].pixels;
                    mergedPixels += cumulativeBridge;
                    std::sort(mergedPixels.begin(), mergedPixels.end());
                    mergedPixels.erase(
                        std::unique(mergedPixels.begin(), mergedPixels.end()),
                        mergedPixels.end());
                    const QPainterPath outline = tracePixels(
                        mergedPixels, regions.imageSize, regions.raster->traceParams);
                    const int pointCount = pathPointCount(outline);
                    const int shapeCount = estimatedShapeCount(
                        mergedPixels.size(), pointCount);
                    const bool improvesSeparate =
                        pointCount < selected.front().separatePointCount
                        || (pointCount == selected.front().separatePointCount
                            && shapeCount
                                < selected.front().separateEstimatedShapeCount);
                    if (outline.isEmpty() || !improvesSeparate) {
                        continue;
                    }
                    NearbyMergeCandidate combined = selected.front();
                    combined.bridgePixels = cumulativeBridge;
                    combined.bridgePointCount = pointCount;
                    combined.bridgeEstimatedShapeCount = shapeCount;
                    combined.straightAttachmentCount = attachmentIndex + 1;
                    combined.bridgeWidth = kDangerousStraightBridgeWidths.back();
                    candidates.push_back(std::move(combined));
                }
                ++processedContactPairs;
                if (progress) {
                    progress(QStringLiteral("Planning Dangerous contour hierarchy"),
                             processedContactPairs, std::max(1, totalContactPairs));
                }
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const NearbyMergeCandidate &left,
                 const NearbyMergeCandidate &right) {
                  const int leftSaving = left.separatePointCount
                      - left.bridgePointCount;
                  const int rightSaving = right.separatePointCount
                      - right.bridgePointCount;
                  if (leftSaving != rightSaving) {
                      return leftSaving > rightSaving;
                  }
                  if (left.bridgeEstimatedShapeCount
                      != right.bridgeEstimatedShapeCount) {
                      return left.bridgeEstimatedShapeCount
                          < right.bridgeEstimatedShapeCount;
                  }
                  return sourcePairKey(left.leftSource, left.rightSource)
                      < sourcePairKey(right.leftSource, right.rightSource);
              });
    DisjointSet hierarchy(sources.size());
    QVector<int> levelByRoot(sources.size(), 0);
    QVector<NearbyMergeCandidate> result;
    result.reserve(candidates.size());
    for (NearbyMergeCandidate &candidate : candidates) {
        const int leftRoot = hierarchy.root(candidate.leftSource);
        const int rightRoot = hierarchy.root(candidate.rightSource);
        candidate.hierarchyLevel =
            std::max(levelByRoot[leftRoot], levelByRoot[rightRoot]) + 1;
        if (leftRoot != rightRoot) {
            hierarchy.unite(leftRoot, rightRoot);
            levelByRoot[hierarchy.root(leftRoot)] = candidate.hierarchyLevel;
        }
        result.push_back(std::move(candidate));
    }
    std::sort(result.begin(), result.end(),
              [](const NearbyMergeCandidate &left,
                 const NearbyMergeCandidate &right) {
                  if (left.hierarchyLevel != right.hierarchyLevel) {
                      return left.hierarchyLevel < right.hierarchyLevel;
                  }
                  if (left.bridgeEstimatedShapeCount
                      != right.bridgeEstimatedShapeCount) {
                      return left.bridgeEstimatedShapeCount
                          < right.bridgeEstimatedShapeCount;
                  }
                  return sourcePairKey(left.leftSource, left.rightSource)
                      < sourcePairKey(right.leftSource, right.rightSource);
              });

    return result;
}

RegionLayerPlan fallbackPlan(const RegionExtractionResult &regions,
                             RegionLayerPlan plan,
                             const QString &reason) {
    if (plan.sameColorMergeCount > 0 || plan.absorbedRegionCount > 0
        || plan.morphologicalClosingCount > 0 || plan.convexSimplificationCount > 0
        || plan.dangerousCycleBreakCount > 0) {
        plan.diagnostics << QStringLiteral(
            "fallback_discarded merges=%1 nearby=%2 edge=%3 hierarchical=%4 "
            "absorbed=%5 morphological=%6 convex=%7 point_reductions=%8 "
            "cycle_breaks=%9")
                                .arg(plan.sameColorMergeCount)
                                .arg(plan.nearbySameColorMergeCount)
                                .arg(plan.edgeComponentMergeCount)
                                .arg(plan.hierarchicalContourMergeCount)
                                .arg(plan.absorbedRegionCount)
                                .arg(plan.morphologicalClosingCount)
                                .arg(plan.convexSimplificationCount)
                                .arg(plan.geometryPointReductionCount)
                                .arg(plan.dangerousCycleBreakCount);
    }
    plan.units.clear();
    plan.sameColorMergeCount = 0;
    plan.nearbySameColorMergeCount = 0;
    plan.edgeComponentMergeCount = 0;
    plan.hierarchicalContourMergeCount = 0;
    plan.absorbedRegionCount = 0;
    plan.largeContainedAbsorptionCount = 0;
    plan.morphologicalClosingCount = 0;
    plan.convexSimplificationCount = 0;
    plan.geometryPointReductionCount = 0;
    plan.dangerousCycleBreakCount = 0;
    plan.orderingEdgeCount = 0;
    plan.validationMismatchPixels = 0;
    plan.fallback = true;
    plan.fallbackReason = reason;
    plan.diagnostics << QStringLiteral("fallback reason=%1").arg(reason);
    for (int i = 0; i < regions.regions.size(); ++i) {
        const ExtractedRegion &region = regions.regions[i];
        if (region.lineart) {
            continue;
        }
        RegionLayerUnit unit;
        unit.color = region.color;
        unit.outline = region.outline;
        unit.sourceRegionIndices.push_back(i);
        unit.area = region.area;
        plan.units.push_back(std::move(unit));
    }
    QVector<QPainterPath> outerPaths;
    QVector<QVector<int>> outgoing(plan.units.size());
    QVector<int> indegree(plan.units.size(), 0);
    int fallbackEdges = 0;
    outerPaths.reserve(plan.units.size());
    for (const RegionLayerUnit &unit : plan.units) {
        outerPaths.push_back(outerPath(unit.outline));
    }
    for (int child = 0; child < plan.units.size(); ++child) {
        for (int parent = 0; parent < plan.units.size(); ++parent) {
            if (child != parent
                && strictlyContains(outerPaths[parent], outerPaths[child])) {
                outgoing[parent].push_back(child);
                ++indegree[child];
                ++fallbackEdges;
            }
        }
    }
    QVector<int> order;
    QVector<bool> scheduled(plan.units.size(), false);
    order.reserve(plan.units.size());
    while (order.size() < plan.units.size()) {
        int next = -1;
        for (int i = 0; i < plan.units.size(); ++i) {
            const int sourceIndex = plan.units[i].sourceRegionIndices.front();
            if (indegree[i] == 0 && !scheduled[i]
                && (next < 0
                    || sourceIndex < plan.units[next].sourceRegionIndices.front())) {
                next = i;
            }
        }
        if (next < 0) {
            plan.diagnostics << QStringLiteral("fallback_order=source-order cycle=yes");
            return plan;
        }
        order.push_back(next);
        scheduled[next] = true;
        for (const int child : outgoing[next]) {
            --indegree[child];
        }
    }
    QVector<RegionLayerUnit> orderedUnits;
    orderedUnits.reserve(plan.units.size());
    for (const int index : order) {
        orderedUnits.push_back(std::move(plan.units[index]));
    }
    plan.units = std::move(orderedUnits);
    plan.diagnostics << QStringLiteral("fallback_order=containment ordering_edges=%1")
                            .arg(fallbackEdges);

    return plan;
}

QImage renderUnits(const QSize &imageSize, const QVector<RegionLayerUnit> &units) {
    QImage rendered(imageSize, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const RegionLayerUnit &unit : units) {
        painter.setBrush(unit.color);
        painter.drawPath(outerPath(unit.outline));
    }
    painter.end();

    return rendered;
}

QImage renderOwners(const QSize &imageSize,
                    const QVector<RegionLayerUnit> &units,
                    const QVector<int> &groupByRegion) {
    QImage rendered(imageSize, QImage::Format_ARGB32);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    for (const RegionLayerUnit &unit : units) {
        const int source = unit.sourceRegionIndices.front();
        const int group = groupByRegion[source];
        const QRgb encodedOwner = 0xff000000u | static_cast<QRgb>(group + 1);
        painter.setBrush(QColor::fromRgba(encodedOwner));
        painter.drawPath(outerPath(unit.outline));
    }
    painter.end();

    return rendered;
}

int decodedOwner(QRgb encodedOwner) {
    return qAlpha(encodedOwner) > 0
        ? static_cast<int>(encodedOwner & 0x00ffffffu) - 1 : -1;
}

int validatePlan(const QSize &imageSize,
                 const QVector<RegionLayerUnit> &baselineUnits,
                 const QVector<RegionLayerUnit> &units,
                 const QImage &baselineOwners,
                 const QVector<int> &groupByRegion,
                 QSet<int> &mismatchGroups) {
    const QImage expected = renderUnits(imageSize, baselineUnits);
    const QImage actual = renderUnits(imageSize, units);
    const QImage actualOwners = renderOwners(imageSize, units, groupByRegion);
    int mismatches = 0;
    mismatchGroups.clear();
    for (int y = 0; y < imageSize.height(); ++y) {
        const QRgb *expectedRow = reinterpret_cast<const QRgb *>(expected.constScanLine(y));
        const QRgb *actualRow = reinterpret_cast<const QRgb *>(actual.constScanLine(y));
        for (int x = 0; x < imageSize.width(); ++x) {
            if (expectedRow[x] != actualRow[x]) {
                ++mismatches;
                const int expectedOwner = decodedOwner(baselineOwners.pixel(x, y));
                const int actualOwner = decodedOwner(actualOwners.pixel(x, y));
                if (expectedOwner >= 0) {
                    mismatchGroups.insert(expectedOwner);
                }
                if (actualOwner >= 0) {
                    mismatchGroups.insert(actualOwner);
                }
            }
        }
    }

    return mismatches;
}

QString indicesText(const QVector<int> &indices) {
    QStringList values;
    values.reserve(indices.size());
    for (const int index : indices) {
        values.push_back(QString::number(index));
    }

    return values.join(QLatin1Char(','));
}

QVector<int> cyclicDependencyComponent(const QVector<QVector<int>> &outgoing,
                                       const QVector<bool> &scheduled) {
    QVector<int> indexes(outgoing.size(), -1);
    QVector<int> lowLinks(outgoing.size(), -1);
    QVector<int> stack;
    QVector<bool> onStack(outgoing.size(), false);
    QVector<QVector<int>> cyclicComponents;
    int nextIndex = 0;
    std::function<void(int)> visit;
    visit = [&](int node) {
        indexes[node] = nextIndex;
        lowLinks[node] = nextIndex;
        ++nextIndex;
        stack.push_back(node);
        onStack[node] = true;
        for (const int next : outgoing[node]) {
            if (scheduled[next]) {
                continue;
            }
            if (indexes[next] < 0) {
                visit(next);
                lowLinks[node] = std::min(lowLinks[node], lowLinks[next]);
            } else if (onStack[next]) {
                lowLinks[node] = std::min(lowLinks[node], indexes[next]);
            }
        }
        if (lowLinks[node] != indexes[node]) {
            return;
        }
        QVector<int> component;
        while (!stack.isEmpty()) {
            const int member = stack.takeLast();
            onStack[member] = false;
            component.push_back(member);
            if (member == node) {
                break;
            }
        }
        const bool selfCycle = component.size() == 1
            && outgoing[component.front()].contains(component.front());
        if (component.size() > 1 || selfCycle) {
            std::sort(component.begin(), component.end());
            cyclicComponents.push_back(std::move(component));
        }
    };
    for (int node = 0; node < outgoing.size(); ++node) {
        if (!scheduled[node] && indexes[node] < 0) {
            visit(node);
        }
    }
    if (cyclicComponents.isEmpty()) {
        return {};
    }
    std::sort(cyclicComponents.begin(), cyclicComponents.end(),
              [](const QVector<int> &left, const QVector<int> &right) {
                  return left.front() < right.front();
              });

    return cyclicComponents.front();
}

PlanOperation rollbackOperationForGroups(const QVector<PlanOperation> &operations,
                                         const QVector<int> &groupByRegion,
                                         const QSet<int> &affectedGroups) {
    PlanOperation selected;
    for (const PlanOperation &operation : operations) {
        if (operation.leftSourceIndex < 0
            || operation.leftSourceIndex >= groupByRegion.size()
            || operation.rightSourceIndex < 0
            || operation.rightSourceIndex >= groupByRegion.size()) {
            continue;
        }
        const int leftGroup = groupByRegion[operation.leftSourceIndex];
        const int rightGroup = groupByRegion[operation.rightSourceIndex];
        if (!affectedGroups.contains(leftGroup)
            && !affectedGroups.contains(rightGroup)) {
            continue;
        }
        const int selectedPriority = operationRollbackPriority(selected.kind);
        const int candidatePriority = operationRollbackPriority(operation.kind);
        const bool better = selected.kind == PlanOperationKind::None
            || candidatePriority > selectedPriority
            || (candidatePriority == selectedPriority
                && operation.foreignOwnerCount > selected.foreignOwnerCount)
            || (candidatePriority == selectedPriority
                && operation.foreignOwnerCount == selected.foreignOwnerCount
                && operation.gap > selected.gap)
            || (candidatePriority == selectedPriority
                && operation.foreignOwnerCount == selected.foreignOwnerCount
                && operation.gap == selected.gap && operation.key < selected.key);
        if (better) {
            selected = operation;
        }
    }

    return selected;
}

QString affectedGroupsText(const QSet<int> &affectedGroups,
                           const QVector<LayerGroup> &groups) {
    QVector<int> sortedGroups = affectedGroups.values();
    std::sort(sortedGroups.begin(), sortedGroups.end());
    QStringList groupValues;
    QStringList sourceValues;
    for (const int group : sortedGroups) {
        groupValues.push_back(QString::number(group));
        for (const int source : groups[group].sourceRegionIndices) {
            sourceValues.push_back(QString::number(source));
        }
    }

    return QStringLiteral("groups=%1 sources=%2")
        .arg(groupValues.join(QLatin1Char(',')), sourceValues.join(QLatin1Char(',')));
}

} // namespace

namespace {

RegionLayerPlan buildRegionLayerPlanAttempt(
    const RegionExtractionResult &regions,
    const PlannerSuppressions &suppressions,
    const PlannerOptions &options,
    PlannerCandidateCache &candidateCache,
    RecoveryAction &recovery,
    const RegionLayerPlanProgress &progress,
    const std::function<bool()> &cancelled) {
    RegionLayerPlan plan;
    QVector<SourceRegion> sources;
    recovery = {};
    for (int i = 0; i < regions.regions.size(); ++i) {
        if (planningCancelled(cancelled)) {
            return cancelledPlan(std::move(plan));
        }
        const ExtractedRegion &region = regions.regions[i];
        if (region.lineart) {
            continue;
        }
        SourceRegion source;
        source.outerPath = outerPath(region.outline);
        source.resultIndex = i;
        source.label = region.id;
        sources.push_back(std::move(source));
    }
    plan.inputRegionCount = sources.size();
    plan.diagnostics << QStringLiteral("source_regions=%1 raster=%2")
                            .arg(plan.inputRegionCount)
                            .arg(regions.raster ? QStringLiteral("yes")
                                                : QStringLiteral("no"));
    if (sources.isEmpty()) {
        return plan;
    }
    if (!regions.raster
        || regions.raster->labels.size()
            != static_cast<size_t>(regions.imageSize.width()) * regions.imageSize.height()) {
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("cleaned raster labels are unavailable"));
    }

    int maximumLabel = -1;
    for (const int label : regions.raster->labels) {
        maximumLabel = std::max(maximumLabel, label);
    }
    for (const SourceRegion &source : sources) {
        maximumLabel = std::max(maximumLabel, source.label);
    }
    QVector<int> sourceDataByLabel(maximumLabel + 1, -1);
    for (int i = 0; i < sources.size(); ++i) {
        const int label = sources[i].label;
        if (label < 0 || sourceDataByLabel[label] >= 0 || sources[i].outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("source labels or contours are invalid"));
        }
        sourceDataByLabel[label] = i;
    }
    const std::vector<int> &labels = regions.raster->labels;
    std::vector<std::uint8_t> derivedForeground;
    const std::vector<std::uint8_t> *foreground = &regions.raster->foreground;
    if (foreground->size() != labels.size()) {
        derivedForeground.resize(labels.size(), 0);
        for (size_t pixel = 0; pixel < labels.size(); ++pixel) {
            derivedForeground[pixel] = labels[pixel] >= 0 ? 1 : 0;
        }
        foreground = &derivedForeground;
    }
    std::vector<int> sourceDataAtPixel(labels.size(), -1);
    for (int pixel = 0; pixel < static_cast<int>(labels.size()); ++pixel) {
        if ((pixel & 4095) == 0 && planningCancelled(cancelled)) {
            return cancelledPlan(std::move(plan));
        }
        const int label = labels[static_cast<size_t>(pixel)];
        if (label >= 0 && label < sourceDataByLabel.size()) {
            const int sourceDataIndex = sourceDataByLabel[label];
            if (sourceDataIndex >= 0) {
                sources[sourceDataIndex].pixels.push_back(pixel);
                sourceDataAtPixel[static_cast<size_t>(pixel)] = sourceDataIndex;
            }
        }
    }
    if (std::any_of(sources.cbegin(), sources.cend(),
                    [](const SourceRegion &source) { return source.pixels.isEmpty(); })) {
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("an extracted source has no raster ownership"));
    }

    DisjointSet sets(sources.size());
    QVector<PlanOperation> acceptedOperations;
    QSet<quint64> adjacentSourcePairs;
    QSet<quint64> rejectedAdjacentPairs;
    const int width = regions.imageSize.width();
    const int height = regions.imageSize.height();
    for (int y = 0; y < height; ++y) {
        if (planningCancelled(cancelled)) {
            return cancelledPlan(std::move(plan));
        }
        for (int x = 0; x < width; ++x) {
            const int pixel = y * width + x;
            const int label = labels[static_cast<size_t>(pixel)];
            const int source = label >= 0 && label < sourceDataByLabel.size()
                ? sourceDataByLabel[label] : -1;
            if (source < 0) {
                continue;
            }
            const int neighbors[] = {
                x + 1 < width ? pixel + 1 : -1,
                y + 1 < height ? pixel + width : -1,
            };
            for (const int neighborPixel : neighbors) {
                if (neighborPixel < 0) {
                    continue;
                }
                const int neighborLabel = labels[static_cast<size_t>(neighborPixel)];
                const int neighbor = neighborLabel >= 0
                        && neighborLabel < sourceDataByLabel.size()
                    ? sourceDataByLabel[neighborLabel] : -1;
                if (neighbor < 0 || neighbor == source
                    || !sameColor(regions.regions[sources[source].resultIndex].color,
                                  regions.regions[sources[neighbor].resultIndex].color)) {
                    continue;
                }
                const int sourceIndex = sources[source].resultIndex;
                const int neighborSourceIndex = sources[neighbor].resultIndex;
                const quint64 pairKey = sourcePairKey(sourceIndex, neighborSourceIndex);
                adjacentSourcePairs.insert(pairKey);
                if (suppressions.adjacentPairs.contains(pairKey)) {
                    if (!rejectedAdjacentPairs.contains(pairKey)) {
                        rejectedAdjacentPairs.insert(pairKey);
                        ++plan.adjacentConflictRejectCount;
                    }
                    continue;
                }
                if (sets.unite(source, neighbor)) {
                    ++plan.sameColorMergeCount;
                    acceptedOperations.push_back({PlanOperationKind::AdjacentMerge, pairKey,
                                                  sourceIndex, neighborSourceIndex});
                }
            }
        }
    }

    QVector<NearbyMergeCandidate> nearbyCandidates;
    int hierarchicalCandidates = candidateCache.hierarchicalCandidateCount;
    if (options.hierarchicalContourMerges) {
        plan.diagnostics << QStringLiteral(
            "hierarchical_route_policy=outer-boundary edge-only "
            "foreground-only straight_visibility=required "
            "color_crossings=forbidden transparency_crossings=forbidden "
            "expanded_region_crossings=forbidden");
    }
    if (candidateCache.ready) {
        nearbyCandidates = candidateCache.candidates;
        plan.diagnostics << QStringLiteral("nearby_candidate_cache=hit candidates=%1")
                                .arg(nearbyCandidates.size());
    } else {
        nearbyCandidates = findNearbyMergeCandidates(
            regions, sources, sourceDataAtPixel, cancelled);
        if (planningCancelled(cancelled)) {
            return cancelledPlan(std::move(plan));
        }
        if (options.hierarchicalContourMerges) {
            if (progress) {
                progress(QStringLiteral("Finding Dangerous contour neighbours"), 0, 0);
            }
            QVector<NearbyMergeCandidate> hierarchyCandidates =
                findStraightHierarchicalContourMergeCandidates(
                    regions, sources, sourceDataAtPixel,
                    regions.raster->lineart, progress, cancelled);
            if (planningCancelled(cancelled)) {
                return cancelledPlan(std::move(plan));
            }
            hierarchicalCandidates = hierarchyCandidates.size();
            nearbyCandidates += std::move(hierarchyCandidates);
            std::sort(nearbyCandidates.begin(), nearbyCandidates.end(),
                      [](const NearbyMergeCandidate &left,
                         const NearbyMergeCandidate &right) {
                          if (left.hierarchicalContourBridge
                              != right.hierarchicalContourBridge) {
                              return left.hierarchicalContourBridge;
                          }
                          if (left.hierarchicalContourBridge
                              && right.hierarchicalContourBridge
                              && left.hierarchyLevel != right.hierarchyLevel) {
                              return left.hierarchyLevel < right.hierarchyLevel;
                          }
                          if (left.gap != right.gap) {
                              return left.gap < right.gap;
                          }
                          if (left.leftSource != right.leftSource) {
                              return left.leftSource < right.leftSource;
                          }
                          return left.rightSource < right.rightSource;
                      });
        }
        candidateCache.candidates = nearbyCandidates;
        candidateCache.hierarchicalCandidateCount = hierarchicalCandidates;
        candidateCache.ready = true;
        plan.diagnostics << QStringLiteral("nearby_candidate_cache=miss candidates=%1")
                                .arg(nearbyCandidates.size());
    }
    QVector<NearbyMergeBridge> nearbyBridges;
    QVector<QVector<int>> expandedPixelsByRoot(sources.size());
    QVector<int> expandedPointCountByRoot(sources.size(), -1);
    QVector<int> expandedShapeCountByRoot(sources.size(), -1);
    QVector<int> expandedOwnerSource(sourceDataAtPixel.size(), -1);
    for (int source = 0; source < sources.size(); ++source) {
        expandedPixelsByRoot[sets.root(source)] += sources[source].pixels;
    }
    const auto normalizePixels = [](QVector<int> *pixels) {
        std::sort(pixels->begin(), pixels->end());
        pixels->erase(std::unique(pixels->begin(), pixels->end()), pixels->end());
    };
    for (QVector<int> &pixels : expandedPixelsByRoot) {
        normalizePixels(&pixels);
    }
    const auto estimateExpandedPixels = [&](const QVector<int> &pixels) {
        const QPainterPath outline = tracePixels(
            pixels, regions.imageSize, regions.raster->traceParams);
        const int pointCount = pathPointCount(outline);

        return QPair<int, int>(estimatedShapeCount(pixels.size(), pointCount),
                               pointCount);
    };
    const auto expandedCost = [&](int root) {
        if (expandedShapeCountByRoot[root] < 0) {
            const QPair<int, int> cost = estimateExpandedPixels(
                expandedPixelsByRoot[root]);
            expandedShapeCountByRoot[root] = cost.first;
            expandedPointCountByRoot[root] = cost.second;
        }

        return QPair<int, int>(expandedShapeCountByRoot[root],
                               expandedPointCountByRoot[root]);
    };
    int alreadyConnectedCandidates = 0;
    int adjacentPairCandidates = 0;
    int rejectedForegroundCandidates = 0;
    int rejectedForeignOwnerCandidates = 0;
    int rejectedExpandedCollisionCandidates = 0;
    int rejectedExpandedCostCandidates = 0;
    int additionalAttachmentCount = 0;
    for (const NearbyMergeCandidate &candidate : nearbyCandidates) {
        if (planningCancelled(cancelled)) {
            return cancelledPlan(std::move(plan));
        }
        const int leftSourceIndex = sources[candidate.leftSource].resultIndex;
        const int rightSourceIndex = sources[candidate.rightSource].resultIndex;
        const quint64 pairKey = sourcePairKey(leftSourceIndex, rightSourceIndex);
        if (adjacentSourcePairs.contains(pairKey)) {
            ++adjacentPairCandidates;
            continue;
        }
        if (suppressions.nearbyPairs.contains(pairKey)) {
            ++plan.nearbyConflictRejectCount;
            plan.diagnostics << QStringLiteral("nearby rejected=%1 "
                                               "sources=%2,%3 gap=%4")
                                    .arg(QStringLiteral("suppressed-operation"))
                                    .arg(sources[candidate.leftSource].resultIndex)
                                    .arg(sources[candidate.rightSource].resultIndex)
                                    .arg(candidate.gap);
            continue;
        }
        const int leftRoot = sets.root(candidate.leftSource);
        const int rightRoot = sets.root(candidate.rightSource);
        const bool alreadyConnected = leftRoot == rightRoot;
        if (alreadyConnected && !candidate.hierarchicalContourBridge) {
            ++alreadyConnectedCandidates;
            continue;
        }
        QVector<int> bridge = candidate.bridgePixels.isEmpty()
            ? foregroundBridge(candidate.leftPixel,
                               candidate.rightPixel,
                               regions.imageSize,
                               *foreground)
            : candidate.bridgePixels;
        if (bridge.isEmpty()) {
            ++rejectedForegroundCandidates;
            continue;
        }
        const bool expandedCollision = std::any_of(
            bridge.cbegin(), bridge.cend(),
            [&](int pixel) {
                const int ownerSource = expandedOwnerSource[pixel];
                if (ownerSource < 0) {
                    return false;
                }
                const int ownerRoot = sets.root(ownerSource);

                return ownerRoot != leftRoot && ownerRoot != rightRoot;
            });
        if (expandedCollision) {
            ++rejectedExpandedCollisionCandidates;
            plan.diagnostics << QStringLiteral(
                "nearby rejected=expanded-region-collision sources=%1,%2 gap=%3")
                                    .arg(leftSourceIndex)
                                    .arg(rightSourceIndex)
                                    .arg(candidate.gap);
            continue;
        }
        QSet<int> foreignOwners;
        for (const int bridgePixel : bridge) {
            const int bridgeSource = sourceDataAtPixel[static_cast<size_t>(bridgePixel)];
            if (bridgeSource >= 0
                && !sameColor(regions.regions[leftSourceIndex].color,
                              regions.regions[sources[bridgeSource].resultIndex].color)) {
                foreignOwners.insert(sources[bridgeSource].resultIndex);
            }
        }
        if (foreignOwners.size() > options.maximumForeignOwners) {
            QVector<int> foreignOwnerValues(foreignOwners.cbegin(), foreignOwners.cend());
            std::sort(foreignOwnerValues.begin(), foreignOwnerValues.end());
            ++plan.nearbyConflictRejectCount;
            ++plan.nearbyForeignOwnerRejectCount;
            ++rejectedForeignOwnerCandidates;
            plan.diagnostics << QStringLiteral("nearby rejected=foreign-owner-conflict "
                                               "sources=%1,%2 gap=%3 foreign_owners=%4")
                                    .arg(leftSourceIndex)
                                    .arg(rightSourceIndex)
                                    .arg(candidate.gap)
                                    .arg(indicesText(foreignOwnerValues));
            continue;
        }
        QVector<int> expandedPixels = expandedPixelsByRoot[leftRoot];
        if (!alreadyConnected) {
            expandedPixels += expandedPixelsByRoot[rightRoot];
        }
        expandedPixels += bridge;
        normalizePixels(&expandedPixels);
        QPair<int, int> expandedBefore;
        QPair<int, int> expandedAfter;
        if (candidate.hierarchicalContourBridge) {
            const QPair<int, int> leftCost = expandedCost(leftRoot);
            if (alreadyConnected) {
                expandedBefore = leftCost;
            } else {
                const QPair<int, int> rightCost = expandedCost(rightRoot);
                expandedBefore = {leftCost.first + rightCost.first,
                                  leftCost.second + rightCost.second};
            }
            expandedAfter = estimateExpandedPixels(expandedPixels);
            const bool lowersCost = expandedAfter.second < expandedBefore.second
                || (expandedAfter.second == expandedBefore.second
                    && expandedAfter.first < expandedBefore.first);
            if (!lowersCost) {
                ++rejectedExpandedCostCandidates;
                plan.diagnostics << QStringLiteral(
                    "nearby rejected=expanded-cost sources=%1,%2 gap=%3 "
                    "estimated_shapes=%4->%5 path_points=%6->%7")
                                        .arg(leftSourceIndex)
                                        .arg(rightSourceIndex)
                                        .arg(candidate.gap)
                                        .arg(expandedBefore.first)
                                        .arg(expandedAfter.first)
                                        .arg(expandedBefore.second)
                                        .arg(expandedAfter.second);
                continue;
            }
        }
        const bool joined = !alreadyConnected
            && sets.unite(candidate.leftSource, candidate.rightSource);
        if (joined) {
            ++plan.sameColorMergeCount;
            ++plan.nearbySameColorMergeCount;
            if (candidate.edgeComponentBridge) {
                ++plan.edgeComponentMergeCount;
            }
            if (candidate.hierarchicalContourBridge) {
                ++plan.hierarchicalContourMergeCount;
            }
            acceptedOperations.push_back({PlanOperationKind::NearbyMerge, pairKey,
                                          leftSourceIndex, rightSourceIndex,
                                          candidate.gap,
                                           static_cast<int>(foreignOwners.size())});
        } else if (candidate.hierarchicalContourBridge) {
            ++additionalAttachmentCount;
            ++plan.hierarchicalContourMergeCount;
        }
        for (const int bridgePixel : bridge) {
            const int bridgeSource = sourceDataAtPixel[static_cast<size_t>(bridgePixel)];
            if (bridgeSource >= 0
                && sameColor(regions.regions[sources[candidate.leftSource].resultIndex].color,
                             regions.regions[sources[bridgeSource].resultIndex].color)
                && sets.unite(candidate.leftSource, bridgeSource)) {
                ++plan.sameColorMergeCount;
                ++plan.nearbySameColorMergeCount;
            }
        }
        const int expandedRoot = sets.root(candidate.leftSource);
        expandedPixelsByRoot[leftRoot].clear();
        expandedPixelsByRoot[rightRoot].clear();
        expandedPixelsByRoot[expandedRoot] = std::move(expandedPixels);
        expandedShapeCountByRoot[leftRoot] = -1;
        expandedShapeCountByRoot[rightRoot] = -1;
        if (candidate.hierarchicalContourBridge) {
            expandedShapeCountByRoot[expandedRoot] = expandedAfter.first;
            expandedPointCountByRoot[expandedRoot] = expandedAfter.second;
        }
        for (const int bridgePixel : bridge) {
            expandedOwnerSource[bridgePixel] = candidate.leftSource;
        }
        NearbyMergeBridge accepted;
        accepted.pixels = std::move(bridge);
        accepted.leftSource = candidate.leftSource;
        nearbyBridges.push_back(std::move(accepted));
        plan.diagnostics << QStringLiteral("nearby color=%1 sources=%2,%3 gap=%4 "
                                           "bridge_steps=%5 detour=%6 foreign_owners=%7 "
                                           "bridge_kind=%8 bridge_fill=%9 "
                                           "bridge_detour_allowance=%10 "
                                           "bridge_path_points=%11 "
                                           "estimated_shapes=%12->%13 "
                                           "hierarchy_level=%14 outer_boundary_contacts=yes "
                                           "straight_visibility=%15 bridge_width=%16 "
                                           "straight_attachments=%17 "
                                           "additional_attachment=%18 expanded_shapes=%19->%20 "
                                           "expanded_points=%21->%22")
                                .arg(regions.regions[sources[candidate.leftSource]
                                                        .resultIndex].color.name(QColor::HexRgb))
                                .arg(sources[candidate.leftSource].resultIndex)
                                .arg(sources[candidate.rightSource].resultIndex)
                                .arg(candidate.gap)
                                .arg(nearbyBridges.back().pixels.size() - 1)
                                .arg(nearbyBridges.back().pixels.size()
                                     - candidate.gap - 2)
                                .arg(foreignOwners.size())
                                .arg(candidate.hierarchicalContourBridge
                                         ? QStringLiteral("hierarchical-contour")
                                         : (candidate.edgeComponentBridge
                                                ? QStringLiteral("edge-component")
                                                : QStringLiteral("bounded")))
                                .arg(candidate.hierarchicalContourBridge
                                         ? QStringLiteral("straight-edge-corridor")
                                         : (candidate.edgeComponentBridge
                                                ? QStringLiteral("two-sided")
                                                : QStringLiteral("path")))
                                .arg(candidate.bridgeDetourAllowance)
                                .arg(candidate.bridgePointCount)
                                .arg(candidate.separateEstimatedShapeCount)
                                .arg(candidate.bridgeEstimatedShapeCount)
                                .arg(candidate.hierarchyLevel)
                                .arg(candidate.hierarchicalContourBridge
                                         ? QStringLiteral("yes")
                                         : QStringLiteral("n/a"))
                                .arg(candidate.bridgeWidth, 0, 'f', 2)
                                .arg(candidate.straightAttachmentCount)
                                .arg(alreadyConnected ? QStringLiteral("yes")
                                                      : QStringLiteral("no"))
                                .arg(candidate.hierarchicalContourBridge
                                         ? expandedBefore.first : 0)
                                .arg(candidate.hierarchicalContourBridge
                                         ? expandedAfter.first : 0)
                                .arg(candidate.hierarchicalContourBridge
                                         ? expandedBefore.second : 0)
                                .arg(candidate.hierarchicalContourBridge
                                         ? expandedAfter.second : 0);
    }
    plan.diagnostics << QStringLiteral("nearby_candidates=%1 accepted_bridges=%2 "
                                       "conflict_rejections=%3 already_connected=%4 "
                                       "adjacent_pair_skips=%5 no_foreground_path=%6 "
                                       "foreign_owner_conflicts=%7 "
                                       "hierarchical_candidates=%8 "
                                       "expanded_collision_rejections=%9 "
                                       "expanded_cost_rejections=%10 "
                                       "additional_attachments=%11")
                            .arg(nearbyCandidates.size())
                            .arg(nearbyBridges.size())
                            .arg(plan.nearbyConflictRejectCount)
                            .arg(alreadyConnectedCandidates)
                            .arg(adjacentPairCandidates)
                            .arg(rejectedForegroundCandidates)
                            .arg(rejectedForeignOwnerCandidates)
                            .arg(hierarchicalCandidates)
                            .arg(rejectedExpandedCollisionCandidates)
                            .arg(rejectedExpandedCostCandidates)
                            .arg(additionalAttachmentCount);
    QVector<int> parentBySource(sources.size(), -1);
    for (int child = 0; child < sources.size(); ++child) {
        double bestSize = std::numeric_limits<double>::max();
        for (int parent = 0; parent < sources.size(); ++parent) {
            if (child == parent
                || !strictlyContains(sources[parent].outerPath, sources[child].outerPath)) {
                continue;
            }
            const double size = enclosureSize(sources[parent].outerPath);
            if (size < bestSize) {
                parentBySource[child] = parent;
                bestSize = size;
            }
        }
        const int parent = parentBySource[child];
        if (parent < 0
            || !sameColor(regions.regions[sources[child].resultIndex].color,
                          regions.regions[sources[parent].resultIndex].color)) {
            continue;
        }
        const int childSourceIndex = sources[child].resultIndex;
        const int parentSourceIndex = sources[parent].resultIndex;
        const quint64 pairKey = sourcePairKey(childSourceIndex, parentSourceIndex);
        if (suppressions.containmentPairs.contains(pairKey)) {
            ++plan.containmentConflictRejectCount;
            continue;
        }
        if (sets.unite(child, parent)) {
            ++plan.sameColorMergeCount;
            acceptedOperations.push_back({PlanOperationKind::ContainmentMerge, pairKey,
                                          childSourceIndex, parentSourceIndex});
        }
    }

    QHash<int, int> groupByRoot;
    QVector<LayerGroup> groups;
    QVector<int> groupBySourceData(sources.size(), -1);
    for (int i = 0; i < sources.size(); ++i) {
        const int root = sets.root(i);
        int groupIndex = groupByRoot.value(root, -1);
        if (groupIndex < 0) {
            groupIndex = groups.size();
            groupByRoot.insert(root, groupIndex);
            groups.push_back(LayerGroup{});
        }
        LayerGroup &group = groups[groupIndex];
        groupBySourceData[i] = groupIndex;
        const ExtractedRegion &region = regions.regions[sources[i].resultIndex];
        if (!group.color.isValid()) {
            group.color = region.color;
        }
        group.sourceRegionIndices.push_back(sources[i].resultIndex);
        group.pixels += sources[i].pixels;
        group.sourceArea += sources[i].pixels.size();
        if (group.firstSourceIndex < 0 || sources[i].resultIndex < group.firstSourceIndex) {
            group.firstSourceIndex = sources[i].resultIndex;
        }
    }
    for (const NearbyMergeBridge &bridge : nearbyBridges) {
        const int groupIndex = groupBySourceData[bridge.leftSource];
        groups[groupIndex].pixels += bridge.pixels;
    }
    for (LayerGroup &group : groups) {
        if (planningCancelled(cancelled)) {
            return cancelledPlan(std::move(plan));
        }
        std::sort(group.sourceRegionIndices.begin(), group.sourceRegionIndices.end());
        std::sort(group.pixels.begin(), group.pixels.end());
        group.pixels.erase(std::unique(group.pixels.begin(), group.pixels.end()),
                           group.pixels.end());
        group.fillArea = group.pixels.size();
        if (group.sourceRegionIndices.size() == 1) {
            group.outline = regions.regions[group.sourceRegionIndices.front()].outline;
        } else {
            group.outline = tracePixels(group.pixels, regions.imageSize,
                                        regions.raster->traceParams);
        }
        const bool geometrySuppressed = std::any_of(
            group.sourceRegionIndices.cbegin(), group.sourceRegionIndices.cend(),
            [&](int sourceIndex) {
                return suppressions.geometrySources.contains(sourceIndex);
            });
        if (options.simplifyAllowedGeometry && !geometrySuppressed) {
            const int originalPointCount = pathPointCount(group.outline);
            SimplifiedGeometry simplified = simplifyAllowedGeometry(
                group.pixels, group.outline, group.color,
                regions, sources, sourceDataAtPixel);
            if (simplified.kind != QStringLiteral("base")) {
                const int addedPixels = simplified.pixels.size() - group.pixels.size();
                plan.geometryPointReductionCount +=
                    originalPointCount - simplified.pointCount;
                if (simplified.kind == QStringLiteral("morphological-close")) {
                    ++plan.morphologicalClosingCount;
                } else if (simplified.kind == QStringLiteral("convex")) {
                    ++plan.convexSimplificationCount;
                }
                plan.diagnostics << QStringLiteral(
                    "dangerous_geometry color=%1 sources=%2 kind=%3 radius=%4 "
                    "path_points=%5->%6 added_pixels=%7")
                                        .arg(group.color.name(QColor::HexRgb))
                                        .arg(indicesText(group.sourceRegionIndices))
                                        .arg(simplified.kind)
                                        .arg(simplified.radius)
                                        .arg(originalPointCount)
                                        .arg(simplified.pointCount)
                                        .arg(addedPixels);
                group.pixels = std::move(simplified.pixels);
                group.outline = std::move(simplified.outline);
                group.fillArea = group.pixels.size();
                PlanOperation geometryOperation;
                geometryOperation.kind = PlanOperationKind::GeometrySimplification;
                geometryOperation.key = static_cast<quint64>(group.firstSourceIndex);
                geometryOperation.leftSourceIndex = group.firstSourceIndex;
                geometryOperation.rightSourceIndex = group.firstSourceIndex;
                geometryOperation.relatedSourceIndices = group.sourceRegionIndices;
                acceptedOperations.push_back(std::move(geometryOperation));
            }
        } else if (geometrySuppressed) {
            plan.diagnostics << QStringLiteral(
                "dangerous_geometry suppressed sources=%1")
                                    .arg(indicesText(group.sourceRegionIndices));
        }
        group.outerPath = outerPath(group.outline);
        if (group.outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("a same-colour union could not be traced"));
        }
    }

    QVector<int> parentByGroup(groups.size(), -1);
    for (int child = 0; child < groups.size(); ++child) {
        double bestSize = std::numeric_limits<double>::max();
        for (int parent = 0; parent < groups.size(); ++parent) {
            if (child == parent
                || !strictlyContains(groups[parent].outerPath, groups[child].outerPath)) {
                continue;
            }
            const double size = enclosureSize(groups[parent].outerPath);
            if (size < bestSize) {
                parentByGroup[child] = parent;
                bestSize = size;
            }
        }
    }

    QVector<QVector<int>> absorbedChildren(groups.size());
    for (int child = 0; child < groups.size(); ++child) {
        const int parent = parentByGroup[child];
        if (parent < 0
            || sameColor(groups[parent].color, groups[child].color)) {
            continue;
        }
        const double fraction = static_cast<double>(groups[child].sourceArea)
            / std::max(1, groups[parent].sourceArea);
        if (options.absorbAllContainedRegions
            || fraction <= kMaximumAbsorbedAreaFraction) {
            const int parentSourceIndex = groups[parent].firstSourceIndex;
            const int childSourceIndex = groups[child].firstSourceIndex;
            const quint64 pairKey = directedSourcePairKey(parentSourceIndex,
                                                          childSourceIndex);
            if (suppressions.absorptionPairs.contains(pairKey)) {
                ++plan.absorptionConflictRejectCount;
                continue;
            }
            absorbedChildren[parent].push_back(child);
            plan.absorbedRegionCount += groups[child].sourceRegionIndices.size();
            if (fraction > kMaximumAbsorbedAreaFraction) {
                plan.largeContainedAbsorptionCount +=
                    groups[child].sourceRegionIndices.size();
            }
            acceptedOperations.push_back({PlanOperationKind::Absorption, pairKey,
                                          parentSourceIndex, childSourceIndex});
        }
    }
    std::function<void(int, QVector<int> *, QVector<int> *)> appendAbsorbed;
    appendAbsorbed = [&](int groupIndex, QVector<int> *pixels, QVector<int> *indices) {
        for (const int child : absorbedChildren[groupIndex]) {
            *pixels += groups[child].pixels;
            *indices += groups[child].sourceRegionIndices;
            appendAbsorbed(child, pixels, indices);
        }
    };
    for (int parent = 0; parent < groups.size(); ++parent) {
        if (absorbedChildren[parent].isEmpty()) {
            continue;
        }
        QVector<int> mergedPixels = groups[parent].pixels;
        appendAbsorbed(parent, &mergedPixels, &groups[parent].absorbedRegionIndices);
        std::sort(groups[parent].absorbedRegionIndices.begin(),
                  groups[parent].absorbedRegionIndices.end());
        groups[parent].outline = tracePixels(mergedPixels, regions.imageSize,
                                             regions.raster->traceParams);
        groups[parent].outerPath = outerPath(groups[parent].outline);
        groups[parent].fillArea = mergedPixels.size();
        if (groups[parent].outerPath.isEmpty()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("an absorbed backdrop could not be traced"));
        }
    }

    QVector<int> groupByRegion(regions.regions.size(), -1);
    for (int source = 0; source < sources.size(); ++source) {
        groupByRegion[sources[source].resultIndex] = groupBySourceData[source];
    }
    const RegionLayerPlan baseline =
        fallbackPlan(regions, RegionLayerPlan{}, QStringLiteral("validation baseline"));
    const QImage baselineOwners =
        renderOwners(regions.imageSize, baseline.units, groupByRegion);
    QSet<quint64> dependencyKeys;
    for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        QImage coverage(regions.imageSize, QImage::Format_Grayscale8);
        if (coverage.isNull()) {
            return fallbackPlan(regions, std::move(plan),
                                QStringLiteral("layer coverage could not be allocated"));
        }
        coverage.fill(0);
        QPainter painter(&coverage);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillPath(groups[groupIndex].outerPath, Qt::white);
        painter.end();
        const QRect bounds = groups[groupIndex].outerPath.boundingRect().toAlignedRect()
            .intersected(QRect(QPoint(0, 0), regions.imageSize));
        for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
            const uchar *row = coverage.constScanLine(y);
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                if (row[x] == 0) {
                    continue;
                }
                const int owner = decodedOwner(baselineOwners.pixel(x, y));
                if (owner < 0 || owner == groupIndex
                    || sameColor(groups[owner].color, groups[groupIndex].color)) {
                    continue;
                }
                const quint64 key =
                    (static_cast<quint64>(static_cast<quint32>(groupIndex)) << 32)
                    | static_cast<quint32>(owner);
                dependencyKeys.insert(key);
            }
        }
    }
    QVector<quint64> sortedDependencies = dependencyKeys.values();
    std::sort(sortedDependencies.begin(), sortedDependencies.end());
    QVector<QVector<int>> outgoing(groups.size());
    QVector<int> indegree(groups.size(), 0);
    for (const quint64 key : sortedDependencies) {
        const int parent = static_cast<int>(key >> 32);
        const int child = static_cast<int>(key & 0xffffffffu);
        outgoing[parent].push_back(child);
        ++indegree[child];
    }
    plan.orderingEdgeCount = sortedDependencies.size();

    QVector<int> order;
    QVector<bool> scheduled(groups.size(), false);
    order.reserve(groups.size());
    while (order.size() < groups.size()) {
        int next = -1;
        for (int i = 0; i < groups.size(); ++i) {
            if (indegree[i] == 0
                && !scheduled[i]
                && (next < 0 || groups[i].firstSourceIndex < groups[next].firstSourceIndex)) {
                next = i;
            }
        }
        if (next < 0) {
            QVector<int> cycleGroups = cyclicDependencyComponent(outgoing, scheduled);
            if (cycleGroups.isEmpty() && options.acceptAnyOutput) {
                for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                    if (!scheduled[groupIndex]) {
                        cycleGroups.push_back(groupIndex);
                    }
                }
            }
            const QSet<int> cycleGroupSet(cycleGroups.cbegin(), cycleGroups.cend());
            recovery.operation = rollbackOperationForGroups(
                acceptedOperations, groupByRegion, cycleGroupSet);
            const bool suppressOperation =
                recovery.operation.kind != PlanOperationKind::None
                && !options.acceptAnyOutput;
            if (suppressOperation) {
                recovery.diagnostic = QStringLiteral(
                    "reason=dependency-cycle %1 action=suppress-%2 sources=%3,%4 "
                    "gap=%5 foreign_owners=%6")
                    .arg(affectedGroupsText(cycleGroupSet, groups),
                         operationKindText(recovery.operation.kind))
                    .arg(recovery.operation.leftSourceIndex)
                    .arg(recovery.operation.rightSourceIndex)
                    .arg(recovery.operation.gap)
                    .arg(recovery.operation.foreignOwnerCount);
                return plan;
            }
            if (!options.acceptAnyOutput) {
                return fallbackPlan(regions, std::move(plan),
                                    QStringLiteral("layer dependencies contain a cycle"));
            }
            recovery = {};
            int overlayChildCount = -1;
            for (const int groupIndex : cycleGroups) {
                if (scheduled[groupIndex]) {
                    continue;
                }
                const int candidateOverlayChildCount = static_cast<int>(std::count_if(
                    absorbedChildren[groupIndex].cbegin(),
                    absorbedChildren[groupIndex].cend(),
                    [&](int child) { return !scheduled[child]; }));
                if (next < 0
                    || candidateOverlayChildCount > overlayChildCount
                    || (candidateOverlayChildCount == overlayChildCount
                        && groups[groupIndex].firstSourceIndex
                            < groups[next].firstSourceIndex)) {
                    next = groupIndex;
                    overlayChildCount = candidateOverlayChildCount;
                }
            }
            if (next < 0) {
                for (int groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
                    if (!scheduled[groupIndex]
                        && (next < 0 || groups[groupIndex].firstSourceIndex
                            < groups[next].firstSourceIndex)) {
                        next = groupIndex;
                    }
                }
                overlayChildCount = 0;
            }
            ++plan.dangerousCycleBreakCount;
            plan.diagnostics << QStringLiteral(
                "dangerous_cycle_break %1 selected_group=%2 selected_sources=%3 "
                "violated_incoming=%4 overlay_children=%5")
                                    .arg(affectedGroupsText(cycleGroupSet, groups))
                                    .arg(next)
                                    .arg(indicesText(groups[next].sourceRegionIndices))
                                    .arg(indegree[next])
                                    .arg(overlayChildCount);
        }
        order.push_back(next);
        scheduled[next] = true;
        indegree[next] = -1;
        for (const int child : outgoing[next]) {
            --indegree[child];
        }
    }

    for (int drawOrder = 0; drawOrder < order.size(); ++drawOrder) {
        const LayerGroup &group = groups[order[drawOrder]];
        RegionLayerUnit unit;
        unit.color = group.color;
        unit.outline = group.outline;
        unit.sourceRegionIndices = group.sourceRegionIndices;
        unit.absorbedRegionIndices = group.absorbedRegionIndices;
        unit.area = group.fillArea;
        plan.diagnostics << QStringLiteral("order=%1 color=%2 sources=%3 absorbed=%4")
                                .arg(drawOrder)
                                .arg(group.color.name(QColor::HexRgb))
                                .arg(indicesText(group.sourceRegionIndices))
                                .arg(indicesText(group.absorbedRegionIndices));
        plan.units.push_back(std::move(unit));
    }
    QSet<int> validationMismatchGroups;
    plan.validationMismatchPixels =
        validatePlan(regions.imageSize, baseline.units, plan.units,
                     baselineOwners, groupByRegion, validationMismatchGroups);
    plan.diagnostics << QStringLiteral("same_color_merges=%1 nearby_same_color_merges=%2 "
                                       "edge_component_merges=%3 "
                                       "hierarchical_contour_merges=%4 "
                                       "nearby_conflict_rejections=%5 "
                                       "nearby_foreign_owner_rejections=%6 "
                                       "adjacent_conflict_rejections=%7 "
                                       "containment_conflict_rejections=%8 "
                                       "absorption_conflict_rejections=%9 "
                                       "conflict_isolated_sources=%10 absorbed_regions=%11 "
                                       "large_contained_absorptions=%12 "
                                       "morphological_closings=%13 "
                                       "convex_simplifications=%14 "
                                       "geometry_point_reductions=%15 "
                                       "dangerous_cycle_breaks=%16 "
                                       "ordering_edges=%17 validation_mismatches=%18")
                            .arg(plan.sameColorMergeCount)
                            .arg(plan.nearbySameColorMergeCount)
                            .arg(plan.edgeComponentMergeCount)
                            .arg(plan.hierarchicalContourMergeCount)
                            .arg(plan.nearbyConflictRejectCount)
                            .arg(plan.nearbyForeignOwnerRejectCount)
                            .arg(plan.adjacentConflictRejectCount)
                            .arg(plan.containmentConflictRejectCount)
                            .arg(plan.absorptionConflictRejectCount)
                            .arg(plan.conflictIsolatedSourceCount)
                            .arg(plan.absorbedRegionCount)
                            .arg(plan.largeContainedAbsorptionCount)
                            .arg(plan.morphologicalClosingCount)
                            .arg(plan.convexSimplificationCount)
                            .arg(plan.geometryPointReductionCount)
                            .arg(plan.dangerousCycleBreakCount)
                            .arg(plan.orderingEdgeCount)
                            .arg(plan.validationMismatchPixels);
    if (plan.validationMismatchPixels > 0) {
        if (options.acceptAnyOutput) {
            recovery.diagnostic = QStringLiteral(
                "reason=validation-mismatch pixels=%1 %2 "
                "action=accept-unsafe-output")
                .arg(plan.validationMismatchPixels)
                .arg(affectedGroupsText(validationMismatchGroups, groups));
            return plan;
        }
        recovery.operation = rollbackOperationForGroups(
            acceptedOperations, groupByRegion, validationMismatchGroups);
        if (recovery.operation.kind != PlanOperationKind::None) {
            recovery.diagnostic = QStringLiteral(
                "reason=validation-mismatch pixels=%1 %2 action=suppress-%3 "
                "sources=%4,%5 gap=%6 foreign_owners=%7")
                .arg(plan.validationMismatchPixels)
                .arg(affectedGroupsText(validationMismatchGroups, groups),
                     operationKindText(recovery.operation.kind))
                .arg(recovery.operation.leftSourceIndex)
                .arg(recovery.operation.rightSourceIndex)
                .arg(recovery.operation.gap)
                .arg(recovery.operation.foreignOwnerCount);
            return plan;
        }
        if (options.simplifyAllowedGeometry
            && (plan.morphologicalClosingCount > 0
                || plan.convexSimplificationCount > 0)) {
            recovery.diagnostic = QStringLiteral(
                "reason=validation-mismatch pixels=%1 %2 "
                "action=accept-dangerous-geometry")
                .arg(plan.validationMismatchPixels)
                .arg(affectedGroupsText(validationMismatchGroups, groups));
            return plan;
        }
        return fallbackPlan(regions, std::move(plan),
                            QStringLiteral("planned layers changed %1 rendered pixels")
                                .arg(plan.validationMismatchPixels));
    }

    return plan;
}

} // namespace

namespace {

RegionLayerPlan finalizedPlannerPlan(
    RegionLayerPlan plan,
    const PlannerSuppressions &suppressions,
    const QStringList &recoveryDiagnostics,
    const QString &validationPolicy,
    int plannerRebuilds,
    int dependencyCycleRebuilds,
    int validationRebuilds) {
    plan.dependencyCycleRebuildCount = dependencyCycleRebuilds;
    plan.suppressedOperationCount = suppressions.nearbyPairs.size()
        + suppressions.adjacentPairs.size()
        + suppressions.containmentPairs.size()
        + suppressions.absorptionPairs.size()
        + suppressions.geometryOperationCount;
    plan.diagnostics = recoveryDiagnostics + plan.diagnostics;
    plan.diagnostics.prepend(QStringLiteral(
        "variant_validation_policy=%1 planner_rebuilds=%2 "
        "dependency_cycle_rebuilds=%3 validation_rebuilds=%4 "
        "suppressed_nearby=%5 suppressed_adjacent=%6 "
        "suppressed_containment=%7 suppressed_absorption=%8 "
        "suppressed_geometry=%9 isolated_sources=0")
                                 .arg(validationPolicy)
                                 .arg(plannerRebuilds)
                                 .arg(dependencyCycleRebuilds)
                                 .arg(validationRebuilds)
                                 .arg(suppressions.nearbyPairs.size())
                                  .arg(suppressions.adjacentPairs.size())
                                  .arg(suppressions.containmentPairs.size())
                                  .arg(suppressions.absorptionPairs.size())
                                  .arg(suppressions.geometryOperationCount));

    return plan;
}

RegionLayerPlan buildRegionLayerPlanWithOptions(
    const RegionExtractionResult &regions,
    const PlannerOptions &options,
    bool acceptValidationMismatch,
    const QString &variantName,
    const RegionLayerPlanProgress &progress,
    const std::function<bool()> &cancelled) {
    PlannerSuppressions suppressions;
    PlannerCandidateCache candidateCache;
    QStringList recoveryDiagnostics;
    int plannerRebuilds = 0;
    int dependencyCycleRebuilds = 0;
    int validationRebuilds = 0;
    bool finalGeometryAttempt = !options.simplifyAllowedGeometry;
    while (true) {
        if (planningCancelled(cancelled)) {
            return cancelledPlan(RegionLayerPlan{});
        }
        PlannerOptions attemptOptions = options;
        attemptOptions.simplifyAllowedGeometry = finalGeometryAttempt
            && options.simplifyAllowedGeometry;
        RecoveryAction recovery;
        RegionLayerPlan plan = buildRegionLayerPlanAttempt(
            regions, suppressions, attemptOptions, candidateCache, recovery,
            progress, cancelled);
        if (plan.cancelled) {
            return plan;
        }
        const bool validationMismatch = recovery.diagnostic.startsWith(
            QStringLiteral("reason=validation-mismatch"));
        if (recovery.empty() || (acceptValidationMismatch && validationMismatch)) {
            if (!finalGeometryAttempt) {
                finalGeometryAttempt = true;
                recoveryDiagnostics.push_back(QStringLiteral(
                    "final_geometry_pass=cached-candidates"));
                if (progress) {
                    progress(QStringLiteral("Simplifying %1 regions").arg(variantName),
                             0, 0);
                }
                continue;
            }
            plan = finalizedPlannerPlan(
                std::move(plan), suppressions, recoveryDiagnostics,
                acceptValidationMismatch ? QStringLiteral("accept-mismatch")
                                         : QStringLiteral("strict"),
                plannerRebuilds, dependencyCycleRebuilds, validationRebuilds);
            if (options.acceptAnyOutput) {
                plan.diagnostics.prepend(QStringLiteral(
                    "WARNING=Dangerous safety checks are disabled; optimized output "
                    "may be visually wrong, overlap other colours, hide regions, or "
                    "use broken layer ordering"));
                plan.diagnostics.prepend(QStringLiteral(
                    "dangerous_safety_policy=accept-any-output "
                    "dependency_cycles=force-order validation=report-only "
                    "operation_suppression=disabled"));
                plan.diagnostics.prepend(QStringLiteral(
                    "dangerous_variant=unsafe-output-accepted "
                    "validation_mismatch_pixels=%1")
                                             .arg(plan.validationMismatchPixels));
            } else if (acceptValidationMismatch && validationMismatch) {
                plan.diagnostics.prepend(QStringLiteral(
                    "dangerous_variant=accepted validation_mismatch_pixels=%1")
                                             .arg(plan.validationMismatchPixels));
            } else if (acceptValidationMismatch) {
                plan.diagnostics.prepend(QStringLiteral(
                    "dangerous_variant=no-validation-difference"));
            }
            return plan;
        }
        ++plannerRebuilds;
        if (progress) {
            progress(QStringLiteral("Resolving %1 dependencies").arg(variantName),
                     0, 0);
        }
        if (recovery.diagnostic.startsWith(QStringLiteral("reason=dependency-cycle"))) {
            ++dependencyCycleRebuilds;
        } else if (validationMismatch) {
            ++validationRebuilds;
        }
        recoveryDiagnostics.push_back(QStringLiteral("rebuild=%1 %2")
                                          .arg(plannerRebuilds)
                                          .arg(recovery.diagnostic));
        switch (recovery.operation.kind) {
        case PlanOperationKind::GeometrySimplification:
            for (const int sourceIndex : recovery.operation.relatedSourceIndices) {
                suppressions.geometrySources.insert(sourceIndex);
            }
            ++suppressions.geometryOperationCount;
            break;
        case PlanOperationKind::NearbyMerge:
            suppressions.nearbyPairs.insert(recovery.operation.key);
            break;
        case PlanOperationKind::AdjacentMerge:
            suppressions.adjacentPairs.insert(recovery.operation.key);
            break;
        case PlanOperationKind::ContainmentMerge:
            suppressions.containmentPairs.insert(recovery.operation.key);
            break;
        case PlanOperationKind::Absorption:
            suppressions.absorptionPairs.insert(recovery.operation.key);
            break;
        case PlanOperationKind::None:
            break;
        }
    }
}

} // namespace

RegionLayerPlanVariants buildRegionLayerPlanVariants(
    const RegionExtractionResult &regions,
    const RegionLayerPlanProgress &progress,
    const std::function<bool()> &cancelled) {
    PlannerOptions dangerousOptions;
    dangerousOptions.hierarchicalContourMerges = true;
    dangerousOptions.absorbAllContainedRegions = true;
    dangerousOptions.simplifyAllowedGeometry = true;
    dangerousOptions.acceptAnyOutput = true;
    dangerousOptions.maximumForeignOwners = 0;
    RegionLayerPlanVariants result;
    if (progress) {
        progress(QStringLiteral("Planning Safe regions"), 0, 0);
    }
    result.safe = buildRegionLayerPlanWithOptions(
        regions, PlannerOptions{}, false, QStringLiteral("Safe"),
        progress, cancelled);
    if (result.safe.cancelled || planningCancelled(cancelled)) {
        result.dangerous.cancelled = true;
        return result;
    }
    if (progress) {
        progress(QStringLiteral("Planning Dangerous regions"), 0, 0);
    }
    result.dangerous = buildRegionLayerPlanWithOptions(
        regions, dangerousOptions, true, QStringLiteral("Dangerous"),
        progress, cancelled);

    return result;
}

RegionLayerPlan buildRegionLayerPlan(const RegionExtractionResult &regions) {
    return buildRegionLayerPlanWithOptions(
        regions, PlannerOptions{}, false, QStringLiteral("Safe"), {}, {});
}

} // namespace gui
