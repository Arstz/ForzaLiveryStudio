#include "cubic_contour.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace gui {
namespace {
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
quint64 key(QPoint p) {
    return (quint64(quint32(p.x())) << 32) | quint32(p.y());
}

struct Reference {
    QPolygonF p;
    QVector<double> arc;
    double perimeter = 0;
    explicit Reference(QPolygonF polygon) : p(std::move(polygon)) {
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
    for (int i = 1; i + 1 < data.size(); ++i) {
        const double error = length(curve.point(u[i]) - data[i]);
        if (error > maximum) {
            maximum = error;
            worst = i;
        }
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

CubicFitOptions rasterFitOptions(const QVector<QPolygonF> &polygons,
                                 const CubicFitOptions &options) {
    if (!options.adaptToRasterNoise)
        return options;
    QVector<double> deviations;
    double perimeter = 0, signedArea = 0;
    for (const auto &polygon : polygons) {
        const Reference ref(polygon);
        perimeter += ref.perimeter;
        signedArea += area(polygon);
        const double radius = std::min(8.0, ref.perimeter / 12);
        for (double s = 0; s < ref.perimeter; s += 1.0) {
            const QPointF before = ref.point(s - radius), after = ref.point(s + radius);
            const QPointF chord = after - before;
            const double n = length(chord);
            if (n > 1e-9)
                deviations.push_back(std::abs(cross(ref.point(s) - (before + after) * 0.5, chord)) /
                                     n);
        }
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
    // Detect persistent corners beyond the noise band, not the tips of raster teeth.
    result.cornerScale = std::max(options.cornerScale, 4 * result.tolerance);
    return result;
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
    const Reference ref(boundary);
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
    QVector<PenPoint> result;
    for (int i = 0; i < spans.size(); ++i) {
        const auto &curve = spans[i].curve,
                   &before = spans[(i + spans.size() - 1) % spans.size()].curve;
        result.push_back({curve.start, spans[i].a.corner ? PenPointKind::Hard : PenPointKind::Soft,
                          before.control2 - curve.start, curve.control - curve.start, true});
    }
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

QVector<PenLoop> fitMaskContours(const std::vector<std::uint8_t> &mask, QSize size, QRect bounds,
                                 const CubicFitOptions &options, QString *error) {
    const auto polygons = pixelBoundaryLoops(mask, size, bounds);
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
    const auto rasterOptions = rasterFitOptions(polygons, options);
    for (int retry = 0; retry < 4; ++retry) {
        auto fit = rasterOptions;
        fit.tolerance *= std::pow(0.5, retry);
        QVector<PenLoop> loops;
        for (int i = 0; i < polygons.size(); ++i)
            loops.push_back({fitCubicContour(polygons[i], fit),
                             i == 0 ? PenLoopKind::Outer : PenLoopKind::Cutout});
        const auto contour = buildPenContour(loops);
        if (contour.valid()) {
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
        PenLoop loop;
        loop.kind = i == 0 ? PenLoopKind::Outer : PenLoopKind::Cutout;
        for (auto p : polygons[i])
            loop.points.push_back({p, PenPointKind::Hard});
        loops.push_back(loop);
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
