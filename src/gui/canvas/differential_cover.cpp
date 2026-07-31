#include "differential_cover.h"
#include "differential_cover_gpu.h"
#include "polygon_mesh.h"

#include <clipper2/clipper.engine.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <set>
#include <tuple>
#include <utility>

namespace gui::cover {
namespace {

constexpr double kGeometryEpsilon = 1e-10;
constexpr double kClipperScale = 1000000.0;
constexpr int kGradientCount = 6;
constexpr int kRouterCandidateCount = 3;
constexpr int kMaximumDistanceDimension = 1024;
constexpr double kAdamBeta1 = 0.9;
constexpr double kAdamBeta2 = 0.999;
constexpr double kAdamEpsilon = 1e-8;
constexpr double kGradientNormLimit = 1000000.0;
constexpr double kGradientStopNorm = 1e-8;
constexpr double kRestartTranslationFraction = 0.15;
constexpr double kRestartAngleRange = 0.2;
constexpr double kRestartScaleRange = 0.12;
constexpr double kInitialRadiusFraction = 0.8;
constexpr double kMinimumAffineScale = 1e-6;
constexpr double kLegalShrinkFactor = 0.9;
constexpr int kLegalShrinkSteps = 64;
constexpr int kGpuLegalizationBatchSteps = 8;
constexpr double kHardBoundaryFraction = 0.6;
constexpr double kComplexityGainWindow = 1.0;
constexpr double kLocalRouterAdvantage = 0.75;
constexpr double kLocalSelectionGainAdvantage = 1.05;
constexpr double kLocalSelectionComplexityRatio = 0.95;
constexpr double kComponentComplexityWeight = 8.0;
constexpr double kHoleComplexityWeight = 16.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kMaximumShapeId = 65535;
constexpr double kStructuralDirectionSinTolerance = 0.0523359562429438;
constexpr double kStructuralMinimumAxisSeparationSin = 0.258819045102521;
constexpr double kStructuralMinimumExplainedBoundaryFraction = 0.9;
constexpr double kStructuralCoordinateToleranceFraction = 0.01;
constexpr double kStructuralMinimumCoordinateTolerance = 0.5;
constexpr double kStructuralMinimumCompactCoverageRatio = 0.98;
constexpr double kStructuralMinimumSeedCoverageRatio = 0.9;
constexpr double kStructuralMinimumLegalScale = 0.9;
constexpr double kStructuralLegalScaleStep = 0.0005;
constexpr int kStructuralMaximumGridCells = 63;
constexpr int kStructuralMaximumSupportLines = 12;
constexpr int kStructuralSearchNodeLimit = 100000;
constexpr double kMeshMinimumCompactCoverageRatio = 0.98;
constexpr double kMeshMinimumLegalScale = 0.9;
constexpr double kMeshLegalScaleStep = 0.0005;

QStringList differentialShapeAssetPaths() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/differential_shapes.json")),
        QDir(cwd).filePath(QStringLiteral("assets/differential_shapes.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/differential_shapes.json")),
    };
}

QVector<int> loadDifferentialShapeIdsFromFile(const QString &path,
                                              QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not open differential shape catalog: %1")
                         .arg(path);
        }
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid differential shape catalog: %1")
                         .arg(parseError.errorString());
        }
        return {};
    }
    if (!document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "differential shape catalog root must be an object");
        }
        return {};
    }

    const QJsonValue shapeIdsValue =
        document.object().value(QStringLiteral("shape_ids"));
    if (!shapeIdsValue.isArray() || shapeIdsValue.toArray().isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "differential shape catalog must contain a non-empty shape_ids array");
        }
        return {};
    }

    const QJsonArray shapeIdsArray = shapeIdsValue.toArray();
    QVector<int> result;
    QSet<int> seenShapeIds;
    result.reserve(shapeIdsArray.size());
    seenShapeIds.reserve(shapeIdsArray.size());
    for (qsizetype index = 0; index < shapeIdsArray.size(); ++index) {
        const QJsonValue value = shapeIdsArray.at(index);
        const int shapeId = value.toInt(-1);
        const double numericShapeId = value.toDouble(-1.0);
        if (!value.isDouble()
            || numericShapeId != static_cast<double>(shapeId)
            || shapeId <= 0
            || shapeId > kMaximumShapeId) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "differential shape catalog entry %1 is not a valid shape id")
                             .arg(index);
            }
            return {};
        }
        if (seenShapeIds.contains(shapeId)) {
            if (error != nullptr) {
                *error = QStringLiteral(
                    "differential shape catalog contains duplicate shape id %1")
                             .arg(shapeId);
            }
            return {};
        }
        seenShapeIds.insert(shapeId);
        result.push_back(shapeId);
    }
    if (error != nullptr) {
        error->clear();
    }

    return result;
}

QVector<int> loadDifferentialShapeIds(QString *error) {
    for (const QString &path : differentialShapeAssetPaths()) {
        if (QFile::exists(path)) {
            return loadDifferentialShapeIdsFromFile(path, error);
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("differential_shapes.json was not found");
    }

    return {};
}

struct Jet {
    std::array<double, kGradientCount> gradient{};
    double value = 0.0;
};

struct JetPoint {
    Jet x;
    Jet y;
};

struct DistanceSeed {
    QPointF point;
    double angle = 0.0;
    double radius = 0.0;
};

enum class CandidateOrigin {
    Greedy,
    LocalComponent,
    WholeComponent,
    HardEdge,
};

struct Candidate {
    Affine transform;
    int shapeId = 0;
    double covered = 0.0;
    double spill = 0.0;
    CandidateOrigin origin = CandidateOrigin::Greedy;
    bool valid = false;
};

struct CandidateInitialization {
    QPointF translationOffset;
    double angleOffset = 0.0;
    double scaleFactor = 1.0;
};

struct CandidateJob {
    const ShapeMesh *shape = nullptr;
    CandidateInitialization initialization;
    Affine transform;
    CandidateOrigin origin = CandidateOrigin::Greedy;
    bool hasTransform = false;
};

struct FixedCandidate {
    const ShapeMesh *shape = nullptr;
    Affine transform;
};

struct CandidateProfile {
    qint64 totalNanoseconds = 0;
    qint64 adamEvaluationNanoseconds = 0;
    qint64 legalizationNanoseconds = 0;
    std::uint64_t adamEvaluations = 0;
    std::uint64_t legalizationEvaluations = 0;
};

struct CandidateJobResult {
    Candidate candidate;
    CandidateProfile profile;
};

struct GpuCandidateState {
    std::array<double, kGradientCount> values{};
    std::array<double, kGradientCount> firstMoment{};
    std::array<double, kGradientCount> secondMoment{};
    Candidate best;
    Candidate bestGpu;
    const CandidateJob *job = nullptr;
    double beta1Power = 1.0;
    double beta2Power = 1.0;
    bool active = true;
};

struct QueueNode {
    int index = 0;
    double distance = 0.0;

    bool operator>(const QueueNode &other) const {
        return distance > other.distance;
    }
};

struct IntersectionJets {
    Jet covered;
    Jet legal;
};

struct ResidualComplexity {
    double score = 0.0;
    int components = 0;
    int holes = 0;
};

struct CandidateSelection {
    Candidate candidate;
    Polygons residual;
    double exactGain = 0.0;
    bool complexityPreferred = false;
    bool valid = false;
};

struct OrientedBounds {
    QPointF center;
    QPointF axisX;
    QPointF axisY;
    double extentX = 0.0;
    double extentY = 0.0;
    bool valid = false;
};

struct EvaluationBounds {
    QVector<QRectF> covered;
    QVector<QRectF> legal;
};

struct ExactCoverState {
    Polygons footprints;
    Polygons coverage;
    Polygons residual;
    double residualArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
};

struct StructuralEdge {
    QPointF direction;
    double length = 0.0;
    double maximumBow = 0.0;
};

struct StructuralAxes {
    QPointF first;
    QPointF second;
    double determinant = 0.0;
    double explainedBoundaryFraction = 0.0;
    bool valid = false;
};

struct StructuralRectangle {
    quint64 mask = 0;
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

struct StructuralCoverPlan {
    QVector<Placement> placements;
    Polygons residual;
    QString reason;
    double explainedBoundaryFraction = 0.0;
    double coverageRatio = 0.0;
    double residualArea = 0.0;
    double residualThickness = 0.0;
    double outsideArea = 0.0;
    int gridCells = 0;
    int rectangleCandidates = 0;
    bool eligible = false;
    bool accepted = false;
    bool seeded = false;
    bool cancelled = false;
};

struct MeshCoverPlan {
    QVector<Placement> placements;
    Polygons residual;
    QString reason;
    double coverageRatio = 0.0;
    double residualArea = 0.0;
    double outsideArea = 0.0;
    double scale = 0.0;
    bool accepted = false;
    bool cancelled = false;
};

struct PruneCandidate {
    Affine transform;
    int index = 0;
    int shapeId = 0;
    double uniqueArea = 0.0;
};

struct PruneNeighbor {
    int index = 0;
    double overlapArea = 0.0;
    double distanceSquared = 0.0;
};

Jet constantJet(double value) {
    Jet result;
    result.value = value;

    return result;
}

Jet parameterJet(double value, int parameter) {
    Jet result = constantJet(value);
    result.gradient[parameter] = 1.0;

    return result;
}

Jet operator+(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value + right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = left.gradient[i] + right.gradient[i];
    }

    return result;
}

Jet operator-(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value - right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = left.gradient[i] - right.gradient[i];
    }

    return result;
}

Jet operator-(const Jet &value) {
    Jet result;
    result.value = -value.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = -value.gradient[i];
    }

    return result;
}

Jet operator*(const Jet &left, const Jet &right) {
    Jet result;
    result.value = left.value * right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = left.gradient[i] * right.value
            + left.value * right.gradient[i];
    }

    return result;
}

Jet operator*(const Jet &left, double right) {
    return left * constantJet(right);
}

Jet operator*(double left, const Jet &right) {
    return constantJet(left) * right;
}

Jet operator/(const Jet &left, const Jet &right) {
    Jet result;
    const double denominator = right.value * right.value;

    result.value = left.value / right.value;
    for (int i = 0; i < kGradientCount; ++i) {
        result.gradient[i] = (left.gradient[i] * right.value
                              - left.value * right.gradient[i])
            / denominator;
    }

    return result;
}

Jet absoluteJet(const Jet &value) {
    return value.value < 0.0 ? -value : value;
}

JetPoint operator+(const JetPoint &left, const JetPoint &right) {
    return {left.x + right.x, left.y + right.y};
}

JetPoint operator-(const JetPoint &left, const JetPoint &right) {
    return {left.x - right.x, left.y - right.y};
}

JetPoint operator*(const JetPoint &point, const Jet &scale) {
    return {point.x * scale, point.y * scale};
}

Jet cross(const JetPoint &left, const JetPoint &right) {
    return left.x * right.y - left.y * right.x;
}

double cross(const Vec2 &left, const Vec2 &right) {
    return left.x * right.y - left.y * right.x;
}

double signedArea(const QPolygonF &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const QPointF &left = polygon[i];
        const QPointF &right = polygon[(i + 1) % polygon.size()];
        result += left.x() * right.y() - left.y() * right.x();
    }

    return result * 0.5;
}

double signedArea(const QVector<Vec2> &polygon) {
    double result = 0.0;
    for (int i = 0; i < polygon.size(); ++i) {
        const Vec2 &left = polygon[i];
        const Vec2 &right = polygon[(i + 1) % polygon.size()];
        result += cross(left, right);
    }

    return result * 0.5;
}

double polygonSetArea(const Polygons &polygons) {
    double result = 0.0;
    for (const QPolygonF &polygon : polygons) {
        result += signedArea(polygon);
    }

    return std::abs(result);
}

QPolygonF normalizedPolygon(QPolygonF polygon) {
    while (polygon.size() > 1
           && QLineF(polygon.front(), polygon.back()).length() <= kGeometryEpsilon) {
        polygon.removeLast();
    }
    if (polygon.size() < 3 || std::abs(signedArea(polygon)) <= kGeometryEpsilon) {
        return {};
    }

    return polygon;
}

QRectF polygonBounds(const Polygons &polygons) {
    QRectF result;
    bool haveBounds = false;
    for (const QPolygonF &polygon : polygons) {
        if (polygon.isEmpty()) {
            continue;
        }
        result = haveBounds ? result.united(polygon.boundingRect())
                            : polygon.boundingRect();
        haveBounds = true;
    }

    return result;
}

QVector<QRectF> individualPolygonBounds(const Polygons &polygons) {
    QVector<QRectF> result;
    result.reserve(polygons.size());
    for (const QPolygonF &polygon : polygons) {
        result.push_back(polygon.boundingRect());
    }

    return result;
}

JetPoint transformedVertex(const Vec2 &vertex, const Affine &transform) {
    const Jet a = parameterJet(transform.a, 0);
    const Jet b = parameterJet(transform.b, 1);
    const Jet c = parameterJet(transform.c, 2);
    const Jet d = parameterJet(transform.d, 3);
    const Jet e = parameterJet(transform.e, 4);
    const Jet f = parameterJet(transform.f, 5);

    return {
        a * vertex.x + c * vertex.y + e,
        b * vertex.x + d * vertex.y + f,
    };
}

JetPoint constantPoint(const QPointF &point) {
    return {constantJet(point.x()), constantJet(point.y())};
}

Jet sideValue(const JetPoint &point,
              const JetPoint &windowStart,
              const JetPoint &windowEnd) {
    return cross(windowEnd - windowStart, point - windowStart);
}

JetPoint intersectionPoint(const JetPoint &start,
                           const JetPoint &end,
                           const Jet &startSide,
                           const Jet &endSide) {
    const Jet denominator = startSide - endSide;
    if (std::abs(denominator.value) <= kGeometryEpsilon) {
        return end;
    }
    const Jet fraction = startSide / denominator;

    return start + (end - start) * fraction;
}

void clipHalfPlane(const QVector<JetPoint> &subject,
                   const JetPoint &windowStart,
                   const JetPoint &windowEnd,
                   QVector<JetPoint> *result) {
    result->clear();
    if (subject.isEmpty()) {
        return;
    }
    result->reserve(subject.size() + 2);

    JetPoint start = subject.back();
    Jet startSide = sideValue(start, windowStart, windowEnd);
    bool startInside = startSide.value >= -kGeometryEpsilon;
    for (const JetPoint &end : subject) {
        const Jet endSide = sideValue(end, windowStart, windowEnd);
        const bool endInside = endSide.value >= -kGeometryEpsilon;
        if (endInside != startInside) {
            result->push_back(intersectionPoint(start, end, startSide, endSide));
        }
        if (endInside) {
            result->push_back(end);
        }
        start = end;
        startSide = endSide;
        startInside = endInside;
    }
}

Jet clippedPolygonArea(const QPolygonF &subject,
                       std::array<JetPoint, 3> window) {
    QVector<JetPoint> clipped;
    clipped.reserve(subject.size() + 3);
    for (const QPointF &point : subject) {
        clipped.push_back(constantPoint(point));
    }
    QVector<JetPoint> scratch;
    scratch.reserve(subject.size() + 6);

    const Jet orientation = cross(window[1] - window[0],
                                  window[2] - window[0]);
    if (orientation.value < 0.0) {
        std::swap(window[1], window[2]);
    }
    for (int edge = 0; edge < 3 && !clipped.isEmpty(); ++edge) {
        clipHalfPlane(clipped, window[edge], window[(edge + 1) % 3], &scratch);
        clipped.swap(scratch);
    }
    if (clipped.size() < 3) {
        return {};
    }

    Jet doubledArea;
    for (int i = 0; i < clipped.size(); ++i) {
        doubledArea = doubledArea
            + cross(clipped[i], clipped[(i + 1) % clipped.size()]);
    }

    return doubledArea * 0.5;
}

Jet clippedConvexPolygonArea(const QPolygonF &subject,
                             const QVector<JetPoint> &window) {
    QVector<JetPoint> clipped;
    clipped.reserve(subject.size() + window.size());
    for (const QPointF &point : subject) {
        clipped.push_back(constantPoint(point));
    }
    QVector<JetPoint> scratch;
    scratch.reserve(subject.size() + window.size() * 2);

    Jet windowArea;
    for (int i = 0; i < window.size(); ++i) {
        windowArea = windowArea
            + cross(window[i], window[(i + 1) % window.size()]);
    }
    const bool reverse = windowArea.value < 0.0;
    for (int edge = 0; edge < window.size() && !clipped.isEmpty(); ++edge) {
        const int start = reverse ? (window.size() - edge) % window.size() : edge;
        const int end = reverse
            ? (window.size() - edge - 1 + window.size()) % window.size()
            : (edge + 1) % window.size();
        clipHalfPlane(clipped, window[start], window[end], &scratch);
        clipped.swap(scratch);
    }
    if (clipped.size() < 3) {
        return {};
    }

    Jet doubledArea;
    for (int i = 0; i < clipped.size(); ++i) {
        doubledArea = doubledArea
            + cross(clipped[i], clipped[(i + 1) % clipped.size()]);
    }

    return doubledArea * 0.5;
}

