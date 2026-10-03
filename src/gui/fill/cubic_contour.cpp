#include "cubic_contour.h"

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace gui {
namespace {
constexpr double kNotchMaximumSpan = 100.0;
constexpr double kNotchAxialEdgeLength = 6.0;
constexpr double kNotchAxialFraction = 0.2;
constexpr double kNotchSimplificationTolerance = 2.5;
constexpr int kNotchMaximumPoints = 10;
constexpr double kMaximumMaskMismatchFraction = 0.12;
constexpr int kMaskBoundaryBand = 5;
constexpr int kMaximumMaskFitAttempts = 5;
constexpr double kOutwardMaskMissFraction = 0.005;
constexpr double kOutwardMaskGrowthFraction = 0.4;
constexpr double kDetailedMaskMissFraction = 0.0075;
constexpr double kDetailedMaskSpillFraction = 0.1;
constexpr double kDetailedRasterTolerance = 3.0;
constexpr std::array<double, 3> kHandleRepairScales{0.9, 0.75, 0.5};

double length(QPointF p) {
    return std::hypot(p.x(), p.y());
}
QPointF unit(QPointF p) {
    const double n = length(p);
    return n > 1e-12 ? p / n : QPointF{};
}
double cross(QPointF a, QPointF b) {
    return a.x() * b.y() - a.y() * b.x();
}
double area(const QPolygonF &p) {
    double a = 0;
    for (int i = 0; i < p.size(); ++i)
        a += cross(p[i], p[(i + 1) % p.size()]);
    return a * 0.5;
}
double distance2(QPointF p, QPointF a, QPointF b) {
    const QPointF d = b - a;
    const double n = QPointF::dotProduct(d, d);
    const double t = n > 1e-20 ? std::clamp(QPointF::dotProduct(p - a, d) / n, 0.0, 1.0) : 0;
    const QPointF delta = p - a - d * t;
    return QPointF::dotProduct(delta, delta);
}
QPolygonF simplifyPolyline(const QPolygonF &points, double tolerance) {
    if (points.size() < 3)
        return points;
    QVector<char> retained(points.size(), false);
    retained.front() = true;
    retained.back() = true;
    QVector<std::pair<int, int>> pending{{0, points.size() - 1}};
    while (!pending.isEmpty()) {
        const auto [first, last] = pending.back();
        pending.removeLast();
        double maximum = tolerance * tolerance;
        int split = -1;
        for (int index = first + 1; index < last; ++index) {
            const double error = distance2(points[index], points[first], points[last]);
            if (error > maximum) {
                maximum = error;
                split = index;
            }
        }
        if (split >= 0) {
            retained[split] = true;
            pending.push_back({first, split});
            pending.push_back({split, last});
        }
    }
    QPolygonF simplified;
    for (int index = 0; index < points.size(); ++index)
        if (retained[index])
            simplified.push_back(points[index]);

    return simplified;
}
quint64 key(QPoint p) {
    return (quint64(quint32(p.x())) << 32) | quint32(p.y());
}

struct Reference {
    QPolygonF p;
    QVector<double> arc;
    double perimeter = 0;
    double outlierFraction = 0;
    explicit Reference(QPolygonF polygon, double trim = 0)
        : p(std::move(polygon)), outlierFraction(trim) {
        while (p.size() > 1 && p.front() == p.back())
            p.removeLast();
        for (int i = 0; i < p.size(); ++i) {
            arc.push_back(perimeter);
            perimeter += length(p[(i + 1) % p.size()] - p[i]);
        }
        arc.push_back(perimeter);
    }
    QPointF point(double s) const {
        s = std::fmod(s, perimeter);
        if (s < 0)
            s += perimeter;
        const int i = std::min(int(p.size()) - 1,
                               int(std::upper_bound(arc.begin(), arc.end(), s) - arc.begin()) - 1);
        const double span = arc[i + 1] - arc[i];
        return p[i] + (p[(i + 1) % p.size()] - p[i]) * (span > 0 ? (s - arc[i]) / span : 0);
    }
    QPointF tangent(double s, double scale) const {
        QPointF direction;
        const int n = std::max(2, int(std::ceil(scale)));
        for (int i = 1; i <= n; ++i) {
            const double d = scale * i / n;
            direction += (point(s + d) - point(s - d)) * d;
        }
        return unit(direction);
    }
};

struct Knot {
    double s;
    bool corner;
    QPointF tangent;
};
struct Span {
    Knot a, b;
    FillBoundarySegment curve;
    double error = 0;
    double split = 0;
};

QPolygonF rectilinearNotch(const Reference &ref, double start, double end) {
    if (end - start > kNotchMaximumSpan)
        return {};
    QVector<std::pair<double, QPointF>> ordered{{start, ref.point(start)},
                                                {end, ref.point(end)}};
    double axialLength = 0.0;
    int axialEdges = 0;
    for (int index = 0; index < ref.p.size(); ++index) {
        const QPointF edge = ref.p[(index + 1) % ref.p.size()] - ref.p[index];
        const bool axial = length(edge) >= kNotchAxialEdgeLength
            && std::min(std::abs(edge.x()), std::abs(edge.y())) < 0.01;
        for (double shift : {0.0, ref.perimeter}) {
            const double edgeStart = ref.arc[index] + shift;
            const double edgeEnd = ref.arc[index + 1] + shift;
            const double overlap = std::max(0.0,
                std::min(end, edgeEnd) - std::max(start, edgeStart));
            if (axial && overlap > 0.0) {
                axialLength += overlap;
                ++axialEdges;
            }
            if (edgeStart > start + 1e-6 && edgeStart < end - 1e-6)
                ordered.push_back({edgeStart, ref.p[index]});
        }
    }
    if (axialEdges < 1 || axialLength < (end - start) * kNotchAxialFraction)
        return {};
    std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        return left.first < right.first;
    });
    QPolygonF raw;
    for (const auto &entry : ordered)
        if (raw.isEmpty() || length(entry.second - raw.back()) > 1e-6)
            raw.push_back(entry.second);
    const auto simplified = simplifyPolyline(raw, kNotchSimplificationTolerance);

    return simplified.size() >= 3 && simplified.size() <= kNotchMaximumPoints
        ? simplified : QPolygonF();
}

