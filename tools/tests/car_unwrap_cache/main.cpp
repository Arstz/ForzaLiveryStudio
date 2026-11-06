#include "car_model_renderer.h"
#include "car_scene.h"
#include "car_unwrap_overlay_cache.h"
#include "zip_extract.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>

#include <cstdio>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void checkInvalidation() {
    gui::CarUnwrapOverlay overlay;
    overlay.sides[0].path.addRect(10, 10, 60, 40);
    overlay.sides[0].wireframe.moveTo(0, 30);
    overlay.sides[0].wireframe.lineTo(90, 30);
    gui::CarUnwrapOverlayCache cache;
    cache.setOverlay(overlay);
    const QImage first = cache.image({}, {100, 80}, 1, true, false);
    require(!first.isNull() && first.pixelColor(20, 20).alpha() > 0, "missing unwrap fill");
    require(first.pixelColor(0, 0).alpha() == 0, "unwrap bounds");
    require(cache.image({}, {100, 80}, 1, true, false).cacheKey() == first.cacheKey(),
            "unchanged overlay was rasterized again");
    const QImage moved = cache.image(QTransform::fromTranslate(10, 0), {100, 80}, 1, true, false);
    require(moved.cacheKey() != first.cacheKey() && moved.pixelColor(15, 20).alpha() == 0
            && moved.pixelColor(30, 20).alpha() > 0, "camera invalidation");
    const QImage resized = cache.image({}, {120, 80}, 1, true, false);
    require(resized.size() == QSize(120, 80), "resize invalidation");
    const QImage scaled = cache.image({}, {120, 80}, 2, true, false);
    require(scaled.size() == QSize(240, 160) && scaled.devicePixelRatio() == 2
            && scaled.pixelColor(40, 40).alpha() > 0, "pixel density invalidation");
    const QImage wireframe = cache.image({}, {100, 80}, 1, false, true);
    require(wireframe.pixelColor(20, 20).alpha() == 0
            && wireframe.pixelColor(20, 29).alpha() > 0
            && wireframe.pixelColor(0, 29).alpha() == 0, "wireframe mode and clipping");
    overlay.sides[0].path = {};
    overlay.sides[0].path.addRect(80, 50, 10, 10);
    cache.setOverlay(overlay);
    require(cache.image({}, {100, 80}, 1, true, false).pixelColor(20, 20).alpha() == 0,
            "overlay replacement invalidation");
    require(cache.image({}, {100, 80}, 1, false, false).isNull(), "disabled overlay retention");
    require(!cache.image({}, {100, 80}, 1, true, false).isNull(), "overlay enable invalidation");
    cache.setOverlay({});
    require(cache.empty() && cache.image({}, {100, 80}, 1, true, true).isNull(), "empty overlay retention");
}

void benchmarkModel(const QString &path) {
    QString error;
    const auto extraction = fls::sharedZipExtraction(path, &error);
    require(bool(extraction), qPrintable(error));
    QDirIterator files(extraction->path(), {QStringLiteral("*.carbin")}, QDir::Files, QDirIterator::Subdirectories);
    require(files.hasNext(), "missing carbin");
    const QString carPath = files.next();
    const fls::CarModel model = fls::loadCarBin(carPath, &error);
    require(!model.meshes.empty(), qPrintable(error));
    const fls::LiveryMaskSet masks = fls::loadLiveryMasks(
        QFileInfo(carPath).absoluteDir().filePath(QStringLiteral("LiveryMasks")), &error);
    const gui::CarUnwrapOverlay overlay = gui::buildCarUnwrapOverlaySet(model, masks, {}).base;
    require(!overlay.empty(), "missing model unwrap");
    QRectF bounds;
    for (const auto &side : overlay.sides) {
        bounds |= side.path.boundingRect();
    }
    const QSize size(1200, 800);
    const double scale = std::min(size.width() / bounds.width(), size.height() / bounds.height()) * 0.9;
    QTransform camera;
    camera.translate(size.width() * 0.5, size.height() * 0.5);
    camera.scale(scale, -scale);
    camera.translate(-bounds.center().x(), -bounds.center().y());
    QJsonObject report;
    report.insert(QStringLiteral("model"), path);
    for (bool wireframe : {false, true}) {
        gui::CarUnwrapOverlayCache cache;
        cache.setOverlay(overlay);
        QElapsedTimer timer;
        timer.start();
        cache.image(camera, size, 1, true, wireframe);
        const double first = timer.nsecsElapsed() / 1000000.0;
        QImage target(size, QImage::Format_ARGB32_Premultiplied);
        target.fill(Qt::transparent);
        QPainter painter(&target);
        constexpr int frames = 100;
        timer.restart();
        for (int frame = 0; frame < frames; ++frame) {
            painter.drawImage(QPointF(), cache.image(camera, size, 1, true, wireframe));
        }
        const double cached = timer.nsecsElapsed() / (1000000.0 * frames);
        report.insert(wireframe ? QStringLiteral("unwrap_wireframe") : QStringLiteral("unwrap"),
                      QJsonObject{{QStringLiteral("rasterize_ms"), first},
                                  {QStringLiteral("cached_frame_ms"), cached}});
    }
    const QByteArray json = QJsonDocument(report).toJson();
    std::fwrite(json.constData(), 1, json.size(), stdout);
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        checkInvalidation();
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--benchmark-model")) {
            benchmarkModel(QString::fromLocal8Bit(argv[2]));
        }
        std::puts("Car unwrap cache checks passed");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