QRectF jetWindowBounds(const QVector<JetPoint> &window) {
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const JetPoint &point : window) {
        minimumX = std::min(minimumX, point.x.value);
        minimumY = std::min(minimumY, point.y.value);
        maximumX = std::max(maximumX, point.x.value);
        maximumY = std::max(maximumY, point.y.value);
    }

    return QRectF(QPointF(minimumX, minimumY),
                  QPointF(maximumX, maximumY));
}

IntersectionJets intersectionAreas(const ShapeMesh &shape,
                                   const Affine &transform,
                                   const Polygons &coveredSubject,
                                   const Polygons &legalSubject,
                                   const EvaluationBounds &subjectBounds) {
    IntersectionJets result;
    if (shape.convex) {
        QVector<JetPoint> window;
        window.reserve(shape.boundary.size());
        for (const Vec2 &point : shape.boundary) {
            window.push_back(transformedVertex(point, transform));
        }
        const QRectF windowBounds = jetWindowBounds(window);
        for (int i = 0; i < coveredSubject.size(); ++i) {
            if (subjectBounds.covered[i].intersects(windowBounds)) {
                result.covered = result.covered
                    + clippedConvexPolygonArea(coveredSubject[i], window);
            }
        }
        for (int i = 0; i < legalSubject.size(); ++i) {
            if (subjectBounds.legal[i].intersects(windowBounds)) {
                result.legal = result.legal
                    + clippedConvexPolygonArea(legalSubject[i], window);
            }
        }

        return result;
    }
    for (const std::array<int, 3> &triangle : shape.triangles) {
        std::array<JetPoint, 3> window = {
            transformedVertex(shape.vertices[triangle[0]], transform),
            transformedVertex(shape.vertices[triangle[1]], transform),
            transformedVertex(shape.vertices[triangle[2]], transform),
        };
        const double minimumX = std::min({
            window[0].x.value, window[1].x.value, window[2].x.value,
        });
        const double minimumY = std::min({
            window[0].y.value, window[1].y.value, window[2].y.value,
        });
        const double maximumX = std::max({
            window[0].x.value, window[1].x.value, window[2].x.value,
        });
        const double maximumY = std::max({
            window[0].y.value, window[1].y.value, window[2].y.value,
        });
        const QRectF windowBounds(
            QPointF(minimumX, minimumY), QPointF(maximumX, maximumY));
        for (int i = 0; i < coveredSubject.size(); ++i) {
            if (!subjectBounds.covered[i].intersects(windowBounds)) {
                continue;
            }
            result.covered =
                result.covered + clippedPolygonArea(coveredSubject[i], window);
        }
        for (int i = 0; i < legalSubject.size(); ++i) {
            if (!subjectBounds.legal[i].intersects(windowBounds)) {
                continue;
            }
            result.legal =
                result.legal + clippedPolygonArea(legalSubject[i], window);
        }
    }

    return result;
}

Jet transformedShapeArea(const ShapeMesh &shape, const Affine &transform) {
    const Jet a = parameterJet(transform.a, 0);
    const Jet b = parameterJet(transform.b, 1);
    const Jet c = parameterJet(transform.c, 2);
    const Jet d = parameterJet(transform.d, 3);

    return absoluteJet(a * d - b * c) * shape.area;
}

AreaGradient areaGradient(const ShapeMesh &shape,
                          const Affine &transform,
                          const Polygons &coveredSubject,
                          const Polygons &legalSubject,
                          const EvaluationBounds &subjectBounds) {
    const IntersectionJets intersections =
        intersectionAreas(shape, transform, coveredSubject, legalSubject,
                          subjectBounds);
    const Jet spill =
        transformedShapeArea(shape, transform) - intersections.legal;
    AreaGradient result;
    result.covered = intersections.covered.value;
    result.spill = std::max(0.0, spill.value);
    result.coveredGradient = intersections.covered.gradient;
    result.spillGradient = spill.gradient;

    return result;
}

bool finiteGradient(const AreaGradient &evaluation) {
    if (!std::isfinite(evaluation.covered) || !std::isfinite(evaluation.spill)) {
        return false;
    }
    for (int i = 0; i < kGradientCount; ++i) {
        if (!std::isfinite(evaluation.coveredGradient[i])
            || !std::isfinite(evaluation.spillGradient[i])) {
            return false;
        }
    }

    return true;
}

Vec2 transformedPoint(const Vec2 &point, const Affine &transform) {
    return {
        transform.a * point.x + transform.c * point.y + transform.e,
        transform.b * point.x + transform.d * point.y + transform.f,
    };
}

QPolygonF transformedBoundary(const ShapeMesh &shape, const Affine &transform) {
    QPolygonF result;
    result.reserve(shape.boundary.size());
    for (const Vec2 &point : shape.boundary) {
        const Vec2 mapped = transformedPoint(point, transform);
        result.push_back(QPointF(mapped.x, mapped.y));
    }
    if (signedArea(result) < 0.0) {
        std::reverse(result.begin(), result.end());
    }

    return result;
}

Clipper2Lib::Paths64 toClipperPaths(const Polygons &polygons) {
    Clipper2Lib::Paths64 result;
    result.reserve(static_cast<size_t>(polygons.size()));
    for (const QPolygonF &polygon : polygons) {
        Clipper2Lib::Path64 path;
        path.reserve(static_cast<size_t>(polygon.size()));
        for (const QPointF &point : polygon) {
            path.emplace_back(std::llround(point.x() * kClipperScale),
                              std::llround(point.y() * kClipperScale));
        }
        if (path.size() >= 3) {
            result.push_back(std::move(path));
        }
    }

    return result;
}

Polygons fromClipperPaths(const Clipper2Lib::Paths64 &paths) {
    Polygons result;
    result.reserve(static_cast<qsizetype>(paths.size()));
    for (const Clipper2Lib::Path64 &path : paths) {
        QPolygonF polygon;
        polygon.reserve(static_cast<qsizetype>(path.size()));
        for (const Clipper2Lib::Point64 &point : path) {
            polygon.push_back(QPointF(static_cast<double>(point.x) / kClipperScale,
                                      static_cast<double>(point.y) / kClipperScale));
        }
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }

    return result;
}

Polygons booleanOperation(const Polygons &subjects,
                          const Polygons &clips,
                          Clipper2Lib::ClipType operation) {
    Clipper2Lib::Clipper64 clipper;
    Clipper2Lib::Paths64 result;
    clipper.AddSubject(toClipperPaths(subjects));
    if (!clips.isEmpty()) {
        clipper.AddClip(toClipperPaths(clips));
    }
    if (!clipper.Execute(operation, Clipper2Lib::FillRule::NonZero, result)) {
        return {};
    }

    return fromClipperPaths(result);
}

Polygons differencePolygons(const Polygons &subjects, const Polygons &clips) {
    return booleanOperation(subjects, clips, Clipper2Lib::ClipType::Difference);
}

Polygons intersectionPolygons(const Polygons &subjects,
                              const Polygons &clips) {
    return booleanOperation(
        subjects, clips, Clipper2Lib::ClipType::Intersection);
}

Polygons unionPolygons(const Polygons &subjects) {
    return booleanOperation(subjects, {}, Clipper2Lib::ClipType::Union);
}

bool samePoint(const QPointF &left, const QPointF &right) {
    return std::abs(left.x() - right.x()) <= kGeometryEpsilon
        && std::abs(left.y() - right.y()) <= kGeometryEpsilon;
}

int vertexIndex(QVector<Vec2> *vertices, const QPointF &point) {
    for (int i = 0; i < vertices->size(); ++i) {
        if (samePoint(QPointF((*vertices)[i].x, (*vertices)[i].y), point)) {
            return i;
        }
    }
    vertices->push_back({point.x(), point.y()});

    return vertices->size() - 1;
}

bool boundaryIsConvex(const QVector<Vec2> &boundary) {
    double sign = 0.0;
    for (int i = 0; i < boundary.size(); ++i) {
        const Vec2 left{
            boundary[(i + 1) % boundary.size()].x - boundary[i].x,
            boundary[(i + 1) % boundary.size()].y - boundary[i].y,
        };
        const Vec2 right{
            boundary[(i + 2) % boundary.size()].x
                - boundary[(i + 1) % boundary.size()].x,
            boundary[(i + 2) % boundary.size()].y
                - boundary[(i + 1) % boundary.size()].y,
        };
        const double turn = cross(left, right);
        if (std::abs(turn) <= kGeometryEpsilon) {
            continue;
        }
        if (sign == 0.0) {
            sign = turn;
        } else if (turn * sign < 0.0) {
            return false;
        }
    }

    return true;
}

ShapeMesh buildShapeMesh(int shapeId, const ShapeGeometry &geometry) {
    ShapeMesh result;
    result.id = shapeId;
    result.triangles.reserve(geometry.triangles.size());
    std::map<std::pair<int, int>, int> edgeCounts;
    for (const ShapeTriangle &triangle : geometry.triangles) {
        if (std::abs(triangle.alpha0 - 1.0) > kGeometryEpsilon
            || std::abs(triangle.alpha1 - 1.0) > kGeometryEpsilon
            || std::abs(triangle.alpha2 - 1.0) > kGeometryEpsilon) {
            result.error = QStringLiteral("shape %1 has non-opaque vertices").arg(shapeId);
            return result;
        }
        const std::array<int, 3> indices = {
            vertexIndex(&result.vertices, triangle.p0),
            vertexIndex(&result.vertices, triangle.p1),
            vertexIndex(&result.vertices, triangle.p2),
        };
        result.triangles.push_back(indices);
        for (int edge = 0; edge < 3; ++edge) {
            const int left = indices[edge];
            const int right = indices[(edge + 1) % 3];
            ++edgeCounts[std::minmax(left, right)];
        }
    }
    if (result.triangles.size() != result.vertices.size() - 2) {
        result.error = QStringLiteral("shape %1 triangulation count is invalid").arg(shapeId);
        return result;
    }

    QVector<QVector<int>> adjacency(result.vertices.size());
    int boundaryEdgeCount = 0;
    for (const auto &[edge, count] : edgeCounts) {
        if (count > 2) {
            result.error = QStringLiteral("shape %1 has a non-manifold edge").arg(shapeId);
            return result;
        }
        if (count == 1) {
            adjacency[edge.first].push_back(edge.second);
            adjacency[edge.second].push_back(edge.first);
            ++boundaryEdgeCount;
        }
    }
    if (boundaryEdgeCount != result.vertices.size()) {
        result.error = QStringLiteral("shape %1 boundary count is invalid").arg(shapeId);
        return result;
    }
    for (const QVector<int> &neighbors : adjacency) {
        if (neighbors.size() != 2) {
            result.error = QStringLiteral("shape %1 boundary is not a loop").arg(shapeId);
            return result;
        }
    }

    int previous = -1;
    int current = 0;
    for (int i = 0; i < result.vertices.size(); ++i) {
        result.boundary.push_back(result.vertices[current]);
        const QVector<int> &neighbors = adjacency[current];
        const int next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
        previous = current;
        current = next;
    }
    if (current != 0) {
        result.error = QStringLiteral("shape %1 boundary did not close").arg(shapeId);
        return result;
    }
    if (signedArea(result.boundary) < 0.0) {
        std::reverse(result.boundary.begin(), result.boundary.end());
    }

    double triangleArea = 0.0;
    for (const std::array<int, 3> &triangle : result.triangles) {
        const Vec2 &a = result.vertices[triangle[0]];
        const Vec2 &b = result.vertices[triangle[1]];
        const Vec2 &c = result.vertices[triangle[2]];
        triangleArea += std::abs(cross(
            Vec2{b.x - a.x, b.y - a.y},
            Vec2{c.x - a.x, c.y - a.y}))
            * 0.5;
    }
    const double boundaryArea = std::abs(signedArea(result.boundary));
    if (std::abs(triangleArea - boundaryArea)
        > std::max(1.0, boundaryArea) * 1e-8) {
        result.error = QStringLiteral("shape %1 triangles overlap or leave gaps")
                           .arg(shapeId);
        return result;
    }
    result.area = boundaryArea;
    result.convex = boundaryIsConvex(result.boundary);

    return result;
}

Polygons normalizedInputPolygons(const Polygons &polygons) {
    Polygons result;
    result.reserve(polygons.size());
    for (QPolygonF polygon : polygons) {
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }
    if (result.size() == 1 && signedArea(result.front()) < 0.0) {
        std::reverse(result.front().begin(), result.front().end());
    }

    return result;
}

DistanceSeed distanceSeed(const Polygons &polygons) {
    const QRectF bounds = polygonBounds(polygons);
    DistanceSeed result;
    result.point = bounds.center();
    if (!bounds.isValid() || bounds.isEmpty()) {
        return result;
    }

    const double maximumExtent = std::max(bounds.width(), bounds.height());
    const double step = std::max(1.0, maximumExtent / kMaximumDistanceDimension);
    const int width = std::max(3, static_cast<int>(std::ceil(bounds.width() / step)) + 3);
    const int height = std::max(3, static_cast<int>(std::ceil(bounds.height() / step)) + 3);
    QImage mask(width, height, QImage::Format_Grayscale8);
    mask.fill(0);
    QPainterPath maskPath;
    maskPath.setFillRule(Qt::WindingFill);
    for (const QPolygonF &polygon : polygons) {
        QPolygonF mapped;
        mapped.reserve(polygon.size());
        for (const QPointF &point : polygon) {
            mapped.push_back(QPointF((point.x() - bounds.left()) / step + 1.0,
                                      (point.y() - bounds.top()) / step + 1.0));
        }
        maskPath.addPolygon(mapped);
    }
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.fillPath(maskPath, Qt::white);
    }

    const double diagonal = std::sqrt(2.0);
    const double infinite = std::numeric_limits<double>::infinity();
    QVector<double> distance(width * height, infinite);
    const auto isInside = [&mask](int x, int y) {
        return mask.constScanLine(y)[x] != 0;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!isInside(x, y)) {
                distance[y * width + x] = 0.0;
            }
        }
    }
    const auto relax = [&distance, width, height](int x, int y,
                                                   int otherX, int otherY,
                                                   double cost) {
        if (otherX < 0 || otherX >= width || otherY < 0 || otherY >= height) {
            return;
        }
        const int index = y * width + x;
        distance[index] = std::min(distance[index],
                                   distance[otherY * width + otherX] + cost);
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!isInside(x, y)) {
                continue;
            }
            relax(x, y, x - 1, y, 1.0);
            relax(x, y, x, y - 1, 1.0);
            relax(x, y, x - 1, y - 1, diagonal);
            relax(x, y, x + 1, y - 1, diagonal);
        }
    }
    for (int y = height - 1; y >= 0; --y) {
        for (int x = width - 1; x >= 0; --x) {
            if (!isInside(x, y)) {
                continue;
            }
            relax(x, y, x + 1, y, 1.0);
            relax(x, y, x, y + 1, 1.0);
            relax(x, y, x + 1, y + 1, diagonal);
            relax(x, y, x - 1, y + 1, diagonal);
        }
    }

    int bestIndex = -1;
    double bestDistance = 0.0;
    for (int i = 0; i < distance.size(); ++i) {
        if (std::isfinite(distance[i]) && distance[i] > bestDistance) {
            bestDistance = distance[i];
            bestIndex = i;
        }
    }
    if (bestIndex < 0) {
        return result;
    }
    const int seedX = bestIndex % width;
    const int seedY = bestIndex / width;
    result.point = QPointF(bounds.left() + (seedX - 0.5) * step,
                           bounds.top() + (seedY - 0.5) * step);
    result.radius = bestDistance * step;

    QVector<QPointF> localPoints;
    const double neighborhood = std::max(step, result.radius * 2.5);
    for (const QPolygonF &polygon : polygons) {
        for (const QPointF &point : polygon) {
            if (QLineF(point, result.point).length() <= neighborhood) {
                localPoints.push_back(point);
            }
        }
    }
    if (localPoints.size() < 3) {
        for (const QPolygonF &polygon : polygons) {
            localPoints += polygon;
        }
    }
    QPointF mean;
    for (const QPointF &point : localPoints) {
        mean += point;
    }
    if (!localPoints.isEmpty()) {
        mean /= localPoints.size();
    }
    double covarianceXX = 0.0;
    double covarianceXY = 0.0;
    double covarianceYY = 0.0;
    for (const QPointF &point : localPoints) {
        const QPointF centered = point - mean;
        covarianceXX += centered.x() * centered.x();
        covarianceXY += centered.x() * centered.y();
        covarianceYY += centered.y() * centered.y();
    }
    result.angle = 0.5 * std::atan2(2.0 * covarianceXY,
                                    covarianceXX - covarianceYY);

    return result;
}

QRectF shapeBounds(const ShapeMesh &shape) {
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const Vec2 &point : shape.boundary) {
        minimumX = std::min(minimumX, point.x);
        minimumY = std::min(minimumY, point.y);
        maximumX = std::max(maximumX, point.x);
        maximumY = std::max(maximumY, point.y);
    }

    return QRectF(QPointF(minimumX, minimumY),
                  QPointF(maximumX, maximumY));
}