QVector<QPointF> samples(const Reference &ref, double a, double b) {
    const int count = std::max(2, int(std::ceil((b - a) / 0.75)));
    QVector<QPointF> points;
    points.reserve(count + 1);
    for (int i = 0; i <= count; ++i)
        points.push_back(ref.point(a + (b - a) * i / count));
    return points;
}

Span fitSpan(const Reference &ref, Knot a, Knot b) {
    Span result{a, b};
    const auto data = samples(ref, a.s, b.s);
    const auto p0 = data.front(), p3 = data.back();
    const double arc = b.s - a.s;
    const QPointF ta = a.corner ? unit(ref.point(a.s + std::min(2.0, arc / 4)) - p0) : a.tangent;
    const QPointF tb = b.corner ? unit(p3 - ref.point(b.s - std::min(2.0, arc / 4))) : b.tangent;
    QVector<double> u(data.size());
    for (int i = 0; i < u.size(); ++i)
        u[i] = double(i) / (u.size() - 1);
    FillBoundarySegment curve{p0, p0 + ta * (arc / 3), p3, true, p3 - tb * (arc / 3)};
    for (int iteration = 0; iteration < 8; ++iteration) {
        double aa = 0, ab = 0, bb = 0, ar = 0, br = 0;
        for (int i = 1; i + 1 < data.size(); ++i) {
            const double t = u[i], v = 1 - t, b0 = v * v * v, b1 = 3 * v * v * t,
                         b2 = 3 * v * t * t, b3 = t * t * t;
            const QPointF va = ta * b1, vb = -tb * b2,
                          r = data[i] - p0 * (b0 + b1) - p3 * (b2 + b3);
            aa += QPointF::dotProduct(va, va);
            ab += QPointF::dotProduct(va, vb);
            bb += QPointF::dotProduct(vb, vb);
            ar += QPointF::dotProduct(va, r);
            br += QPointF::dotProduct(vb, r);
        }
        const double determinant = aa * bb - ab * ab;
        double alpha = arc / 3, beta = arc / 3;
        if (determinant > 1e-12) {
            alpha = (ar * bb - br * ab) / determinant;
            beta = (br * aa - ar * ab) / determinant;
        }
        // Positive bounded handles prevent local loops and reversed tangents.
        alpha = std::clamp(alpha, arc * 0.01, arc * 0.8);
        beta = std::clamp(beta, arc * 0.01, arc * 0.8);
        curve.control = p0 + ta * alpha;
        curve.control2 = p3 - tb * beta;
        for (int i = 1; i + 1 < data.size(); ++i) {
            const QPointF d = curve.derivative(u[i]), r = curve.point(u[i]) - data[i];
            const double denominator =
                QPointF::dotProduct(d, d) + QPointF::dotProduct(r, curve.secondDerivative(u[i]));
            if (denominator > 1e-12)
                u[i] =
                    std::clamp(u[i] - QPointF::dotProduct(r, d) / denominator, u[i - 1], u[i + 1]);
        }
    }
    double maximum = 0;
    int worst = data.size() / 2;
    QVector<double> errors;
    errors.reserve(std::max(0, int(data.size()) - 2));
    for (int i = 1; i + 1 < data.size(); ++i) {
        const double error = length(curve.point(u[i]) - data[i]);
        errors.push_back(error);
        if (error > maximum) {
            maximum = error;
            worst = i;
        }
    }
    if (ref.outlierFraction > 0 && !errors.isEmpty()) {
        const int keep = std::clamp(int(std::ceil(errors.size() * (1 - ref.outlierFraction))) - 1,
                                    0, int(errors.size()) - 1);
        std::nth_element(errors.begin(), errors.begin() + keep, errors.end());
        maximum = errors[keep];
    }
    // Also measure curve -> boundary. Corresponding sample error alone can miss a bulge.
    const int steps = std::clamp(int(std::ceil(curve.controlLength() / 0.5)), 8, 1024);
    for (int i = 1; i < steps; ++i) {
        const QPointF q = curve.point(double(i) / steps);
        double best = std::numeric_limits<double>::max();
        for (int j = 1; j < data.size(); ++j)
            best = std::min(best, distance2(q, data[j - 1], data[j]));
        maximum = std::max(maximum, std::sqrt(best));
    }
    result.curve = curve;
    result.error = maximum;
    result.split = a.s + arc * std::clamp(double(worst) / (data.size() - 1), 0.2, 0.8);
    return result;
}

double lineError(const Reference &ref, const Span &span) {
    const auto data = samples(ref, span.a.s, span.b.s);
    const QPointF start = data.front(), end = data.back();
    double error = 0;
    for (const QPointF &point : data)
        error = std::max(error, std::sqrt(distance2(point, start, end)));
    const int steps = std::clamp(int(std::ceil(length(end - start) / 0.5)), 8, 1024);
    for (int i = 1; i < steps; ++i) {
        const QPointF point = start + (end - start) * (double(i) / steps);
        double nearest = std::numeric_limits<double>::max();
        for (int j = 1; j < data.size(); ++j)
            nearest = std::min(nearest, distance2(point, data[j - 1], data[j]));
        error = std::max(error, std::sqrt(nearest));
    }

    return error;
}

