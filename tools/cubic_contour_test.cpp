#include "bucket_fill.h"
#include "cubic_contour.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPainterPathStroker>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
double norm(QPointF p) {
    return std::hypot(p.x(), p.y());
}

void geometry() {
    gui::FillBoundarySegment s{{0, 0}, {0, 100}, {100, 0}, true, {100, 100}};
    require(std::abs(s.signedArea() + 6000) < 1e-8, "Cubic area must include both controls");
    auto [a, b] = s.split(0.37);
    for (int i = 0; i <= 100; ++i) {
        const double t = i / 100.0;
        require(norm(a.point(t) - s.point(t * 0.37)) < 1e-9,
                "Left de Casteljau split changed curve");
        require(norm(b.point(t) - s.point(0.37 + t * 0.63)) < 1e-9,
                "Right de Casteljau split changed curve");
    }
    QPainterPath path;
    path.moveTo(0, 0);
    path.cubicTo(0, 100, 100, 100, 100, 0);
    path.lineTo(70, -20);
    path.lineTo(0, 0);
    QString error;
    auto loops = gui::cubicPathLoops(path, &error);
    require(!loops.isEmpty(), qPrintable(error));
    const auto before = gui::penSegments(loops[0].points);
    require(before.size() >= 4 && before[0].curved && before[1].curved,
            "A curved hard-to-hard span needs a soft anchor");
    for (int i = 0; i <= 20; ++i) {
        const double t = i / 20.0;
        require(norm(before[0].point(t) - s.point(t * 0.5)) < 1e-9
            && norm(before[1].point(t) - s.point(0.5 + t * 0.5)) < 1e-9,
            "Authored cubic changed while splitting hard endpoints");
    }
    for (int i = 0; i < loops[0].points.size(); ++i)
        if (loops[0].points[i].kind == gui::PenPointKind::Hard
            && loops[0].points[(i + 1) % loops[0].points.size()].kind == gui::PenPointKind::Hard)
            require(!before[i].curved, "Hard-to-hard segment retained a curve");
    gui::insertPenAnchor(loops[0].points, 1, s.point(0.4));
    const auto after = gui::penSegments(loops[0].points);
    require(after.size() == before.size() + 1, "Inserting an anchor must split one span");
    require(norm(after[0].point(1) - s.point(0.4)) < 1e-6, "Inserted anchor misses curve");
    require(norm(after[1].control2 - s.control2) > 1, "Split must retain native cubic handles");
    require(std::abs(after[0].signedArea() + after[1].signedArea()
        + after[2].signedArea() - s.signedArea()) < 1e-8,
            "Split changed area");
    QTransform transform;
    transform.scale(2, 0.4);
    transform.shear(0.3, -0.2);
    transform.translate(7, 9);
    for (auto &p : loops[0].points)
        gui::transformPenPoint(p, transform);
    const auto transformed = gui::penSegments(loops[0].points);
    for (int i = 0; i < after.size(); ++i)
        for (int j = 0; j <= 20; ++j)
            require(norm(transformed[i].point(j / 20.0) - transform.map(after[i].point(j / 20.0))) <
                        1e-8,
                    "Transform lost cubic handles");
    const QVector<gui::PenPoint> hardCorners{
        {{0, 0}, gui::PenPointKind::Hard, {0, -40}, {0, 40}, true},
        {{100, 0}, gui::PenPointKind::Hard, {0, 40}, {0, -40}, true},
        {{50, 80}, gui::PenPointKind::Hard, {40, 0}, {-40, 0}, true}};
    for (const auto &segment : gui::penSegments(hardCorners))
        require(!segment.curved, "Hard-to-hard connection must be straight");
    QPainterPath ellipse;
    ellipse.addEllipse(QRectF(0, 0, 100, 70));
    const auto smooth = gui::cubicPathLoops(ellipse);
    require(smooth.size() == 1, "Smooth closed cubic loop must be valid without a hard seam");
    for (const auto &p : smooth[0].points)
        require(p.kind == gui::PenPointKind::Soft, "Ellipse should have no hard seam");
    QPainterPath leaf;
    leaf.moveTo(0, 0);
    leaf.cubicTo(80, -60, 80, 60, 0, 0);
    leaf.closeSubpath();
    const auto leafLoops = gui::cubicPathLoops(leaf);
    require(leafLoops.size() == 1 && leafLoops[0].points.size() == 3,
            "A single closed cubic must retain its filled area");
    double leafArea = 0;
    for (const auto &segment : gui::penSegments(leafLoops[0].points))
        leafArea += segment.signedArea();
    require(std::abs(leafArea - gui::FillBoundarySegment{{0, 0}, {80, -60}, {0, 0}, true, {80, 60}}
                                    .signedArea()) < 1e-8,
            "Splitting a closed cubic changed its area");
}