Affine initialTransform(const ShapeMesh &shape,
                        const DistanceSeed &seed,
                        double angleOffset,
                        double scaleFactor,
                        const QPointF &translationOffset) {
    const QRectF bounds = shapeBounds(shape);
    const QPointF localCenter = bounds.center();
    double sourceRadius = 0.0;
    for (const Vec2 &point : shape.boundary) {
        sourceRadius = std::max(sourceRadius,
                                QLineF(QPointF(point.x, point.y), localCenter).length());
    }
    const double scale = std::max(
        kMinimumAffineScale,
        seed.radius * kInitialRadiusFraction
            / std::max(sourceRadius, kGeometryEpsilon) * scaleFactor);
    const double angle = seed.angle + angleOffset;
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    Affine result;
    result.a = cosine * scale;
    result.b = sine * scale;
    result.c = -sine * scale;
    result.d = cosine * scale;
    result.e = seed.point.x() + translationOffset.x()
        - result.a * localCenter.x() - result.c * localCenter.y();
    result.f = seed.point.y() + translationOffset.y()
        - result.b * localCenter.x() - result.d * localCenter.y();

    return result;
}

Affine initialTransform(const CandidateJob &job,
                        const DistanceSeed &seed) {
    if (job.hasTransform) {
        return job.transform;
    }

    return initialTransform(
        *job.shape,
        seed,
        job.initialization.angleOffset,
        job.initialization.scaleFactor,
        job.initialization.translationOffset);
}

OrientedBounds orientedBounds(const QVector<QPointF> &points) {
    OrientedBounds result;
    if (points.size() < 3) {
        return result;
    }

    QPointF mean;
    for (const QPointF &point : points) {
        mean += point;
    }
    mean /= points.size();
    double covarianceXX = 0.0;
    double covarianceXY = 0.0;
    double covarianceYY = 0.0;
    for (const QPointF &point : points) {
        const QPointF centered = point - mean;
        covarianceXX += centered.x() * centered.x();
        covarianceXY += centered.x() * centered.y();
        covarianceYY += centered.y() * centered.y();
    }
    const double angle = 0.5 * std::atan2(
        2.0 * covarianceXY, covarianceXX - covarianceYY);
    result.axisX = QPointF(std::cos(angle), std::sin(angle));
    result.axisY = QPointF(-result.axisX.y(), result.axisX.x());
    double minimumX = std::numeric_limits<double>::max();
    double minimumY = std::numeric_limits<double>::max();
    double maximumX = std::numeric_limits<double>::lowest();
    double maximumY = std::numeric_limits<double>::lowest();
    for (const QPointF &point : points) {
        const double projectedX =
            QPointF::dotProduct(point, result.axisX);
        const double projectedY =
            QPointF::dotProduct(point, result.axisY);
        minimumX = std::min(minimumX, projectedX);
        minimumY = std::min(minimumY, projectedY);
        maximumX = std::max(maximumX, projectedX);
        maximumY = std::max(maximumY, projectedY);
    }
    result.extentX = maximumX - minimumX;
    result.extentY = maximumY - minimumY;
    result.center =
        result.axisX * ((minimumX + maximumX) * 0.5)
        + result.axisY * ((minimumY + maximumY) * 0.5);
    result.valid =
        result.extentX > kGeometryEpsilon
        && result.extentY > kGeometryEpsilon;

    return result;
}

Affine orientedBoundsTransform(const OrientedBounds &source,
                               const OrientedBounds &target,
                               double axisXSign,
                               double axisYSign) {
    const QPointF targetAxisX = target.axisX * axisXSign;
    const QPointF targetAxisY = target.axisY * axisYSign;
    const double scaleX = target.extentX / source.extentX;
    const double scaleY = target.extentY / source.extentY;
    Affine result;
    result.a =
        targetAxisX.x() * scaleX * source.axisX.x()
        + targetAxisY.x() * scaleY * source.axisY.x();
    result.b =
        targetAxisX.y() * scaleX * source.axisX.x()
        + targetAxisY.y() * scaleY * source.axisY.x();
    result.c =
        targetAxisX.x() * scaleX * source.axisX.y()
        + targetAxisY.x() * scaleY * source.axisY.y();
    result.d =
        targetAxisX.y() * scaleX * source.axisX.y()
        + targetAxisY.y() * scaleY * source.axisY.y();
    result.e =
        target.center.x()
        - result.a * source.center.x()
        - result.c * source.center.y();
    result.f =
        target.center.y()
        - result.b * source.center.x()
        - result.d * source.center.y();

    return result;
}

QVector<CandidateJob> wholeComponentJobs(
    const Polygons &residual,
    const QVector<ShapeMesh> &catalog) {
    QVector<QPolygonF> outerPolygons;
    for (const QPolygonF &polygon : residual) {
        if (signedArea(polygon) > kGeometryEpsilon) {
            outerPolygons.push_back(polygon);
        }
    }
    if (outerPolygons.isEmpty() && !residual.isEmpty()) {
        outerPolygons.push_back(residual.front());
    }

    QVector<CandidateJob> result;
    for (const QPolygonF &polygon : outerPolygons) {
        const OrientedBounds target =
            orientedBounds(QVector<QPointF>(polygon.cbegin(), polygon.cend()));
        if (!target.valid) {
            continue;
        }
        for (const ShapeMesh &shape : catalog) {
            QVector<QPointF> shapePoints;
            shapePoints.reserve(shape.boundary.size());
            for (const Vec2 &point : shape.boundary) {
                shapePoints.push_back(QPointF(point.x, point.y));
            }
            const OrientedBounds source = orientedBounds(shapePoints);
            if (!source.valid) {
                continue;
            }
            CandidateJob job;
            job.shape = &shape;
            job.transform = orientedBoundsTransform(
                source, target, 1.0, 1.0);
            job.origin = CandidateOrigin::WholeComponent;
            job.hasTransform = true;
            result.push_back(job);
        }
    }

    return result;
}

const ShapeMesh *shapeById(const QVector<ShapeMesh> &catalog,
                           int shapeId) {
    const auto found = std::find_if(
        catalog.cbegin(), catalog.cend(),
        [shapeId](const ShapeMesh &shape) {
            return shape.id == shapeId;
        });

    return found == catalog.cend() ? nullptr : &*found;
}

QPolygonF shapePolygon(const ShapeMesh &shape) {
    QPolygonF result;
    result.reserve(shape.boundary.size());
    for (const Vec2 &point : shape.boundary) {
        result.push_back(QPointF(point.x, point.y));
    }

    return result;
}

Affine fromQTransform(const QTransform &transform) {
    return {
        transform.m11(),
        transform.m12(),
        transform.m21(),
        transform.m22(),
        transform.dx(),
        transform.dy(),
    };
}

QVector<FixedCandidate> hardEdgeCandidates(
    const QVector<ContourSpan> &boundarySpans,
    const QVector<ShapeMesh> &catalog,
    const std::function<bool()> &cancelled) {
    QVector<FixedCandidate> result;
    if (boundarySpans.size() < 3) {
        return result;
    }

    double straightLength = 0.0;
    double totalLength = 0.0;
    PolygonMeshRequest request;
    request.points.reserve(boundarySpans.size());
    for (const ContourSpan &span : boundarySpans) {
        request.points.push_back(span.start);
        const double chordLength =
            QLineF(span.start, span.end).length();
        const double spanLength = span.curved
            ? QLineF(span.start, span.control).length()
                + QLineF(span.control, span.end).length()
            : chordLength;
        totalLength += spanLength;
        if (!span.curved) {
            straightLength += chordLength;
        }
    }
    if (straightLength
        < totalLength * kHardBoundaryFraction) {
        return result;
    }

    const ShapeMesh *square = shapeById(catalog, 101);
    const ShapeMesh *triangle = shapeById(catalog, 103);
    if (square == nullptr || triangle == nullptr) {
        return result;
    }
    request.sources.square = shapePolygon(*square);
    request.sources.triangle = shapePolygon(*triangle);
    const PolygonMeshResult mesh = meshPolygon(request, cancelled);
    if (!mesh.error.isEmpty() || mesh.cancelled) {
        return result;
    }

    result.reserve(mesh.placements.size());
    for (const PolygonMeshPlacement &placement : mesh.placements) {
        const ShapeMesh *shape = shapeById(catalog, placement.shapeId);
        if (shape != nullptr) {
            result.push_back({
                shape,
                fromQTransform(placement.transform),
            });
        }
    }

    return result;
}

std::array<double, kGradientCount> affineValues(const Affine &transform) {
    return {
        transform.a, transform.b, transform.c,
        transform.d, transform.e, transform.f,
    };
}

Affine affineFromValues(const std::array<double, kGradientCount> &values) {
    return {
        values[0], values[1], values[2],
        values[3], values[4], values[5],
    };
}

bool lexicographicTransformLess(const Affine &left, const Affine &right) {
    return affineValues(left) < affineValues(right);
}

bool betterCandidate(const Candidate &left, const Candidate &right) {
    if (!left.valid) {
        return false;
    }
    if (!right.valid) {
        return true;
    }
    if (std::abs(left.covered - right.covered) > kGeometryEpsilon) {
        return left.covered > right.covered;
    }
    if (left.shapeId != right.shapeId) {
        return left.shapeId < right.shapeId;
    }

    return lexicographicTransformLess(left.transform, right.transform);
}

Candidate legalCandidate(const ShapeMesh &shape,
                         Affine transform,
                         const Polygons &residual,
                         const Polygons &mayCover,
                         const EvaluationBounds &subjectBounds,
                         const FillOptions &options,
                         CandidateProfile *profile) {
    QElapsedTimer timer;
    timer.start();
    Candidate result;
    result.shapeId = shape.id;
    for (int step = 0; step <= kLegalShrinkSteps; ++step) {
        ++profile->legalizationEvaluations;
        const AreaGradient evaluation =
            areaGradient(shape, transform, residual, mayCover, subjectBounds);
        if (finiteGradient(evaluation)
            && evaluation.spill <= options.epsSpill
            && evaluation.covered >= options.epsGain) {
            result.transform = transform;
            result.covered = evaluation.covered;
            result.spill = std::max(0.0, evaluation.spill);
            result.valid = true;
            profile->legalizationNanoseconds += timer.nsecsElapsed();

            return result;
        }
        transform.a *= kLegalShrinkFactor;
        transform.b *= kLegalShrinkFactor;
        transform.c *= kLegalShrinkFactor;
        transform.d *= kLegalShrinkFactor;
    }
    profile->legalizationNanoseconds += timer.nsecsElapsed();

    return result;
}

CandidateJobResult optimizeCandidate(
    const CandidateJob &job,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    const DistanceSeed &seed,
    const std::function<bool()> &cancelled) {
    QElapsedTimer jobTimer;
    jobTimer.start();
    CandidateJobResult result;
    const ShapeMesh &shape = *job.shape;
    std::array<double, kGradientCount> values =
        affineValues(initialTransform(job, seed));
    std::array<double, kGradientCount> firstMoment{};
    std::array<double, kGradientCount> secondMoment{};
    result.candidate = legalCandidate(
        shape, affineFromValues(values), residual, mayCover,
        subjectBounds, options, &result.profile);
    double beta1Power = 1.0;
    double beta2Power = 1.0;
    for (int iteration = 1; iteration <= options.adamIterations; ++iteration) {
        if (cancelled && cancelled()) {
            result.candidate = {};
            result.profile.totalNanoseconds = jobTimer.nsecsElapsed();
            return result;
        }
        const Affine transform = affineFromValues(values);
        QElapsedTimer evaluationTimer;
        evaluationTimer.start();
        const AreaGradient evaluation =
            areaGradient(shape, transform, residual, mayCover, subjectBounds);
        result.profile.adamEvaluationNanoseconds +=
            evaluationTimer.nsecsElapsed();
        ++result.profile.adamEvaluations;
        if (!finiteGradient(evaluation)) {
            break;
        }

        std::array<double, kGradientCount> scoreGradient{};
        double gradientNormSquared = 0.0;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            scoreGradient[parameter] = evaluation.coveredGradient[parameter]
                - options.spillWeight * evaluation.spillGradient[parameter];
            gradientNormSquared += scoreGradient[parameter]
                * scoreGradient[parameter];
        }
        const double gradientNorm = std::sqrt(gradientNormSquared);
        if (gradientNorm <= kGradientStopNorm) {
            break;
        }
        if (gradientNorm > kGradientNormLimit) {
            const double factor = kGradientNormLimit / gradientNorm;
            for (double &gradient : scoreGradient) {
                gradient *= factor;
            }
        }

        beta1Power *= kAdamBeta1;
        beta2Power *= kAdamBeta2;
        for (int parameter = 0; parameter < kGradientCount; ++parameter) {
            firstMoment[parameter] = kAdamBeta1 * firstMoment[parameter]
                + (1.0 - kAdamBeta1) * scoreGradient[parameter];
            secondMoment[parameter] = kAdamBeta2 * secondMoment[parameter]
                + (1.0 - kAdamBeta2) * scoreGradient[parameter]
                    * scoreGradient[parameter];
            const double correctedFirst = firstMoment[parameter]
                / (1.0 - beta1Power);
            const double correctedSecond = secondMoment[parameter]
                / (1.0 - beta2Power);
            values[parameter] += options.adamLearningRate * correctedFirst
                / (std::sqrt(correctedSecond) + kAdamEpsilon);
        }
        const Candidate candidate = legalCandidate(
            shape, affineFromValues(values), residual, mayCover,
            subjectBounds, options, &result.profile);
        if (betterCandidate(candidate, result.candidate)) {
            result.candidate = candidate;
        }
    }
    result.profile.totalNanoseconds = jobTimer.nsecsElapsed();

    return result;
}

bool evaluateGpuBatch(
    GpuAreaEvaluator *evaluator,
    const QVector<GpuEvaluationRequest> &requests,
    QVector<AreaGradient> *evaluations,
    FillProfile *profile,
    bool legalization) {
    const bool optimizerBackend =
        evaluator->supportsOptimizerEvaluation();
    if (!evaluator->evaluate(requests, evaluations)
        || (!legalization && optimizerBackend
            && !evaluator->supportsOptimizerEvaluation())) {
        return false;
    }
    if (legalization) {
        profile->legalizationEvaluations += requests.size();
    } else {
        profile->adamEvaluations += requests.size();
    }

    return true;
}

void evaluateCpuBatch(
    const QVector<GpuEvaluationRequest> &requests,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    QThreadPool *candidatePool,
    QVector<AreaGradient> *evaluations,
    FillProfile *profile) {
    evaluations->fill({}, requests.size());
    std::vector<qint64> durations(
        static_cast<size_t>(requests.size()));
    for (int requestIndex = 0; requestIndex < requests.size();
         ++requestIndex) {
        candidatePool->start([&, requestIndex]() {
            QElapsedTimer timer;
            timer.start();
            const GpuEvaluationRequest &request = requests[requestIndex];
            (*evaluations)[requestIndex] = areaGradient(
                *request.shape, request.transform,
                residual, mayCover, subjectBounds);
            durations[static_cast<size_t>(requestIndex)] =
                timer.nsecsElapsed();
        });
    }
    candidatePool->waitForDone();
    for (const qint64 duration : durations) {
        const double seconds = static_cast<double>(duration) * 1e-9;
        profile->candidateWorkerSeconds += seconds;
        profile->adamEvaluationWorkerSeconds += seconds;
    }
    profile->adamEvaluations += requests.size();
}

bool legalCandidatesGpu(
    const QVector<const ShapeMesh *> &shapes,
    const QVector<Affine> &initialTransforms,
    const FillOptions &options,
    GpuAreaEvaluator *evaluator,
    FillProfile *profile,
    QVector<Candidate> *candidates,
    const std::function<bool()> &cancelled,
    bool *wasCancelled) {
    QVector<int> pending;
    pending.reserve(shapes.size());
    for (int index = 0; index < shapes.size(); ++index) {
        pending.push_back(index);
    }
    QVector<Affine> transforms = initialTransforms;
    candidates->fill({}, shapes.size());
    for (int step = 0;
         step <= kLegalShrinkSteps && !pending.isEmpty();
         step += kGpuLegalizationBatchSteps) {
        if (cancelled && cancelled()) {
            *wasCancelled = true;
            return true;
        }
        const int batchSteps = std::min(
            kGpuLegalizationBatchSteps,
            kLegalShrinkSteps - step + 1);
        QVector<GpuEvaluationRequest> requests;
        requests.reserve(pending.size() * batchSteps);
        for (const int pendingIndex : pending) {
            Affine transform = transforms[pendingIndex];
            for (int batchStep = 0; batchStep < batchSteps; ++batchStep) {
                requests.push_back({
                    shapes[pendingIndex],
                    transform,
                });
                transform.a *= kLegalShrinkFactor;
                transform.b *= kLegalShrinkFactor;
                transform.c *= kLegalShrinkFactor;
                transform.d *= kLegalShrinkFactor;
            }
        }
        QVector<AreaGradient> evaluations;
        if (!evaluateGpuBatch(
                evaluator, requests, &evaluations, profile, true)) {
            return false;
        }

        QVector<int> nextPending;
        nextPending.reserve(pending.size());
        for (int pendingOffset = 0; pendingOffset < pending.size();
             ++pendingOffset) {
            const int pendingIndex = pending[pendingOffset];
            bool accepted = false;
            for (int batchStep = 0; batchStep < batchSteps; ++batchStep) {
                const int requestIndex =
                    pendingOffset * batchSteps + batchStep;
                const AreaGradient &evaluation = evaluations[requestIndex];
                const double spillSlack =
                    evaluator->usesDoublePrecision()
                    ? 0.0
                    : std::max(
                          0.05,
                          std::abs(evaluation.covered) * 1e-4);
                const double minimumCovered =
                    evaluator->usesDoublePrecision()
                    ? options.epsGain
                    : options.epsGain * 0.5;
                if (!finiteGradient(evaluation)
                    || evaluation.spill > options.epsSpill + spillSlack
                    || evaluation.covered < minimumCovered) {
                    continue;
                }
                Candidate candidate;
                candidate.transform = requests[requestIndex].transform;
                candidate.shapeId = shapes[pendingIndex]->id;
                candidate.covered = evaluation.covered;
                candidate.spill = std::max(0.0, evaluation.spill);
                candidate.valid = true;
                (*candidates)[pendingIndex] = candidate;
                accepted = true;
                break;
            }
            if (accepted) {
                continue;
            }
            for (int batchStep = 0; batchStep < batchSteps; ++batchStep) {
                transforms[pendingIndex].a *= kLegalShrinkFactor;
                transforms[pendingIndex].b *= kLegalShrinkFactor;
                transforms[pendingIndex].c *= kLegalShrinkFactor;
                transforms[pendingIndex].d *= kLegalShrinkFactor;
            }
            nextPending.push_back(pendingIndex);
        }
        pending = std::move(nextPending);
    }

    return true;
}

