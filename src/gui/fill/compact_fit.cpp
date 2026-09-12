#include "compact_fit.h"
#include "catalog_cover_internal.h"
#include "compact_fit_quality.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace gui::compact {
namespace {

constexpr int kRefinementLevels = 7;
constexpr int kMovesPerLevel = 6;
constexpr int kRefitPasses = 3;
constexpr int kMergeRounds = 64;
constexpr int kMergeNeighbors = 4;
constexpr int kRetainedSeeds = 6;
constexpr int kSeedOrientations = 16;
constexpr int kPrimaryOrientations = 8;
constexpr int kPrimaryRetainedSeeds = 3;
constexpr int kDeletionTrials = 2;
constexpr int kRefitNeighbors = 4;
constexpr double kDeepErrorWeight = 16.0;
constexpr double kInitialStepFraction = 0.04;
constexpr double kScoreEpsilon = 1e-7;
constexpr double kMissingWeight = 4.0;
constexpr double kBoundaryWeight = 8.0;
constexpr double kGapRepairWeight = 128.0;
constexpr double kBoundaryEnergyPerLength = 0.025;
constexpr double kLengthPerDefect = 80.0;
constexpr int kWorkReportInterval = 256;
constexpr int kMaximumMergeRefinements = 12;
constexpr double kMaximumExcessTurn = 0.6;
constexpr double kConvexitySlack = 0.005;
constexpr std::array<int, 28> kSearchShapeIds = {101, 102, 103, 109, 110, 120, 122, 124,
    126, 127, 128, 129, 130, 136, 139, 812, 901, 930, 2110, 2113, 2117, 2118,
    2123, 2133, 2134, 2135, 2136, 2321};

using catalog::Polygons;

struct Piece {
    PenPlacement placement;
    Polygons polygons;
    QRectF bounds;
};

struct Objective {
    Polygons target;
    Polygons preferred;
    Polygons inner;
    Polygons outer;
    std::shared_ptr<BoundaryModel> boundary;
    BoundaryMetrics targetMetrics;
    std::function<void(int)> workProgress;
    mutable int evaluations = 0;
    mutable int qualityEvaluations = 0;
    mutable int mergeRefinements = 0;
    int defectLimit = 0;
    double allowance = 0.0;
    double areaBudget = 0.0;
    double inwardAllowance = 0.0;
    double fittingInwardAllowance = 0.0;
    double qualityLimit = 0.0;
    double cornerAllowance = 0.0;
    double boundaryWeight = kBoundaryWeight;
};

struct Context {
    Polygons residual;
    Polygons others;
    Polygons innerResidual;
    mutable std::optional<Polygons> inwardResidual;
    const Objective *objective = nullptr;
};

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

Piece makePiece(const PenPrimitive &shape, const QTransform &transform) {
    Piece result;
    result.placement.shapeId = shape.shapeId;
    result.placement.transform = catalog::emittedTransform(transform);
    result.polygons = catalog::mapped(shape, result.placement.transform);
    result.bounds = catalog::painterPath(result.polygons).boundingRect();
    result.placement.area = catalog::area(result.polygons);

    return result;
}

Polygons support(const QVector<Piece> &pieces, const QVector<int> &excluded = {}) {
    Polygons result;
    for (int index = 0; index < pieces.size(); ++index) {
        if (!excluded.contains(index)) {
            result += pieces[index].polygons;
        }
    }

    return catalog::unite(result);
}

Context contextFor(const QVector<Piece> &pieces, const QVector<int> &excluded,
                   const Objective &objective) {
    Context result;
    result.objective = &objective;
    result.others = support(pieces, excluded);
    result.residual = catalog::subtract(objective.preferred, result.others);
    result.innerResidual = catalog::subtract(objective.inner, result.others);

    return result;
}

double gain(const Piece &piece, const Context &context) {
    ++context.objective->evaluations;
    if (context.objective->workProgress && context.objective->evaluations % kWorkReportInterval == 0) {
        context.objective->workProgress(context.objective->evaluations);
    }
    const auto exclusive = catalog::subtract(piece.polygons, context.others);
    const double covered = catalog::area(catalog::intersect(piece.polygons, context.residual));
    const double deepCovered = catalog::area(catalog::intersect(piece.polygons, context.innerResidual));
    const double spill = catalog::area(catalog::subtract(exclusive, context.objective->target));
    const double deepSpill = catalog::area(catalog::subtract(exclusive, context.objective->outer));

    return kMissingWeight * covered - spill + kDeepErrorWeight * (deepCovered - deepSpill);
}

double boundaryCost(const Polygons &coverage, const Objective &objective) {
    const double missingBeyondAllowance = catalog::area(catalog::subtract(objective.target,
        catalog::expanded(coverage, objective.fittingInwardAllowance)));

    return objective.boundaryWeight * objective.boundary->energy(objective.boundary->measure(coverage))
        + kGapRepairWeight * missingBeyondAllowance;
}

double qualityGain(const Piece &piece, const Context &context) {
    const double areaGain = gain(piece, context);
    if (!catalog::subtract(piece.polygons, context.objective->outer).isEmpty()) {
        return -std::numeric_limits<double>::infinity();
    }
    const auto coverage = catalog::unite(context.others + piece.polygons);
    if (!context.inwardResidual) {
        context.inwardResidual = catalog::subtract(context.objective->target,
            catalog::expanded(context.others, context.objective->fittingInwardAllowance));
    }
    const double missingBeyondAllowance = catalog::area(catalog::subtract(*context.inwardResidual,
        catalog::expanded(piece.polygons, context.objective->fittingInwardAllowance)));
    ++context.objective->qualityEvaluations;

    return areaGain - context.objective->boundaryWeight * context.objective->boundary->energy(context.objective->boundary->measure(coverage))
        - kGapRepairWeight * missingBeyondAllowance;
}

double error(const Polygons &coverage, const Objective &objective) {
    return catalog::area(catalog::subtract(objective.target, coverage))
        + catalog::area(catalog::subtract(coverage, objective.target));
}

bool acceptable(const Polygons &coverage, const Objective &objective) {
    if (coverage.isEmpty() || error(coverage, objective) > objective.areaBudget) {
        return false;
    }
    if (!catalog::subtract(coverage, objective.outer).isEmpty()) {
        return false;
    }

    if (!catalog::subtract(objective.target, catalog::expanded(coverage, objective.inwardAllowance)).isEmpty()) {
        return false;
    }
    const auto metrics = objective.boundary->measure(coverage);

    return objective.boundary->energy(metrics) <= objective.qualityLimit
        && metrics.maximumExcessTurn <= kMaximumExcessTurn
        && metrics.maximumCornerDistance <= objective.cornerAllowance
        && metrics.cornerDefects <= objective.defectLimit
        && metrics.components == objective.targetMetrics.components
        && metrics.holes == objective.targetMetrics.holes;
}

QTransform movedTransform(const Piece &piece, int parameter, double amount) {
    const QPointF center = piece.bounds.center();
    QTransform movement;
    movement.translate(center.x(), center.y());
    if (parameter == 0) {
        movement.translate(amount, 0.0);
    } else if (parameter == 1) {
        movement.translate(0.0, amount);
    } else if (parameter == 2) {
        movement.scale(std::exp(amount), 1.0);
    } else if (parameter == 3) {
        movement.scale(1.0, std::exp(amount));
    } else if (parameter == 4) {
        movement.rotateRadians(amount);
    } else {
        movement.shear(amount, 0.0);
    }
    movement.translate(-center.x(), -center.y());

    return piece.placement.transform * movement;
}

Piece refine(Piece piece, const PenPrimitive &shape, const Context &context,
              const std::function<bool()> &cancelled, bool boundaryAware = false) {
    const auto evaluate = [&](const Piece &candidate) {
        return boundaryAware ? qualityGain(candidate, context) : gain(candidate, context);
    };
    double best = evaluate(piece);
    const double extent = std::max(piece.bounds.width(), piece.bounds.height());
    for (int level = 0; level < kRefinementLevels && !stopped(cancelled); ++level) {
        const double fraction = std::ldexp(kInitialStepFraction, -level);
        for (int iteration = 0; iteration < kMovesPerLevel; ++iteration) {
            bool changed = false;
            for (int parameter = 0; parameter < 6; ++parameter) {
                for (double sign : {-1.0, 1.0}) {
                    const double step = sign * fraction * (parameter < 2 ? extent : 1.0);
                    const auto transform = movedTransform(piece, parameter, step);
                    if (std::abs(transform.determinant()) < catalog::kMinimumDeterminant) {
                        continue;
                    }
                    Piece trial = makePiece(shape, transform);
                    const double score = evaluate(trial);
                    if (score > best + kScoreEpsilon) {
                        piece = std::move(trial);
                        best = score;
                        changed = true;
                    }
                }
            }
            if (!changed) {
                break;
            }
        }
    }

    return piece;
}

const PenPrimitive &primitiveFor(int id, const QVector<catalog::Primitive> &primitives) {
    const auto found = std::find_if(primitives.begin(), primitives.end(), [id](const auto &entry) {
        return entry.shape.shapeId == id;
    });
    if (found == primitives.end()) {
        throw std::runtime_error("Compact fit seed is outside the opaque dictionary");
    }

    return found->shape;
}

void refit(QVector<Piece> *pieces, const Objective &objective,
            const QVector<catalog::Primitive> &primitives,
            const std::function<bool()> &cancelled, int passes = kRefitPasses) {
    for (int pass = 0; pass < passes && !stopped(cancelled); ++pass) {
        for (int index = 0; index < pieces->size() && !stopped(cancelled); ++index) {
            const auto context = contextFor(*pieces, {index}, objective);
            (*pieces)[index] = refine((*pieces)[index],
                primitiveFor((*pieces)[index].placement.shapeId, primitives), context, cancelled, true);
        }
    }
}

void refitNeighborhood(QVector<Piece> *pieces, const QRectF &changed,
                        const Objective &objective, const QVector<catalog::Primitive> &primitives,
                        const std::function<bool()> &cancelled) {
    QVector<std::pair<double, int>> neighbors;
    for (int index = 0; index < pieces->size(); ++index) {
        const QPointF delta = (*pieces)[index].bounds.center() - changed.center();
        neighbors.push_back({QPointF::dotProduct(delta, delta), index});
    }
    std::sort(neighbors.begin(), neighbors.end());
    for (int neighbor = 0; neighbor < std::min(kRefitNeighbors, static_cast<int>(neighbors.size())) && !stopped(cancelled); ++neighbor) {
        const int index = neighbors[neighbor].second;
        const auto context = contextFor(*pieces, {index}, objective);
        (*pieces)[index] = refine((*pieces)[index], primitiveFor((*pieces)[index].placement.shapeId, primitives),
            context, cancelled, true);
    }
}

QVector<int> removalOrder(const QVector<Piece> &pieces, const Objective &objective) {
    QVector<std::pair<double, int>> scores;
    QVector<int> result;
    for (int index = 0; index < pieces.size(); ++index) {
        const auto context = contextFor(pieces, {index}, objective);
        scores.push_back({gain(pieces[index], context), index});
    }
    std::sort(scores.begin(), scores.end());
    for (const auto &entry : scores) {
        result.push_back(entry.second);
    }

    return result;
}

void prune(QVector<Piece> *pieces, const Objective &objective,
           const std::function<bool()> &cancelled) {
    bool changed = true;
    while (changed && pieces->size() > 1 && !stopped(cancelled)) {
        changed = false;
        for (int index : removalOrder(*pieces, objective)) {
            if (acceptable(support(*pieces, {index}), objective)) {
                pieces->removeAt(index);
                changed = true;
                break;
            }
        }
    }
}

QTransform boundsTransform(const PenPrimitive &shape, const QPolygonF &points,
                            double angle, bool reflected) {
    QTransform frame;
    frame.rotateRadians(angle);
    const QRectF bounds = frame.inverted().map(points).boundingRect();
    QTransform transform;
    transform.translate(bounds.center().x(), bounds.center().y());
    transform.scale((reflected ? -1.0 : 1.0) * bounds.width() / shape.bounds.width(),
                    bounds.height() / shape.bounds.height());
    transform.translate(-shape.bounds.center().x(), -shape.bounds.center().y());

    return transform * frame;
}

QTransform momentFrame(const Polygons &polygons) {
    double twiceArea = 0.0;
    double firstX = 0.0;
    double firstY = 0.0;
    double secondX = 0.0;
    double secondY = 0.0;
    double mixed = 0.0;
    const QPointF origin = catalog::painterPath(polygons).boundingRect().center();
    for (const auto &polygon : polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF a = polygon[index] - origin;
            const QPointF b = polygon[(index + 1) % polygon.size()] - origin;
            const double cross = a.x() * b.y() - b.x() * a.y();
            twiceArea += cross;
            firstX += (a.x() + b.x()) * cross;
            firstY += (a.y() + b.y()) * cross;
            secondX += (a.x() * a.x() + a.x() * b.x() + b.x() * b.x()) * cross;
            secondY += (a.y() * a.y() + a.y() * b.y() + b.y() * b.y()) * cross;
            mixed += (2 * a.x() * a.y() + a.x() * b.y() + b.x() * a.y() + 2 * b.x() * b.y()) * cross;
        }
    }
    if (std::abs(twiceArea) < catalog::kMinimumDeterminant) {
        return {};
    }
    const double x = firstX / (3 * twiceArea);
    const double y = firstY / (3 * twiceArea);
    const double horizontal = std::sqrt(std::max(catalog::kMinimumDeterminant,
        secondX / (6 * twiceArea) - x * x));
    const double shear = (mixed / (12 * twiceArea) - x * y) / horizontal;
    const double vertical = std::sqrt(std::max(catalog::kMinimumDeterminant,
        secondY / (6 * twiceArea) - y * y - shear * shear));