QImage raster(const QPainterPath &path, QSize size) {
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter p(&image);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    p.drawPath(path);
    return image;
}

void checkMask(const char *name, const QPainterPath &path, QSize size, QPoint seed, int maxNodes,
               int holes, const QString &output = {}) {
    const auto image = raster(path, size);
    const auto fill = gui::floodGuideRegion(image, seed, 0);
    require(fill.valid(), "Fixture seed is invalid");
    const auto boundary = gui::pixelBoundaryLoops(fill.mask, size, fill.bounds);
    require(boundary.size() == holes + 1, "Exact pixel boundaries lost topology");
    double area = 0;
    for (const auto &loop : boundary)
        for (int i = 0; i < loop.size(); ++i) {
            const auto a = loop[i], b = loop[(i + 1) % loop.size()];
            area += (a.x() * b.y() - a.y() * b.x()) * 0.5;
        }
    require(std::abs(area - fill.area) < 1e-9, "Pixel-edge loops must match selected area exactly");
    QElapsedTimer timer;
    timer.start();
    QString error;
    const auto fitted = gui::fitMaskContours(fill.mask, size, fill.bounds, {}, &error);
    const auto elapsed = timer.elapsed();
    require(!fitted.isEmpty(), qPrintable(error));
    const auto contour = gui::buildPenContour(fitted);
    require(contour.valid(), qPrintable(contour.error));
    require(fitted.size() == holes + 1, "Cubic fitting lost a hole");
    int nodes = 0, hard = 0;
    for (const auto &loop : fitted)
        for (const auto &point : loop.points) {
            ++nodes;
            hard += point.kind == gui::PenPointKind::Hard;
            require(point.explicitHandles, "Fitted contour must store native cubic handles");
            if (point.kind == gui::PenPointKind::Soft)
                require(std::abs(point.incoming.x() * point.outgoing.y() -
                                 point.incoming.y() * point.outgoing.x()) < 1e-6,
                        "Smooth join tangents are not aligned");
        }
    std::cout << name << ": nodes=" << nodes << " corners=" << hard << " time_ms=" << elapsed
              << '\n';
    require(nodes <= maxNodes, "Contour has too many anchors");
    QPainterPath reference;
    reference.setFillRule(Qt::OddEvenFill);
    for (const auto &loop : boundary) {
        reference.addPolygon(loop);
        reference.closeSubpath();
    }
    QPainterPathStroker stroker;
    stroker.setWidth(1.8);
    const auto corridor = stroker.createStroke(reference);
    // Every pixel center outside the boundary uncertainty band must keep its classification.
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x) {
            const QPointF p(x + 0.5, y + 0.5);
            if (!corridor.contains(p))
                require(contour.path.contains(p) == (fill.mask[size_t(y) * size.width() + x] != 0),
                        "Fitting changed coverage away from the boundary");
        }
    if (!output.isEmpty()) {
        QImage preview(size * 3, QImage::Format_ARGB32);
        preview.fill(QColor(140, 140, 140));
        QPainter painter(&preview);
        painter.scale(3, 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(130, 75, 105));
        painter.drawPath(reference);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(20, 150, 255), 0.65));
        painter.setBrush(QColor(100, 100, 180, 65));
        painter.drawPath(contour.path);
        for (const auto &loop : fitted)
            for (const auto &point : loop.points) {
                painter.setPen(QPen(Qt::black, 0.4));
                painter.setBrush(point.kind == gui::PenPointKind::Hard ? QColor(240, 60, 60)
                                                                       : QColor(Qt::white));
                painter.drawEllipse(point.position, 1.6, 1.6);
            }
        painter.end();
        require(preview.save(output), "Cannot save contour diagnostic");
    }
}