void exactCandidatesCpuBatch(
    const QVector<const ShapeMesh *> &shapes,
    const QVector<Candidate> &gpuCandidates,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    QThreadPool *candidatePool,
    FillProfile *profile,
    QVector<Candidate> *candidates) {
    candidates->fill({}, gpuCandidates.size());
    std::vector<CandidateProfile> profiles(
        static_cast<size_t>(gpuCandidates.size()));
    for (int index = 0; index < gpuCandidates.size(); ++index) {
        if (!gpuCandidates[index].valid) {
            continue;
        }
        candidatePool->start([&, index]() {
            (*candidates)[index] = legalCandidate(
                *shapes[index], gpuCandidates[index].transform,
                residual, mayCover, subjectBounds, options,
                &profiles[static_cast<size_t>(index)]);
        });
    }
    candidatePool->waitForDone();
    for (const CandidateProfile &candidateProfile : profiles) {
        const double seconds = static_cast<double>(
            candidateProfile.legalizationNanoseconds) * 1e-9;
        profile->candidateWorkerSeconds += seconds;
        profile->legalizationWorkerSeconds += seconds;
        profile->legalizationEvaluations +=
            candidateProfile.legalizationEvaluations;
    }
}

bool optimizeCandidatesGpu(
    const std::vector<CandidateJob> &jobs,
    const Polygons &residual,
    const Polygons &mayCover,
    const EvaluationBounds &subjectBounds,
    const FillOptions &options,
    const DistanceSeed &seed,
    GpuAreaEvaluator *evaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    const std::function<bool()> &cancelled,
    std::vector<Candidate> *results,
    bool *wasCancelled) {
    QVector<GpuCandidateState> states;
    states.reserve(static_cast<qsizetype>(jobs.size()));
    for (const CandidateJob &job : jobs) {
        GpuCandidateState state;
        state.job = &job;
        state.values = affineValues(initialTransform(job, seed));
        states.push_back(state);
    }

    QVector<const ShapeMesh *> initialShapes;
    QVector<Affine> initialTransforms;
    initialShapes.reserve(states.size());
    initialTransforms.reserve(states.size());
    for (int index = 0; index < states.size(); ++index) {
        initialShapes.push_back(states[index].job->shape);
        initialTransforms.push_back(affineFromValues(states[index].values));
    }
    QVector<Candidate> initialGpuCandidates;
    if (!legalCandidatesGpu(
            initialShapes, initialTransforms, options, evaluator, profile,
            &initialGpuCandidates, cancelled, wasCancelled)) {
        return false;
    }
    QVector<Candidate> initialCandidates = initialGpuCandidates;
    if (!evaluator->usesDoublePrecision()) {
        exactCandidatesCpuBatch(
            initialShapes, initialGpuCandidates, residual, mayCover,
            subjectBounds, options, candidatePool, profile,
            &initialCandidates);
    }
    for (int index = 0; index < states.size(); ++index) {
        states[index].best = initialCandidates[index];
        states[index].bestGpu = initialGpuCandidates[index];
    }

    for (int iteration = 1; iteration <= options.adamIterations;
         ++iteration) {
        if (*wasCancelled || (cancelled && cancelled())) {
            *wasCancelled = true;
            break;
        }

        QVector<int> activeIndices;
        QVector<GpuEvaluationRequest> requests;
        for (int index = 0; index < states.size(); ++index) {
            if (!states[index].active) {
                continue;
            }
            activeIndices.push_back(index);
            requests.push_back({
                states[index].job->shape,
                affineFromValues(states[index].values),
            });
        }
        if (requests.isEmpty()) {
            break;
        }

        QVector<AreaGradient> evaluations;
        if (evaluator->supportsOptimizerEvaluation()) {
            if (!evaluateGpuBatch(
                    evaluator, requests, &evaluations,
                    profile, false)) {
                return false;
            }
        } else {
            evaluateCpuBatch(
                requests, residual, mayCover, subjectBounds,
                candidatePool, &evaluations, profile);
        }
        QVector<int> legalIndices;
        QVector<const ShapeMesh *> legalShapes;
        QVector<Affine> legalTransforms;
        for (int requestIndex = 0; requestIndex < activeIndices.size();
             ++requestIndex) {
            GpuCandidateState &state =
                states[activeIndices[requestIndex]];
            const AreaGradient &evaluation = evaluations[requestIndex];
            if (!finiteGradient(evaluation)) {
                state.active = false;
                continue;
            }

            std::array<double, kGradientCount> scoreGradient{};
            double gradientNormSquared = 0.0;
            for (int parameter = 0; parameter < kGradientCount;
                 ++parameter) {
                scoreGradient[parameter] =
                    evaluation.coveredGradient[parameter]
                    - options.spillWeight
                        * evaluation.spillGradient[parameter];
                gradientNormSquared += scoreGradient[parameter]
                    * scoreGradient[parameter];
            }
            const double gradientNorm = std::sqrt(gradientNormSquared);
            if (gradientNorm <= kGradientStopNorm) {
                state.active = false;
                continue;
            }
            if (gradientNorm > kGradientNormLimit) {
                const double factor = kGradientNormLimit / gradientNorm;
                for (double &gradient : scoreGradient) {
                    gradient *= factor;
                }
            }

            state.beta1Power *= kAdamBeta1;
            state.beta2Power *= kAdamBeta2;
            for (int parameter = 0; parameter < kGradientCount;
                 ++parameter) {
                state.firstMoment[parameter] =
                    kAdamBeta1 * state.firstMoment[parameter]
                    + (1.0 - kAdamBeta1) * scoreGradient[parameter];
                state.secondMoment[parameter] =
                    kAdamBeta2 * state.secondMoment[parameter]
                    + (1.0 - kAdamBeta2) * scoreGradient[parameter]
                        * scoreGradient[parameter];
                const double correctedFirst =
                    state.firstMoment[parameter]
                    / (1.0 - state.beta1Power);
                const double correctedSecond =
                    state.secondMoment[parameter]
                    / (1.0 - state.beta2Power);
                state.values[parameter] +=
                    options.adamLearningRate * correctedFirst
                    / (std::sqrt(correctedSecond) + kAdamEpsilon);
            }
            legalIndices.push_back(activeIndices[requestIndex]);
            legalShapes.push_back(state.job->shape);
            legalTransforms.push_back(affineFromValues(state.values));
        }
        if (legalIndices.isEmpty()) {
            continue;
        }

        QVector<Candidate> gpuLegalCandidates;
        if (!legalCandidatesGpu(
                legalShapes, legalTransforms, options, evaluator,
                profile, &gpuLegalCandidates, cancelled, wasCancelled)) {
            return false;
        }
        QVector<int> competitiveIndices;
        QVector<const ShapeMesh *> competitiveShapes;
        QVector<Candidate> competitiveGpuCandidates;
        for (int index = 0; index < legalIndices.size(); ++index) {
            GpuCandidateState &state = states[legalIndices[index]];
            if (betterCandidate(
                    gpuLegalCandidates[index], state.bestGpu)) {
                state.bestGpu = gpuLegalCandidates[index];
                if (evaluator->usesDoublePrecision()) {
                    if (betterCandidate(
                            gpuLegalCandidates[index], state.best)) {
                        state.best = gpuLegalCandidates[index];
                    }
                    continue;
                }
                competitiveIndices.push_back(legalIndices[index]);
                competitiveShapes.push_back(state.job->shape);
                competitiveGpuCandidates.push_back(
                    gpuLegalCandidates[index]);
            }
        }
        QVector<Candidate> competitiveCandidates;
        exactCandidatesCpuBatch(
            competitiveShapes, competitiveGpuCandidates,
            residual, mayCover, subjectBounds, options,
            candidatePool, profile, &competitiveCandidates);
        for (int index = 0; index < competitiveIndices.size(); ++index) {
            GpuCandidateState &state = states[competitiveIndices[index]];
            if (betterCandidate(competitiveCandidates[index], state.best)) {
                state.best = competitiveCandidates[index];
            }
        }
    }

    results->clear();
    results->reserve(jobs.size());
    for (const GpuCandidateState &state : states) {
        results->push_back(*wasCancelled ? Candidate{} : state.best);
    }

    return true;
}

CandidateInitialization candidateInitialization(
    const DistanceSeed &seed,
    int restart,
    std::mt19937_64 *random) {
    CandidateInitialization result;
    if (restart == 0) {
        return result;
    }

    std::uniform_real_distribution<double> unitDistribution(-1.0, 1.0);
    result.angleOffset = unitDistribution(*random) * kRestartAngleRange;
    result.scaleFactor =
        1.0 + unitDistribution(*random) * kRestartScaleRange;
    result.translationOffset =
        QPointF(unitDistribution(*random), unitDistribution(*random))
        * (seed.radius * kRestartTranslationFraction);

    return result;
}

double polygonPerimeter(const Polygons &polygons) {
    double result = 0.0;
    for (const QPolygonF &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            result += QLineF(
                polygon[index],
                polygon[(index + 1) % polygon.size()]).length();
        }
    }

    return result;
}

ResidualComplexity residualComplexity(const Polygons &polygons) {
    ResidualComplexity result;
    QVector<QPointF> points;
    for (const QPolygonF &polygon : polygons) {
        points += polygon;
        if (signedArea(polygon) >= 0.0) {
            ++result.components;
        } else {
            ++result.holes;
        }
    }
    if (!polygons.isEmpty() && result.components == 0) {
        result.components = polygons.size();
        result.holes = 0;
    }
    const double area = std::max(
        kGeometryEpsilon, polygonSetArea(polygons));
    const double perimeter = polygonPerimeter(polygons);
    result.score =
        perimeter * perimeter / (4.0 * kPi * area)
        + kComponentComplexityWeight
            * std::max(0, result.components - 1)
        + kHoleComplexityWeight * result.holes;

    return result;
}

Polygons componentAtPoint(const Polygons &polygons,
                          const QPointF &point) {
    int outerIndex = -1;
    double outerArea = std::numeric_limits<double>::max();
    for (int index = 0; index < polygons.size(); ++index) {
        const double area = signedArea(polygons[index]);
        if (area > kGeometryEpsilon
            && std::abs(area) < outerArea
            && polygons[index].containsPoint(point, Qt::OddEvenFill)) {
            outerIndex = index;
            outerArea = std::abs(area);
        }
    }
    if (outerIndex < 0) {
        for (int index = 0; index < polygons.size(); ++index) {
            const double area = std::abs(signedArea(polygons[index]));
            if (area > kGeometryEpsilon
                && (outerIndex < 0 || area > outerArea)) {
                outerIndex = index;
                outerArea = area;
            }
        }
    }
    if (outerIndex < 0) {
        return polygons;
    }

    Polygons result{polygons[outerIndex]};
    const QPolygonF &outer = polygons[outerIndex];
    for (int index = 0; index < polygons.size(); ++index) {
        if (index == outerIndex || polygons[index].isEmpty()
            || signedArea(polygons[index]) >= 0.0) {
            continue;
        }
        if (outer.containsPoint(
                polygons[index].front(), Qt::OddEvenFill)) {
            result.push_back(polygons[index]);
        }
    }

    return result;
}

double descriptorAspect(const QRectF &bounds) {
    const double minimum = std::max(
        kGeometryEpsilon,
        std::min(bounds.width(), bounds.height()));
    const double maximum =
        std::max(bounds.width(), bounds.height());

    return maximum / minimum;
}

double descriptorDistance(const ShapeMesh &shape,
                          const Polygons &residual) {
    const QRectF residualBounds = polygonBounds(residual);
    const QRectF shapeBoundsValue = shapeBounds(shape);
    const double residualArea = std::max(
        kGeometryEpsilon,
        residualBounds.width() * residualBounds.height());
    const double shapeArea = std::max(
        kGeometryEpsilon,
        shapeBoundsValue.width() * shapeBoundsValue.height());
    const double residualFill =
        polygonSetArea(residual) / residualArea;
    const double shapeFill = shape.area / shapeArea;

    return std::abs(
               std::log(descriptorAspect(shapeBoundsValue))
               - std::log(descriptorAspect(residualBounds)))
        + 2.0 * std::abs(shapeFill - residualFill);
}

QVector<const ShapeMesh *> routedShapes(const Polygons &residual,
                                        const QVector<ShapeMesh> &catalog,
                                        bool useRouter) {
    QVector<const ShapeMesh *> result;
    for (const ShapeMesh &shape : catalog) {
        if (shape.valid()) {
            result.push_back(&shape);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const ShapeMesh *left, const ShapeMesh *right) {
                  return left->id < right->id;
              });
    if (!useRouter || result.size() <= kRouterCandidateCount) {
        return result;
    }

    std::stable_sort(result.begin(), result.end(),
                     [&residual](
                         const ShapeMesh *left, const ShapeMesh *right) {
                         const double leftDistance =
                             descriptorDistance(*left, residual);
                         const double rightDistance =
                             descriptorDistance(*right, residual);
                         if (std::abs(leftDistance - rightDistance)
                             > kGeometryEpsilon) {
                             return leftDistance < rightDistance;
                         }
                         return left->id < right->id;
                     });
    result.resize(kRouterCandidateCount);

    return result;
}

Candidate fixedCandidate(const FixedCandidate &fixed,
                         const Polygons &residual,
                         const Polygons &mayCover,
                         const EvaluationBounds &subjectBounds,
                         const FillOptions &options,
                         CandidateProfile *profile) {
    QElapsedTimer timer;
    timer.start();
    const AreaGradient evaluation = areaGradient(
        *fixed.shape, fixed.transform,
        residual, mayCover, subjectBounds);
    ++profile->legalizationEvaluations;
    profile->legalizationNanoseconds += timer.nsecsElapsed();
    Candidate result;
    if (!finiteGradient(evaluation)
        || evaluation.spill > options.epsSpill
        || evaluation.covered < options.epsGain) {
        return result;
    }
    result.transform = fixed.transform;
    result.shapeId = fixed.shape->id;
    result.covered = evaluation.covered;
    result.spill = std::max(0.0, evaluation.spill);
    result.origin = CandidateOrigin::HardEdge;
    result.valid = true;

    return result;
}

CandidateSelection selectCandidate(
    const QVector<Candidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &residual,
    double epsGain) {
    CandidateSelection result;
    const bool havePrimary = std::any_of(
        candidates.cbegin(), candidates.cend(),
        [](const Candidate &candidate) {
            return candidate.valid
                && candidate.origin
                    != CandidateOrigin::LocalComponent;
        });
    if (!havePrimary) {
        return result;
    }
    double maximumCovered = 0.0;
    for (const Candidate &candidate : candidates) {
        if (candidate.valid
            && candidate.origin
                != CandidateOrigin::LocalComponent) {
            maximumCovered =
                std::max(maximumCovered, candidate.covered);
        }
    }
    if (maximumCovered < epsGain) {
        return result;
    }

    struct ScoredCandidate {
        Candidate candidate;
        Polygons residual;
        double exactGain = 0.0;
        double complexity = 0.0;
    };
    QVector<ScoredCandidate> scored;
    const double previousArea = polygonSetArea(residual);
    for (const Candidate &candidate : candidates) {
        if (!candidate.valid) {
            continue;
        }
        const bool local =
            candidate.origin == CandidateOrigin::LocalComponent;
        if ((local
             && candidate.covered
                 < maximumCovered
                     * kLocalSelectionGainAdvantage)
            || (!local
                && candidate.covered
                    < maximumCovered
                        * kComplexityGainWindow)) {
            continue;
        }
        const ShapeMesh *shape = shapeById(catalog, candidate.shapeId);
        if (shape == nullptr) {
            continue;
        }
        const Polygons footprint{
            transformedBoundary(*shape, candidate.transform),
        };
        Polygons nextResidual =
            differencePolygons(residual, footprint);
        const double exactGain =
            previousArea - polygonSetArea(nextResidual);
        if (!std::isfinite(exactGain) || exactGain < epsGain) {
            continue;
        }
        scored.push_back({
            candidate,
            std::move(nextResidual),
            exactGain,
            0.0,
        });
    }
    if (scored.isEmpty()) {
        return result;
    }

    double primaryComplexity =
        std::numeric_limits<double>::max();
    for (ScoredCandidate &candidate : scored) {
        candidate.complexity =
            residualComplexity(candidate.residual).score;
        if (candidate.candidate.origin
            != CandidateOrigin::LocalComponent) {
            primaryComplexity = std::min(
                primaryComplexity, candidate.complexity);
        }
    }
    int areaWinner = -1;
    for (int index = 0; index < scored.size(); ++index) {
        if (scored[index].candidate.origin
                == CandidateOrigin::LocalComponent
            && scored[index].complexity
                > primaryComplexity
                    * kLocalSelectionComplexityRatio) {
            continue;
        }
        if (areaWinner < 0
            || betterCandidate(
                scored[index].candidate,
                scored[areaWinner].candidate)) {
            areaWinner = index;
        }
    }
    if (areaWinner < 0) {
        return result;
    }
    int winner = -1;
    for (int index = 0; index < scored.size(); ++index) {
        if (std::abs(
                scored[index].candidate.covered
                - scored[areaWinner].candidate.covered)
            > kGeometryEpsilon) {
            continue;
        }
        if (scored[index].candidate.origin
                == CandidateOrigin::LocalComponent
            && scored[index].complexity
                > primaryComplexity
                    * kLocalSelectionComplexityRatio) {
            continue;
        }
        if (winner < 0
            || scored[index].complexity
                < scored[winner].complexity - kGeometryEpsilon
            || (std::abs(
                    scored[index].complexity
                    - scored[winner].complexity)
                    <= kGeometryEpsilon
                && betterCandidate(
                    scored[index].candidate,
                    scored[winner].candidate))) {
            winner = index;
        }
    }
    result.candidate = scored[winner].candidate;
    result.candidate.covered = scored[winner].exactGain;
    result.residual = std::move(scored[winner].residual);
    result.exactGain = scored[winner].exactGain;
    result.complexityPreferred = winner != areaWinner;
    result.valid = true;

    return result;
}