void fitRecursive(const Reference &ref, Knot a, Knot b, double tolerance, double tangentScale,
                  QVector<Span> &out, int depth = 0) {
    Span span = fitSpan(ref, a, b);
    if (span.error <= tolerance || b.s - a.s < 0.8 || depth >= 24) {
        out.push_back(span);
        return;
    }
    Knot middle{span.split, false, ref.tangent(span.split, tangentScale)};
    fitRecursive(ref, a, middle, tolerance, tangentScale, out, depth + 1);
    fitRecursive(ref, middle, b, tolerance, tangentScale, out, depth + 1);
}

double energy(const Span &span, const Reference &ref) {
    // Inspired by Alvarez & Morel (2026), doi:10.1007/s10851-026-01282-0.
    // Arc-length-weighted distance energy, plus a reverse term to protect narrow details.
    const auto data = samples(ref, span.a.s, span.b.s);
    const int count = std::clamp(int(std::ceil(span.curve.controlLength())), 12, 256);
    double e = 0;
    QVector<QPointF> curve;
    for (int i = 0; i <= count; ++i) {
        const double t = double(i) / count;
        const QPointF p = span.curve.point(t);
        curve.push_back(p);
        double d = std::numeric_limits<double>::max();
        for (int j = 1; j < data.size(); ++j)
            d = std::min(d, distance2(p, data[j - 1], data[j]));
        e += std::sqrt(d) * length(span.curve.derivative(t)) / count;
    }
    for (const auto p : data) {
        double d = std::numeric_limits<double>::max();
        for (int j = 1; j < curve.size(); ++j)
            d = std::min(d, distance2(p, curve[j - 1], curve[j]));
        e += std::sqrt(d) * (span.b.s - span.a.s) / (data.size() - 1);
    }
    return e;
}

QVector<Knot> initialKnots(const Reference &ref, double scale) {
    struct Candidate {
        double s, score;
    };
    QVector<Candidate> candidates;
    const double radius = std::min(scale, ref.perimeter / 12);
    // Persistent turning at two scales distinguishes corners from stair steps.
    for (double s = 0; s < ref.perimeter; s += 0.5) {
        const auto turn = [&](double r) {
            return std::acos(std::clamp(QPointF::dotProduct(unit(ref.point(s) - ref.point(s - r)),
                                                            unit(ref.point(s + r) - ref.point(s))),
                                        -1.0, 1.0));
        };
        const double small = turn(radius), large = turn(radius * 2);
        if (small > 1.05 && small > large * 0.72)
            candidates.push_back({s, small});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](auto a, auto b) { return a.score > b.score; });
    QVector<Knot> result;
    for (const auto candidate : candidates) {
        bool near = false;
        for (const auto &k : result) {
            const double d = std::abs(k.s - candidate.s);
            if (std::min(d, ref.perimeter - d) < radius * 1.5) {
                near = true;
                break;
            }
        }
        if (!near)
            result.push_back({candidate.s, true, {}});
    }
    // Smooth contours need no artificial hard seam.
    if (result.isEmpty())
        result.push_back({0, false, ref.tangent(0, radius)});
    std::sort(result.begin(), result.end(), [](auto a, auto b) { return a.s < b.s; });
    QVector<Knot> seeds;
    for (int i = 0; i < result.size(); ++i) {
        const Knot a = result[i];
        const double end =
            i + 1 < result.size() ? result[i + 1].s : result.front().s + ref.perimeter;
        seeds.push_back(a);
        const int pieces = std::max(1, int(std::ceil((end - a.s) / (ref.perimeter / 4))));
        for (int j = 1; j < pieces; ++j) {
            const double s = a.s + (end - a.s) * j / pieces;
            seeds.push_back({s, false, ref.tangent(s, radius)});
        }
    }
    return seeds;
}

void classifyJoins(QVector<PenPoint> &points) {
    for (auto &p : points) {
        const auto incoming = unit(-p.incoming), outgoing = unit(p.outgoing);
        p.kind = QPointF::dotProduct(incoming, outgoing) > 1.0 - 1e-8 ? PenPointKind::Soft
                                                                      : PenPointKind::Hard;
    }
}

std::vector<std::uint8_t> removeRasterPinholes(const std::vector<std::uint8_t> &mask, QSize size,
                                               QRect bounds) {
    if (size.width() <= 0 || size.height() <= 0 ||
        mask.size() != size_t(size.width()) * size.height())
        return mask;
    bounds = bounds.intersected(QRect(QPoint(0, 0), size));
    auto cleaned = mask;
    std::vector<std::uint8_t> seen(mask.size());
    QVector<int> pending, component;
    constexpr int kMaximumPinholeArea = 8;
    const int width = size.width();
    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const int first = y * width + x;
            if (mask[first] || seen[first])
                continue;
            pending.clear();
            component.clear();
            pending.push_back(first);
            seen[first] = 1;
            bool exterior = false;
            while (!pending.isEmpty()) {
                const int index = pending.back();
                pending.removeLast();
                const int px = index % width, py = index / width;
                if (px == bounds.left() || px == bounds.right() || py == bounds.top() ||
                    py == bounds.bottom())
                    exterior = true;
                if (component.size() <= kMaximumPinholeArea)
                    component.push_back(index);
                for (const QPoint neighbor : {QPoint(px - 1, py), QPoint(px + 1, py),
                                              QPoint(px, py - 1), QPoint(px, py + 1)}) {
                    if (!bounds.contains(neighbor))
                        continue;
                    const int adjacent = neighbor.y() * width + neighbor.x();
                    if (!mask[adjacent] && !seen[adjacent]) {
                        seen[adjacent] = 1;
                        pending.push_back(adjacent);
                    }
                }
            }
            if (!exterior && component.size() <= kMaximumPinholeArea)
                for (int index : component)
                    cleaned[index] = 1;
        }
    return cleaned;
}