    return QTransform(horizontal, shear, 0, vertical, x + origin.x(), y + origin.y());
}

QPointF radialAnchor(const Polygons &polygons, const QTransform &normalization) {
    QPointF result;
    double maximum = -1.0;
    for (const auto &polygon : polygons) {
        for (const auto &point : polygon) {
            const auto normalized = normalization.map(point);
            const double radius = QPointF::dotProduct(normalized, normalized);
            if (radius > maximum) {
                maximum = radius;
                result = normalized;
            }
        }
    }

    return result;
}

QVector<Piece> replacementSeeds(const Polygons &target, const Context &context,
                                 const QVector<catalog::Primitive> &primitives,
                                 const std::function<bool()> &cancelled, bool broad) {
    QVector<std::pair<double, Piece>> ranked;
    QVector<Piece> result;
    QPolygonF points;
    for (const auto &polygon : target) {
        points += polygon;
    }
    if (points.size() < 3) {
        return result;
    }
    const auto targetFrame = momentFrame(target);
    const auto targetAnchor = radialAnchor(target, targetFrame.inverted());
    for (const auto &primitive : primitives) {
        if (stopped(cancelled)) {
            return {};
        }
        if (!broad) {
            std::pair<double, Piece> best{-std::numeric_limits<double>::infinity(), {}};
            for (int orientation = 0; orientation < kPrimaryOrientations; ++orientation) {
                for (bool reflected : {false, true}) {
                    auto piece = makePiece(primitive.shape, boundsTransform(primitive.shape, points,
                        orientation * std::numbers::pi / kPrimaryOrientations, reflected));
                    const double score = gain(piece, context);
                    if (score > best.first + kScoreEpsilon) {
                        best = {score, std::move(piece)};
                    }
                }
            }
            const auto position = std::find_if(ranked.begin(), ranked.end(), [&](const auto &entry) {
                return best.first > entry.first + kScoreEpsilon;
            });
            if (position != ranked.end() || ranked.size() < kPrimaryRetainedSeeds) {
                ranked.insert(position, std::move(best));
                if (ranked.size() > kPrimaryRetainedSeeds) {
                    ranked.removeLast();
                }
            }
            continue;
        }
        const auto sourceFrame = momentFrame(primitive.shape.contours).inverted();
        const auto sourceAnchor = radialAnchor(primitive.shape.contours, sourceFrame);
        std::pair<double, Piece> best{-std::numeric_limits<double>::infinity(), {}};
        for (bool reflected : {false, true}) {
            const double angle = std::atan2(targetAnchor.y(), targetAnchor.x())
                - std::atan2(sourceAnchor.y(), reflected ? -sourceAnchor.x() : sourceAnchor.x());
            QTransform orientation;
            orientation.rotateRadians(angle);
            orientation.scale(reflected ? -1.0 : 1.0, 1.0);
            auto piece = makePiece(primitive.shape, sourceFrame * orientation * targetFrame);
            const double score = gain(piece, context);
            if (score > best.first + kScoreEpsilon) {
                best = {score, std::move(piece)};
            }
        }
        for (int orientation = 0; orientation < kSeedOrientations; ++orientation) {
            for (bool reflected : {false, true}) {
                const double angle = orientation * 2.0 * std::numbers::pi / kSeedOrientations;
                QTransform orientationFrame;
                orientationFrame.rotateRadians(angle);
                orientationFrame.scale(reflected ? -1.0 : 1.0, 1.0);
                for (const auto &transform : {boundsTransform(primitive.shape, points, angle, reflected),
                        sourceFrame * orientationFrame * targetFrame}) {
                    auto piece = makePiece(primitive.shape, transform);
                    const double score = gain(piece, context);
                    if (score > best.first + kScoreEpsilon) {
                        best = {score, std::move(piece)};
                    }
                }
            }
        }
        const auto position = std::find_if(ranked.begin(), ranked.end(), [&](const auto &entry) {
            return best.first > entry.first + kScoreEpsilon;
        });
        if (position != ranked.end() || ranked.size() < kRetainedSeeds) {
            ranked.insert(position, std::move(best));
            if (ranked.size() > kRetainedSeeds) {
                ranked.removeLast();
            }
        }
    }
    for (auto &entry : ranked) {
        const auto &primitive = primitiveFor(entry.second.placement.shapeId, primitives);
        result.push_back(refine(std::move(entry.second), primitive, context, cancelled));
    }

    return result;
}