Polygons placementFootprints(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog) {
    Polygons result;
    result.reserve(placements.size());
    for (const Placement &placement : placements) {
        const ShapeMesh *shape =
            shapeById(catalog, placement.shapeId);
        if (shape != nullptr) {
            result.push_back(
                transformedBoundary(*shape, placement.transform));
        }
    }

    return result;
}

ExactCoverState exactCoverState(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea) {
    ExactCoverState result;
    result.footprints =
        placementFootprints(placements, catalog);
    result.coverage = unionPolygons(result.footprints);
    result.residual =
        differencePolygons(mustCover, result.coverage);
    result.residualArea = polygonSetArea(result.residual);
    result.coveredArea =
        std::max(0.0, targetArea - result.residualArea);
    result.outsideArea = polygonSetArea(
        differencePolygons(
            result.coverage, mayCover));

    return result;
}

void refreshPlacementGains(
    QVector<Placement> *placements,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover) {
    Polygons residual = mustCover;
    double previousArea = polygonSetArea(residual);
    for (Placement &placement : *placements) {
        const ShapeMesh *shape =
            shapeById(catalog, placement.shapeId);
        if (shape == nullptr) {
            placement.coveredArea = 0.0;
            continue;
        }
        const Polygons footprint{
            transformedBoundary(*shape, placement.transform),
        };
        Polygons nextResidual =
            differencePolygons(residual, footprint);
        const double nextArea = polygonSetArea(nextResidual);
        placement.coveredArea =
            std::max(0.0, previousArea - nextArea);
        residual = std::move(nextResidual);
        previousArea = nextArea;
    }
}

QVector<Placement> scaledMeshPlacements(
    const QVector<Placement> &placements,
    const QPointF &center,
    double scale) {
    QVector<Placement> result = placements;
    for (Placement &placement : result) {
        placement.transform.a *= scale;
        placement.transform.b *= scale;
        placement.transform.c *= scale;
        placement.transform.d *= scale;
        placement.transform.e =
            placement.transform.e * scale
            + center.x() * (1.0 - scale);
        placement.transform.f =
            placement.transform.f * scale
            + center.y() * (1.0 - scale);
    }

    return result;
}

MeshCoverPlan meshCoverPlan(
    const QVector<FixedCandidate> &candidates,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    const std::function<bool()> &cancelled) {
    MeshCoverPlan result;
    if (candidates.isEmpty()) {
        result.reason =
            QStringLiteral("polygon mesh is unavailable");
        return result;
    }
    if (candidates.size() > options.budget) {
        result.reason =
            QStringLiteral("polygon mesh exceeds the shape budget");
        return result;
    }

    QVector<Placement> placements;
    placements.reserve(candidates.size());
    for (const FixedCandidate &candidate : candidates) {
        placements.push_back({
            candidate.transform,
            candidate.shape->id,
            0.0,
        });
    }
    const QPointF center =
        polygonBounds(mustCover).center();
    const double outsideLimit =
        options.epsSpill
        * static_cast<double>(placements.size());
    for (double scale = 1.0;
         scale + kGeometryEpsilon
             >= kMeshMinimumLegalScale;
         scale -= kMeshLegalScaleStep) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.reason =
                QStringLiteral("cancelled");
            return result;
        }
        QVector<Placement> trial =
            scaledMeshPlacements(
                placements, center, scale);
        ExactCoverState state =
            exactCoverState(
                trial, catalog,
                mustCover, mayCover,
                targetArea);
        if (state.outsideArea
                > outsideLimit
                    + kGeometryEpsilon) {
            continue;
        }
        result.placements =
            std::move(trial);
        result.residual =
            std::move(state.residual);
        result.residualArea =
            state.residualArea;
        result.outsideArea =
            state.outsideArea;
        result.coverageRatio =
            targetArea > kGeometryEpsilon
            ? state.coveredArea / targetArea
            : 0.0;
        result.scale = scale;
        result.accepted =
            result.coverageRatio
            >= kMeshMinimumCompactCoverageRatio;
        result.reason = result.accepted
            ? QStringLiteral("compact polygon mesh")
            : QStringLiteral("polygon mesh coverage is insufficient");
        refreshPlacementGains(
            &result.placements,
            catalog, mustCover);
        return result;
    }
    result.reason =
        QStringLiteral("polygon mesh legalization failed");

    return result;
}

double pointCross(const QPointF &left, const QPointF &right) {
    return left.x() * right.y() - left.y() * right.x();
}

QPointF canonicalDirection(const QPointF &delta) {
    const double length = std::hypot(delta.x(), delta.y());
    if (length <= kGeometryEpsilon) {
        return {};
    }
    QPointF result = delta / length;
    if (result.x() < 0.0
        || (std::abs(result.x()) <= kGeometryEpsilon
            && result.y() < 0.0)) {
        result = -result;
    }

    return result;
}

double directionSinDistance(const QPointF &left,
                            const QPointF &right) {
    return std::abs(pointCross(left, right));
}

StructuralAxes structuralAxes(
    const QVector<StructuralEdge> &edges) {
    StructuralAxes result;
    double totalLength = 0.0;
    for (const StructuralEdge &edge : edges) {
        totalLength += edge.length;
    }
    if (edges.size() < 4 || totalLength <= kGeometryEpsilon) {
        return result;
    }

    int bestFirst = -1;
    int bestSecond = -1;
    double bestExplained = -1.0;
    double bestError = std::numeric_limits<double>::max();
    for (int first = 0; first < edges.size(); ++first) {
        for (int second = first + 1; second < edges.size(); ++second) {
            const QPointF firstAxis = edges[first].direction;
            const QPointF secondAxis = edges[second].direction;
            if (directionSinDistance(firstAxis, secondAxis)
                < kStructuralMinimumAxisSeparationSin) {
                continue;
            }
            double explained = 0.0;
            double error = 0.0;
            for (const StructuralEdge &edge : edges) {
                const double distance = std::min(
                    directionSinDistance(edge.direction, firstAxis),
                    directionSinDistance(edge.direction, secondAxis));
                if (distance <= kStructuralDirectionSinTolerance) {
                    explained += edge.length;
                }
                error += edge.length * distance;
            }
            if (explained > bestExplained + kGeometryEpsilon
                || (std::abs(explained - bestExplained)
                        <= kGeometryEpsilon
                    && error < bestError - kGeometryEpsilon)) {
                bestFirst = first;
                bestSecond = second;
                bestExplained = explained;
                bestError = error;
            }
        }
    }
    if (bestFirst < 0 || bestSecond < 0) {
        return result;
    }

    QPointF firstSum;
    QPointF secondSum;
    for (const StructuralEdge &edge : edges) {
        const double firstDistance =
            directionSinDistance(
                edge.direction,
                edges[bestFirst].direction);
        const double secondDistance =
            directionSinDistance(
                edge.direction,
                edges[bestSecond].direction);
        if (std::min(firstDistance, secondDistance)
            > kStructuralDirectionSinTolerance) {
            continue;
        }
        if (firstDistance <= secondDistance) {
            firstSum += edge.direction * edge.length;
        } else {
            secondSum += edge.direction * edge.length;
        }
    }
    result.first = canonicalDirection(firstSum);
    result.second = canonicalDirection(secondSum);
    result.determinant =
        pointCross(result.first, result.second);
    if (std::abs(result.determinant)
        < kStructuralMinimumAxisSeparationSin) {
        return {};
    }
    if (result.determinant < 0.0) {
        std::swap(result.first, result.second);
        result.determinant = -result.determinant;
    }

    double explained = 0.0;
    for (const StructuralEdge &edge : edges) {
        if (std::min(
                directionSinDistance(
                    edge.direction, result.first),
                directionSinDistance(
                    edge.direction, result.second))
            <= kStructuralDirectionSinTolerance) {
            explained += edge.length;
        }
    }
    result.explainedBoundaryFraction =
        explained / totalLength;
    result.valid =
        result.explainedBoundaryFraction
        >= kStructuralMinimumExplainedBoundaryFraction;

    return result;
}

QPointF structuralCoordinates(
    const QPointF &point,
    const StructuralAxes &axes) {
    return {
        pointCross(point, axes.second)
            / axes.determinant,
        pointCross(axes.first, point)
            / axes.determinant,
    };
}

QPointF structuralPoint(
    double first,
    double second,
    const StructuralAxes &axes) {
    return axes.first * first
        + axes.second * second;
}

QVector<double> clusteredCoordinates(
    QVector<double> values,
    double tolerance) {
    std::sort(values.begin(), values.end());
    QVector<double> result;
    QVector<int> counts;
    for (const double value : values) {
        if (result.isEmpty()
            || value - result.back() > tolerance) {
            result.push_back(value);
            counts.push_back(1);
            continue;
        }
        const int count = counts.back();
        result.back() =
            (result.back() * count + value)
            / static_cast<double>(count + 1);
        counts.back() = count + 1;
    }

    return result;
}

double nearestCoordinate(
    double value,
    const QVector<double> &coordinates) {
    const auto found = std::min_element(
        coordinates.cbegin(), coordinates.cend(),
        [value](double left, double right) {
            return std::abs(left - value)
                < std::abs(right - value);
        });

    return found == coordinates.cend()
        ? value : *found;
}

bool sameCoordinate(double left, double right) {
    return std::abs(left - right)
        <= kGeometryEpsilon;
}

QPolygonF simplifyStructuralPolygon(QPolygonF polygon) {
    bool changed = true;
    while (changed && polygon.size() >= 3) {
        changed = false;
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF previous =
                polygon[
                    (index + polygon.size() - 1)
                    % polygon.size()];
            const QPointF current = polygon[index];
            const QPointF next =
                polygon[(index + 1) % polygon.size()];
            if (QLineF(previous, current).length()
                    <= kGeometryEpsilon
                || (sameCoordinate(
                        previous.x(), current.x())
                    && sameCoordinate(
                        current.x(), next.x()))
                || (sameCoordinate(
                        previous.y(), current.y())
                    && sameCoordinate(
                        current.y(), next.y()))) {
                polygon.removeAt(index);
                changed = true;
                break;
            }
        }
    }

    return polygon;
}

QVector<double> structuralSupportCoordinates(
    const QPolygonF &polygon,
    bool firstCoordinate) {
    QVector<double> result;
    result.reserve(polygon.size());
    for (const QPointF &point : polygon) {
        result.push_back(
            firstCoordinate ? point.x() : point.y());
    }
    std::sort(result.begin(), result.end());
    result.erase(
        std::unique(
            result.begin(), result.end(),
            [](double left, double right) {
                return sameCoordinate(left, right);
            }),
        result.end());

    return result;
}

QVector<StructuralRectangle> structuralRectangles(
    const QPolygonF &polygon,
    const QVector<double> &firstCoordinates,
    const QVector<double> &secondCoordinates,
    quint64 *occupiedMask,
    int *occupiedCount) {
    const int firstCells =
        firstCoordinates.size() - 1;
    const int secondCells =
        secondCoordinates.size() - 1;
    *occupiedMask = 0;
    for (int second = 0;
         second < secondCells; ++second) {
        for (int first = 0;
             first < firstCells; ++first) {
            const QPointF center(
                (firstCoordinates[first]
                 + firstCoordinates[first + 1])
                    * 0.5,
                (secondCoordinates[second]
                 + secondCoordinates[second + 1])
                    * 0.5);
            if (polygon.containsPoint(
                    center, Qt::OddEvenFill)) {
                const int bit =
                    second * firstCells + first;
                *occupiedMask |= quint64{1} << bit;
            }
        }
    }
    *occupiedCount =
        std::popcount(*occupiedMask);

    QVector<StructuralRectangle> result;
    for (int top = 0; top < secondCells; ++top) {
        for (int bottom = top + 1;
             bottom <= secondCells; ++bottom) {
            for (int left = 0;
                 left < firstCells; ++left) {
                quint64 mask = 0;
                for (int right = left + 1;
                     right <= firstCells; ++right) {
                    bool full = true;
                    for (int second = top;
                         second < bottom; ++second) {
                        const int bit =
                            second * firstCells
                            + right - 1;
                        mask |= quint64{1} << bit;
                        if ((*occupiedMask
                             & (quint64{1} << bit))
                            == 0) {
                            full = false;
                        }
                    }
                    if (full) {
                        result.push_back({
                            mask,
                            left,
                            right,
                            top,
                            bottom,
                        });
                    } else {
                        break;
                    }
                }
            }
        }
    }

    QVector<StructuralRectangle> maximal;
    for (int index = 0; index < result.size(); ++index) {
        bool dominated = false;
        for (int other = 0;
             other < result.size(); ++other) {
            if (index == other
                || result[index].mask
                    == result[other].mask) {
                continue;
            }
            if ((result[index].mask
                 & ~result[other].mask)
                == 0) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            maximal.push_back(result[index]);
        }
    }
    std::stable_sort(
        maximal.begin(), maximal.end(),
        [](const StructuralRectangle &left,
           const StructuralRectangle &right) {
            const int leftCount =
                std::popcount(left.mask);
            const int rightCount =
                std::popcount(right.mask);
            if (leftCount != rightCount) {
                return leftCount > rightCount;
            }
            return std::tie(
                       left.top, left.left,
                       left.bottom, left.right)
                < std::tie(
                       right.top, right.left,
                       right.bottom, right.right);
        });

    return maximal;
}

QVector<StructuralRectangle> minimumRectangleCover(
    quint64 occupiedMask,
    const QVector<StructuralRectangle> &rectangles,
    bool *searchLimited) {
    QVector<QVector<int>> rectanglesByCell(
        kStructuralMaximumGridCells);
    for (int index = 0;
         index < rectangles.size(); ++index) {
        for (int bit = 0;
             bit < kStructuralMaximumGridCells; ++bit) {
            if ((rectangles[index].mask
                 & (quint64{1} << bit))
                != 0) {
                rectanglesByCell[bit].push_back(index);
            }
        }
    }

    QVector<int> best;
    quint64 greedyCovered = 0;
    while ((greedyCovered & occupiedMask)
           != occupiedMask) {
        int bestIndex = -1;
        int bestGain = 0;
        for (int index = 0;
             index < rectangles.size(); ++index) {
            const int gain = std::popcount(
                rectangles[index].mask
                & occupiedMask
                & ~greedyCovered);
            if (gain > bestGain) {
                bestIndex = index;
                bestGain = gain;
            }
        }
        if (bestIndex < 0) {
            return {};
        }
        best.push_back(bestIndex);
        greedyCovered |= rectangles[bestIndex].mask;
    }

    QVector<int> current;
    int searchedNodes = 0;
    *searchLimited = false;
    std::function<void(quint64)> search =
        [&](quint64 covered) {
            if (++searchedNodes
                > kStructuralSearchNodeLimit) {
                *searchLimited = true;
                return;
            }
            if ((covered & occupiedMask)
                == occupiedMask) {
                if (current.size() < best.size()) {
                    best = current;
                }
                return;
            }
            if (current.size() + 1 >= best.size()) {
                return;
            }

            const quint64 uncovered =
                occupiedMask & ~covered;
            int selectedBit = -1;
            int selectedOptions =
                std::numeric_limits<int>::max();
            for (int bit = 0;
                 bit < kStructuralMaximumGridCells;
                 ++bit) {
                if ((uncovered
                     & (quint64{1} << bit))
                    == 0) {
                    continue;
                }
                int options = 0;
                for (const int rectangle :
                     rectanglesByCell[bit]) {
                    if ((rectangles[rectangle].mask
                         & uncovered)
                        != 0) {
                        ++options;
                    }
                }
                if (options < selectedOptions) {
                    selectedBit = bit;
                    selectedOptions = options;
                }
            }
            if (selectedBit < 0
                || selectedOptions == 0) {
                return;
            }
            for (const int rectangle :
                 rectanglesByCell[selectedBit]) {
                const quint64 next =
                    covered
                    | rectangles[rectangle].mask;
                if (next == covered) {
                    continue;
                }
                current.push_back(rectangle);
                search(next);
                current.removeLast();
                if (*searchLimited) {
                    return;
                }
            }
        };
    search(0);

    QVector<StructuralRectangle> result;
    result.reserve(best.size());
    for (const int index : best) {
        result.push_back(rectangles[index]);
    }

    return result;
}