CubicFitOptions rasterFitOptions(const QPolygonF &polygon,
                                 const CubicFitOptions &options) {
    if (!options.adaptToRasterNoise)
        return options;
    QVector<double> deviations;
    double perimeter = 0;
    const Reference ref(polygon);
    // Pixel teeth inflate the exact perimeter and make A/P claim the stroke is
    // narrower precisely when its boundary needs more smoothing.
    const int chords = std::max(8, int(std::ceil(ref.perimeter / 8)));
    QPointF previous = ref.point(0);
    for (int i = 1; i <= chords; ++i) {
        const QPointF current = ref.point(ref.perimeter * i / chords);
        perimeter += length(current - previous);
        previous = current;
    }
    const double signedArea = area(polygon);
    const double radius = std::min(8.0, ref.perimeter / 12);
    for (double s = 0; s < ref.perimeter; s += 1.0) {
        const QPointF before = ref.point(s - radius), after = ref.point(s + radius);
        const QPointF chord = after - before;
        const double n = length(chord);
        if (n > 1e-9)
            deviations.push_back(std::abs(cross(ref.point(s) - (before + after) * 0.5, chord)) /
                                 n);
    }
    if (deviations.isEmpty() || perimeter <= 0)
        return options;
    // A robust quantile ignores isolated real corners. Two deviations cover both sides
    // of the noisy edge; quarter-pixel steps avoid reacting to insignificant changes.
    const int index = int(0.9 * (deviations.size() - 1));
    std::nth_element(deviations.begin(), deviations.begin() + index, deviations.end());
    const double noiseTolerance = std::ceil(deviations[index] * 8) / 4;
    const double strokeWidth = 2 * std::abs(signedArea) / perimeter;
    CubicFitOptions result = options;
    result.tolerance = std::max(options.tolerance, std::min(noiseTolerance, strokeWidth * 0.3));
    if (options.rasterToleranceCeiling > 0.0)
        result.tolerance = std::min(result.tolerance,
                                    std::max(options.tolerance, options.rasterToleranceCeiling));
    // Detect persistent corners beyond the noise band, not the tips of raster teeth.
    result.cornerScale = std::max(options.cornerScale, 4 * result.tolerance);
    if (result.tolerance >= 2.0)
        result.outlierFraction = std::max(options.outlierFraction, 0.05);
    return result;
}

bool matchesRasterMask(const PenContour &contour, const std::vector<std::uint8_t> &mask,
                       QSize size, QRect bounds) {
    const QRect fittedBounds = contour.path.boundingRect().toAlignedRect();
    const QRect checkBounds = bounds.united(fittedBounds).adjusted(-1, -1, 1, 1)
        .intersected(QRect(QPoint(0, 0), size));
    QImage raster(checkBounds.size(), QImage::Format_Grayscale8);
    raster.fill(255);
    {
        QPainter painter(&raster);
        painter.translate(-checkBounds.topLeft());
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawPath(contour.path);
    }
    int selectedPixels = 0;
    int differingPixels = 0;
    for (int y = checkBounds.top(); y <= checkBounds.bottom(); ++y) {
        const auto *rasterRow = raster.constScanLine(y - checkBounds.top());
        for (int x = checkBounds.left(); x <= checkBounds.right(); ++x) {
            const bool selected = mask[size_t(y) * size.width() + x] != 0;
            selectedPixels += selected;
            if (selected == (rasterRow[x - checkBounds.left()] == 0))
                continue;
            ++differingPixels;
            bool nearBoundary = false;
            for (int dy = -kMaskBoundaryBand; dy <= kMaskBoundaryBand && !nearBoundary; ++dy)
                for (int dx = -kMaskBoundaryBand; dx <= kMaskBoundaryBand; ++dx) {
                    const int neighborX = x + dx, neighborY = y + dy;
                    if (neighborX < 0 || neighborY < 0 || neighborX >= size.width()
                        || neighborY >= size.height()
                        || (mask[size_t(neighborY) * size.width() + neighborX] != 0) != selected) {
                        nearBoundary = true;
                        break;
                    }
                }
            if (!nearBoundary)
                return false;
        }
    }

    return differingPixels <= selectedPixels * kMaximumMaskMismatchFraction;
}

struct RasterMaskDifference {
    int missed = 0;
    int spill = 0;
};

RasterMaskDifference rasterMaskDifference(const PenContour &contour,
                                          const std::vector<std::uint8_t> &mask,
                                          QSize size, QRect bounds) {
    bounds = bounds.united(contour.path.boundingRect().toAlignedRect())
                 .intersected(QRect(QPoint(0, 0), size));
    QImage raster(bounds.size(), QImage::Format_Grayscale8);
    raster.fill(255);
    {
        QPainter painter(&raster);
        painter.translate(-bounds.topLeft());
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawPath(contour.path);
    }
    RasterMaskDifference difference;
    for (int y = bounds.top(); y <= bounds.bottom(); ++y) {
        const auto *row = raster.constScanLine(y - bounds.top());
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            const bool selected = mask[size_t(y) * size.width() + x] != 0;
            const bool covered = row[x - bounds.left()] == 0;
            difference.missed += selected && !covered;
            difference.spill += !selected && covered;
        }
    }

    return difference;
}

