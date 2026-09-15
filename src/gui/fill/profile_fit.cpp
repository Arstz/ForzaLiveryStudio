#include "profile_fit.h"
#include "profile_fit_batch.h"
#include "profile_fit_selection.h"
#include "compact_fit_quality.h"
#include "greedy_cover.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace gui::profile {
namespace {

using catalog::Polygons;
using Bits = QVector<quint64>;

constexpr int kProfileSamples = compute::kSamples;
constexpr int kSpanBatchSize = 32;
constexpr int kCandidatesPerSpan = 6;
constexpr int kMaximumCandidates = 2400;
constexpr int kMaximumTrials = 160000;
constexpr int kMaximumProfileWork = 640000;
constexpr int kGridExtent = 640;
constexpr int kBodyCenters = 24;
constexpr int kStructuralTrials = 50000;
constexpr int kTriangleShapeId = 103;
constexpr double kCornerAngle = 0.3;
constexpr double kMinimumTriangleCornerCross = 0.1;
constexpr double kTriangleStraightnessFraction = 0.1;
constexpr double kProfileError = 0.65;
constexpr double kTangentError = 0.2;
constexpr double kBoundaryOffset = 0.3;
constexpr double kMinimumLength = 1e-8;
constexpr double kSearchExtent = 512.0;
constexpr std::array<int, 24> kCurveShapes = {102, 109, 110, 120, 122, 124, 126, 127,
    128, 129, 130, 136, 139, 812, 901, 930, 2110, 2113, 2117, 2118, 2134, 2135, 2136, 2321};

struct Trace {
    QPolygonF points;
    QVector<double> lengths;
    QVector<double> corners;
    double perimeter = 0.0;
};

struct Profile {
    QPolygonF points;
    QTransform normalization;
    QTransform anchors;
    int primitive = 0;
};

struct CurveJob {
    QPolygonF target;
    QTransform anchors;
    double length = 0.0;
};

struct Candidate {
    PenPlacement placement;
    Polygons polygons;
    QPainterPath path;
    QRectF bounds;
    Bits cells;
    Bits boundary;
    double error = 0.0;
    double spill = 0.0;
    double span = 0.0;
};

struct Grid {
    QPointF origin;
    Bits target;
    int width = 0;
    int height = 0;
    double step = 1.0;
};

QPointF unit(const QPointF &point) {
    const double length = std::hypot(point.x(), point.y());

    return length > kMinimumLength ? point / length : QPointF();
}

double cross(const QPointF &first, const QPointF &second) {
    return first.x() * second.y() - first.y() * second.x();
}

bool stopped(const std::function<bool()> &cancelled) {
    return cancelled && cancelled();
}

Trace makeTrace(const QPolygonF &points) {
    Trace result;
    result.points = points;
    for (int index = 0; index < points.size(); ++index) {
        const auto incoming = unit(points[index] - points[(index + points.size() - 1) % points.size()]);
        const auto outgoing = unit(points[(index + 1) % points.size()] - points[index]);
        result.lengths.push_back(result.perimeter);
        if (std::abs(std::atan2(cross(incoming, outgoing), QPointF::dotProduct(incoming, outgoing))) > kCornerAngle) {
            result.corners.push_back(result.perimeter);
        }
        result.perimeter += QLineF(points[index], points[(index + 1) % points.size()]).length();
    }
    result.lengths.push_back(result.perimeter);

    return result;
}

QPointF pointAt(const Trace &trace, double offset) {
    offset = std::fmod(offset, trace.perimeter);
    if (offset < 0) {
        offset += trace.perimeter;
    }
    const auto upper = std::upper_bound(trace.lengths.begin(), trace.lengths.end(), offset);
    const int index = std::clamp(static_cast<int>(upper - trace.lengths.begin()) - 1, 0,
        static_cast<int>(trace.points.size()) - 1);
    const double length = trace.lengths[index + 1] - trace.lengths[index];
    const double fraction = length > kMinimumLength ? (offset - trace.lengths[index]) / length : 0.0;

    return trace.points[index] + (trace.points[(index + 1) % trace.points.size()] - trace.points[index]) * fraction;
}

QPolygonF sampleArc(const Trace &trace, double start, double length) {
    QPolygonF result;
    for (int index = 0; index < kProfileSamples; ++index) {
        result.push_back(pointAt(trace, start + length * index / (kProfileSamples - 1)));
    }

    return result;
}

QVector<Profile> sourceProfiles(const QVector<catalog::Primitive> &primitives) {
    QVector<Profile> result;
    for (int primitive = 0; primitive < primitives.size(); ++primitive) {
        const auto &shape = primitives[primitive].shape;
        if (std::find(kCurveShapes.begin(), kCurveShapes.end(), shape.shapeId) == kCurveShapes.end()) {
            continue;
        }
        for (const auto &polygon : shape.contours) {
            const auto trace = makeTrace(polygon);
            QVector<std::pair<double, double>> intervals;
            if (trace.corners.isEmpty()) {
                const int starts = shape.shapeId == 102 ? 1 : 4;
                for (int index = 0; index < starts; ++index) {
                    for (double fraction : {0.125, 0.25, 0.5}) {
                        intervals.push_back({trace.perimeter * index / starts, trace.perimeter * fraction});
                    }
                }
            } else {
                for (int index = 0; index < trace.corners.size(); ++index) {
                    const double start = trace.corners[index];
                    const double end = index + 1 < trace.corners.size() ? trace.corners[index + 1]
                        : trace.corners.front() + trace.perimeter;
                    intervals.push_back({start, end - start});
                    for (double fraction : {0.0, 0.25, 0.5}) {
                        intervals.push_back({start + (end - start) * fraction, (end - start) * 0.5});
                    }
                }
            }
            for (const auto &[start, length] : intervals) {
                auto points = sampleArc(trace, start, length);
                const QPointF origin = points[kProfileSamples / 2];
                const double scale = QLineF(points.front(), points.back()).length();
                if (scale < kMinimumLength || std::abs(cross(points.back() - points.front(), origin - points.front())) < scale * scale * 0.01) {
                    continue;
                }
                for (bool reverse : {false, true}) {
                    if (reverse) {
                        std::reverse(points.begin(), points.end());
                    }
                    Profile profile;
                    profile.primitive = primitive;
                    profile.normalization.scale(1.0 / scale, 1.0 / scale);
                    profile.normalization.translate(-origin.x(), -origin.y());
                    profile.points = profile.normalization.map(points);
                    profile.anchors = catalog::affineFromAnchors({profile.points.front(), profile.points[kProfileSamples / 2], profile.points.back()},
                        {QPointF(0, 0), QPointF(0, 1), QPointF(1, 0)});
                    result.push_back(std::move(profile));
                }
            }
        }
    }

    return result;
}

compute::Arc numericArc(const QPolygonF &points) {
    compute::Arc result;
    for (int index = 0; index < compute::kSamples; ++index) {
        result.points[index] = {points[index].x(), points[index].y()};
    }

    return result;
}

compute::Transform numericTransform(const QTransform &transform) {
    return {{transform.m11(), transform.m12(), transform.m21(), transform.m22(), transform.dx(), transform.dy()}};
}

QTransform fittedTransform(const compute::Transform &transform) {
    const auto &values = transform.values;

    return QTransform(values[0], values[1], values[2], values[3], values[4], values[5]);
}

bool probesInside(const PenPrimitive &shape, const QTransform &transform, const QPainterPath &envelope) {
    for (const auto &polygon : shape.contours) {
        const int stride = std::max(1, static_cast<int>(polygon.size()) / 16);
        for (int index = 0; index < polygon.size(); index += stride) {
            if (!envelope.contains(transform.map(polygon[index]))) {
                return false;
            }
        }
    }

    return true;
}

std::optional<Candidate> candidateFor(const PenPrimitive &shape, const QTransform &transform,
                                     const catalog::Region &region, const Polygons &outer,
                                     const compact::BoundaryModel &boundary, double scale) {
    Candidate result;
    result.placement.shapeId = shape.shapeId;
    result.placement.transform = catalog::emittedTransform(transform);
    result.polygons = catalog::mapped(shape, result.placement.transform);
    if (result.polygons.isEmpty() || !catalog::subtract(result.polygons, outer).isEmpty()) {
        return {};
    }
    for (const auto &polygon : result.polygons) {
        for (int index = 0; index < polygon.size(); ++index) {
            const auto next = polygon[(index + 1) % polygon.size()];
            const auto tangent = unit(next - polygon[index]);
            for (double fraction : {0.0, 0.5}) {
                const auto point = polygon[index] + (next - polygon[index]) * fraction;
                if (region.requiredPath.contains(point)) {
                    continue;
                }
                const auto reference = boundary.reference(point);
                if (!reference.corner && reference.distance > scale * 0.02
                    && std::abs(cross(tangent, reference.tangent)) > kTangentError) {
                    return {};
                }
            }
        }
    }
    result.path = catalog::painterPath(result.polygons);
    result.bounds = result.path.boundingRect();
    result.spill = catalog::area(catalog::subtract(result.polygons, region.required));
    result.placement.area = catalog::area(result.polygons);

    return result;
}

double searchScale(const catalog::Region &region, double observationScale) {
    return std::max(observationScale, std::max(region.bounds.width(), region.bounds.height()) / kSearchExtent);
}

QVector<CurveJob> curveJobs(const catalog::Region &region, const compact::FillOptions &options,
                           const std::function<bool()> &cancelled) {
    QVector<CurveJob> result;
    const double scale = searchScale(region, options.observationScale);
    for (const auto &polygon : region.required) {
        const auto trace = makeTrace(polygon);
        QVector<double> starts = trace.corners;
        for (double offset = 0; offset < trace.perimeter; offset += scale * 12.0) {
            starts.push_back(offset);
        }
        std::sort(starts.begin(), starts.end());
        for (double start : starts) {
            if (stopped(cancelled)) {
                return result;
            }
            double available = trace.perimeter * 0.5;
            for (double corner : trace.corners) {
                const double distance = std::fmod(corner - start + trace.perimeter, trace.perimeter);
                if (distance > 0.01) {
                    available = std::min(available, distance);
                }
            }
            double previousLength = -1.0;
            for (double requested : {356.0, 220.0, 136.0, 84.0, 52.0, 32.0, 20.0, 12.0}) {
                const double length = std::min(available, requested * scale);
                if (length < options.observationScale * 6.0 || std::abs(length - previousLength) < kMinimumLength) {
                    continue;
                }
                previousLength = length;
                auto target = sampleArc(trace, start, length);
                const auto chord = target.back() - target.front();
                if (std::abs(cross(chord, target[kProfileSamples / 2] - target.front()))
                    < QLineF({}, chord).length() * options.observationScale * 0.1) {
                    continue;
                }
                const auto original = target;
                for (int index = 0; index < target.size(); ++index) {
                    const auto tangent = unit(original[std::min(index + 1, static_cast<int>(target.size()) - 1)]
                        - original[std::max(0, index - 1)]);
                    target[index] += QPointF(tangent.y(), -tangent.x()) * (kBoundaryOffset * options.observationScale);
                }
                const auto anchors = catalog::affineFromAnchors({QPointF(0, 0), QPointF(0, 1), QPointF(1, 0)},
                    {target.front(), target[kProfileSamples / 2], target.back()});
                result.push_back({std::move(target), anchors, length});
            }
        }
    }

    return result;
}

struct RankedFit {
    compute::Fit fit;
    int profile = 0;
    double area = 0.0;
};

QVector<Candidate> validateCurveFits(const std::vector<RankedFit> &fits, int capacity, const CurveJob &job,
                                      const QVector<Profile> &profiles, const QVector<catalog::Primitive> &primitives,
                                      const catalog::Region &region, const Polygons &outer,
                                      const compact::BoundaryModel &boundary, double scale, int *validated) {
    auto localRegion = region;
    const auto envelope = catalog::painterPath(outer);
    const auto localBoundary = boundary;
    QVector<Candidate> result;
    std::vector<ProfileAlternative> alternatives;
    std::vector<ProfileAlternative> validatedAlternatives;
    const int validationCapacity = capacity + std::min(capacity, 2);
    for (const auto &fit : fits) {
        alternatives.push_back({fit.area, fit.fit.error / scale, fit.fit.tangentError, 0.0});
    }
    localRegion.requiredPath = catalog::painterPath(region.required);
    for (int index : profileAlternativeOrder(alternatives)) {
        const auto &ranked = fits[index];
        if (result.size() >= validationCapacity) {
            break;
        }
        const auto &profile = profiles[ranked.profile];
        const auto &shape = primitives[profile.primitive].shape;
        if (std::count_if(result.cbegin(), result.cend(), [&](const auto &candidate) {
                return candidate.placement.shapeId == shape.shapeId;
            }) >= 2) {
            continue;
        }
        if (std::any_of(result.cbegin(), result.cend(), [&](const auto &candidate) {
            return candidate.placement.shapeId == shape.shapeId
                && candidate.placement.transform == catalog::emittedTransform(
                    profile.normalization * fittedTransform(ranked.fit.transform));
        })) {
            continue;
        }
        const auto transform = profile.normalization * fittedTransform(ranked.fit.transform);
        if (!probesInside(shape, transform, envelope)) {
            continue;
        }
        ++*validated;
        auto candidate = candidateFor(shape, transform, localRegion, outer, localBoundary, scale);
        if (candidate) {
            candidate->error = ranked.fit.error;
            candidate->span = job.length;
            validatedAlternatives.push_back({candidate->placement.area - candidate->spill,
                ranked.fit.error / scale, ranked.fit.tangentError,
                candidate->spill / std::max(candidate->placement.area, kMinimumLength)});
            result.push_back(std::move(*candidate));
        }
    }

    QVector<Candidate> retained;
    const auto order = profileAlternativeOrder(validatedAlternatives);
    for (int index : order) {
        const int familyCount = std::count_if(retained.cbegin(), retained.cend(), [&](const auto &candidate) {
            return candidate.placement.shapeId == result[index].placement.shapeId;
        });
        if (familyCount < 2 && retained.size() < capacity) {
            retained.push_back(result[index]);
        }
    }

    return retained;
}

void addCurveCandidates(const catalog::Region &region, const Polygons &outer,
                        const QVector<catalog::Primitive> &primitives, const compact::BoundaryModel &boundary,
                        const compact::FillOptions &options, const std::function<bool()> &cancelled,
                        QVector<Candidate> *pool, QJsonObject *diagnostics) {
    const auto profiles = sourceProfiles(primitives);
    const auto jobs = curveJobs(region, options, cancelled);
    const int budget = static_cast<int>(std::clamp<qint64>(static_cast<qint64>(profiles.size()) * jobs.size(),
        kMaximumTrials, kMaximumProfileWork));
    std::vector<compute::Arc> sources;
    std::vector<compute::Arc> targets;
    for (const auto &profile : profiles) {
        sources.push_back(numericArc(profile.points));
    }
    for (const auto &job : jobs) {
        targets.push_back(numericArc(job.target));
    }
    ProfileFitter fitter(std::move(sources), std::move(targets), options.useGpu);
    ProfileWorkers workers;
    int trials = 0;
    int fits = 0;
    int processed = 0;
    int validations = 0;
    double validationMilliseconds = 0.0;
    for (int start = 0; start < jobs.size() && !stopped(cancelled); start += kSpanBatchSize) {
        const int end = std::min(start + kSpanBatchSize, static_cast<int>(jobs.size()));
        const int remainingSpans = jobs.size() - start;
        const int remainingSlots = std::max(0, kMaximumCandidates - static_cast<int>(pool->size()));
        std::vector<compute::Job> numericJobs;
        std::vector<int> capacities(end - start);
        std::vector<std::vector<RankedFit>> ranked(end - start);
        for (int index = start; index < end; ++index) {
            const auto &job = jobs[index];
            const int remaining = jobs.size() - index;
            const int limit = std::min(static_cast<int>(profiles.size()), (budget - trials) / remaining);
            const int local = index - start;
            capacities[local] = std::min(kCandidatesPerSpan,
                remainingSlots * (local + 1) / remainingSpans - remainingSlots * local / remainingSpans);
            for (int sample = 0; sample < limit; ++sample) {
                const int profileIndex = static_cast<qint64>(sample) * profiles.size() / limit;
                const auto &profile = profiles[profileIndex];
                const auto &shape = primitives[profile.primitive].shape;
                const auto initial = profile.anchors * job.anchors;
                const auto transform = profile.normalization * initial;
                const double area = shape.area * std::abs(transform.determinant());
                ++trials;
                if (capacities[local] == 0 || !std::isfinite(area) || area > region.area * 1.02
                    || area < options.observationScale * options.observationScale) {
                    continue;
                }
                numericJobs.push_back({numericTransform(initial), profileIndex, index,
                    std::max(options.observationScale * 2.0, job.length * 0.03)});
            }
            processed += limit > 0;
        }
        std::vector<compute::Fit> fitted;
        if (!fitter.evaluate(numericJobs, &fitted, cancelled)) {
            break;
        }
        for (size_t index = 0; index < fitted.size(); ++index) {
            const auto &fit = fitted[index];
            const auto &numeric = numericJobs[index];
            const auto &profile = profiles[numeric.source];
            const auto &shape = primitives[profile.primitive].shape;
            const auto transform = profile.normalization * fittedTransform(fit.transform);
            const double area = shape.area * std::abs(transform.determinant());
            fits += fit.fitted;
            if (!fit.valid || fit.error > kProfileError * options.observationScale
                || !std::isfinite(area) || area > region.area * 1.02 || area <= 0) {
                continue;
            }
            ranked[numeric.target - start].push_back({fit, numeric.source, area});
        }
        QElapsedTimer timer;
        timer.start();
        std::vector<QVector<Candidate>> retained(end - start);
        std::vector<int> checked(end - start, 0);
        if (!workers.run(end - start, [&](int local) {
            retained[local] = validateCurveFits(ranked[local], capacities[local], jobs[start + local],
                profiles, primitives, region, outer, boundary, options.observationScale, &checked[local]);
        }, cancelled)) {
            break;
        }
        validationMilliseconds += timer.nsecsElapsed() / 1e6;
        for (int local = 0; local < end - start; ++local) {
            *pool += retained[local];
            validations += checked[local];
        }
        if (options.workProgress) {
            options.workProgress(0, trials, budget);
        }
    }
    auto backend = fitter.diagnostics();
    backend.insert(QStringLiteral("validationMilliseconds"), validationMilliseconds);
    backend.insert(QStringLiteral("geometryValidations"), validations);
    diagnostics->insert(QStringLiteral("profileCompute"), backend);
    diagnostics->insert(QStringLiteral("profiles"), profiles.size());
    diagnostics->insert(QStringLiteral("profileTrials"), trials);
    diagnostics->insert(QStringLiteral("profileTrialBudget"), budget);
    diagnostics->insert(QStringLiteral("profileFits"), fits);
    diagnostics->insert(QStringLiteral("curveCandidates"), pool->size());
    diagnostics->insert(QStringLiteral("curveJobs"), jobs.size());
    diagnostics->insert(QStringLiteral("processedCurveJobs"), processed);
    diagnostics->insert(QStringLiteral("searchScale"), searchScale(region, options.observationScale));
}

void setRange(Bits *bits, int first, int end) {
    while (first < end) {
        const int count = std::min(end - first, 64 - first % 64);
        const quint64 mask = count == 64 ? ~quint64(0) : ((quint64(1) << count) - 1) << (first % 64);
        (*bits)[first / 64] |= mask;
        first += count;
    }
}

Bits rasterize(const Polygons &polygons, const Grid &grid) {
    Bits result((grid.width * grid.height + 63) / 64, 0);
    struct Crossing {
        double position;
        int winding;
    };
    for (int row = 0; row < grid.height; ++row) {
        const double ordinate = grid.origin.y() + (row + 0.5) * grid.step;
        QVector<Crossing> crossings;
        for (const auto &polygon : polygons) {
            for (int index = 0; index < polygon.size(); ++index) {
                const auto first = polygon[index];
                const auto second = polygon[(index + 1) % polygon.size()];
                if (ordinate < std::min(first.y(), second.y()) || ordinate >= std::max(first.y(), second.y())) {
                    continue;
                }
                crossings.push_back({first.x() + (second.x() - first.x()) * (ordinate - first.y()) / (second.y() - first.y()),
                    second.y() > first.y() ? 1 : -1});
            }
        }
        std::sort(crossings.begin(), crossings.end(), [](const auto &first, const auto &second) {
            return first.position < second.position;
        });
        int winding = 0;
        double start = 0.0;
        for (const auto &crossing : crossings) {
            if (winding != 0) {
                const int first = std::clamp(static_cast<int>(std::ceil((start - grid.origin.x()) / grid.step - 0.5)), 0, grid.width);
                const int end = std::clamp(static_cast<int>(std::ceil((crossing.position - grid.origin.x()) / grid.step - 0.5)), 0, grid.width);
                setRange(&result, row * grid.width + first, row * grid.width + end);
            }
            winding += crossing.winding;
            start = crossing.position;
        }
    }

    return result;
}

Grid makeGrid(const catalog::Region &region, double scale) {
    Grid result;
    result.origin = region.bounds.topLeft();
    result.step = std::max(scale * 0.5, std::max(region.bounds.width(), region.bounds.height()) / kGridExtent);
    result.width = std::max(1, static_cast<int>(std::ceil(region.bounds.width() / result.step)));
    result.height = std::max(1, static_cast<int>(std::ceil(region.bounds.height() / result.step)));
    const QRectF frame = region.bounds.adjusted(-scale * 2.0, -scale * 2.0, scale * 2.0, scale * 2.0);
    const Polygons complement = catalog::subtract({QPolygonF(frame)}, region.required);
    result.target = rasterize(catalog::subtract(region.required, catalog::expanded(complement, scale * 0.35)), result);

    return result;
}

QVector<QPointF> boundaryWitnesses(const catalog::Region &region, double scale) {
    QVector<QPointF> result;
    for (const auto &polygon : region.required) {
        const auto trace = makeTrace(polygon);
        const int count = std::max(3, static_cast<int>(std::ceil(trace.perimeter / scale)));
        for (int index = 0; index < count; ++index) {
            const double offset = trace.perimeter * index / count;
            const auto tangent = unit(pointAt(trace, offset + scale * 0.25) - pointAt(trace, offset - scale * 0.25));
            result.push_back(pointAt(trace, offset) - QPointF(tangent.y(), -tangent.x()) * (scale * 0.35));
        }
    }

    return result;
}

void indexCandidate(Candidate *candidate, const Grid &grid, const QVector<QPointF> &witnesses) {
    candidate->cells = rasterize(candidate->polygons, grid);
    candidate->boundary = Bits((witnesses.size() + 63) / 64, 0);
    for (int index = 0; index < witnesses.size(); ++index) {
        if (candidate->bounds.contains(witnesses[index]) && candidate->path.contains(witnesses[index])) {
            candidate->boundary[index / 64] |= quint64(1) << (index % 64);
        }
    }
}

int marginal(const Bits &candidate, const Bits &missing) {
    int result = 0;
    for (int index = 0; index < missing.size(); ++index) {
        result += std::popcount(candidate[index] & missing[index]);
    }

    return result;
}

void removeCovered(Bits *missing, const Bits &candidate) {
    for (int index = 0; index < missing->size(); ++index) {
        (*missing)[index] &= ~candidate[index];
    }
}

const PenPrimitive *primitiveFor(int id, const QVector<catalog::Primitive> &primitives) {
    const auto found = std::find_if(primitives.begin(), primitives.end(), [id](const auto &primitive) {
        return primitive.shape.shapeId == id;
    });

    return found == primitives.end() ? nullptr : &found->shape;
}

QTransform bodyTransform(const PenPrimitive &shape, const QPointF &center, double angle, double ratio, double radius) {
    QTransform transform;
    transform.translate(center.x(), center.y());
    transform.rotate(angle);
    transform.scale(radius * ratio / shape.bounds.width(), radius / shape.bounds.height());
    transform.translate(-shape.bounds.center().x(), -shape.bounds.center().y());

    return transform;
}

void addBodyAt(const QPointF &center, const catalog::Region &region, const Polygons &outer,
               const QVector<catalog::Primitive> &primitives, const compact::BoundaryModel &boundary,
               double scale, QVector<Candidate> *pool) {
    for (int id : {102, 101, 109, 110, 124, 2117}) {
        const auto *shape = primitiveFor(id, primitives);
        if (!shape) {
            continue;
        }
        for (double ratio : {1.0, 2.0, 4.0, 8.0}) {
            for (int angle = 0; angle < (id == 101 || id == 102 ? 180 : 360); angle += ratio == 1.0 && id == 102 ? 180 : 30) {
                double lower = 0.0;
                double upper = std::max(region.bounds.width(), region.bounds.height()) * 2.0;
                for (int iteration = 0; iteration < 12; ++iteration) {
                    const double radius = (lower + upper) * 0.5;
                    if (probesInside(*shape, bodyTransform(*shape, center, angle, ratio, radius), region.requiredPath)) {
                        lower = radius;
                    } else {
                        upper = radius;
                    }
                }
                if (lower < scale) {
                    continue;
                }
                auto candidate = candidateFor(*shape, bodyTransform(*shape, center, angle, ratio, lower * 0.995), region, outer, boundary, scale);
                if (candidate && candidate->spill < scale * scale * 0.01) {
                    pool->push_back(std::move(*candidate));
                }
            }
        }
    }
}

void addBodyCandidates(const catalog::Region &region, const Polygons &outer,
                       const QVector<catalog::Primitive> &primitives, const compact::BoundaryModel &boundary,
                       double scale, const std::function<bool()> &cancelled, QVector<Candidate> *pool) {
    QVector<std::pair<QPointF, double>> centers;
    const double spacing = std::max(region.bounds.width(), region.bounds.height()) / 24.0;
    for (double ordinate = region.bounds.top() + spacing * 0.5; ordinate < region.bounds.bottom(); ordinate += spacing) {
        for (double abscissa = region.bounds.left() + spacing * 0.5; abscissa < region.bounds.right(); abscissa += spacing) {
            const QPointF point(abscissa, ordinate);
            if (region.requiredPath.contains(point)) {
                centers.push_back({point, boundary.reference(point).distance});
            }
        }
    }
    std::stable_sort(centers.begin(), centers.end(), [](const auto &first, const auto &second) {
        return first.second > second.second;
    });
    QVector<QPointF> retained;
    for (const auto &[center, distance] : centers) {
        const bool nearby = std::any_of(retained.begin(), retained.end(), [&](const auto &other) {
            return QLineF(center, other).length() < std::max(spacing, distance * 0.65);
        });
        if (nearby) {
            continue;
        }
        retained.push_back(center);
        addBodyAt(center, region, outer, primitives, boundary, scale, pool);
        if (retained.size() >= kBodyCenters || stopped(cancelled)) {
            break;
        }
    }
}

void addStraightCandidates(const catalog::Region &region, const Polygons &outer,
                           const QVector<catalog::Primitive> &primitives, const compact::BoundaryModel &boundary,
                           double scale, const std::function<bool()> &cancelled, QVector<Candidate> *pool) {
    const double placementScale = searchScale(region, scale);
    int trials = 0;
    for (const auto &polygon : region.required) {
        const auto trace = makeTrace(polygon);
        QVector<double> starts = trace.corners;
        for (double offset = 0; offset < trace.perimeter; offset += 20.0 * placementScale) {
            starts.push_back(offset);
        }
        for (int index = 0; index < polygon.size(); ++index) {
            if (trace.lengths[index + 1] - trace.lengths[index] > placementScale * 4.0) {
                starts.push_back(trace.lengths[index]);
            }
        }
        for (double start : starts) {
            double available = trace.perimeter * 0.5;
            for (double corner : trace.corners) {
                const double distance = std::fmod(corner - start + trace.perimeter, trace.perimeter);
                if (distance > 0.01) {
                    available = std::min(available, distance);
                }
            }
            double minimum = 0.0;
            double maximum = available;
            const auto initialTangent = unit(pointAt(trace, start + scale * 0.5) - pointAt(trace, start));
            for (int iteration = 0; iteration < 14; ++iteration) {
                const double trial = (minimum + maximum) * 0.5;
                const auto points = sampleArc(trace, start, trial);
                if (std::all_of(points.begin(), points.end(), [&](const auto &point) {
                    return std::abs(cross(initialTangent, point - points.front())) < scale * 0.08;
                })) {
                    minimum = trial;
                } else {
                    maximum = trial;
                }
            }
            available = minimum;
            double previousLength = -1.0;
            for (double fraction : {1.0, 0.75, 0.5}) {
                const double length = available * fraction;
                if (length < 2.0 * scale || std::abs(previousLength - length) < kMinimumLength) {
                    continue;
                }
                previousLength = length;
                const auto target = sampleArc(trace, start, length);
                const auto tangent = unit(target.back() - target.front());
                if (std::any_of(target.begin(), target.end(), [&](const auto &point) {
                    return std::abs(cross(tangent, point - target.front())) > scale * 0.1;
                })) {
                    continue;
                }
                const QPointF inward(-tangent.y(), tangent.x());
                for (int id : {101}) {
                    const auto *shape = primitiveFor(id, primitives);
                    if (!shape || shape->contours.isEmpty()) {
                        continue;
                    }
                    const auto &contour = shape->contours.front();
                    const auto sourceTangent = unit(contour[1] - contour[0]);
                    const QPointF sourceNormal(-sourceTangent.y(), sourceTangent.x());
                    for (double shear : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
                        for (double depth : {128.0, 64.0, 32.0, 16.0, 8.0, 4.0, 2.0}) {
                            if (++trials > kStructuralTrials || stopped(cancelled)) {
                                return;
                            }
                            const auto transform = catalog::affineFromAnchors({contour[0], contour[1], contour[0] + sourceNormal * QLineF(contour[0], contour[1]).length()},
                                {target.front() - inward * (scale * 0.02), target.back() - inward * (scale * 0.02),
                                    target.front() + (inward + tangent * shear) * (depth * placementScale)});
                            auto candidate = candidateFor(*shape, transform, region, outer, boundary, scale);
                            if (candidate) {
                                pool->push_back(std::move(*candidate));
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void addCornerTriangles(const catalog::Region &region, const Polygons &outer,
                        const QVector<catalog::Primitive> &primitives, const compact::BoundaryModel &boundary,
                        double scale, const std::function<bool()> &cancelled, QVector<Candidate> *pool) {
    const auto *shape = primitiveFor(kTriangleShapeId, primitives);
    if (!shape || shape->contours.size() != 1) {
        return;
    }
    const auto source = catalog::convexHull(shape->contours.front());
    if (source.size() != 3) {
        return;
    }
    for (const auto &polygon : region.required) {
        const auto trace = makeTrace(polygon);
        for (int index = 0; index < trace.corners.size() && !stopped(cancelled); ++index) {
            const double start = trace.corners[(index + trace.corners.size() - 1) % trace.corners.size()];
            const double middle = trace.corners[index];
            const double end = trace.corners[(index + 1) % trace.corners.size()];
            const auto corner = pointAt(trace, middle);
            const auto first = pointAt(trace, start);
            const auto last = pointAt(trace, end);
            if (cross(unit(first - corner), unit(last - corner)) >= -kMinimumTriangleCornerCross) {
                continue;
            }
            const auto straight = [&](double offset, double finish) {
                const auto points = sampleArc(trace, offset, std::fmod(finish - offset + trace.perimeter, trace.perimeter));
                const auto tangent = unit(points.back() - points.front());

                return std::all_of(points.begin(), points.end(), [&](const auto &point) {
                    return std::abs(cross(tangent, point - points.front())) <= scale * kTriangleStraightnessFraction;
                });
            };
            if (!straight(start, middle) || !straight(middle, end)) {
                continue;
            }
            const auto transform = catalog::affineFromAnchors({source[0], source[1], source[2]}, {first, corner, last});
            auto candidate = candidateFor(*shape, transform, region, outer, boundary, scale);
            if (candidate) {
                pool->push_back(std::move(*candidate));
            }
        }
    }
}

void addCornerCandidates(const catalog::Region &region, const Polygons &outer,
                         const QVector<catalog::Primitive> &primitives, const compact::BoundaryModel &boundary,
                         double scale, const std::function<bool()> &cancelled, QVector<Candidate> *pool) {
    const auto envelope = catalog::painterPath(outer);
    const double placementScale = searchScale(region, scale);
    int trials = 0;
    for (const auto &polygon : region.required) {
        const auto trace = makeTrace(polygon);
        for (double offset : trace.corners) {
            const auto corner = pointAt(trace, offset);
            const auto first = unit(pointAt(trace, offset - scale * 0.5) - corner);
            const auto second = unit(pointAt(trace, offset + scale * 0.5) - corner);
            if (cross(first, second) >= -0.1) {
                continue;
            }
            for (int id : {101, 103, 109, 110}) {
                const auto *shape = primitiveFor(id, primitives);
                if (!shape) {
                    continue;
                }
                for (const auto &contour : shape->contours) {
                    const auto sourceTrace = makeTrace(contour);
                    for (double sourceOffset : sourceTrace.corners) {
                        const double extent = std::max(shape->bounds.width(), shape->bounds.height());
                        const auto sourceCorner = pointAt(sourceTrace, sourceOffset);
                        const auto sourceFirst = unit(pointAt(sourceTrace, sourceOffset - extent * 0.001) - sourceCorner) * extent;
                        const auto sourceSecond = unit(pointAt(sourceTrace, sourceOffset + extent * 0.001) - sourceCorner) * extent;
                        if (cross(sourceFirst, sourceSecond) >= -extent * extent * 0.1) {
                            continue;
                        }
                        for (double length : {256.0, 128.0, 64.0, 32.0, 16.0, 8.0, 4.0}) {
                            for (double ratio : {0.25, 0.5, 1.0, 2.0, 4.0}) {
                                if (++trials > kStructuralTrials || stopped(cancelled)) {
                                    return;
                                }
                                const auto transform = catalog::affineFromAnchors({sourceCorner, sourceCorner + sourceFirst, sourceCorner + sourceSecond},
                                    {corner, corner + first * (length * placementScale), corner + second * (length * ratio * placementScale)});
                                if (!probesInside(*shape, transform, envelope)) {
                                    continue;
                                }
                                auto candidate = candidateFor(*shape, transform, region, outer, boundary, scale);
                                if (candidate) {
                                    pool->push_back(std::move(*candidate));
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

Polygons coverageOf(const QVector<Candidate> &pool, const QVector<int> &selected) {
    Polygons result;
    for (int index : selected) {
        result += pool[index].polygons;
    }

    return catalog::unite(result);
}

void includeBits(Bits *coverage, const Bits &candidate) {
    for (int index = 0; index < coverage->size(); ++index) {
        (*coverage)[index] |= candidate[index];
    }
}

bool containsBits(const Bits &coverage, const Bits &required) {
    for (int index = 0; index < required.size(); ++index) {
        if (required[index] & ~coverage[index]) {
            return false;
        }
    }

    return true;
}

bool removable(const QVector<Candidate> &pool, const QVector<int> &selected, int omit,
                const Bits &requiredCells, const Bits &requiredBoundary) {
    Bits cells(requiredCells.size(), 0);
    Bits boundary(requiredBoundary.size(), 0);
    for (int index : selected) {
        if (index != omit) {
            includeBits(&cells, pool[index].cells);
            includeBits(&boundary, pool[index].boundary);
        }
    }

    return containsBits(cells, requiredCells) && containsBits(boundary, requiredBoundary);
}

void reduceSelection(const QVector<Candidate> &pool, const Grid &grid, const compact::BoundaryModel &model,
                      const catalog::Region &region, const compact::FillOptions &options,
                      const std::function<bool()> &cancelled, QVector<int> *selected) {
    Bits requiredCells(grid.target.size(), 0);
    Bits requiredBoundary(pool.front().boundary.size(), 0);
    for (int index : *selected) {
        includeBits(&requiredCells, pool[index].cells);
        includeBits(&requiredBoundary, pool[index].boundary);
    }
    for (int index = 0; index < requiredCells.size(); ++index) {
        requiredCells[index] &= grid.target[index];
    }
    for (int position = selected->size() - 1; position >= 0; --position) {
        if (removable(pool, *selected, (*selected)[position], requiredCells, requiredBoundary)) {
            selected->removeAt(position);
        }
    }
    for (int pass = 0; pass < 8 && !stopped(cancelled); ++pass) {
        Bits onceCells(requiredCells.size(), 0), twiceCells(requiredCells.size(), 0);
        Bits onceBoundary(requiredBoundary.size(), 0), twiceBoundary(requiredBoundary.size(), 0);
        for (int index : *selected) {
            for (int word = 0; word < onceCells.size(); ++word) {
                twiceCells[word] |= onceCells[word] & pool[index].cells[word];
                onceCells[word] |= pool[index].cells[word];
            }
            for (int word = 0; word < onceBoundary.size(); ++word) {
                twiceBoundary[word] |= onceBoundary[word] & pool[index].boundary[word];
                onceBoundary[word] |= pool[index].boundary[word];
            }
        }
        QVector<Bits> privateCells, privateBoundary;
        for (int index : *selected) {
            Bits cells(requiredCells.size(), 0), boundary(requiredBoundary.size(), 0);
            for (int word = 0; word < cells.size(); ++word) {
                cells[word] = pool[index].cells[word] & requiredCells[word] & ~twiceCells[word];
            }
            for (int word = 0; word < boundary.size(); ++word) {
                boundary[word] = pool[index].boundary[word] & requiredBoundary[word] & ~twiceBoundary[word];
            }
            privateCells.push_back(std::move(cells));
            privateBoundary.push_back(std::move(boundary));
        }
        const double baselineEnergy = model.energy(model.measure(coverageOf(pool, *selected)));
        QVector<int> best = *selected;
        for (int replacement = 0; replacement < pool.size() && !stopped(cancelled); ++replacement) {
            QVector<int> eligible;
            for (int position = 0; position < selected->size(); ++position) {
                if (containsBits(pool[replacement].cells, privateCells[position])
                    && containsBits(pool[replacement].boundary, privateBoundary[position])) {
                    eligible.push_back((*selected)[position]);
                }
            }
            if (eligible.size() < 2 || selected->contains(replacement)) {
                continue;
            }
            auto trial = *selected;
            trial.push_back(replacement);
            for (auto iterator = eligible.rbegin(); iterator != eligible.rend(); ++iterator) {
                if (removable(pool, trial, *iterator, requiredCells, requiredBoundary)) {
                    trial.removeOne(*iterator);
                }
            }
            if (trial.size() >= best.size()) {
                continue;
            }
            const auto coverage = coverageOf(pool, trial);
            const double errorArea = catalog::area(catalog::subtract(region.required, coverage))
                + catalog::area(catalog::subtract(coverage, region.required));
            if (errorArea <= region.area * options.areaErrorRatio
                && model.energy(model.measure(coverage)) <= baselineEnergy + options.observationScale * 0.01) {
                best = std::move(trial);
            }
        }
        if (best.size() >= selected->size()) {
            break;
        }
        *selected = std::move(best);
    }
}

QVector<int> selectCandidates(QVector<Candidate> *pool, const catalog::Region &region,
                              const Grid &grid, const QVector<QPointF> &witnesses,
                              const compact::FillOptions &options, const std::function<bool()> &cancelled,
                              QJsonObject *diagnostics) {
    Bits missing = grid.target;
    Bits boundary((witnesses.size() + 63) / 64, 0);
    int scoreEvaluations = 0;
    setRange(&boundary, 0, witnesses.size());
    for (auto &candidate : *pool) {
        if (stopped(cancelled)) {
            return {};
        }
        indexCandidate(&candidate, grid, witnesses);
    }
    const double boundaryWeight = region.area / std::max(1, static_cast<int>(witnesses.size())) * 4.0;
    const auto selected = greedyCover(pool->size(), options.shapeBudget,
        [&](int index) {
            ++scoreEvaluations;
            const auto &candidate = (*pool)[index];

            return (marginal(candidate.cells, missing) * grid.step * grid.step
                + marginal(candidate.boundary, boundary) * boundaryWeight) / (1.0 + candidate.error * 0.5 / options.observationScale);
        },
        [&](int index) {
            removeCovered(&missing, (*pool)[index].cells);
            removeCovered(&boundary, (*pool)[index].boundary);
        }, [&] { return stopped(cancelled); });
    diagnostics->insert(QStringLiteral("selectionScoreEvaluations"), scoreEvaluations);
    diagnostics->insert(QStringLiteral("selectionShapeLimit"), options.shapeBudget);
    diagnostics->insert(QStringLiteral("selectionShapeLimitReached"), selected.size() >= options.shapeBudget);
    diagnostics->insert(QStringLiteral("missingCells"), marginal(missing, missing));
    diagnostics->insert(QStringLiteral("missingWitnesses"), marginal(boundary, boundary));

    return selected;
}

catalog::FillResult buildSeed(const PenFillRequest &request, const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options, const std::function<bool()> &cancelled) {
    catalog::FillResult result;
    QJsonObject timings;
    QElapsedTimer stageTimer;
    stageTimer.start();
    const auto recordTime = [&](const QString &name) {
        timings.insert(name, stageTimer.nsecsElapsed() / 1e6);
        stageTimer.start();
    };
    try {
        if (!std::isfinite(options.boundaryAllowance) || options.boundaryAllowance <= 0
            || !std::isfinite(options.observationScale) || options.observationScale <= 0 || options.shapeBudget < 1
            || !std::isfinite(options.inwardAllowance) || options.inwardAllowance <= 0
            || !std::isfinite(options.areaErrorRatio) || options.areaErrorRatio <= 0 || options.areaErrorRatio >= 1
            || options.evaluationBudget < 1) {
            throw std::runtime_error("Profile fit requires positive contour allowances and a shape budget");
        }
        const auto region = catalog::buildRegion(request, cancelled);
        const auto outer = catalog::expanded(region.required, options.boundaryAllowance);
        const compact::BoundaryModel boundary(region.required, options.observationScale);
        const auto grid = makeGrid(region, options.observationScale);
        const auto witnesses = boundaryWitnesses(region, options.observationScale);
        QVector<Candidate> pool;
        recordTime(QStringLiteral("setup"));
        addCurveCandidates(region, outer, primitives, boundary, options, cancelled, &pool, &result.diagnostics);
        recordTime(QStringLiteral("curves"));
        addStraightCandidates(region, outer, primitives, boundary, options.observationScale, cancelled, &pool);
        recordTime(QStringLiteral("straightEdges"));
        const int beforeTriangles = pool.size();
        addCornerTriangles(region, outer, primitives, boundary, options.observationScale, cancelled, &pool);
        result.diagnostics.insert(QStringLiteral("cornerTriangleCandidates"), pool.size() - beforeTriangles);
        addCornerCandidates(region, outer, primitives, boundary, options.observationScale, cancelled, &pool);
        recordTime(QStringLiteral("corners"));
        result.diagnostics.insert(QStringLiteral("boundaryCandidates"), pool.size());
        addBodyCandidates(region, outer, primitives, boundary, options.observationScale, cancelled, &pool);
        recordTime(QStringLiteral("interior"));
        result.diagnostics.insert(QStringLiteral("totalCandidates"), pool.size());
        auto selected = selectCandidates(&pool, region, grid, witnesses, options, cancelled, &result.diagnostics);
        recordTime(QStringLiteral("selection"));
        result.diagnostics.insert(QStringLiteral("greedyCount"), selected.size());
        if (!pool.isEmpty() && !stopped(cancelled)) {
            reduceSelection(pool, grid, boundary, region, options, cancelled, &selected);
        }
        recordTime(QStringLiteral("reduction"));
        const auto coverage = coverageOf(pool, selected);
        const auto missing = catalog::subtract(region.required, coverage);
        const auto spill = catalog::subtract(coverage, region.required);
        result.fill.targetArea = region.area;
        result.fill.coveredArea = region.area - catalog::area(missing);
        result.fill.outsideArea = catalog::area(spill);
        result.fill.unfilled = catalog::painterPath(missing);
        result.diagnostics.insert(QStringLiteral("boundary"), boundary.diagnostics(boundary.measure(coverage)));
        result.diagnostics.insert(QStringLiteral("count"), selected.size());
        result.diagnostics.insert(QStringLiteral("missingArea"), catalog::area(missing));
        result.diagnostics.insert(QStringLiteral("spillArea"), catalog::area(spill));
        result.diagnostics.insert(QStringLiteral("deepMissingArea"), catalog::area(catalog::subtract(region.required, catalog::expanded(coverage, options.inwardAllowance))));
        QJsonObject counts;
        QJsonArray selectedDetails;
        for (int index : selected) {
            result.fill.placements.push_back(pool[index].placement);
            const auto key = QString::number(pool[index].placement.shapeId);
            counts.insert(key, counts.value(key).toInt() + 1);
            selectedDetails.push_back(QJsonObject{{QStringLiteral("id"), pool[index].placement.shapeId},
                {QStringLiteral("area"), pool[index].placement.area}, {QStringLiteral("span"), pool[index].span},
                {QStringLiteral("error"), pool[index].error}, {QStringLiteral("witnesses"), marginal(pool[index].boundary, pool[index].boundary)}});
        }
        result.diagnostics.insert(QStringLiteral("selected"), selectedDetails);
        result.diagnostics.insert(QStringLiteral("shapeIds"), counts);
        if (stopped(cancelled)) {
            result.fill.cancelled = true;
            result.fill.placements.clear();
        }
    } catch (const std::exception &exception) {
        result.fill.error = QString::fromUtf8(exception.what());
        result.fill.cancelled = stopped(cancelled);
        result.fill.placements.clear();
    }
    result.diagnostics.insert(QStringLiteral("stageMilliseconds"), timings);

    return result;
}

} // namespace

static catalog::FillResult fillAttempt(const PenFillRequest &request, const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options, const std::function<bool()> &cancelled,
                               const std::function<void(int, double, double)> &progress) {
    if (!options.initialPlacements.isEmpty()) {
        return compact::fillRegion(request, primitives, options, cancelled, progress);
    }
    const int seedWork = std::max(1, options.evaluationBudget / 5);
    const int totalWork = options.evaluationBudget + seedWork;
    auto seedOptions = options;
    seedOptions.workProgress = [&](int count, int evaluated, int budget) {
        if (options.workProgress) {
            options.workProgress(count, static_cast<int>(static_cast<double>(evaluated) * seedWork / budget), totalWork);
        }
    };
    auto seed = buildSeed(request, primitives, seedOptions, cancelled);
    if (!seed.fill.error.isEmpty() || seed.fill.cancelled) {
        return seed;
    }
    if (seed.fill.placements.isEmpty()) {
        seed.fill.error = QStringLiteral("Curve-first search could not construct a placement seed");

        return seed;
    }
    if (options.workProgress) {
        options.workProgress(seed.fill.placements.size(), seedWork, totalWork);
    }
    auto refinement = options;
    refinement.initialPlacements = seed.fill.placements;
    refinement.workProgress = [&](int count, int evaluated, int) {
        if (options.workProgress) {
            options.workProgress(count, seedWork + std::min(evaluated, options.evaluationBudget), totalWork);
        }
    };
    auto result = compact::fillRegion(request, primitives, refinement, cancelled, progress);
    if (options.retainFailedFill && !result.fill.error.isEmpty() && result.fill.placements.isEmpty()
        && !result.fill.cancelled && !stopped(cancelled)) {
        seed.fill.error = result.fill.error;
        seed.fill.shapeLimit = options.shapeBudget;
        result.fill = std::move(seed.fill);
        result.diagnostics.insert(QStringLiteral("retainedAfterError"), true);
        result.diagnostics.insert(QStringLiteral("retainedProfileSeed"), true);
        result.diagnostics.insert(QStringLiteral("approximationVerified"), false);
    }
    result.diagnostics.insert(QStringLiteral("strategy"), QStringLiteral("curve-first profile cover with union refinement"));
    result.diagnostics.insert(QStringLiteral("profileSeed"), seed.diagnostics);
    result.diagnostics.insert(QStringLiteral("profileTrialBudget"), seed.diagnostics.value(QStringLiteral("profileTrialBudget")));
    result.diagnostics.insert(QStringLiteral("structuralTrialBudget"), kStructuralTrials);
    if (options.workProgress && !result.fill.cancelled) {
        options.workProgress(result.fill.placements.size(), totalWork, totalWork);
    }

    return result;
}

catalog::FillResult fillRegion(const PenFillRequest &request, const QVector<catalog::Primitive> &primitives,
                               const compact::FillOptions &options, const std::function<bool()> &cancelled,
                               const std::function<void(int, double, double)> &progress) {
    const bool retry = options.initialPlacements.isEmpty() && std::isfinite(options.boundaryAllowance)
        && options.boundaryAllowance > compact::kDefaultBoundaryAllowance;
    const int attempts = retry ? 2 : 1;
    const int attemptWork = options.evaluationBudget + std::max(1, options.evaluationBudget / 5);
    catalog::FillResult result;
    QJsonArray history;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto current = options;
        if (retry && attempt == 0) {
            current.boundaryAllowance = compact::kDefaultBoundaryAllowance;
        }
        current.workProgress = [&](int count, int evaluated, int) {
            if (options.workProgress) {
                options.workProgress(count, attempt * attemptWork + evaluated, attempts * attemptWork);
            }
        };
        auto candidate = fillAttempt(request, primitives, current, cancelled, progress);
        history.push_back(QJsonObject{{QStringLiteral("boundaryAllowance"), current.boundaryAllowance},
            {QStringLiteral("error"), candidate.fill.error}, {QStringLiteral("count"), candidate.fill.placements.size()}});
        const bool finished = candidate.fill.error.isEmpty() || candidate.fill.cancelled || stopped(cancelled);
        if (finished || result.fill.placements.isEmpty() || !candidate.fill.placements.isEmpty()) {
            result = std::move(candidate);
        }
        if (finished) {
            break;
        }
    }
    result.diagnostics.insert(QStringLiteral("requestedBoundaryAllowance"), options.boundaryAllowance);
    result.diagnostics.insert(QStringLiteral("attempts"), history);
    if (options.workProgress && !result.fill.cancelled && options.evaluationBudget > 0) {
        options.workProgress(result.fill.placements.size(), attempts * attemptWork, attempts * attemptWork);
    }

    return result;
}

} // namespace gui::profile