Affine structuralRectangleTransform(
    const ShapeMesh &square,
    const StructuralAxes &axes,
    const QVector<double> &firstCoordinates,
    const QVector<double> &secondCoordinates,
    const StructuralRectangle &rectangle,
    double firstScale,
    double secondScale,
    double firstOffset = 0.0,
    double secondOffset = 0.0) {
    const QRectF source = shapeBounds(square);
    const double firstCenter =
        (firstCoordinates[rectangle.left]
         + firstCoordinates[rectangle.right])
        * 0.5 + firstOffset;
    const double secondCenter =
        (secondCoordinates[rectangle.top]
         + secondCoordinates[rectangle.bottom])
        * 0.5 + secondOffset;
    const double firstExtent =
        (firstCoordinates[rectangle.right]
         - firstCoordinates[rectangle.left])
        * firstScale;
    const double secondExtent =
        (secondCoordinates[rectangle.bottom]
         - secondCoordinates[rectangle.top])
        * secondScale;
    const QPointF targetOrigin =
        structuralPoint(
            firstCenter - firstExtent * 0.5,
            secondCenter - secondExtent * 0.5,
            axes);
    const QPointF firstVector =
        axes.first * firstExtent;
    const QPointF secondVector =
        axes.second * secondExtent;
    Affine result;
    result.a =
        firstVector.x() / source.width();
    result.b =
        firstVector.y() / source.width();
    result.c =
        secondVector.x() / source.height();
    result.d =
        secondVector.y() / source.height();
    result.e =
        targetOrigin.x()
        - result.a * source.left()
        - result.c * source.top();
    result.f =
        targetOrigin.y()
        - result.b * source.left()
        - result.d * source.top();

    return result;
}

std::optional<Affine> legalStructuralRectangle(
    const ShapeMesh &square,
    const StructuralAxes &axes,
    const QVector<double> &firstCoordinates,
    const QVector<double> &secondCoordinates,
    const StructuralRectangle &rectangle,
    const Polygons &mayCover,
    double outsideAllowance,
    double translationTolerance,
    const std::function<bool()> &cancelled) {
    constexpr std::array<double, 9>
        kTranslationFactors = {
            0.0,
            -0.25, 0.25,
            -0.5, 0.5,
            -0.75, 0.75,
            -1.0, 1.0,
        };
    const auto translatedCandidate =
        [&](double firstScale,
            double secondScale) {
            Affine bestTransform;
            double bestOutsideArea =
                std::numeric_limits<double>::max();
            for (const double firstFactor :
                 kTranslationFactors) {
                for (const double secondFactor :
                     kTranslationFactors) {
                    const Affine transform =
                        structuralRectangleTransform(
                            square, axes,
                            firstCoordinates,
                            secondCoordinates,
                            rectangle,
                            firstScale,
                            secondScale,
                            firstFactor
                                * translationTolerance,
                            secondFactor
                                * translationTolerance);
                    const Polygons footprint{
                        transformedBoundary(
                            square, transform),
                    };
                    const double outsideArea =
                        polygonSetArea(
                            differencePolygons(
                                footprint,
                                mayCover));
                    if (outsideArea
                        < bestOutsideArea
                            - kGeometryEpsilon) {
                        bestTransform = transform;
                        bestOutsideArea =
                            outsideArea;
                    }
                }
            }

            return std::make_pair(
                bestTransform,
                bestOutsideArea);
        };
    double firstScale = 1.0;
    double secondScale = 1.0;
    while (firstScale
               >= kStructuralMinimumLegalScale
                   - kGeometryEpsilon
           && secondScale
               >= kStructuralMinimumLegalScale
                   - kGeometryEpsilon) {
        if (cancelled && cancelled()) {
            return std::nullopt;
        }
        const auto current =
            translatedCandidate(
                firstScale,
                secondScale);
        if (current.second
            <= outsideAllowance
                + kGeometryEpsilon) {
            return current.first;
        }
        const bool canShrinkFirst =
            firstScale
                - kStructuralLegalScaleStep
            >= kStructuralMinimumLegalScale
                - kGeometryEpsilon;
        const bool canShrinkSecond =
            secondScale
                - kStructuralLegalScaleStep
            >= kStructuralMinimumLegalScale
                - kGeometryEpsilon;
        if (!canShrinkFirst
            && !canShrinkSecond) {
            break;
        }
        const double firstOutside =
            canShrinkFirst
            ? translatedCandidate(
                  firstScale
                      - kStructuralLegalScaleStep,
                  secondScale).second
            : std::numeric_limits<double>::max();
        const double secondOutside =
            canShrinkSecond
            ? translatedCandidate(
                  firstScale,
                  secondScale
                      - kStructuralLegalScaleStep).second
            : std::numeric_limits<double>::max();
        if (firstOutside
            < secondOutside
                - kGeometryEpsilon) {
            firstScale -=
                kStructuralLegalScaleStep;
        } else {
            secondScale -=
                kStructuralLegalScaleStep;
        }
    }

    return std::nullopt;
}

StructuralCoverPlan structuralCoverPlan(
    const QVector<ContourSpan> &boundarySpans,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    const std::function<bool()> &cancelled) {
    StructuralCoverPlan result;
    const ShapeMesh *square =
        shapeById(catalog, 101);
    if (square == nullptr) {
        result.reason =
            QStringLiteral("Square is unavailable");
        return result;
    }
    if (boundarySpans.size() < 4) {
        result.reason =
            QStringLiteral("too few boundary spans");
        return result;
    }

    QVector<QPointF> vertices;
    QVector<StructuralEdge> edges;
    vertices.reserve(boundarySpans.size());
    edges.reserve(boundarySpans.size());
    for (const ContourSpan &span : boundarySpans) {
        vertices.push_back(span.start);
    }
    const QRectF bounds =
        polygonBounds(mustCover);
    const double diagonal =
        std::hypot(bounds.width(), bounds.height());
    const double coordinateTolerance =
        std::max(
            kStructuralMinimumCoordinateTolerance,
            diagonal
                * kStructuralCoordinateToleranceFraction);
    for (const ContourSpan &span : boundarySpans) {
        const QPointF chord =
            span.end - span.start;
        const double length =
            std::hypot(chord.x(), chord.y());
        if (length <= kGeometryEpsilon) {
            result.reason =
                QStringLiteral("degenerate boundary span");
            return result;
        }
        double maximumBow = 0.0;
        if (span.curved) {
            maximumBow =
                std::abs(
                    pointCross(
                        chord,
                        span.control - span.start))
                / length * 0.5;
            if (maximumBow
                > coordinateTolerance) {
                result.reason =
                    QStringLiteral("boundary curvature exceeds structural tolerance");
                return result;
            }
        }
        edges.push_back({
            canonicalDirection(chord),
            length,
            maximumBow,
        });
    }

    const StructuralAxes axes =
        structuralAxes(edges);
    result.explainedBoundaryFraction =
        axes.explainedBoundaryFraction;
    if (!axes.valid) {
        result.reason =
            QStringLiteral("boundary does not resolve to two axes");
        return result;
    }
    const double basisTolerance =
        coordinateTolerance
        / axes.determinant;
    QVector<QPointF> coordinateVertices;
    QVector<double> rawFirstCoordinates;
    QVector<double> rawSecondCoordinates;
    coordinateVertices.reserve(vertices.size());
    rawFirstCoordinates.reserve(vertices.size());
    rawSecondCoordinates.reserve(vertices.size());
    for (const QPointF &vertex : vertices) {
        const QPointF coordinate =
            structuralCoordinates(vertex, axes);
        coordinateVertices.push_back(coordinate);
        rawFirstCoordinates.push_back(
            coordinate.x());
        rawSecondCoordinates.push_back(
            coordinate.y());
    }
    const QVector<double> firstClusters =
        clusteredCoordinates(
            rawFirstCoordinates,
            basisTolerance);
    const QVector<double> secondClusters =
        clusteredCoordinates(
            rawSecondCoordinates,
            basisTolerance);
    QPolygonF snapped;
    snapped.reserve(coordinateVertices.size());
    for (const QPointF &coordinate :
         coordinateVertices) {
        snapped.push_back({
            nearestCoordinate(
                coordinate.x(), firstClusters),
            nearestCoordinate(
                coordinate.y(), secondClusters),
        });
    }
    snapped =
        simplifyStructuralPolygon(
            std::move(snapped));
    if (snapped.size() < 4) {
        result.reason =
            QStringLiteral("snapped boundary collapsed");
        return result;
    }
    for (int index = 0;
         index < snapped.size(); ++index) {
        const QPointF &left = snapped[index];
        const QPointF &right =
            snapped[(index + 1)
                    % snapped.size()];
        if (sameCoordinate(
                left.x(), right.x())
            == sameCoordinate(
                left.y(), right.y())) {
            result.reason =
                QStringLiteral("snapped boundary is not axis aligned");
            return result;
        }
    }

    const QVector<double> firstCoordinates =
        structuralSupportCoordinates(
            snapped, true);
    const QVector<double> secondCoordinates =
        structuralSupportCoordinates(
            snapped, false);
    if (firstCoordinates.size() < 2
        || secondCoordinates.size() < 2
        || firstCoordinates.size()
            > kStructuralMaximumSupportLines
        || secondCoordinates.size()
            > kStructuralMaximumSupportLines) {
        result.reason =
            QStringLiteral("structural grid is too large");
        return result;
    }
    const int totalCells =
        (firstCoordinates.size() - 1)
        * (secondCoordinates.size() - 1);
    if (totalCells <= 0
        || totalCells
            > kStructuralMaximumGridCells) {
        result.reason =
            QStringLiteral("structural grid cell limit exceeded");
        return result;
    }

    quint64 occupiedMask = 0;
    int occupiedCount = 0;
    const QVector<StructuralRectangle>
        rectangles =
            structuralRectangles(
                snapped,
                firstCoordinates,
                secondCoordinates,
                &occupiedMask,
                &occupiedCount);
    result.gridCells = occupiedCount;
    result.rectangleCandidates =
        rectangles.size();
    if (occupiedCount == 0
        || rectangles.isEmpty()) {
        result.reason =
            QStringLiteral("structural grid is empty");
        return result;
    }

    bool searchLimited = false;
    const QVector<StructuralRectangle>
        selected =
            minimumRectangleCover(
                occupiedMask,
                rectangles,
                &searchLimited);
    if (selected.isEmpty()) {
        result.reason = searchLimited
            ? QStringLiteral("rectangle search limit reached")
            : QStringLiteral("rectangle cover is unavailable");
        return result;
    }
    if (selected.size() > options.budget) {
        result.reason =
            QStringLiteral("rectangle cover exceeds the shape budget");
        return result;
    }

    const double outsideAllowance =
        options.epsSpill
        / static_cast<double>(selected.size());
    result.placements.reserve(selected.size());
    for (const StructuralRectangle &rectangle :
         selected) {
        const std::optional<Affine> transform =
            legalStructuralRectangle(
                *square, axes,
                firstCoordinates,
                secondCoordinates,
                rectangle, mayCover,
                outsideAllowance,
                basisTolerance,
                cancelled);
        if (!transform.has_value()) {
            result.cancelled =
                cancelled && cancelled();
            result.reason = result.cancelled
                ? QStringLiteral("cancelled")
                : QStringLiteral("rectangle legalization failed");
            return result;
        }
        result.placements.push_back({
            *transform,
            square->id,
            0.0,
        });
    }

    const ExactCoverState state =
        exactCoverState(
            result.placements, catalog,
            mustCover, mayCover,
            targetArea);
    result.residual = state.residual;
    result.residualArea =
        state.residualArea;
    result.outsideArea =
        state.outsideArea;
    result.coverageRatio =
        targetArea > kGeometryEpsilon
        ? state.coveredArea / targetArea
        : 0.0;
    result.eligible = true;
    result.residualThickness =
        result.residual.isEmpty()
        ? 0.0
        : distanceSeed(result.residual).radius;
    result.accepted =
        (result.coverageRatio
             >= kStructuralMinimumCompactCoverageRatio
         || (result.coverageRatio
             >= kStructuralMinimumSeedCoverageRatio
             && result.residualThickness
                 <= coordinateTolerance))
        && result.outsideArea
            <= options.epsSpill
                + kGeometryEpsilon;
    result.seeded =
        !result.accepted
        && result.coverageRatio
            >= kStructuralMinimumSeedCoverageRatio
        && result.outsideArea
            <= options.epsSpill
                + kGeometryEpsilon;
    result.reason = result.accepted
        ? (result.coverageRatio
                   >= kStructuralMinimumCompactCoverageRatio
               ? QStringLiteral("compact structural cover")
               : QStringLiteral("compact structural boundary residual"))
        : (result.seeded
               ? QStringLiteral("structural residual seed")
               : QStringLiteral("structural coverage is insufficient"));
    refreshPlacementGains(
        &result.placements,
        catalog, mustCover);

    return result;
}

QVector<PruneCandidate> pruneCandidateOrder(
    const QVector<Placement> &placements,
    const Polygons &mustCover,
    const ExactCoverState &currentState) {
    QVector<PruneCandidate> result;
    result.reserve(placements.size());
    for (int index = 0; index < placements.size(); ++index) {
        Polygons reducedFootprints =
            currentState.footprints;
        reducedFootprints.removeAt(index);
        const Polygons reducedCoverage =
            unionPolygons(reducedFootprints);
        const Polygons reducedResidual =
            differencePolygons(
                mustCover, reducedCoverage);
        const double reducedResidualArea =
            polygonSetArea(reducedResidual);
        const Placement &placement = placements[index];
        result.push_back({
            placement.transform,
            index,
            placement.shapeId,
            std::max(
                0.0,
                reducedResidualArea
                    - currentState.residualArea),
        });
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const PruneCandidate &left,
           const PruneCandidate &right) {
            if (std::abs(left.uniqueArea - right.uniqueArea)
                > kGeometryEpsilon) {
                return left.uniqueArea < right.uniqueArea;
            }
            if (left.shapeId != right.shapeId) {
                return left.shapeId < right.shapeId;
            }
            if (lexicographicTransformLess(
                    left.transform, right.transform)) {
                return true;
            }
            if (lexicographicTransformLess(
                    right.transform, left.transform)) {
                return false;
            }
            return left.index < right.index;
        });

    return result;
}

QVector<PruneNeighbor> pruneNeighbors(
    const QPolygonF &removedFootprint,
    const ExactCoverState &trialState) {
    QVector<PruneNeighbor> result;
    const Polygons removed{removedFootprint};
    const QRectF removedBounds =
        removedFootprint.boundingRect();
    for (int index = 0;
         index < trialState.footprints.size(); ++index) {
        const QPolygonF &footprint =
            trialState.footprints[index];
        const QRectF footprintBounds =
            footprint.boundingRect();
        if (!footprintBounds.intersects(removedBounds)) {
            continue;
        }
        const double overlapArea = polygonSetArea(
            intersectionPolygons(
                Polygons{footprint}, removed));
        const QPointF centerDelta =
            footprintBounds.center()
            - removedBounds.center();
        result.push_back({
            index,
            overlapArea,
            QPointF::dotProduct(
                centerDelta, centerDelta),
        });
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const PruneNeighbor &left,
           const PruneNeighbor &right) {
            if (std::abs(
                    left.overlapArea - right.overlapArea)
                > kGeometryEpsilon) {
                return left.overlapArea > right.overlapArea;
            }
            if (std::abs(
                    left.distanceSquared
                    - right.distanceSquared)
                > kGeometryEpsilon) {
                return left.distanceSquared
                    < right.distanceSquared;
            }
            return left.index < right.index;
        });

    return result;
}

void accumulateCandidateProfile(
    const CandidateProfile &candidateProfile,
    FillProfile *profile) {
    const qint64 workerNanoseconds =
        candidateProfile.totalNanoseconds > 0
        ? candidateProfile.totalNanoseconds
        : candidateProfile.legalizationNanoseconds;
    profile->candidateWorkerSeconds +=
        static_cast<double>(
            workerNanoseconds) * 1e-9;
    profile->adamEvaluationWorkerSeconds +=
        static_cast<double>(
            candidateProfile.adamEvaluationNanoseconds) * 1e-9;
    profile->legalizationWorkerSeconds +=
        static_cast<double>(
            candidateProfile.legalizationNanoseconds) * 1e-9;
    profile->adamEvaluations +=
        candidateProfile.adamEvaluations;
    profile->legalizationEvaluations +=
        candidateProfile.legalizationEvaluations;
}