std::vector<std::uint8_t> expandMask(const std::vector<std::uint8_t> &mask, QSize size,
                                     QRect bounds, int radius) {
    auto expanded = mask;
    const QRect imageBounds(QPoint(0, 0), size);
    for (int step = 0; step < radius; ++step) {
        const auto previous = expanded;
        bounds = bounds.adjusted(-1, -1, 1, 1).intersected(imageBounds);
        for (int y = bounds.top(); y <= bounds.bottom(); ++y)
            for (int x = bounds.left(); x <= bounds.right(); ++x) {
                const size_t index = size_t(y) * size.width() + x;
                if (!previous[index])
                    continue;
                if (x > 0)
                    expanded[index - 1] = 1;
                if (x + 1 < size.width())
                    expanded[index + 1] = 1;
                if (y > 0)
                    expanded[index - size.width()] = 1;
                if (y + 1 < size.height())
                    expanded[index + size.width()] = 1;
            }
    }

    return expanded;
}

int contourNodeCount(const QVector<PenLoop> &loops) {
    int count = 0;
    for (const auto &loop : loops)
        count += loop.points.size();

    return count;
}
} // namespace

QVector<QPolygonF> pixelBoundaryLoops(const std::vector<std::uint8_t> &mask, QSize size,
                                      QRect bounds) {
    QVector<QPolygonF> loops;
    if (size.width() <= 0 || size.height() <= 0 ||
        mask.size() != size_t(size.width()) * size.height())
        return loops;
    bounds = bounds.intersected(QRect(QPoint(0, 0), size));
    struct Edge {
        QPoint a, b;
        int direction;
        bool used = false;
    };
    QVector<Edge> edges;
    QHash<quint64, QVector<int>> starts;
    const auto selected = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < size.width() && y < size.height() &&
               mask[size_t(y) * size.width() + x] != 0;
    };
    const auto add = [&](QPoint a, QPoint b, int d) {
        starts[key(a)].push_back(edges.size());
        edges.push_back({a, b, d});
    };
    for (int y = bounds.top(); y <= bounds.bottom(); ++y)
        for (int x = bounds.left(); x <= bounds.right(); ++x) {
            if (!selected(x, y))
                continue;
            if (!selected(x, y - 1))
                add({x, y}, {x + 1, y}, 0);
            if (!selected(x + 1, y))
                add({x + 1, y}, {x + 1, y + 1}, 1);
            if (!selected(x, y + 1))
                add({x + 1, y + 1}, {x, y + 1}, 2);
            if (!selected(x - 1, y))
                add({x, y + 1}, {x, y}, 3);
        }
    for (int first = 0; first < edges.size(); ++first) {
        if (edges[first].used)
            continue;
        QPolygonF loop;
        int current = first;
        bool closed = false;
        for (int count = 0; count <= edges.size(); ++count) {
            auto &edge = edges[current];
            edge.used = true;
            loop.push_back(edge.a);
            if (edge.b == edges[first].a) {
                closed = true;
                break;
            }
            int next = -1, best = 5;
            for (int candidate : starts.value(key(edge.b))) {
                if (edges[candidate].used)
                    continue;
                const int turn = (edges[candidate].direction - edge.direction + 4) % 4;
                // Keep the selected cell on the right at a diagonal contact.
                const int rank = turn == 1 ? 0 : turn == 0 ? 1 : turn == 3 ? 2 : 3;
                if (rank < best) {
                    best = rank;
                    next = candidate;
                }
            }
            if (next < 0)
                break;
            current = next;
        }
        if (!closed)
            return {};
        QPolygonF reduced;
        for (int i = 0; i < loop.size(); ++i) {
            const auto a = loop[(i + loop.size() - 1) % loop.size()], b = loop[i],
                       c = loop[(i + 1) % loop.size()];
            if (cross(b - a, c - b) != 0)
                reduced.push_back(b);
        }
        if (reduced.size() >= 3)
            loops.push_back(std::move(reduced));
    }
    std::sort(loops.begin(), loops.end(),
              [](const auto &a, const auto &b) { return std::abs(area(a)) > std::abs(area(b)); });
    return loops;
}