QVector<QVector<int>> exposedNeighbors(const QVector<Piece> &pieces, const Objective &objective) {
    QVector<QVector<int>> neighbors(pieces.size());
    for (const auto &polygon : objective.boundary->observationSupport(support(pieces))) {
        QVector<int> owners;
        for (int edge = 0; edge < polygon.size(); ++edge) {
            const QPointF point = (polygon[edge] + polygon[(edge + 1) % polygon.size()]) * 0.5;
            int owner = -1;
            double nearest = std::numeric_limits<double>::infinity();
            for (int piece = 0; piece < pieces.size(); ++piece) {
                if (!pieces[piece].bounds.adjusted(-objective.inwardAllowance, -objective.inwardAllowance,
                    objective.inwardAllowance, objective.inwardAllowance).contains(point)) {
                    continue;
                }
                for (const auto &contour : pieces[piece].polygons) {
                    for (int segment = 0; segment < contour.size(); ++segment) {
                        const auto delta = contour[(segment + 1) % contour.size()] - contour[segment];
                        const double length = QPointF::dotProduct(delta, delta);
                        const double parameter = length > catalog::kMinimumDeterminant
                            ? std::clamp(QPointF::dotProduct(point - contour[segment], delta) / length, 0.0, 1.0) : 0.0;
                        const auto displacement = contour[segment] + delta * parameter - point;
                        const double distance = QPointF::dotProduct(displacement, displacement);
                        if (distance < nearest) {
                            nearest = distance;
                            owner = piece;
                        }
                    }
                }
            }
            if (owner >= 0 && (owners.isEmpty() || owners.back() != owner)) {
                owners.push_back(owner);
            }
        }
        for (int index = 0; index < owners.size(); ++index) {
            const int first = owners[index];
            const int second = owners[(index + 1) % owners.size()];
            if (first != second && !neighbors[first].contains(second)) {
                neighbors[first].push_back(second);
                neighbors[second].push_back(first);
            }
        }
    }
    for (auto &adjacent : neighbors) {
        std::sort(adjacent.begin(), adjacent.end());
    }

    return neighbors;
}