Candidate optimizeExistingPlacement(
    const Placement &placement,
    const QVector<ShapeMesh> &catalog,
    const Polygons &subject,
    const Polygons &mayCover,
    const FillOptions &options,
    GpuAreaEvaluator *gpuEvaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    const std::function<bool()> &cancelled,
    bool *wasCancelled) {
    Candidate result;
    const ShapeMesh *shape =
        shapeById(catalog, placement.shapeId);
    if (shape == nullptr || subject.isEmpty()) {
        return result;
    }
    CandidateJob job;
    job.shape = shape;
    job.transform = placement.transform;
    job.hasTransform = true;
    const std::vector<CandidateJob> jobs{job};
    const EvaluationBounds subjectBounds{
        individualPolygonBounds(subject),
        individualPolygonBounds(mayCover),
    };
    const DistanceSeed seed = distanceSeed(subject);
    ++profile->candidateJobs;
    ++profile->pruneOptimizations;
    QElapsedTimer batchTimer;
    batchTimer.start();
    bool usedGpu = false;
    bool gpuCancelled = false;
    if (gpuEvaluator != nullptr
        && gpuEvaluator->available()
        && gpuEvaluator->setSubjects(subject, mayCover)) {
        std::vector<Candidate> gpuResults;
        usedGpu = optimizeCandidatesGpu(
            jobs, subject, mayCover, subjectBounds,
            options, seed, gpuEvaluator, candidatePool,
            profile, cancelled, &gpuResults,
            &gpuCancelled);
        if (usedGpu && !gpuResults.empty()
            && gpuResults.front().valid) {
            CandidateProfile exactProfile;
            result = legalCandidate(
                *shape, gpuResults.front().transform,
                subject, mayCover, subjectBounds,
                options, &exactProfile);
            accumulateCandidateProfile(
                exactProfile, profile);
        }
    }
    if (!usedGpu && !gpuCancelled) {
        const CandidateJobResult cpuResult =
            optimizeCandidate(
                job, subject, mayCover, subjectBounds,
                options, seed, cancelled);
        result = cpuResult.candidate;
        accumulateCandidateProfile(
            cpuResult.profile, profile);
    }
    profile->candidateBatchWallSeconds +=
        static_cast<double>(
            batchTimer.nsecsElapsed()) * 1e-9;
    *wasCancelled = gpuCancelled
        || (cancelled && cancelled());

    return result;
}

bool sameTransform(
    const Affine &left,
    const Affine &right) {
    const auto leftValues = affineValues(left);
    const auto rightValues = affineValues(right);
    for (int index = 0;
         index < kGradientCount; ++index) {
        if (std::abs(
                leftValues[index] - rightValues[index])
            > kGeometryEpsilon) {
            return false;
        }
    }

    return true;
}

bool acceptablePruneState(
    const ExactCoverState &state,
    double residualLimit,
    double outsideLimit) {
    return state.residualArea
            <= residualLimit + kGeometryEpsilon
        && state.outsideArea
            <= outsideLimit + kGeometryEpsilon;
}

bool prunePlacements(
    QVector<Placement> *placements,
    ExactCoverState *currentState,
    const QVector<ShapeMesh> &catalog,
    const Polygons &mustCover,
    const Polygons &mayCover,
    double targetArea,
    const FillOptions &options,
    GpuAreaEvaluator *gpuEvaluator,
    QThreadPool *candidatePool,
    FillProfile *profile,
    QElapsedTimer *elapsed,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress) {
    QElapsedTimer pruneTimer;
    pruneTimer.start();
    const double residualLimit =
        std::max(
            currentState->residualArea
                + options.epsArea,
            targetArea
                * (1.0
                   - kStructuralMinimumCompactCoverageRatio));
    const double outsideLimit =
        currentState->outsideArea + options.epsSpill;
    while (!placements->isEmpty()) {
        if (cancelled && cancelled()) {
            profile->pruneWallSeconds +=
                static_cast<double>(
                    pruneTimer.nsecsElapsed()) * 1e-9;
            return false;
        }
        ++profile->prunePasses;
        const QVector<PruneCandidate> order =
            pruneCandidateOrder(
                *placements, mustCover,
                *currentState);
        bool removedInPass = false;
        for (const PruneCandidate &pruneCandidate : order) {
            if (cancelled && cancelled()) {
                profile->pruneWallSeconds +=
                    static_cast<double>(
                        pruneTimer.nsecsElapsed()) * 1e-9;
                return false;
            }
            ++profile->pruneAttempts;
            QVector<Placement> trial = *placements;
            const Placement removed =
                trial.takeAt(pruneCandidate.index);
            const ShapeMesh *removedShape =
                shapeById(catalog, removed.shapeId);
            if (removedShape == nullptr) {
                continue;
            }
            const QPolygonF removedFootprint =
                transformedBoundary(
                    *removedShape, removed.transform);
            ExactCoverState trialState =
                exactCoverState(
                    trial, catalog, mustCover,
                    mayCover, targetArea);
            QSet<int> adjustedIndices;
            if (!acceptablePruneState(
                    trialState,
                    residualLimit,
                    outsideLimit)) {
                const QVector<PruneNeighbor> neighbors =
                    pruneNeighbors(
                        removedFootprint, trialState);
                for (const PruneNeighbor &neighbor : neighbors) {
                    Polygons fixedFootprints =
                        trialState.footprints;
                    fixedFootprints.removeAt(
                        neighbor.index);
                    const Polygons fixedCoverage =
                        unionPolygons(fixedFootprints);
                    const Polygons fixedResidual =
                        differencePolygons(
                            mustCover, fixedCoverage);
                    bool optimizationCancelled = false;
                    const Candidate optimized =
                        optimizeExistingPlacement(
                            trial[neighbor.index],
                            catalog, fixedResidual,
                            mayCover, options,
                            gpuEvaluator, candidatePool,
                            profile, cancelled,
                            &optimizationCancelled);
                    if (optimizationCancelled) {
                        profile->pruneWallSeconds +=
                            static_cast<double>(
                                pruneTimer.nsecsElapsed()) * 1e-9;
                        return false;
                    }
                    if (!optimized.valid
                        || sameTransform(
                            optimized.transform,
                            trial[neighbor.index].transform)) {
                        continue;
                    }
                    QVector<Placement> adjustedTrial = trial;
                    adjustedTrial[neighbor.index].transform =
                        optimized.transform;
                    ExactCoverState adjustedState =
                        exactCoverState(
                            adjustedTrial, catalog,
                            mustCover, mayCover,
                            targetArea);
                    if (adjustedState.residualArea
                            >= trialState.residualArea
                                - kGeometryEpsilon
                        || adjustedState.outsideArea
                            > outsideLimit
                                + kGeometryEpsilon) {
                        continue;
                    }
                    trial = std::move(adjustedTrial);
                    trialState = std::move(adjustedState);
                    adjustedIndices.insert(neighbor.index);
                    if (acceptablePruneState(
                            trialState,
                            residualLimit,
                            outsideLimit)) {
                        break;
                    }
                }
            }
            if (!acceptablePruneState(
                    trialState,
                    residualLimit,
                    outsideLimit)) {
                continue;
            }
            *placements = std::move(trial);
            *currentState = std::move(trialState);
            refreshPlacementGains(
                placements, catalog, mustCover);
            ++profile->prunedPlacements;
            profile->adjustedPlacements +=
                adjustedIndices.size();
            removedInPass = true;
            if (progress) {
                progress({
                    static_cast<int>(placements->size()),
                    targetArea,
                    currentState->coveredArea,
                    currentState->residualArea,
                    static_cast<double>(
                        elapsed->elapsed()) / 1000.0,
                });
            }
            break;
        }
        if (!removedInPass) {
            break;
        }
    }
    profile->pruneWallSeconds +=
        static_cast<double>(
            pruneTimer.nsecsElapsed()) * 1e-9;

    return true;
}

std::uint64_t derivedSeed(const Polygons &polygons) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const QPolygonF &polygon : polygons) {
        for (const QPointF &point : polygon) {
            const qint64 x = std::llround(point.x() * kClipperScale);
            const qint64 y = std::llround(point.y() * kClipperScale);
            result ^= static_cast<std::uint64_t>(x);
            result *= 1099511628211ULL;
            result ^= static_cast<std::uint64_t>(y);
            result *= 1099511628211ULL;
        }
    }

    return result;
}

bool validOptions(const FillOptions &options) {
    return options.budget > 0 && options.adamIterations > 0
        && options.restarts >= 0
        && std::isfinite(options.spillWeight) && options.spillWeight > 0.0
        && std::isfinite(options.epsArea) && options.epsArea >= 0.0
        && std::isfinite(options.epsGain) && options.epsGain > 0.0
        && std::isfinite(options.epsSpill) && options.epsSpill >= 0.0
        && std::isfinite(options.adamLearningRate)
        && options.adamLearningRate > 0.0
        && std::isfinite(
            options.inactivityTimeoutSeconds)
        && options.inactivityTimeoutSeconds >= 0.0;
}

void mergeRepairProfile(
    const FillProfile &repair,
    FillProfile *profile) {
    profile->greedySetupWallSeconds +=
        repair.greedySetupWallSeconds;
    profile->candidateBatchWallSeconds +=
        repair.candidateBatchWallSeconds;
    profile->candidateWorkerSeconds +=
        repair.candidateWorkerSeconds;
    profile->adamEvaluationWorkerSeconds +=
        repair.adamEvaluationWorkerSeconds;
    profile->legalizationWorkerSeconds +=
        repair.legalizationWorkerSeconds;
    profile->residualUpdateWallSeconds +=
        repair.residualUpdateWallSeconds;
    profile->finalMeasurementWallSeconds +=
        repair.finalMeasurementWallSeconds;
    profile->gpuEvaluationWallSeconds +=
        repair.gpuEvaluationWallSeconds;
    profile->candidateJobs += repair.candidateJobs;
    profile->adamEvaluations += repair.adamEvaluations;
    profile->legalizationEvaluations +=
        repair.legalizationEvaluations;
    profile->gpuBatches += repair.gpuBatches;
    profile->gpuIntersectionTasks +=
        repair.gpuIntersectionTasks;
    profile->wholeComponentJobs +=
        repair.wholeComponentJobs;
    profile->hardEdgeCandidates +=
        repair.hardEdgeCandidates;
    profile->complexitySelections +=
        repair.complexitySelections;
    profile->localComponentPlacements +=
        repair.localComponentPlacements;
    profile->wholeComponentPlacements +=
        repair.wholeComponentPlacements;
    profile->hardEdgePlacements +=
        repair.hardEdgePlacements;
    profile->workerThreads = std::max(
        profile->workerThreads,
        repair.workerThreads);
    if (profile->gpuError.isEmpty()) {
        profile->gpuError = repair.gpuError;
    }
}

} // namespace

QVector<ShapeMesh> buildShapeCatalog(const ShapeGeometryStore &geometry,
                                     QString *error) {
    const QVector<int> shapeIds = loadDifferentialShapeIds(error);
    if (shapeIds.isEmpty()) {
        return {};
    }

    QVector<ShapeMesh> result;
    result.reserve(shapeIds.size());
    for (const int shapeId : shapeIds) {
        const ShapeGeometry *source = geometry.shape(shapeId);
        if (source == nullptr) {
            if (error != nullptr) {
                *error = QStringLiteral("shape %1 is unavailable").arg(shapeId);
            }
            return {};
        }
        ShapeMesh shape = buildShapeMesh(shapeId, *source);
        if (!shape.valid()) {
            if (error != nullptr) {
                *error = shape.error.isEmpty()
                    ? QStringLiteral("shape %1 is invalid").arg(shapeId)
                    : shape.error;
            }
            return {};
        }
        result.push_back(std::move(shape));
    }
    if (error != nullptr) {
        error->clear();
    }

    return result;
}

Polygons polygonsFromPainterPath(const QPainterPath &path) {
    Polygons result;
    const QList<QPolygonF> subpaths = path.toSubpathPolygons();
    result.reserve(subpaths.size());
    for (QPolygonF polygon : subpaths) {
        polygon = normalizedPolygon(std::move(polygon));
        if (!polygon.isEmpty()) {
            result.push_back(std::move(polygon));
        }
    }
    if (result.size() == 1 && signedArea(result.front()) < 0.0) {
        std::reverse(result.front().begin(), result.front().end());
    }

    return result;
}

QPainterPath painterPathFromPolygons(const Polygons &polygons) {
    QPainterPath result;
    result.setFillRule(Qt::WindingFill);
    for (const QPolygonF &polygon : polygons) {
        result.addPolygon(polygon);
        result.closeSubpath();
    }

    return result;
}

AreaGradient evaluateAreaGradient(const ShapeMesh &shape,
                                  const Affine &transform,
                                  const Polygons &coveredSubject,
                                  const Polygons &legalSubject) {
    const EvaluationBounds subjectBounds{
        individualPolygonBounds(coveredSubject),
        individualPolygonBounds(legalSubject),
    };

    return areaGradient(shape, transform, coveredSubject, legalSubject,
                        subjectBounds);
}