void masks(const QString &output) {
    QPainterPath rectangle;
    rectangle.addRect(4, 5, 80, 60);
    checkMask("rectangle", rectangle, {100, 80}, {10, 10}, 4, 0);
    const auto rectangleMask = gui::floodGuideRegion(raster(rectangle, {100, 80}), {10, 10}, 0);
    const auto rectangleLoops = gui::fitMaskContours(
        rectangleMask.mask, rectangleMask.imageSize, rectangleMask.bounds);
    require(rectangleLoops.size() == 1, "Rectangle contour was not fitted");
    for (const auto &segment : gui::penSegments(rectangleLoops.front().points))
        require(!segment.curved, "Straight hard-to-hard edge retained curved handles");
    QPainterPath ellipse;
    ellipse.addEllipse(10, 10, 140, 90);
    checkMask("ellipse", ellipse, {170, 120}, {80, 50}, 12, 0);
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addEllipse(10, 10, 140, 90);
    ring.addEllipse(30, 25, 100, 60);
    checkMask("ring", ring, {170, 120}, {80, 15}, 24, 1);
    QPainterPath steppedRing;
    steppedRing.setFillRule(Qt::OddEvenFill);
    steppedRing.addRect(10, 10, 110, 90);
    steppedRing.addPolygon(QPolygonF{{35, 35}, {55, 35}, {55, 47}, {65, 47},
                                    {65, 35}, {85, 35}, {85, 75}, {35, 75}});
    checkMask("stepped cutout", steppedRing, {130, 110}, {20, 20}, 30, 1);
    const auto steppedImage = raster(steppedRing, {130, 110});
    const auto steppedMask = gui::floodGuideRegion(steppedImage, {20, 20}, 0);
    const auto steppedLoops = gui::fitMaskContours(steppedMask.mask,
        steppedMask.imageSize, steppedMask.bounds);
    require(steppedLoops.size() == 2, "Stepped cutout was lost");
    for (const QPointF corner : {QPointF(55, 35), QPointF(55, 47),
                                 QPointF(65, 47), QPointF(65, 35)}) {
        bool preserved = false;
        for (const auto &point : steppedLoops[1].points)
            preserved |= point.kind == gui::PenPointKind::Hard
                && norm(point.position - corner) <= 2.5;
        require(preserved, "A stepped cutout corner was rounded away");
    }
    QPainterPath stroke;
    stroke.moveTo(20, 150);
    stroke.cubicTo(20, 100, 40, 15, 60, 15);
    stroke.cubicTo(75, 15, 90, 65, 105, 90);
    stroke.cubicTo(130, 45, 165, 20, 175, 30);
    stroke.cubicTo(180, 35, 180, 40, 180, 45);
    QPainterPathStroker thick;
    thick.setWidth(6);
    thick.setCapStyle(Qt::RoundCap);
    thick.setJoinStyle(Qt::MiterJoin);
    checkMask("cat ears", thick.createStroke(stroke), {200, 170}, {20, 140}, 48, 0, output);
    thick.setWidth(2);
    checkMask("thin stroke", thick.createStroke(stroke), {200, 170}, {20, 140}, 70, 0);
    thick.setWidth(6);
    const auto coarseImage = raster(thick.createStroke(stroke), {200, 170});
    const auto coarseMask = gui::floodGuideRegion(coarseImage, {20, 140}, 0);
    gui::CubicFitOptions coarseOptions;
    coarseOptions.tolerance = 16;
    const auto coarseLoops = gui::fitMaskContours(coarseMask.mask, coarseMask.imageSize,
        coarseMask.bounds, coarseOptions);
    const auto coarseContour = gui::buildPenContour(coarseLoops);
    require(coarseContour.valid() && coarseLoops.front().points.size() <= 70,
            "High curve tolerance lost a usable compact contour");
    for (int sample = 1; sample < 20; ++sample)
        require(coarseContour.path.contains(stroke.pointAtPercent(sample / 20.0)),
                "High curve tolerance lost the center of a thin selected stroke");
    QPainterPath tiny;
    tiny.addRect(4, 4, 1, 1);
    checkMask("single pixel", tiny, {10, 10}, {4, 4}, 4, 0);
    const std::vector<std::uint8_t> diagonal{1, 0, 0, 1};
    require(gui::pixelBoundaryLoops(diagonal, {2, 2}, {0, 0, 2, 2}).size() == 2,
            "Diagonal contact must not join components");
    require(gui::fitMaskContours(diagonal, {2, 2}, {0, 0, 2, 2}).isEmpty(),
            "Disconnected masks must not become cutouts");
    require(gui::pixelBoundaryLoops({1}, {2, 2}, {0, 0, 2, 2}).isEmpty(),
            "Invalid mask size must be rejected");
    std::vector<std::uint8_t> pinhole(8 * 8, 1);
    for (int y = 3; y < 5; ++y)
        for (int x = 3; x < 5; ++x)
            pinhole[y * 8 + x] = 0;
    require(gui::fitMaskContours(pinhole, {8, 8}, {0, 0, 8, 8}).size() == 1,
            "Raster pinholes should be removed before fitting");
    gui::CubicFitOptions exact;
    exact.adaptToRasterNoise = false;
    require(gui::fitMaskContours(pinhole, {8, 8}, {0, 0, 8, 8}, exact).size() == 2,
            "Explicit exact-mask fitting must retain small holes");
}