QVector<PenPoint> fitCubicContour(const QPolygonF &boundary, const CubicFitOptions &options) {
    if (boundary.size() < 3 || !std::isfinite(options.tolerance) || options.tolerance <= 0 ||
        !std::isfinite(options.cornerScale) || options.cornerScale <= 0)
        return {};
    for (auto p : boundary)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            return {};
    const Reference ref(boundary, options.outlierFraction);
    if (ref.perimeter <= 1e-9)
        return {};
    const double scale = std::min(options.cornerScale, ref.perimeter / 12);
    auto knots = initialKnots(ref, scale);
    const double tangentScale = std::min(scale * 2, ref.perimeter / 12);
    for (auto &k : knots)
        if (!k.corner)
            k.tangent = ref.tangent(k.s, tangentScale);
    QVector<Span> spans;
    for (int i = 0; i < knots.size(); ++i) {
        Knot end = knots[(i + 1) % knots.size()];
        if (i + 1 == knots.size())
            end.s += ref.perimeter;
        fitRecursive(ref, knots[i], end, options.tolerance, tangentScale, spans);
    }
    // Merge adjacent smooth spans whenever one cubic satisfies the same error bound.
    for (int pass = 0; pass < 3; ++pass) {
        bool changed = false;
        for (int i = 0; i + 1 < spans.size() && spans.size() > 3;) {
            if (!spans[i].b.corner) {
                const auto merged = fitSpan(ref, spans[i].a, spans[i + 1].b);
                if (merged.error <= options.tolerance) {
                    spans[i] = merged;
                    spans.removeAt(i + 1);
                    changed = true;
                    continue;
                }
            }
            ++i;
        }
        if (!changed)
            break;
    }
    // Jointly relax shared tangents and slide smooth endpoints along the reference.
    // Each accepted update lowers distance energy and keeps the maximum-error gate.
    for (int pass = 0; pass < std::clamp(options.refinementPasses, 0, 8); ++pass) {
        bool changed = false;
        for (int i = 0; i < spans.size(); ++i) {
            const int previous = (i + spans.size() - 1) % spans.size();
            if (spans[i].a.corner)
                continue;
            double best = energy(spans[previous], ref) + energy(spans[i], ref);
            const Knot original = spans[i].a;
            for (int trial = 0; trial < 4; ++trial) {
                Knot middle = original;
                if (trial < 2)
                    middle.s += (trial == 0 ? -0.5 : 0.5) / double(pass + 1);
                else {
                    const double angle = (trial == 2 ? -0.06 : 0.06) / double(pass + 1);
                    middle.tangent = {original.tangent.x() * std::cos(angle) -
                                          original.tangent.y() * std::sin(angle),
                                      original.tangent.x() * std::sin(angle) +
                                          original.tangent.y() * std::cos(angle)};
                }
                Knot leftEnd = middle;
                if (i == 0)
                    leftEnd.s += ref.perimeter;
                if (leftEnd.s - spans[previous].a.s < 1 || spans[i].b.s - middle.s < 1)
                    continue;
                auto left = fitSpan(ref, spans[previous].a, leftEnd),
                     right = fitSpan(ref, middle, spans[i].b);
                if (left.error > options.tolerance || right.error > options.tolerance)
                    continue;
                const double score = energy(left, ref) + energy(right, ref);
                if (score < best - 1e-5) {
                    spans[previous] = left;
                    spans[i] = right;
                    best = score;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }
    for (Span &span : spans) {
        if (!span.a.corner || !span.b.corner)
            continue;
        const QPointF start = span.curve.start, end = span.curve.end;
        const double chordLength = length(end - start);
        double bow = 0;
        if (options.adaptToRasterNoise && chordLength <= 25.0) {
            for (int sample = 1; sample < 16; ++sample)
                bow = std::max(bow, std::sqrt(distance2(
                    span.curve.point(double(sample) / 16), start, end)));
        }
        const double areaChange = std::abs(span.curve.signedArea() - cross(start, end) * 0.5);
        const bool shallowRasterSpan = options.adaptToRasterNoise && chordLength <= 25.0
            && bow <= 4.0 && areaChange <= 50.0;
        const double error = shallowRasterSpan ? 0.0 : lineError(ref, span);
        if (!shallowRasterSpan && error > options.tolerance)
            continue;
        const QPointF chord = span.curve.end - span.curve.start;
        span.curve.control = span.curve.start + chord / 3;
        span.curve.control2 = span.curve.start + chord * (2.0 / 3.0);
        span.curve.curved = false;
        span.error = error;
    }
    QVector<PenPoint> result;
    for (int i = 0; i < spans.size(); ++i) {
        const auto &curve = spans[i].curve,
                   &before = spans[(i + spans.size() - 1) % spans.size()].curve;
        result.push_back({curve.start, spans[i].a.corner ? PenPointKind::Hard : PenPointKind::Soft,
                          before.control2 - curve.start, curve.control - curve.start, true});
    }
    if (options.preserveRasterNotches) {
        QVector<char> removed(result.size(), false);
        QVector<char> linearIncoming(result.size(), false);
        QVector<char> linearOutgoing(result.size(), false);
        QVector<QPolygonF> inserted(result.size());
        for (int first = 0; first < spans.size(); ++first) {
            if (!spans[first].a.corner)
                continue;
            int last = (first + 1) % spans.size();
            int interior = 0;
            while (last != first && !spans[last].a.corner) {
                ++interior;
                last = (last + 1) % spans.size();
            }
            if (last == first || (interior == 0
                && spans[first].curve.flatness() <= 1e-9))
                continue;
            const double start = spans[first].a.s;
            const double end = spans[last].a.s
                + (last <= first ? ref.perimeter : 0.0);
            const auto notch = rectilinearNotch(ref, start, end);
            if (notch.isEmpty())
                continue;
            linearOutgoing[first] = true;
            linearIncoming[last] = true;
            for (int index = (first + 1) % spans.size(); index != last;
                 index = (index + 1) % spans.size())
                removed[index] = true;
            for (int index = 1; index + 1 < notch.size(); ++index)
                inserted[first].push_back(notch[index]);
        }
        QVector<PenPoint> adjusted;
        QVector<char> linearEdge;
        for (int index = 0; index < result.size(); ++index) {
            if (removed[index])
                continue;
            PenPoint point = result[index];
            if (linearIncoming[index])
                point.incoming = {};
            if (linearOutgoing[index])
                point.outgoing = {};
            adjusted.push_back(point);
            linearEdge.push_back(linearOutgoing[index]);
            for (const QPointF &position : inserted[index]) {
                adjusted.push_back({position, PenPointKind::Hard, {}, {}, true});
                linearEdge.push_back(true);
            }
        }
        result = std::move(adjusted);
        for (int index = 0; index < result.size(); ++index) {
            const int next = (index + 1) % result.size();
            if (linearEdge[index] && result[index].kind == PenPointKind::Hard
                && result[next].kind == PenPointKind::Hard) {
                const QPointF handle = (result[next].position - result[index].position) / 3;
                result[index].outgoing = handle;
                result[next].incoming = -handle;
            }
        }
    }
    splitCurvedHardSpans(result);
    return result;
}

QVector<PenLoop> cubicPathLoops(const QPainterPath &path, QString *error) {
    QVector<PenLoop> loops;
    QVector<PenPoint> points;
    bool open = false;
    const auto finish = [&]() {
        if (points.isEmpty())
            return;
        if (points.size() > 1 && length(points.back().position - points.front().position) < 1e-7) {
            points.front().incoming = points.back().incoming;
            points.removeLast();
            if (points.size() == 1) {
                const auto anchor = points.front();
                FillBoundarySegment closed{anchor.position, anchor.position + anchor.outgoing,
                                           anchor.position, true,
                                           anchor.position + anchor.incoming};
                const auto [first, tail] = closed.split(1.0 / 3.0);
                const auto [middle, last] = tail.split(0.5);
                points = {{first.start, PenPointKind::Hard, last.control2 - first.start,
                           first.control - first.start, true},
                          {middle.start, PenPointKind::Soft, first.control2 - middle.start,
                           middle.control - middle.start, true},
                          {last.start, PenPointKind::Soft, middle.control2 - last.start,
                           last.control - last.start, true}};
            }
        } else if (points.size() > 1) {
            open = true;
            points.clear();
            return;
        }
        classifyJoins(points);
        splitCurvedHardSpans(points);
        // A two-cubic closed shape is valid; split one span to support existing editing minima.
        if (points.size() == 2)
            insertPenAnchor(points, 1, penSegments(points)[0].point(0.5));
        classifyJoins(points);
        if (points.size() >= 3)
            loops.push_back({points, PenLoopKind::Outer});
        points.clear();
    };
    for (int i = 0; i < path.elementCount(); ++i) {
        const auto e = path.elementAt(i);
        const QPointF p(e.x, e.y);
        if (e.isMoveTo()) {
            finish();
            points.push_back({p, PenPointKind::Hard, {}, {}, true});
        } else if (e.isLineTo() && !points.isEmpty()) {
            points.back().outgoing = (p - points.back().position) / 3;
            points.push_back({p, PenPointKind::Hard, -points.back().outgoing, {}, true});
        } else if (e.type == QPainterPath::CurveToElement && !points.isEmpty() &&
                   i + 2 < path.elementCount()) {
            const auto c2 = path.elementAt(++i), end = path.elementAt(++i);
            points.back().outgoing = p - points.back().position;
            points.push_back({{end.x, end.y},
                              PenPointKind::Hard,
                              QPointF(c2.x - end.x, c2.y - end.y),
                              {},
                              true});
        }
    }
    finish();
    if (open) {
        if (error)
            *error = QStringLiteral("The outline contains an open contour");
        return {};
    }
    std::sort(loops.begin(), loops.end(), [](const auto &a, const auto &b) {
        double aa = 0, bb = 0;
        for (const auto &s : penSegments(a.points))
            aa += s.signedArea();
        for (const auto &s : penSegments(b.points))
            bb += s.signedArea();
        return std::abs(aa) > std::abs(bb);
    });
    for (int i = 1; i < loops.size(); ++i) {
        if (!penPath(loops.front().points).contains(loops[i].points.front().position)) {
            if (error)
                *error = QStringLiteral("The outline contains multiple independent outer contours");
            return {};
        }
        loops[i].kind = PenLoopKind::Cutout;
    }
    const auto contour = buildPenContour(loops);
    if (!contour.valid()) {
        if (error)
            *error = contour.error;
        return {};
    }
    return loops;
}

namespace {
PenLoop rawMaskLoop(const QPolygonF &polygon, PenLoopKind kind) {
    PenLoop loop;
    loop.kind = kind;
    for (const QPointF point : polygon)
        loop.points.push_back({point, PenPointKind::Hard});

    return loop;
}

PenLoop fitMaskLoop(const QPolygonF &polygon, CubicFitOptions fit, double cornerScale,
                   int firstAttempt, PenLoopKind kind) {
    for (int attempt = firstAttempt; attempt < kMaximumMaskFitAttempts; ++attempt) {
        auto options = fit;
        options.tolerance *= std::pow(0.5, attempt);
        options.cornerScale = std::max(cornerScale, 4 * options.tolerance);
        if (options.tolerance < 2.0)
            options.outlierFraction = 0;
        PenLoop loop{fitCubicContour(polygon, options), kind};
        if (buildPenContour(loop.points).valid())
            return loop;
        for (double scale : kHandleRepairScales) {
            PenLoop contracted = loop;
            for (PenPoint &point : contracted.points) {
                point.incoming *= scale;
                point.outgoing *= scale;
            }
            if (buildPenContour(contracted.points).valid())
                return contracted;
        }
    }
    return rawMaskLoop(polygon, kind);
}
} // namespace

QVector<PenLoop> fitMaskContours(const std::vector<std::uint8_t> &mask, QSize size, QRect bounds,
                                 const CubicFitOptions &options, QString *error) {
    if (options.outwardFitPixels > 0 && options.adaptToRasterNoise) {
        CubicFitOptions baseOptions = options;
        baseOptions.outwardFitPixels = 0;
        auto best = fitMaskContours(mask, size, bounds, baseOptions, error);
        if (best.isEmpty())
            return best;
        const int selected = int(std::count_if(mask.begin(), mask.end(),
                                                [](std::uint8_t value) { return value != 0; }));
        const int allowedMisses = std::max(2, int(std::floor(selected * kOutwardMaskMissFraction)));
        if (rasterMaskDifference(buildPenContour(best), mask, size, bounds).missed <= allowedMisses)
            return best;

        const auto cleaned = removeRasterPinholes(mask, size, bounds);
        const auto originalPolygons = pixelBoundaryLoops(cleaned, size, bounds);
        const int originalArea = int(std::count_if(cleaned.begin(), cleaned.end(),
                                                    [](std::uint8_t value) { return value != 0; }));
        const int maximumNodes = std::max(contourNodeCount(best) * 2,
                                          contourNodeCount(best) + 12);
        const QRect imageBounds(QPoint(0, 0), size);
        for (int radius = 1; radius <= options.outwardFitPixels; ++radius) {
            const QRect expandedBounds = bounds.adjusted(-radius, -radius, radius, radius)
                                             .intersected(imageBounds);
            const auto expanded = removeRasterPinholes(
                expandMask(cleaned, size, bounds, radius), size, expandedBounds);
            const int expandedArea = int(std::count_if(expanded.begin(), expanded.end(),
                                                        [](std::uint8_t value) { return value != 0; }));
            if (expandedArea > originalArea * (1.0 + kOutwardMaskGrowthFraction))
                break;
            const auto polygons = pixelBoundaryLoops(expanded, size, expandedBounds);
            if (polygons.size() != originalPolygons.size())
                break;
            bool sameWinding = true;
            for (int i = 0; i < polygons.size(); ++i)
                sameWinding &= (area(polygons[i]) > 0) == (area(originalPolygons[i]) > 0);
            if (!sameWinding)
                break;
            auto candidate = fitMaskContours(expanded, size, expandedBounds, baseOptions);
            if (candidate.isEmpty() || contourNodeCount(candidate) > maximumNodes)
                continue;
            const auto contour = buildPenContour(candidate);
            if (!contour.valid())
                continue;
            if (rasterMaskDifference(contour, mask, size, bounds).missed <= allowedMisses) {
                if (error)
                    error->clear();
                return candidate;
            }
        }

        const QRect detailedBounds = bounds.adjusted(-1, -1, 1, 1).intersected(imageBounds);
        const auto detailedMask = removeRasterPinholes(
            expandMask(cleaned, size, bounds, 1), size, detailedBounds);
        const auto detailedPolygons = pixelBoundaryLoops(detailedMask, size, detailedBounds);
        bool sameWinding = detailedPolygons.size() == originalPolygons.size();
        for (int i = 0; i < detailedPolygons.size() && sameWinding; ++i)
            sameWinding = (area(detailedPolygons[i]) > 0)
                == (area(originalPolygons[i]) > 0);
        const int detailedArea = int(std::count_if(detailedMask.begin(), detailedMask.end(),
                                                   [](std::uint8_t value) { return value != 0; }));
        if (sameWinding && detailedArea <= originalArea * (1.0 + kOutwardMaskGrowthFraction)) {
            CubicFitOptions detailedOptions = baseOptions;
            detailedOptions.rasterToleranceCeiling = kDetailedRasterTolerance;
            auto candidate = fitMaskContours(detailedMask, size, detailedBounds, detailedOptions);
            if (!candidate.isEmpty() && contourNodeCount(candidate) <= maximumNodes) {
                const auto contour = buildPenContour(candidate);
                if (contour.valid()) {
                    const auto difference = rasterMaskDifference(contour, mask, size, bounds);
                    if (difference.missed <= selected * kDetailedMaskMissFraction
                        && difference.spill <= selected * kDetailedMaskSpillFraction) {
                        if (error)
                            error->clear();
                        return candidate;
                    }
                }
            }
        }

        return best;
    }
    const auto cleaned = options.adaptToRasterNoise ? removeRasterPinholes(mask, size, bounds) : mask;
    const auto polygons = pixelBoundaryLoops(cleaned, size, bounds);
    if (polygons.isEmpty()) {
        if (error)
            *error = QStringLiteral("The selected mask has no closed boundary");
        return {};
    }
    // Never silently reinterpret another component as a hole.
    for (int i = 1; i < polygons.size(); ++i)
        if (area(polygons[i]) > 0) {
            if (error)
                *error = QStringLiteral("The selected mask contains disconnected regions");
            return {};
        }
    QVector<CubicFitOptions> rasterOptions;
    rasterOptions.reserve(polygons.size());
    for (const auto &polygon : polygons)
        rasterOptions.push_back(rasterFitOptions(polygon, options));
    for (int retry = 0; retry < kMaximumMaskFitAttempts; ++retry) {
        QVector<PenLoop> loops;
        for (int i = 0; i < polygons.size(); ++i) {
            auto fit = rasterOptions[i];
            fit.preserveRasterNotches = i > 0 && fit.adaptToRasterNoise;
            loops.push_back(fitMaskLoop(polygons[i], fit, options.cornerScale, retry,
                                       i == 0 ? PenLoopKind::Outer : PenLoopKind::Cutout));
        }
        const auto contour = buildPenContour(loops);
        if (contour.valid() && matchesRasterMask(contour, cleaned, size, bounds)) {
            if (error)
                error->clear();
            return loops;
        }
        for (int i = 1; i < loops.size(); ++i)
            loops[i] = rawMaskLoop(polygons[i], PenLoopKind::Cutout);
        const auto simpleCutouts = buildPenContour(loops);
        if (simpleCutouts.valid() && matchesRasterMask(simpleCutouts, cleaned, size, bounds)) {
            if (error)
                error->clear();
            return loops;
        }
        if (error)
            *error = contour.error;
    }
    // Preserve topology exactly if a thin or touching feature cannot be safely smoothed.
    QVector<PenLoop> loops;
    for (int i = 0; i < polygons.size(); ++i) {
        loops.push_back(rawMaskLoop(polygons[i],
                                   i == 0 ? PenLoopKind::Outer : PenLoopKind::Cutout));
    }
    const auto contour = buildPenContour(loops);
    if (!contour.valid()) {
        if (error)
            *error = contour.error;
        return {};
    }
    if (error)
        error->clear();
    return loops;
}
} // namespace gui