FillResult analyticCoverFillInternal(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress,
    bool postProcess) {
    FillResult result;
    const Polygons mustCover = normalizedInputPolygons(input.mustCover);
    const Polygons mayCover = normalizedInputPolygons(input.mayCover);
    if (mustCover.isEmpty() || mayCover.isEmpty()) {
        result.error = QStringLiteral("Differential cover input is empty");
        return result;
    }
    if (!validOptions(options)) {
        result.error = QStringLiteral("Differential cover options are invalid");
        return result;
    }
    if (catalog.isEmpty()
        || std::any_of(catalog.cbegin(), catalog.cend(),
                       [](const ShapeMesh &shape) { return !shape.valid(); })) {
        result.error = QStringLiteral("Differential cover catalog is invalid");
        return result;
    }

    const double targetArea = polygonSetArea(mustCover);
    QElapsedTimer elapsed;
    elapsed.start();
    if (progress) {
        progress({
            0,
            targetArea,
            0.0,
            targetArea,
            0.0,
        });
    }
    const StructuralCoverPlan structural =
        structuralCoverPlan(
            input.boundarySpans, catalog,
            mustCover, mayCover,
            targetArea, options, cancelled);
    result.profile.structuralReason =
        structural.reason;
    result.profile.structuralExplainedBoundaryFraction =
        structural.explainedBoundaryFraction;
    result.profile.structuralCoverageRatio =
        structural.coverageRatio;
    result.profile.structuralResidualArea =
        structural.residualArea;
    result.profile.structuralResidualThickness =
        structural.residualThickness;
    result.profile.structuralOutsideArea =
        structural.outsideArea;
    result.profile.structuralGridCells =
        structural.gridCells;
    result.profile.structuralRectangleCandidates =
        structural.rectangleCandidates;
    result.profile.structuralRectangles =
        structural.placements.size();
    result.profile.structuralAccepted =
        structural.accepted;
    result.profile.structuralSeeded =
        structural.seeded;
    if (structural.cancelled) {
        result.cancelled = true;
        result.residual = mustCover;
        result.residualArea = targetArea;
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        return result;
    }
    if (structural.accepted) {
        result.placements =
            structural.placements;
        result.residual =
            structural.residual;
        result.residualArea =
            structural.residualArea;
        result.coveredArea =
            targetArea - result.residualArea;
        result.outsideArea =
            structural.outsideArea;
        result.stalled =
            result.residualArea > options.epsArea;
        result.profile.evaluationBackend =
            QStringLiteral("Structural cover");
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        if (progress) {
            progress({
                static_cast<int>(
                    result.placements.size()),
                targetArea,
                result.coveredArea,
                result.residualArea,
                static_cast<double>(
                    elapsed.elapsed()) / 1000.0,
            });
        }
        return result;
    }
    if (structural.seeded) {
        result.placements =
            structural.placements;
        result.residual =
            structural.residual;
        if (progress) {
            progress({
                static_cast<int>(
                    result.placements.size()),
                targetArea,
                targetArea
                    - structural.residualArea,
                structural.residualArea,
                static_cast<double>(
                    elapsed.elapsed()) / 1000.0,
            });
        }
    } else {
        result.residual = mustCover;
    }
    const QVector<FixedCandidate> hardCandidates =
        hardEdgeCandidates(
            input.boundarySpans,
            catalog, cancelled);
    result.profile.hardEdgeCandidates =
        hardCandidates.size();
    const MeshCoverPlan mesh =
        meshCoverPlan(
            hardCandidates, catalog,
            mustCover, mayCover,
            targetArea, options,
            cancelled);
    result.profile.meshReason =
        mesh.reason;
    result.profile.meshCoverageRatio =
        mesh.coverageRatio;
    result.profile.meshResidualArea =
        mesh.residualArea;
    result.profile.meshOutsideArea =
        mesh.outsideArea;
    result.profile.meshScale =
        mesh.scale;
    result.profile.meshPlacements =
        mesh.placements.size();
    result.profile.meshAccepted =
        mesh.accepted;
    if (mesh.cancelled) {
        result.cancelled = true;
        result.residual = mustCover;
        result.residualArea = targetArea;
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        return result;
    }
    if (mesh.accepted) {
        result.placements =
            mesh.placements;
        result.residual =
            mesh.residual;
        result.residualArea =
            mesh.residualArea;
        result.coveredArea =
            targetArea - result.residualArea;
        result.outsideArea =
            mesh.outsideArea;
        result.stalled =
            result.residualArea > options.epsArea;
        result.profile.evaluationBackend =
            QStringLiteral("Polygon mesh");
        result.profile.totalWallSeconds =
            static_cast<double>(
                elapsed.nsecsElapsed()) * 1e-9;
        if (progress) {
            progress({
                static_cast<int>(
                    result.placements.size()),
                targetArea,
                result.coveredArea,
                result.residualArea,
                static_cast<double>(
                    elapsed.elapsed()) / 1000.0,
            });
        }
        return result;
    }
    const std::uint64_t seedValue = options.seed == 0
        ? derivedSeed(mustCover) : options.seed;
    std::mt19937_64 random(seedValue);
    QThreadPool candidatePool;
    candidatePool.setMaxThreadCount(
        std::max(1, QThread::idealThreadCount() - 1));
    result.profile.workerThreads = candidatePool.maxThreadCount();
    std::unique_ptr<GpuAreaEvaluator> gpuEvaluator =
        options.useGpu ? createGpuAreaEvaluator(catalog) : nullptr;
    if (gpuEvaluator != nullptr && gpuEvaluator->available()) {
        result.profile.evaluationBackend =
            gpuEvaluator->stats().backend;
    } else {
        result.profile.evaluationBackend = QStringLiteral("CPU");
    }
    auto updateGpuProfile = [&]() {
        if (gpuEvaluator == nullptr) {
            return;
        }
        const GpuEvaluatorStats stats = gpuEvaluator->stats();
        result.profile.gpuAdapter = stats.adapter;
        result.profile.gpuError = stats.error;
        result.profile.gpuBatches = stats.batches;
        result.profile.gpuIntersectionTasks = stats.intersectionTasks;
        result.profile.gpuEvaluationWallSeconds = stats.wallSeconds;
        result.profile.evaluationBackend = stats.backend;
        if (!stats.error.isEmpty()
            && stats.backend != QStringLiteral("CPU")) {
            result.profile.evaluationBackend +=
                QStringLiteral(" with fallback");
        }
    };
    while (polygonSetArea(result.residual) > options.epsArea
           && result.placements.size() < options.budget) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.residualArea = polygonSetArea(result.residual);
            result.coveredArea = targetArea - result.residualArea;
            result.profile.totalWallSeconds =
                static_cast<double>(elapsed.nsecsElapsed()) * 1e-9;
            updateGpuProfile();
            return result;
        }

        ++result.profile.greedySteps;
        QElapsedTimer setupTimer;
        setupTimer.start();
        const EvaluationBounds subjectBounds{
            individualPolygonBounds(result.residual),
            individualPolygonBounds(mayCover),
        };
        const DistanceSeed seed = distanceSeed(result.residual);
        const Polygons routedComponent =
            componentAtPoint(result.residual, seed.point);
        QVector<const ShapeMesh *> candidates =
            routedShapes(result.residual, catalog, options.useRouter);
        const ResidualComplexity structure =
            residualComplexity(result.residual);
        QSet<const ShapeMesh *> localFallbackShapes;
        if (options.useRouter && structure.components > 1) {
            const QVector<const ShapeMesh *> localCandidates =
                routedShapes(
                    routedComponent, catalog, options.useRouter);
            double globalComponentDistance =
                std::numeric_limits<double>::max();
            for (const ShapeMesh *shape : candidates) {
                globalComponentDistance = std::min(
                    globalComponentDistance,
                    descriptorDistance(
                        *shape, routedComponent));
            }
            for (const ShapeMesh *shape : localCandidates) {
                if (!candidates.contains(shape)
                    && descriptorDistance(
                           *shape, routedComponent)
                        < globalComponentDistance
                            * kLocalRouterAdvantage) {
                    candidates.push_back(shape);
                    localFallbackShapes.insert(shape);
                    break;
                }
            }
        }
        std::vector<CandidateJob> jobs;
        jobs.reserve(static_cast<size_t>(
            candidates.size() * (options.restarts + 1)));
        std::mt19937_64 localRandom;
        bool localRandomReady = false;
        for (const ShapeMesh *shape : candidates) {
            const bool local =
                localFallbackShapes.contains(shape);
            if (local && !localRandomReady) {
                localRandom = random;
                localRandomReady = true;
            }
            for (int restart = 0; restart <= options.restarts; ++restart) {
                CandidateJob job;
                job.shape = shape;
                job.initialization =
                    candidateInitialization(
                        seed, restart,
                        local ? &localRandom : &random);
                job.origin = local
                    ? CandidateOrigin::LocalComponent
                    : CandidateOrigin::Greedy;
                jobs.push_back(job);
            }
        }
        if (result.placements.isEmpty()) {
            const QVector<CandidateJob> componentJobs =
                wholeComponentJobs(result.residual, catalog);
            result.profile.wholeComponentJobs +=
                componentJobs.size();
            for (const CandidateJob &job : componentJobs) {
                jobs.push_back(job);
            }
        }
        result.profile.greedySetupWallSeconds +=
            static_cast<double>(setupTimer.nsecsElapsed()) * 1e-9;
        result.profile.candidateJobs += jobs.size();
        std::vector<CandidateJobResult> jobResults(jobs.size());
        QElapsedTimer candidateBatchTimer;
        candidateBatchTimer.start();
        bool usedGpu = false;
        bool gpuCancelled = false;
        if (gpuEvaluator != nullptr && gpuEvaluator->available()
            && gpuEvaluator->setSubjects(result.residual, mayCover)) {
            std::vector<Candidate> gpuResults;
            usedGpu = optimizeCandidatesGpu(
                jobs, result.residual, mayCover, subjectBounds,
                options, seed, gpuEvaluator.get(), &candidatePool,
                &result.profile, cancelled,
                &gpuResults, &gpuCancelled);
            if (usedGpu) {
                for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
                    if (!gpuResults[jobIndex].valid) {
                        continue;
                    }
                    CandidateProfile exactProfile;
                    jobResults[jobIndex].candidate = legalCandidate(
                        *jobs[jobIndex].shape,
                        gpuResults[jobIndex].transform,
                        result.residual, mayCover, subjectBounds,
                        options, &exactProfile);
                    jobResults[jobIndex].profile = exactProfile;
                }
            }
        }
        if (!usedGpu && !gpuCancelled) {
            for (size_t jobIndex = 0; jobIndex < jobs.size(); ++jobIndex) {
                candidatePool.start([&, jobIndex]() {
                    const CandidateJob &job = jobs[jobIndex];
                    jobResults[jobIndex] = optimizeCandidate(
                        job, result.residual, mayCover, subjectBounds,
                        options, seed, cancelled);
                });
            }
            candidatePool.waitForDone();
        }
        QVector<Candidate> rankedCandidates;
        rankedCandidates.reserve(
            static_cast<qsizetype>(jobResults.size())
            + hardCandidates.size());
        for (size_t jobIndex = 0;
             jobIndex < jobResults.size(); ++jobIndex) {
            jobResults[jobIndex].candidate.origin =
                jobs[jobIndex].origin;
            rankedCandidates.push_back(
                jobResults[jobIndex].candidate);
        }
        for (const FixedCandidate &fixed : hardCandidates) {
            CandidateProfile fixedProfile;
            rankedCandidates.push_back(fixedCandidate(
                fixed, result.residual, mayCover,
                subjectBounds, options, &fixedProfile));
            result.profile.candidateWorkerSeconds +=
                static_cast<double>(
                    fixedProfile.legalizationNanoseconds) * 1e-9;
            result.profile.legalizationWorkerSeconds +=
                static_cast<double>(
                    fixedProfile.legalizationNanoseconds) * 1e-9;
            result.profile.legalizationEvaluations +=
                fixedProfile.legalizationEvaluations;
        }
        result.profile.candidateBatchWallSeconds +=
            static_cast<double>(candidateBatchTimer.nsecsElapsed()) * 1e-9;
        for (const CandidateJobResult &jobResult : jobResults) {
            result.profile.candidateWorkerSeconds +=
                static_cast<double>(jobResult.profile.totalNanoseconds) * 1e-9;
            result.profile.adamEvaluationWorkerSeconds +=
                static_cast<double>(
                    jobResult.profile.adamEvaluationNanoseconds) * 1e-9;
            result.profile.legalizationWorkerSeconds +=
                static_cast<double>(
                    jobResult.profile.legalizationNanoseconds) * 1e-9;
            result.profile.adamEvaluations +=
                jobResult.profile.adamEvaluations;
            result.profile.legalizationEvaluations +=
                jobResult.profile.legalizationEvaluations;
        }
        if (cancelled && cancelled()) {
            result.cancelled = true;
            result.residualArea = polygonSetArea(result.residual);
            result.coveredArea = targetArea - result.residualArea;
            result.profile.totalWallSeconds =
                static_cast<double>(elapsed.nsecsElapsed()) * 1e-9;
            updateGpuProfile();
            return result;
        }
        QElapsedTimer residualTimer;
        residualTimer.start();
        CandidateSelection selection = selectCandidate(
            rankedCandidates, catalog,
            result.residual, options.epsGain);
        result.profile.residualUpdateWallSeconds +=
            static_cast<double>(residualTimer.nsecsElapsed()) * 1e-9;
        if (!selection.valid) {
            result.stalled = true;
            break;
        }
        if (selection.complexityPreferred) {
            ++result.profile.complexitySelections;
        }
        if (selection.candidate.origin
            == CandidateOrigin::LocalComponent) {
            ++result.profile.localComponentPlacements;
        } else if (selection.candidate.origin
            == CandidateOrigin::WholeComponent) {
            ++result.profile.wholeComponentPlacements;
        } else if (selection.candidate.origin
                   == CandidateOrigin::HardEdge) {
            ++result.profile.hardEdgePlacements;
        }

        result.placements.push_back({
            selection.candidate.transform,
            selection.candidate.shapeId,
            selection.exactGain,
        });
        result.residual = std::move(selection.residual);
        const double nextArea = polygonSetArea(result.residual);
        if (progress) {
            const double elapsedSeconds =
                static_cast<double>(elapsed.elapsed()) / 1000.0;
            progress({
                static_cast<int>(result.placements.size()),
                targetArea,
                std::max(0.0, targetArea - nextArea),
                nextArea,
                elapsedSeconds,
            });
        }
    }

    const bool greedyBudgetHit =
        result.placements.size() >= options.budget
        && polygonSetArea(result.residual) > options.epsArea;
    ExactCoverState coverState = exactCoverState(
        result.placements, catalog,
        mustCover, mayCover, targetArea);
    FillProfile repairProfile;
    bool haveRepairProfile = false;
    Polygons prePruneResidual;
    if (postProcess) {
        result.profile.prePruneResidualArea =
            coverState.residualArea;
        prePruneResidual = coverState.residual;
        if (!prunePlacements(
                &result.placements, &coverState,
                catalog, mustCover, mayCover,
                targetArea, options, gpuEvaluator.get(),
                &candidatePool, &result.profile, &elapsed,
                cancelled, progress)) {
            result.cancelled = true;
        }
        result.profile.postPruneResidualArea =
            coverState.residualArea;
        Polygons repairTarget;
        const double compactResidualLimit =
            targetArea
            * (1.0
               - kStructuralMinimumCompactCoverageRatio);
        if (coverState.residualArea
                > compactResidualLimit
                    + kGeometryEpsilon) {
            repairTarget =
                differencePolygons(
                    coverState.residual,
                    prePruneResidual);
        }
        result.profile.repairTargetArea =
            polygonSetArea(repairTarget);
        const int remainingBudget =
            options.budget
            - static_cast<int>(
                result.placements.size());
        if (!result.cancelled
            && result.profile.repairTargetArea
                > options.epsArea
            && remainingBudget > 0) {
            FillInput repairInput;
            repairInput.mustCover = repairTarget;
            repairInput.mayCover = mayCover;
            FillOptions repairOptions = options;
            repairOptions.budget = remainingBudget;
            const int placementOffset =
                result.placements.size();
            const double coveredOffset =
                coverState.coveredArea;
            const auto repairProgress =
                [&, placementOffset, coveredOffset](
                    const FillProgress &update) {
                    if (!progress) {
                        return;
                    }
                    const double combinedCovered =
                        std::min(
                            targetArea,
                            coveredOffset
                                + update.coveredArea);
                    progress({
                        placementOffset
                            + update.placementCount,
                        targetArea,
                        combinedCovered,
                        std::max(
                            0.0,
                            targetArea - combinedCovered),
                        static_cast<double>(
                            elapsed.elapsed()) / 1000.0,
                    });
                };
            QElapsedTimer repairTimer;
            repairTimer.start();
            FillResult repair =
                analyticCoverFillInternal(
                    repairInput, catalog,
                    repairOptions, cancelled,
                    repairProgress, false);
            result.profile.repairWallSeconds =
                static_cast<double>(
                    repairTimer.nsecsElapsed()) * 1e-9;
            result.profile.repairPlacements =
                static_cast<int>(
                    repair.placements.size());
            result.profile.repairSteps =
                repair.profile.greedySteps;
            result.profile.repairCoveredArea =
                repair.coveredArea;
            repairProfile = repair.profile;
            haveRepairProfile = true;
            for (const Placement &placement :
                 repair.placements) {
                result.placements.push_back(
                    placement);
            }
            result.cancelled = repair.cancelled;
            result.stalled = repair.stalled;
            if (!repair.error.isEmpty()) {
                result.error = repair.error;
            }
        }
    }
    QElapsedTimer finalTimer;
    finalTimer.start();
    coverState = exactCoverState(
        result.placements, catalog,
        mustCover, mayCover, targetArea);
    result.residual = std::move(coverState.residual);
    result.residualArea = coverState.residualArea;
    result.coveredArea = coverState.coveredArea;
    result.outsideArea = coverState.outsideArea;
    if (postProcess) {
        result.profile.postRepairNewGapArea =
            polygonSetArea(
                differencePolygons(
                    coverState.residual,
                    prePruneResidual));
    }
    result.budgetHit = greedyBudgetHit
        || (result.placements.size() >= options.budget
            && result.residualArea > options.epsArea);
    result.profile.finalMeasurementWallSeconds =
        static_cast<double>(finalTimer.nsecsElapsed()) * 1e-9;
    result.profile.totalWallSeconds =
        static_cast<double>(elapsed.nsecsElapsed()) * 1e-9;
    updateGpuProfile();
    if (haveRepairProfile) {
        mergeRepairProfile(
            repairProfile, &result.profile);
    }
    if (postProcess && progress
        && result.profile.repairPlacements > 0) {
        progress({
            static_cast<int>(
                result.placements.size()),
            targetArea,
            result.coveredArea,
            result.residualArea,
            static_cast<double>(
                elapsed.elapsed()) / 1000.0,
        });
    }

    return result;
}

FillResult analyticCoverFill(
    const FillInput &input,
    const QVector<ShapeMesh> &catalog,
    const FillOptions &options,
    const std::function<bool()> &cancelled,
    const std::function<void(const FillProgress &)> &progress) {
    using ActivityClock =
        std::chrono::steady_clock;
    const auto activityNanoseconds = []() {
        return std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                ActivityClock::now()
                    .time_since_epoch())
            .count();
    };
    std::atomic<qint64> lastActivity{
        activityNanoseconds(),
    };
    std::atomic_bool timedOut{false};
    int previousPlacementCount = -1;
    double previousCoveredArea = -1.0;
    const qint64 timeoutNanoseconds =
        static_cast<qint64>(
            options.inactivityTimeoutSeconds
            * 1e9);
    const auto stopRequested = [&]() {
        if (cancelled && cancelled()) {
            return true;
        }
        if (timeoutNanoseconds <= 0) {
            return false;
        }
        const bool expired =
            activityNanoseconds()
                - lastActivity.load(
                    std::memory_order_relaxed)
            >= timeoutNanoseconds;
        if (expired) {
            timedOut.store(
                true,
                std::memory_order_relaxed);
        }

        return expired;
    };
    const auto activityProgress =
        [&](const FillProgress &update) {
            const bool changed =
                update.placementCount
                    != previousPlacementCount
                || std::abs(
                       update.coveredArea
                       - previousCoveredArea)
                    > kGeometryEpsilon;
            previousPlacementCount =
                update.placementCount;
            previousCoveredArea =
                update.coveredArea;
            if (changed) {
                lastActivity.store(
                    activityNanoseconds(),
                    std::memory_order_relaxed);
            }
            if (progress) {
                progress(update);
            }
        };
    FillResult result =
        analyticCoverFillInternal(
            input, catalog, options,
            stopRequested,
            activityProgress, true);
    result.timedOut =
        timedOut.load(
            std::memory_order_relaxed);
    result.cancelled =
        result.cancelled || result.timedOut;

    return result;
}

#ifdef FLS_DIFFERENTIAL_COVER_TESTS
double placementUnionAreaForTesting(
    const QVector<Placement> &placements,
    const QVector<ShapeMesh> &catalog) {
    return polygonSetArea(
        unionPolygons(
            placementFootprints(
                placements, catalog)));
}
#endif

QTransform toQTransform(const Affine &transform) {
    return QTransform(transform.a, transform.b,
                      transform.c, transform.d,
                      transform.e, transform.f);
}

} // namespace gui::cover