void realBucketMask() {
    QFile file(QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/bucket_neko_mask.json"));
    require(file.open(QIODevice::ReadOnly), "Cannot open the real bucket mask fixture");
    const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    const auto dimensions = fixture.value("size").toArray();
    const QSize size(dimensions[0].toInt(), dimensions[1].toInt());
    const auto origin = fixture.value("origin").toArray();
    const QPoint offset(origin[0].toInt(), origin[1].toInt());
    // Retain the source coordinates as well as the exact selected pixels.
    QImage source(size + QSize(offset.x(), offset.y()), QImage::Format_ARGB32);
    source.fill(Qt::white);
    for (const auto &entry : fixture.value("runs").toArray()) {
        const auto run = entry.toArray();
        auto *row = reinterpret_cast<QRgb *>(source.scanLine(run[0].toInt() + offset.y()));
        for (int x = run[1].toInt(); x < run[2].toInt(); ++x)
            row[x + offset.x()] = qRgb(243, 125, 168);
    }
    const auto seed = fixture.value("seed").toArray();
    const auto fill =
        gui::floodGuideRegion(source, offset + QPoint(seed[0].toInt(), seed[1].toInt()),
                              fixture.value("tolerance").toInt());
    require(fill.valid() && fill.area == 35552, "Real bucket selection changed");
    QString error;
    const auto loops = gui::fitMaskContours(fill.mask, source.size(), fill.bounds, {}, &error);
    const auto contour = gui::buildPenContour(loops);
    require(contour.valid(), qPrintable(error.isEmpty() ? contour.error : error));
    require(loops.size() == 1, "Real bucket stroke changed topology");
    int corners = 0;
    for (const auto &point : loops.front().points) {
        corners += point.kind == gui::PenPointKind::Hard;
        require(point.explicitHandles, "Real stroke fell back to pixel-edge anchors");
    }
    std::cout << "real Neko bucket: nodes=" << loops.front().points.size() << " corners=" << corners
              << '\n';
    require(loops.front().points.size() <= 40 && corners <= 8,
            "Raster roughness fragmented the real bucket stroke");

    QPainterPath boundary;
    for (const auto &polygon : gui::pixelBoundaryLoops(fill.mask, source.size(), fill.bounds)) {
        boundary.addPolygon(polygon);
        boundary.closeSubpath();
    }
    QPainterPathStroker stroker;
    stroker.setWidth(9.0); // 4.5 source pixels on either side of the raw edge.
    const auto band = raster(stroker.createStroke(boundary), source.size());
    const auto fitted = raster(contour.path, source.size());
    int differing = 0, deepErrors = 0;
    const QRect checkBounds = fill.bounds.adjusted(-5, -5, 5, 5).intersected(source.rect());
    for (int y = checkBounds.top(); y <= checkBounds.bottom(); ++y) {
        const auto *fitRow = reinterpret_cast<const QRgb *>(fitted.constScanLine(y));
        const auto *bandRow = reinterpret_cast<const QRgb *>(band.constScanLine(y));
        for (int x = checkBounds.left(); x <= checkBounds.right(); ++x) {
            const bool mismatch =
                (qRed(fitRow[x]) == 0) != (fill.mask[size_t(y) * source.width() + x] != 0);
            differing += mismatch;
            deepErrors += mismatch && qRed(bandRow[x]) != 0;
        }
    }
    std::cout << "real Neko bucket: boundary-only difference=" << differing
              << " deep errors=" << deepErrors << '\n';
    require(deepErrors == 0, "Real stroke changed outside the raster uncertainty band");
    require(differing < fill.area * 0.12, "Real stroke lost too much of its silhouette");

    QImage translucent(source.size(), QImage::Format_ARGB32);
    translucent.fill(qRgba(255, 255, 255, 0));
    for (int y = fill.bounds.top(); y <= fill.bounds.bottom(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(translucent.scanLine(y));
        for (int x = fill.bounds.left(); x <= fill.bounds.right(); ++x) {
            if (!fill.mask[size_t(y) * source.width() + x])
                continue;
            const int hash = (x * 73 + y * 151 + x * y * 13) % 19;
            row[x] = qRgba(243, 125, 168, hash < 2 ? 114 : hash > 16 ? 142 : 128);
        }
    }
    const QPoint translucentSeed = offset + QPoint(seed[0].toInt(), seed[1].toInt());
    translucent.setPixel(translucentSeed, qRgba(243, 125, 168, 128));
    int previousNodes = -1;
    for (int tolerance : {8, 16}) {
        const auto selected = gui::floodGuideRegion(translucent, translucentSeed, tolerance);
        require(selected.valid(), "Half-opacity selection failed");
        const auto fittedLoops =
            gui::fitMaskContours(selected.mask, selected.imageSize, selected.bounds, {}, &error);
        require(error.isEmpty() && fittedLoops.size() == 1,
                "Half-opacity noise broke the stroke topology");
        const int nodes = fittedLoops.front().points.size();
        std::cout << "half-opacity tolerance " << tolerance << ": nodes=" << nodes << '\n';
        require(nodes <= 36, "Half-opacity noise created too many anchors");
        if (previousNodes >= 0)
            require(std::abs(nodes - previousNodes) <= 16,
                    "Nearby bucket tolerances produced inconsistent contours");
        previousNodes = nodes;
    }
}

void realLeftHandMasks() {
    QFile file(QStringLiteral(FLS_SOURCE_DIR "/tools/fixtures/bucket_tests_left_hand_mask.json"));
    require(file.open(QIODevice::ReadOnly), "Cannot open the left-hand mask fixture");
    const auto fixture = QJsonDocument::fromJson(file.readAll()).object();
    const auto dimensions = fixture.value("size").toArray();
    const QSize size(dimensions[0].toInt(), dimensions[1].toInt());
    for (const auto &entry : fixture.value("selections").toArray()) {
        const auto selection = entry.toObject();
        std::vector<std::uint8_t> mask(size_t(size.width()) * size.height(), 0);
        QRect bounds;
        int area = 0;
        for (const auto &value : selection.value("runs").toArray()) {
            const auto run = value.toArray();
            const int y = run[0].toInt(), first = run[1].toInt(), end = run[2].toInt();
            require(y >= 0 && y < size.height() && first >= 0 && end <= size.width()
                        && first < end, "Invalid left-hand mask run");
            for (int x = first; x < end; ++x)
                mask[size_t(y) * size.width() + x] = 1;
            bounds = bounds.united(QRect(QPoint(first, y), QPoint(end - 1, y)));
            area += end - first;
        }
        require(area == selection.value("area").toInt(), "Left-hand mask area changed");
        QString error;
        const auto loops = gui::fitMaskContours(mask, size, bounds, {}, &error);
        const auto contour = gui::buildPenContour(loops);
        require(contour.valid() && loops.size() == selection.value("expected_loops").toInt(),
                qPrintable(error.isEmpty() ? contour.error : error));
        int nodes = 0, soft = 0;
        for (const auto &loop : loops)
            for (const auto &point : loop.points) {
                ++nodes;
                soft += point.kind == gui::PenPointKind::Soft;
            }
        require(nodes <= selection.value("max_nodes").toInt()
                    && soft >= selection.value("min_soft").toInt(),
                "A left-hand mask fell back to a pixel staircase");
    }
}
void replayImage(int argc, char **argv) {
    const QImage image(QString::fromLocal8Bit(argv[2]));
    const auto fill = gui::floodGuideRegion(image, {atoi(argv[3]), atoi(argv[4])}, atoi(argv[5]));
    require(fill.valid(), qPrintable(fill.error));
    gui::CubicFitOptions options;
    options.tolerance = atof(argv[6]);
    if (argc > 8)
        options.cornerScale = atof(argv[8]);
    QElapsedTimer timer;
    timer.start();
    QString error;
    const auto loops =
        gui::fitMaskContours(fill.mask, fill.imageSize, fill.bounds, options, &error);
    require(!loops.isEmpty(), qPrintable(error));
    if (argc > 9) {
        QJsonArray outputLoops;
        for (const auto &loop : loops) {
            QJsonArray points;
            for (const auto &point : loop.points) {
                const QPointF world(point.position.x() - image.width() * 0.5,
                                    image.height() * 0.5 - point.position.y());
                QJsonObject entry;
                entry.insert("position", QJsonArray{world.x(), world.y()});
                entry.insert("kind", point.kind == gui::PenPointKind::Hard ? "hard" : "soft");
                entry.insert("incoming", QJsonArray{point.incoming.x(), -point.incoming.y()});
                entry.insert("outgoing", QJsonArray{point.outgoing.x(), -point.outgoing.y()});
                entry.insert("explicitHandles", point.explicitHandles);
                points.push_back(entry);
            }
            QJsonObject entry;
            entry.insert("kind", loop.kind == gui::PenLoopKind::Outer ? "outer" : "cutout");
            entry.insert("points", points);
            outputLoops.push_back(entry);
        }
        QJsonObject request;
        request.insert("curveModel", "cubic-anchors-v1");
        request.insert("boundaryTolerance", 0.1);
        request.insert("loops", outputLoops);
        QFile output(QString::fromLocal8Bit(argv[9]));
        require(output.open(QIODevice::WriteOnly), "Cannot write contour replay request");
        output.write(QJsonDocument(QJsonObject{{"request", request}}).toJson());
    }
    int nodes = 0, corners = 0;
    for (const auto &loop : loops)
        for (const auto &p : loop.points) {
            ++nodes;
            corners += p.kind == gui::PenPointKind::Hard;
        }
    std::cout << "pixels=" << fill.area << " loops=" << loops.size() << " nodes=" << nodes
              << " corners=" << corners << " time_ms=" << timer.elapsed() << std::endl;
    const QRect crop = fill.bounds.adjusted(-12, -12, 12, 12).intersected(image.rect());
    QImage preview(crop.size(), QImage::Format_ARGB32);
    preview.fill(QColor(140, 140, 140));
    QPainter painter(&preview);
    painter.translate(-crop.topLeft());
    for (int y = fill.bounds.top(); y <= fill.bounds.bottom(); ++y)
        for (int x = fill.bounds.left(); x <= fill.bounds.right(); ++x)
            if (fill.mask[size_t(y) * image.width() + x])
                painter.fillRect(x, y, 1, 1, QColor(130, 75, 105));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(20, 150, 255), 0.8));
    painter.setBrush(QColor(100, 100, 180, 65));
    painter.drawPath(gui::buildPenContour(loops).path);
    for (const auto &loop : loops)
        for (const auto &p : loop.points) {
            painter.setPen(QPen(Qt::black, 0.7));
            painter.setBrush(p.kind == gui::PenPointKind::Hard ? QColor(240, 60, 60)
                                                               : QColor(Qt::white));
            painter.drawEllipse(p.position, 2.6, 2.6);
        }
    painter.end();
    require(preview.save(QString::fromLocal8Bit(argv[7])), "Cannot save real-image replay");
}
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        if (argc > 1 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--image")) {
            require(argc >= 8,
                    "Usage: --image file x y colorTolerance fitTolerance output.bmp [cornerScale]");
            replayImage(argc, argv);
            return 0;
        }
        geometry();
        masks(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString{});
        realBucketMask();
        realLeftHandMasks();
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    std::cout << "Native cubic contour tests passed\n";
    return 0;
}