QVector<int> boundaryDistances(const QVector<QVector<int>> &neighbors, int source) {
    QVector<int> distances(neighbors.size(), neighbors.size());
    QVector<int> queue = {source};
    distances[source] = 0;
    for (int index = 0; index < queue.size(); ++index) {
        for (int other : neighbors[queue[index]]) {
            if (distances[other] == neighbors.size()) {
                distances[other] = distances[queue[index]] + 1;
                queue.push_back(other);
            }
        }
    }

    return distances;
}

bool merge(QVector<Piece> *pieces, const Objective &objective,
            const QVector<catalog::Primitive> &primitives,
            const std::function<bool()> &cancelled, bool broad, bool exposed = false) {
    const auto adjacency = exposed ? exposedNeighbors(*pieces, objective) : QVector<QVector<int>>();
    const auto order = removalOrder(*pieces, objective);
    for (int index : order) {
        if (stopped(cancelled)) {
            return false;
        }
        QVector<std::pair<double, int>> neighbors;
        const auto distances = exposed ? boundaryDistances(adjacency, index) : QVector<int>();
        for (int other = 0; other < pieces->size(); ++other) {
            if (other != index) {
                const QPointF displacement = (*pieces)[other].bounds.center() - (*pieces)[index].bounds.center();
                const double distance = QPointF::dotProduct(displacement, displacement);
                const double score = exposed && !adjacency[index].isEmpty()
                    ? distances[other] + distance / (distance + 1.0) : distance;
                neighbors.push_back({score, other});
            }
        }
        std::sort(neighbors.begin(), neighbors.end());
        QVector<int> excluded = {index};
        for (int neighbor = 0; neighbor < std::min(kMergeNeighbors, static_cast<int>(neighbors.size())); ++neighbor) {
            excluded.push_back(neighbors[neighbor].second);
            const auto context = contextFor(*pieces, excluded, objective);
            Polygons local;
            for (int member : excluded) {
                local += (*pieces)[member].polygons;
            }
            const auto target = catalog::intersect(catalog::unite(local), objective.target);
            for (const auto &replacement : replacementSeeds(target, context, primitives, cancelled, broad)) {
                QVector<Piece> trial;
                for (int member = 0; member < pieces->size(); ++member) {
                    if (!excluded.contains(member)) {
                        trial.push_back((*pieces)[member]);
                    }
                }
                trial.push_back(replacement);
                if (acceptable(support(trial), objective)) {
                    *pieces = std::move(trial);
                    return true;
                }
                const auto trialSupport = support(trial);
                if (objective.mergeRefinements < kMaximumMergeRefinements && !stopped(cancelled)
                    && error(trialSupport, objective) < objective.areaBudget * 1.25
                    && catalog::area(catalog::subtract(objective.target,
                        catalog::expanded(trialSupport, objective.inwardAllowance))) < objective.areaBudget * 0.02
                    && catalog::area(catalog::subtract(trialSupport, objective.outer)) < objective.areaBudget * 0.001) {
                    ++objective.mergeRefinements;
                    trial.back() = refine(replacement, primitiveFor(replacement.placement.shapeId, primitives), context, cancelled, true);
                    if (acceptable(support(trial), objective)) {
                        *pieces = std::move(trial);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

Objective makeObjective(const catalog::Region &region, const FillOptions &options) {
    Objective result;
    Polygons boundary;
    for (const auto &polygon : region.required) {
        for (int index = 0; index < polygon.size(); ++index) {
            const QPointF delta(options.inwardAllowance, options.inwardAllowance);
            const QPointF start = polygon[index];
            const QPointF end = polygon[(index + 1) % polygon.size()];
            QPolygonF corners;
            for (const auto &point : {start, end}) {
                corners += QPolygonF({point - delta, point + QPointF(delta.x(), -delta.y()),
                    point + delta, point + QPointF(-delta.x(), delta.y())});
            }
            boundary.push_back(catalog::convexHull(corners));
        }
    }
    result.target = region.required;
    result.preferred = catalog::expanded(region.required, options.inwardAllowance * 0.5);
    result.boundary = std::make_shared<BoundaryModel>(region.required, options.observationScale);
    result.targetMetrics = result.boundary->measure(region.required);
    result.qualityLimit = std::max(1.0, result.boundary->perimeter() * kBoundaryEnergyPerLength);
    result.defectLimit = std::max(1, static_cast<int>(result.boundary->perimeter() / (options.observationScale * kLengthPerDefect)));
    result.inner = catalog::subtract(region.required, catalog::unite(boundary));
    result.outer = catalog::expanded(region.required, options.boundaryAllowance);
    result.allowance = options.boundaryAllowance;
    result.areaBudget = region.area * options.areaErrorRatio;
    result.inwardAllowance = options.inwardAllowance;
    result.fittingInwardAllowance = std::max(options.inwardAllowance * 0.5,
        options.inwardAllowance - options.observationScale * 0.1);
    result.cornerAllowance = options.observationScale * 0.5;

    return result;
}

double hullArea(const Polygons &polygons) {
    QPolygonF points;
    for (const auto &polygon : polygons) {
        points += polygon;
    }

    return catalog::area({catalog::convexHull(points)});
}

QVector<catalog::Primitive> wholeRegionCatalog(const Objective &objective,
                                              const QVector<catalog::Primitive> &primitives) {
    QVector<catalog::Primitive> result;
    const double targetArea = catalog::area(objective.target);
    const double lower = (targetArea - objective.areaBudget)
        / std::max(hullArea(objective.outer), catalog::kMinimumDeterminant);
    const double upper = (targetArea + objective.areaBudget)
        / std::max(hullArea(objective.inner), catalog::kMinimumDeterminant);
    for (const auto &primitive : primitives) {
        const double convexity = primitive.shape.area
            / std::max(hullArea(primitive.shape.contours), catalog::kMinimumDeterminant);
        if (convexity + kConvexitySlack >= lower && convexity - kConvexitySlack <= upper) {
            result.push_back(primitive);
        }
    }

    return result;
}


} // namespace

catalog::FillResult fillRegion(const PenFillRequest &request,
                               const QVector<catalog::Primitive> &primitives,
                               const FillOptions &options,
                               const std::function<bool()> &cancelled,
                               const std::function<void(int, double, double)> &progress) {
    catalog::FillResult result;
    try {
        if (!std::isfinite(options.boundaryAllowance) || options.boundaryAllowance <= 0
            || !std::isfinite(options.areaErrorRatio) || options.areaErrorRatio <= 0
            || options.areaErrorRatio >= 1 || options.shapeBudget < 1 || options.evaluationBudget < 1
            || !std::isfinite(options.inwardAllowance) || options.inwardAllowance <= 0
            || !std::isfinite(options.observationScale) || options.observationScale <= 0) {
            throw std::runtime_error("Compact fit requires positive contour and work allowances and an area ratio between zero and one");
        }
        const auto region = catalog::buildRegion(request, cancelled);
        auto objective = makeObjective(region, options);
        const auto recognitionCatalog = wholeRegionCatalog(objective, primitives);
        int currentCount = 0;
        objective.workProgress = [&](int evaluated) {
            if (options.workProgress) {
                options.workProgress(currentCount, evaluated, options.evaluationBudget);
            }
        };
        const auto stopWork = [&] {
            return stopped(cancelled) || objective.evaluations >= options.evaluationBudget;
        };
        const auto stopSpatialSearch = [&] {
            return stopWork() || objective.evaluations >= options.evaluationBudget - options.evaluationBudget / 5;
        };
        const auto stopMerging = [&] {
            return stopWork() || objective.evaluations >= options.evaluationBudget - options.evaluationBudget / 10;
        };
        PenFillRequest seedRequest = request;
        QVector<Piece> pieces;
        QVector<Piece> incumbent;
        QVector<catalog::Primitive> searchCatalog;
        QJsonArray history;
        seedRequest.primitives.clear();
        for (const auto &primitive : primitives) {
            seedRequest.primitives.push_back(primitive.shape);
            if (std::find(kSearchShapeIds.begin(), kSearchShapeIds.end(), primitive.shape.shapeId) != kSearchShapeIds.end()
                || (objective.targetMetrics.holes > 0 && primitive.shape.contours.size() > 1)) {
                searchCatalog.push_back(primitive);
            }
        }
        PenFillResult seed;
        if (options.initialPlacements.isEmpty()) {
            seed = fillPenPath(seedRequest, cancelled);
        } else {
            seed.placements = options.initialPlacements;
        }
        if (!seed.error.isEmpty()) {
            throw std::runtime_error(seed.error.toStdString());
        }
        for (const auto &placement : seed.placements) {
            pieces.push_back(makePiece(primitiveFor(placement.shapeId, primitives), placement.transform));
        }
        result.diagnostics.insert(QStringLiteral("seedCount"), pieces.size());
        result.diagnostics.insert(QStringLiteral("searchCatalogSize"), searchCatalog.size());
        result.diagnostics.insert(QStringLiteral("wholeRegionCatalogSize"), recognitionCatalog.size());
        result.diagnostics.insert(QStringLiteral("seedBoundary"), objective.boundary->diagnostics(objective.boundary->measure(support(pieces))));
        if (acceptable(support(pieces), objective)) {
            incumbent = pieces;
        }
        auto report = [&] {
            currentCount = pieces.size();
            const auto coverage = support(pieces);
            const double missing = catalog::area(catalog::subtract(objective.target, coverage));
            const double spill = catalog::area(catalog::subtract(coverage, objective.target));
            history.push_back(QJsonObject{{QStringLiteral("count"), pieces.size()},
                {QStringLiteral("missing"), missing}, {QStringLiteral("spill"), spill}});
            if (acceptable(coverage, objective)
                && (incumbent.isEmpty() || pieces.size() < incumbent.size()
                    || (pieces.size() == incumbent.size() && error(coverage, objective) < error(support(incumbent), objective)))) {
                incumbent = pieces;
            }
            if (progress) {
                progress(pieces.size(), missing, spill);
            }
        };
        report();
        if (pieces.size() > 1 && !recognitionCatalog.isEmpty() && !stopWork()) {
            const auto context = contextFor({}, {}, objective);
            for (const auto &replacement : replacementSeeds(objective.target, context, recognitionCatalog, stopWork, true)) {
                if (acceptable(replacement.polygons, objective)) {
                    pieces = {replacement};
                    report();
                    break;
                }
            }
        }
        if (pieces.size() > 1) {
            const auto original = pieces;
            const auto score = [&](const QVector<Piece> &candidate) {
                const auto coverage = support(candidate);
                if (!catalog::subtract(coverage, objective.outer).isEmpty()) {
                    return -std::numeric_limits<double>::infinity();
                }
                return kMissingWeight * catalog::area(catalog::intersect(coverage, objective.preferred))
                    - catalog::area(catalog::subtract(coverage, objective.target))
                    - kDeepErrorWeight * catalog::area(catalog::subtract(coverage, objective.outer))
                    - boundaryCost(coverage, objective);
            };
            double bestScore = score(pieces);
            for (double padding : {0.125, 0.25, 0.5, 0.75}) {
                auto trial = original;
                for (auto &piece : trial) {
                    QTransform expansion;
                    expansion.translate(piece.bounds.center().x(), piece.bounds.center().y());
                    expansion.scale(1 + 2 * padding / std::max(piece.bounds.width(), padding),
                        1 + 2 * padding / std::max(piece.bounds.height(), padding));
                    expansion.translate(-piece.bounds.center().x(), -piece.bounds.center().y());
                    piece = makePiece(primitiveFor(piece.placement.shapeId, primitives), piece.placement.transform * expansion);
                }
                const double candidateScore = score(trial);
                if (candidateScore > bestScore) {
                    pieces = std::move(trial);
                    bestScore = candidateScore;
                }
            }
            report();
        }
        for (int pass = 0; pass < kRefitPasses && !stopWork(); ++pass) {
            refit(&pieces, objective, primitives, stopWork, 1);
            report();
            if (acceptable(support(pieces), objective)) {
                break;
            }
        }
        if (!acceptable(support(pieces), objective) && !incumbent.isEmpty()) {
            pieces = incumbent;
        }
        prune(&pieces, objective, stopWork);
        report();
        for (int round = 0; round < kMergeRounds && pieces.size() > 1 && !stopSpatialSearch(); ++round) {
            bool reduced = false;
            const auto order = removalOrder(pieces, objective);
            for (int trialIndex = 0; trialIndex < std::min(kDeletionTrials, static_cast<int>(order.size())); ++trialIndex) {
                auto trial = pieces;
                const auto changed = trial[order[trialIndex]].bounds;
                trial.removeAt(order[trialIndex]);
                refitNeighborhood(&trial, changed, objective, primitives, stopSpatialSearch);
                if (acceptable(support(trial), objective)) {
                    pieces = std::move(trial);
                    reduced = true;
                    break;
                }
            }
            if (!reduced) {
                reduced = merge(&pieces, objective, searchCatalog, stopSpatialSearch, false);
            }
            if (!reduced && !stopSpatialSearch()) {
                reduced = merge(&pieces, objective, searchCatalog, stopSpatialSearch, true);
            }
            if (!reduced) {
                break;
            }
            prune(&pieces, objective, stopSpatialSearch);
            report();
        }
        int exposedMerges = 0;
        while (!incumbent.isEmpty() && pieces.size() > 1 && !stopMerging()
                && merge(&pieces, objective, searchCatalog, stopMerging, false, true)) {
            ++exposedMerges;
            prune(&pieces, objective, stopWork);
            report();
        }
        result.diagnostics.insert(QStringLiteral("exposedBoundaryMerges"), exposedMerges);
        if (!incumbent.isEmpty()) {
            pieces = incumbent;
        }
        if (!pieces.isEmpty()) {
            auto polished = pieces;
            objective.boundaryWeight *= 4.0;
            refit(&polished, objective, primitives, stopWork, 1);
            const auto before = support(pieces);
            const auto after = support(polished);
            if (acceptable(after, objective)
                && objective.boundary->energy(objective.boundary->measure(after))
                    < objective.boundary->energy(objective.boundary->measure(before))) {
                pieces = std::move(polished);
            }
        }
        const auto coverage = support(pieces);
        result.fill.cancelled = stopped(cancelled);
        result.fill.shapeLimit = options.shapeBudget;
        result.fill.targetArea = region.area;
        result.fill.coveredArea = catalog::area(catalog::intersect(coverage, objective.target));
        result.fill.outsideArea = catalog::area(catalog::subtract(coverage, objective.target));
        result.fill.unfilled = catalog::painterPath(catalog::subtract(objective.target, coverage));
        result.diagnostics.insert(QStringLiteral("history"), history);
        result.diagnostics.insert(QStringLiteral("boundaryAllowance"), options.boundaryAllowance);
        result.diagnostics.insert(QStringLiteral("areaErrorRatio"), options.areaErrorRatio);
        result.diagnostics.insert(QStringLiteral("inwardAllowance"), options.inwardAllowance);
        result.diagnostics.insert(QStringLiteral("fittingInwardAllowance"), objective.fittingInwardAllowance);
        result.diagnostics.insert(QStringLiteral("targetBoundary"), objective.boundary->diagnostics(objective.targetMetrics));
        result.diagnostics.insert(QStringLiteral("observationScale"), options.observationScale);
        result.diagnostics.insert(QStringLiteral("maximumExcessTurnLimit"), kMaximumExcessTurn);
        result.diagnostics.insert(QStringLiteral("cornerDistanceLimit"), objective.cornerAllowance);
        result.diagnostics.insert(QStringLiteral("cornerDefectLimit"), objective.defectLimit);
        result.diagnostics.insert(QStringLiteral("missingBeyondInward"), catalog::area(catalog::subtract(objective.target,
            catalog::expanded(coverage, objective.inwardAllowance))));
        result.diagnostics.insert(QStringLiteral("outsideEnvelope"), catalog::area(catalog::subtract(coverage, objective.outer)));
        result.diagnostics.insert(QStringLiteral("boundaryQuality"), objective.boundary->diagnostics(objective.boundary->measure(coverage)));
        result.diagnostics.insert(QStringLiteral("boundaryEnergyLimit"), objective.qualityLimit);
        result.diagnostics.insert(QStringLiteral("qualityEvaluations"), objective.qualityEvaluations);
        result.diagnostics.insert(QStringLiteral("mergeRefinements"), objective.mergeRefinements);
        result.diagnostics.insert(QStringLiteral("evaluations"), objective.evaluations);
        result.diagnostics.insert(QStringLiteral("evaluationBudget"), options.evaluationBudget);
        result.diagnostics.insert(QStringLiteral("evaluationBudgetReached"), objective.evaluations >= options.evaluationBudget);
        result.diagnostics.insert(QStringLiteral("geometryModel"), QStringLiteral("triangle union, 1e-6 grid, reconstructed float transforms"));
        result.diagnostics.insert(QStringLiteral("displacementMetric"), QStringLiteral("asymmetric support distance, L-infinity world units"));
        QJsonObject histogram;
        for (const auto &piece : pieces) {
            const auto key = QString::number(piece.placement.shapeId);
            histogram[key] = histogram[key].toInt() + 1;
        }
        result.diagnostics.insert(QStringLiteral("shapeCounts"), histogram);
        result.diagnostics.insert(QStringLiteral("approximationVerified"), acceptable(coverage, objective));
        if (!result.fill.cancelled && !acceptable(coverage, objective)) {
            throw std::runtime_error("Compact fit could not meet the requested approximation allowance");
        }
        if (pieces.size() > options.shapeBudget) {
            throw std::runtime_error("Compact fit exceeds the shape budget");
        }
        if (!result.fill.cancelled) {
            for (const auto &piece : pieces) {
                result.fill.placements.push_back(piece.placement);
            }
        }
    } catch (const std::exception &exception) {
        result.fill.error = QString::fromUtf8(exception.what());
        result.fill.cancelled = stopped(cancelled);
    }

    return result;
}

} // namespace gui::compact
