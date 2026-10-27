#include "optimizer.h"

#include "shape_geometry_store.h"
#include "image_io.h"
#include "compact_fit.h"
#include "profile_fit.h"
#include "cubic_contour.h"
#include "compact_fit_catalog_internal.h"
#include "project_codec.h"
#include "matrix_math.h"
#include "layer.h"
#include "native_shape_renderer.h"
#include "region_fill.h"
#include "region_extract.h"
#include <QOffscreenSurface>
#include <QOpenGLExtraFunctions>
#include <QSvgRenderer>
#include <QtEndian>
#include <zlib.h>

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <atomic>
#include <future>
#include <queue>

namespace {

QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
    return file.readAll();
}

void write(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        throw std::runtime_error(("Cannot write " + path).toStdString());
}

void save(const QString &path, const QImage &image) {
    const auto rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray raw;
    raw.reserve((rgba.width() * 4 + 1) * rgba.height());
    for (int y = 0; y < rgba.height(); ++y) {
        raw.append('\0');
        raw.append(reinterpret_cast<const char *>(rgba.constScanLine(y)), rgba.width() * 4);
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    QByteArray compressed(static_cast<qsizetype>(compressedSize), '\0');
    if (compress2(reinterpret_cast<Bytef *>(compressed.data()), &compressedSize,
                  reinterpret_cast<const Bytef *>(raw.constData()), static_cast<uLong>(raw.size()), 6) != Z_OK)
        throw std::runtime_error("PNG compression failed");
    compressed.resize(static_cast<qsizetype>(compressedSize));
    QByteArray png("\x89PNG\r\n\x1a\n", 8);
    const auto integer = [](quint32 value) {
        QByteArray bytes(4, '\0');
        qToBigEndian(value, bytes.data());
        return bytes;
    };
    const auto chunk = [&](const QByteArray &type, const QByteArray &bytes) {
        const QByteArray payload = type + bytes;
        png += integer(static_cast<quint32>(bytes.size())) + payload
            + integer(static_cast<quint32>(crc32(0, reinterpret_cast<const Bytef *>(payload.constData()),
                                               static_cast<uInt>(payload.size()))));
    };
    chunk("IHDR", integer(rgba.width()) + integer(rgba.height()) + QByteArray("\x08\x06\0\0\0", 5));
    chunk("IDAT", compressed);
    chunk("IEND", {});
    write(path, png);
}

QImage load(const QString &path) {
    QString error;
    const auto image = gui::readGuideImage(path, nullptr, &error);
    if (image.isNull()) throw std::runtime_error(error.toStdString());
    return image;
}

#include "raster_manifest.inc"

QImage svgRender(const QByteArray &data, QSize size) {
    QSvgRenderer renderer(data);
    if (!renderer.isValid()) throw std::runtime_error("Invalid SVG");
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    renderer.render(&painter, QRectF(QPointF(), QSizeF(size)));
    return image;
}

double number(const QCommandLineParser &parser, const QString &name) {
    bool ok = false;
    const double value = parser.value(name).toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < 0)
        throw std::runtime_error(("Invalid --" + name).toStdString());
    return value;
}

int integer(const QCommandLineParser &parser, const QString &name) {
    const double value = number(parser, name);
    if (value != std::floor(value) || value > std::numeric_limits<int>::max())
        throw std::runtime_error(("Invalid integer --" + name).toStdString());
    return static_cast<int>(value);
}

QJsonObject opacityAudit(const QVector<gui::SvgVectorObject> &objects, const QImage &image) {
    int translucent = 0;
    qint64 partial = 0;
    for (const auto &object : objects) translucent += object.color.alpha() != 255;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = image.pixelColor(x, y).alpha();
            partial += alpha > 0 && alpha < 255;
        }
    return {{"nonopaque_fills", translucent}, {"partial_coverage_pixels", double(partial)},
            {"fractional_pixel_alpha_includes_fill_opacity", translucent > 0},
            {"boundary_antialiasing_enabled", true}};
}

QJsonArray appearanceGroups(const QVector<opaque::Item> &items) {
    std::map<QRgb, QJsonArray> groups;
    for (int index = 0; index < items.size(); ++index) groups[items[index].color.rgb()].append(index);
    QJsonArray result;
    for (const auto &[color, indices] : groups)
        result.append(QJsonObject{{"fill", QColor(color).name()}, {"draw_item_positions", indices}});
    return result;
}

#include "compact_output.inc"

QJsonObject benchmarkCompactFit(const QVector<gui::SvgVectorObject> &before,
                               const QVector<gui::SvgVectorObject> &after,
                               int limit, int evaluationBudget, QSize size, const QDir &output) {
    if (before.size() > limit || after.size() > limit)
        return {{"measured", false}, {"reason", "Complete scene exceeds compact-fit-limit"}, {"path_limit", limit}};
    CompactOutputOptions settings;
    settings.evaluations = evaluationBudget;
    settings.cacheDirectory = output.filePath("compact_cache");
    return {{"measured", true}, {"leeway_used", false}, {"evaluation_budget_per_path", evaluationBudget},
        {"before", fitCompactScene(before, size, settings, output, "before")},
        {"after", fitCompactScene(after, size, settings, output, "after")}};
}

int runFillOnly(const QCommandLineParser &parser) {
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || !parser.isSet("reference") || !parser.isSet("output"))
        throw std::runtime_error("Provide an internal contour document, --reference and --output");
    const auto input = read(arguments.front());
    QSvgRenderer source(input);
    const auto size = source.defaultSize();
    const auto reference = load(parser.value("reference"));
    if (!source.isValid() || size.isEmpty() || reference.size() != size)
        throw std::runtime_error("Contours and reference must have identical native dimensions");
    const auto document = gui::extractSvgVectorObjects(input, size);
    if (!document.supportsObjectSelection() || document.objects.isEmpty())
        throw std::runtime_error("Use closed solid-color contours supported by Bucket");
    const auto target = svgRender(input, size);
    const auto opacity = opacityAudit(document.objects, target);
    opaque::Options auditOptions;
    auditOptions.flattenTolerance = number(parser, "flatten-tolerance");
    const auto bucket = opaque::auditPenTargets(document.objects, auditOptions);
    const auto conversion = opaque::imageError(reference, target);
    if (opacity.value("nonopaque_fills").toInt() || !bucket.value("passed").toBool()
        || conversion.value("rmse").toDouble() > number(parser, "conversion-rmse")
        || conversion.value("max_channel_error").toDouble() > number(parser, "conversion-max-error"))
        throw std::runtime_error("The opaque baseline must pass conversion, opacity and Bucket validation");
    QDir output(parser.value("output"));
    if (!QDir().mkpath(output.absolutePath())) throw std::runtime_error("Cannot create output directory");
    const QStringList artifacts{"generated_compact.3so", "generated_compact.png", "generated_compact_difference.png",
        "generated_compact_progress.json", "generated_unculled.png", "generated_culling_difference.png",
        "generated_rejected.png", "source_difference.png", "report.json"};
    for (const auto &artifact : artifacts) {
        const auto destination = QFileInfo(output.filePath(artifact)).absoluteFilePath();
        for (const auto &inputPath : QStringList{arguments.front(), parser.value("reference"),
                                                parser.value("original"), parser.value("compact-fit-seeds")})
            if (!inputPath.isEmpty() && destination.compare(QFileInfo(inputPath).absoluteFilePath(), Qt::CaseInsensitive) == 0)
                throw std::runtime_error("Output artifacts would overwrite an input");
    }
    for (const auto &artifact : artifacts) QFile::remove(output.filePath(artifact));
    CompactOutputOptions settings;
    settings.nativeRender = true;
    settings.polygonSeed = parser.isSet("compact-fit-polygon-seed");
    settings.useGpu = !parser.isSet("compact-fit-cpu");
    settings.allowSceneWarnings = parser.isSet("scene-validated-fills");
    settings.cacheDirectory = parser.isSet("compact-fit-cache") ? parser.value("compact-fit-cache") : output.filePath("compact_cache");
    settings.evaluations = integer(parser, "compact-fit-evaluations");
    settings.shapeBudget = integer(parser, "compact-fit-shape-budget");
    settings.totalShapeBudget = integer(parser, "compact-fit-total-shape-budget");
    settings.workers = integer(parser, "compact-fit-workers");
    settings.renderSamples = integer(parser, "compact-fit-render-samples");
    settings.boundary = number(parser, "compact-fit-boundary");
    settings.inward = number(parser, "compact-fit-inward");
    settings.areaRatio = number(parser, "compact-fit-area-ratio");
    settings.flattenTolerance = number(parser, "flatten-tolerance");
    if (!settings.evaluations || !settings.shapeBudget || !settings.totalShapeBudget || !settings.workers || !settings.renderSamples
        || settings.renderSamples > 4 || settings.boundary <= 0 || settings.inward <= 0
        || settings.areaRatio <= 0 || settings.areaRatio >= 1)
        throw std::runtime_error("Compact Fit requires positive work and quality tolerances");
    if (parser.isSet("compact-fit-seeds")) {
        const auto manifest = QJsonDocument::fromJson(read(parser.value("compact-fit-seeds"))).object();
        const auto regions = manifest.value("regions").toArray();
        if (manifest.value("version").toInt() != 1
            || manifest.value("input_sha256").toString().toLatin1() != hash(input)
            || regions.size() != document.objects.size())
            throw std::runtime_error("Polygon seeds do not match the input contour document");
        for (const auto &region : regions) {
            QVector<QPolygonF> triangles;
            for (const auto &entry : region.toArray()) {
                const auto values = entry.toArray();
                if (values.size() != 6) throw std::runtime_error("Invalid polygon seed triangle");
                for (const auto &value : values)
                    if (!value.isDouble() || !std::isfinite(value.toDouble()))
                        throw std::runtime_error("Non-finite polygon seed coordinate");
                triangles.push_back({QPointF(values[0].toDouble(), values[1].toDouble()),
                    QPointF(values[2].toDouble(), values[3].toDouble()), QPointF(values[4].toDouble(), values[5].toDouble())});
            }
            settings.seedTriangles.push_back(triangles);
        }
    }
    auto report = fitCompactScene(document.objects, size, settings, output, "generated");
    report.insert("native_dimensions", QJsonArray{size.width(), size.height()});
    report.insert("downsampled", false);
    report.insert("source_to_opaque", conversion);
    report.insert("bucket_input", bucket);
    report.insert("opacity", opacity);
    bool accepted = report.value("verified").toBool()
        || (settings.allowSceneWarnings && report.value("scene_candidate").toBool());
    QJsonArray failures;
    if (accepted) {
        const auto image = load(output.filePath("generated_compact.png"));
        const auto error = opaque::imageError(target, image);
        const auto local = opaque::appearanceError(target, image, integer(parser, "abstraction-local-window"));
        if (error.value("rmse").toDouble() > number(parser, "render-rmse")
            || error.value("max_channel_error").toDouble() > number(parser, "render-max-error")
            || local.value("max_local_rmse").toDouble() > number(parser, "abstraction-local-rmse")) {
            accepted = false;
            failures.append("Filled output exceeds the rendering-error budget");
        }
        if (report.value("silhouette_difference_area").toDouble() > number(parser, "silhouette-tolerance")) {
            accepted = false;
            failures.append("Filled output exceeds the external-silhouette error budget");
        }
        report.insert("opaque_to_filled", error);
        report.insert("source_to_filled", opaque::imageError(reference, image));
        if (parser.isSet("original")) {
            const auto original = load(parser.value("original"));
            if (original.size() != size) throw std::runtime_error("Original raster dimensions must match contours");
            report.insert("original_to_opaque", opaque::imageError(original, target));
            report.insert("original_to_filled", opaque::imageError(original, image));
            save(output.filePath("source_difference.png"), opaque::difference(original, image));
        }
    } else if (!report.value("shape_budget_passed").toBool())
        failures.append("Complete image exceeds the total Compact Fit shape budget");
    else failures.append("One or more regions failed Compact Fit verification");
    report.insert("accepted", accepted);
    report.insert("scene_verified", accepted);
    report.insert("verification_scope", report.value("verified").toBool() ? "per-region and scene" : "scene");
    report.insert("status", accepted ? "filled" : "fill_rejected");
    report.insert("rejections", failures);
    if (!accepted) {
        if (QFile::exists(output.filePath("generated_compact.png"))) {
            if (!QFile::copy(output.filePath("generated_compact.png"), output.filePath("generated_rejected.png")))
                throw std::runtime_error("Cannot retain the rejected native render");
            report.insert("rejected_render", "generated_rejected.png");
        }
        QFile::remove(output.filePath("generated_compact.3so"));
        QFile::remove(output.filePath("generated_compact.png"));
    }
    write(output.filePath("report.json"), QJsonDocument(report).toJson());
    std::printf("%s\n", QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    return accepted ? 0 : 3;
}

int prepareFillTargets(const QCommandLineParser &parser) {
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || !parser.isSet("output"))
        throw std::runtime_error("Provide internal contours and an output directory");
    const auto input = read(arguments.front());
    QSvgRenderer source(input);
    const auto size = source.defaultSize();
    const auto document = gui::extractSvgVectorObjects(input, size);
    if (!source.isValid() || size.isEmpty() || !document.supportsObjectSelection() || document.objects.isEmpty())
        throw std::runtime_error("Use valid closed solid-color contours");
    QDir output(parser.value("output"));
    if (!QDir().mkpath(output.absolutePath())) throw std::runtime_error("Cannot create target directory");
    for (const auto &name : {"fill_targets.json", "fill_targets.png"})
        if (QFileInfo(output.filePath(name)).absoluteFilePath().compare(QFileInfo(arguments.front()).absoluteFilePath(), Qt::CaseInsensitive) == 0)
            throw std::runtime_error("Target artifacts would overwrite the contour input");
    QJsonArray targets;
    QVector<gui::SvgVectorObject> flattened;
    for (const auto &object : document.objects) {
        if (object.color.alpha() != 255) throw std::runtime_error("Compact Fit targets must be opaque");
        gui::RegionPenLoopConversionOptions options;
        options.preserveInputCurves = true;
        const auto conversion = gui::regionOutlineToPenLoops(object.path, options);
        if (!conversion.valid()) throw std::runtime_error(conversion.error.toStdString());
        gui::PenFillRequest request;
        request.loops = conversion.loops;
        const auto region = gui::catalog::buildRegion(request, {});
        QJsonArray rings;
        for (const auto &polygon : region.required) {
            QJsonArray points;
            for (const auto &point : polygon) points.append(QJsonArray{point.x(), point.y()});
            rings.append(points);
        }
        targets.append(QJsonObject{{"fill", object.color.name()}, {"rings", rings}});
        flattened.push_back({gui::catalog::painterPath(region.required), object.color});
    }
    const auto rendered = svgRender(opaque::svg(flattened, size), size);
    QJsonObject manifest{{"version", 1}, {"input_sha256", QString::fromLatin1(hash(input))},
        {"regions", targets}, {"native_dimensions", QJsonArray{size.width(), size.height()}},
        {"opaque_to_compact_target", opaque::imageError(svgRender(input, size), rendered)}};
    write(output.filePath("fill_targets.json"), QJsonDocument(manifest).toJson());
    save(output.filePath("fill_targets.png"), rendered);
    return 0;
}

int recoverFillCurves(const QCommandLineParser &parser) {
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || !parser.isSet("output"))
        throw std::runtime_error("Provide contours and an output directory");
    const auto input = read(arguments.front());
    QSvgRenderer source(input);
    const auto size = source.defaultSize();
    const auto document = gui::extractSvgVectorObjects(input, size);
    if (!source.isValid() || size.isEmpty() || !document.supportsObjectSelection())
        throw std::runtime_error("Use closed solid-color contours");
    gui::CubicFitOptions options;
    options.tolerance = number(parser, "curve-fit-tolerance");
    options.adaptToRasterNoise = false;
    if (options.tolerance <= 0) throw std::runtime_error("Curve tolerance must be positive");
    QVector<gui::SvgVectorObject> recovered;
    QJsonArray paths;
    for (const auto &object : document.objects) {
        QString error;
        const auto original = gui::cubicPathLoops(object.path, &error);
        if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
        QPainterPath candidate;
        candidate.setFillRule(Qt::OddEvenFill);
        int before = 0, after = 0, curves = 0;
        for (const auto &loop : original) {
            before += loop.points.size();
            auto points = loop.points;
            const auto segments = gui::penSegments(points);
            const bool curved = std::any_of(segments.begin(), segments.end(), [](const auto &span) { return span.curved; });
            if (!curved && points.size() > 4) {
                QPolygonF boundary;
                for (const auto &point : points) boundary.push_back(point.position);
                const auto fitted = gui::fitCubicContour(boundary, options);
                if (fitted.size() >= 3 && fitted.size() < points.size()) {
                    points = fitted;
                    const auto clamp = [&](QPointF point) {
                        return QPointF(std::clamp(point.x(), 0.0, double(size.width())),
                            std::clamp(point.y(), 0.0, double(size.height())));
                    };
                    for (auto &point : points) {
                        const auto incoming = clamp(point.position + point.incoming);
                        const auto outgoing = clamp(point.position + point.outgoing);
                        point.position = clamp(point.position);
                        point.incoming = incoming - point.position;
                        point.outgoing = outgoing - point.position;
                    }
                }
            }
            after += points.size();
            for (const auto &segment : gui::penSegments(points)) curves += segment.curved;
            candidate.addPath(gui::penPath(points));
        }
        const auto checked = gui::cubicPathLoops(candidate, &error);
        const auto valid = error.isEmpty() ? gui::buildPenContour(checked) : gui::PenContour{};
        const bool accepted = error.isEmpty() && valid.valid();
        recovered.push_back({accepted ? candidate : object.path, object.color});
        paths.append(QJsonObject{{"before_anchors", before}, {"after_anchors", accepted ? after : before},
            {"curved_spans", accepted ? curves : 0}, {"accepted", accepted}, {"error", error}});
    }
    QDir output(parser.value("output"));
    if (!QDir().mkpath(output.absolutePath())) throw std::runtime_error("Cannot create curve output directory");
    for (const auto &name : {"recovered.svg", "recovery.json"})
        if (QFileInfo(output.filePath(name)).absoluteFilePath().compare(
                QFileInfo(arguments.front()).absoluteFilePath(), Qt::CaseInsensitive) == 0)
            throw std::runtime_error("Curve output would overwrite its input");
    write(output.filePath("recovered.svg"), opaque::svg(recovered, size));
    write(output.filePath("recovery.json"), QJsonDocument(QJsonObject{
        {"method", "Bucket cubic contour fitter"}, {"tolerance", options.tolerance}, {"paths", paths},
        {"native_dimensions", QJsonArray{size.width(), size.height()}},
        {"image_error", opaque::imageError(svgRender(input, size), svgRender(opaque::svg(recovered, size), size))}}).toJson());
    return 0;
}

int run(const QCommandLineParser &parser) {
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || !parser.isSet("reference") || !parser.isSet("output"))
        throw std::runtime_error("Provide baseline.svg --reference splat.png --output directory");
    QElapsedTimer clock;
    clock.start();
    const auto sourceInput = read(arguments.front());
    const QImage reference = load(parser.value("reference"));
    const auto raster = parser.isSet("raster-regions")
        ? traceRasterManifest(arguments.front(), sourceInput, reference.size(), number(parser, "trace-tolerance"),
                              number(parser, "trace-corner-threshold")) : RasterManifest{};
    const auto input = parser.isSet("raster-regions") ? raster.svg : sourceInput;
    QSvgRenderer svg(input);
    const auto size = svg.defaultSize();
    if (!svg.isValid() || size.isEmpty()) throw std::runtime_error("SVG needs valid native dimensions");
    if (reference.isNull() || reference.size() != size)
        throw std::runtime_error("Reference and SVG must have identical native dimensions");
    QDir output(parser.value("output"));
    if (!QDir().mkpath(output.absolutePath())) throw std::runtime_error("Cannot create output directory");
    const auto file = [&](const QString &name) { return output.filePath(name); };
    QStringList inputs{arguments.front(), parser.value("reference")};
    inputs += raster.masks;
    if (parser.isSet("original")) inputs.push_back(parser.value("original"));
    QStringList artifacts{"baseline.svg", "optimized.svg", "before.png", "after.png",
        "difference.png", "optimization_difference.png", "source_to_opaque_diff.png", "baseline_fls.svg", "optimized_fls.svg",
        "baseline_fls.png", "optimized_fls.png", "opaque_to_fls_diff.png", "report.json", "ordering.json",
        "before_compact.svg", "after_compact.svg", "before_compact.png", "after_compact.png",
        "before_compact.3so", "after_compact.3so", "before_compact_difference.png", "after_compact_difference.png",
        "before_unculled.png", "after_unculled.png", "before_culling_difference.png", "after_culling_difference.png",
        "before_compact_progress.json", "after_compact_progress.json",
        "abstracted.svg", "abstracted.png", "abstraction_difference.png", "topology_difference.png", "abstraction.json",
        "abstraction_partition.dat", "palette_only.svg", "palette_only.png", "palette_difference.png",
        "regional_candidate.svg", "regional_candidate.png", "regional_difference.png",
        "topology_candidate.svg", "topology_candidate.png", "topology_candidate_difference.png",
        "reconstruction_candidate.svg", "reconstruction_candidate.png", "reconstruction_candidate_difference.png",
        "bucket_candidate.svg", "bucket_candidate.png", "bucket_baseline.svg", "bucket_difference.png",
        "viewport.svg", "viewport.png", "viewport_difference.png",
        "ordering_candidate.svg", "ordering_candidate.png", "ordering_candidate_difference.png", "support_trials.json",
        "fixed_color_ownership.svg", "support_latest_plan.svg", "absorbed.svg", "absorbed.png", "absorption_difference.png", "absorption.json",
        "absorption_regions_before.svg", "absorption_regions_after.svg", "absorption_target.png", "absorption_input.png"};
    for (int index = 0; index < integer(parser, "compact-fit-sample"); ++index) {
        const QString directory = QString("compact_sample_%1/").arg(index, 2, 10, QChar('0'));
        for (const auto &name : QStringList{"before.svg", "after.svg", "report.json", "before_compact.svg",
                                            "after_compact.svg", "before_compact.png", "after_compact.png",
                                            "before_compact.3so", "after_compact.3so", "before_compact_difference.png",
                                            "after_compact_difference.png", "before_compact_progress.json", "after_compact_progress.json"})
            artifacts.push_back(directory + name);
    }
    for (const auto &name : artifacts) {
        const auto target = QFileInfo(file(name)).canonicalFilePath();
        if (target.isEmpty()) continue;
        for (const auto &inputPath : inputs) {
            const auto source = QFileInfo(inputPath).canonicalFilePath();
#ifdef Q_OS_WIN
            const bool same = target.compare(source, Qt::CaseInsensitive) == 0;
#else
            const bool same = target == source;
#endif
            if (same) throw std::runtime_error("Output artifacts would overwrite an input; choose a separate output directory");
        }
    }
    for (const QString name : {"baseline_fls.svg", "optimized_fls.svg", "baseline_fls.png",
                               "optimized_fls.png", "opaque_to_fls_diff.png", "ordering.json", "baseline.svg", "optimized.svg"})
        QFile::remove(file(name));
    for (const QString name : {"before_compact.svg", "after_compact.svg", "before_compact.png", "after_compact.png",
                               "before_compact.3so", "after_compact.3so", "before_compact_difference.png", "after_compact_difference.png",
                               "before_compact_progress.json", "after_compact_progress.json"})
        QFile::remove(file(name));
    for (const QString name : {"abstracted.svg", "abstracted.png", "abstraction_difference.png", "topology_difference.png",
                               "abstraction.json", "palette_only.svg", "palette_only.png", "palette_difference.png",
                               "regional_candidate.svg", "regional_candidate.png", "regional_difference.png",
                               "topology_candidate.svg", "topology_candidate.png", "topology_candidate_difference.png",
                               "reconstruction_candidate.svg", "reconstruction_candidate.png", "reconstruction_candidate_difference.png"})
        QFile::remove(file(name));
    for (const QString name : {"bucket_candidate.svg", "bucket_candidate.png", "bucket_baseline.svg", "bucket_difference.png",
                               "viewport.svg", "viewport.png", "viewport_difference.png",
                               "ordering_candidate.svg", "ordering_candidate.png", "ordering_candidate_difference.png",
                               "optimization_difference.png", "support_trials.json", "fixed_color_ownership.svg", "support_latest_plan.svg",
                               "absorbed.svg", "absorbed.png", "absorption_difference.png", "absorption.json",
                               "absorption_regions_before.svg", "absorption_regions_after.svg", "absorption_target.png", "absorption_input.png"})
        QFile::remove(file(name));
    opaque::Options options;
    QJsonArray supportTrials;
    options.onSupportTrial = [&](const QJsonObject &trial) {
        if (trial.contains("artifact")) {
            const auto name = trial.value("artifact").toString();
            if (name != "fixed_color_ownership.svg" && name != "support_latest_plan.svg")
                throw std::runtime_error("Unknown color-support diagnostic artifact");
            write(file(name), trial.value("svg").toString().toUtf8());
            return;
        }
        supportTrials.append(trial);
        write(file("support_trials.json"), QJsonDocument(supportTrials).toJson());
    };
    options.flattenTolerance = number(parser, "flatten-tolerance");
    options.areaTolerance = number(parser, "area-tolerance");
    options.visibleAreaTolerance = number(parser, "visible-area-tolerance");
    options.silhouetteTolerance = number(parser, "silhouette-tolerance");
    options.renderRmse = number(parser, "render-rmse");
    options.renderMaxError = number(parser, "render-max-error");
    options.maximumPasses = integer(parser, "passes");
    options.maximumChordSpans = integer(parser, "chord-spans");
    options.pathWeight = number(parser, "weight-paths");
    options.segmentWeight = number(parser, "weight-segments");
    options.controlPointWeight = number(parser, "weight-controls");
    options.holeWeight = number(parser, "weight-holes");
    options.componentWeight = number(parser, "weight-components");
    options.orderingWeight = number(parser, "weight-ordering");
    options.nativeItemWeight = number(parser, "weight-native-items");
    options.nativeSegmentWeight = number(parser, "weight-native-segments");
    options.strategy = parser.value("strategy");
    if (!QStringList{"legacy", "current", "difficulty", "savings", "incremental", "supports"}.contains(options.strategy))
        throw std::runtime_error("Strategy must be legacy, current, difficulty, savings, incremental or supports");
    options.hardPointWeight = number(parser, "hard-point-weight");
    options.softPointWeight = number(parser, "soft-point-weight");
    options.concavityWeight = number(parser, "concavity-weight");
    options.lookaheadWeight = number(parser, "lookahead-weight");
    options.shortlistSize = integer(parser, "shortlist");
    options.groupNeighbors = integer(parser, "group-neighbors");
    options.completionCandidates = integer(parser, "completion-candidates");
    options.compareStrategies = parser.isSet("compare-strategies");
    options.componentLimit = integer(parser, "component-limit");
    options.checkpointEdits = integer(parser, "checkpoint-edits");
    options.progress = parser.isSet("progress");
    opaque::ColorOptions colorOptions;
    colorOptions.tolerance = number(parser, "color-tolerance");
    colorOptions.detailScale = number(parser, "detail-scale");
    colorOptions.boundaryTolerance = number(parser, "abstraction-boundary-tolerance");
    colorOptions.renderRmse = number(parser, "abstraction-rmse");
    colorOptions.renderMaxError = number(parser, "abstraction-max-error");
    colorOptions.localRmse = number(parser, "abstraction-local-rmse");
    colorOptions.localWindow = integer(parser, "abstraction-local-window");
    colorOptions.contrastRetention = number(parser, "contrast-retention");
    colorOptions.coverageOverlap = number(parser, "abstraction-overlap");
    colorOptions.reconstruct = !parser.isSet("skip-region-reconstruction");
    colorOptions.cachePath = file("abstraction_partition.dat");
    if (parser.isSet("abstract-colors") && options.strategy != "incremental")
        throw std::runtime_error("Color abstraction requires the incremental strategy");
    if (parser.isSet("raster-regions") && parser.isSet("abstract-colors"))
        throw std::runtime_error("Raster-region input retains its shared palette; abstract-colors would change it");
    if (parser.isSet("absorb-colors") && (parser.isSet("abstract-colors")
        || (options.strategy != "incremental" && options.strategy != "supports")))
        throw std::runtime_error("Color absorption requires incremental or supports and cannot be combined with abstract-colors");
    const int absorptionPasses = integer(parser, "absorption-passes");
    const double sourceGrowth = number(parser, "absorption-source-growth");
    const double sourceEdgeContrast = number(parser, "absorption-edge-contrast");
    if (options.componentLimit < 1 || options.checkpointEdits < 1)
        throw std::runtime_error("Component-limit and checkpoint-edits must be positive");
    if (options.shortlistSize < 1 || options.completionCandidates < 6)
        throw std::runtime_error("Shortlist must be positive; completion-candidates must be at least 6");
    if (parser.isSet("structural") && options.strategy == "legacy")
        throw std::runtime_error("Structural mode requires a path strategy");
    if (options.compareStrategies && (options.strategy == "legacy" || options.strategy == "incremental" || options.strategy == "supports"))
        throw std::runtime_error("Compare-strategies requires current, difficulty or savings strategy");
    if (options.strategy != "legacy" && (options.nativeItemWeight > 0 || options.nativeSegmentWeight > 0))
        throw std::runtime_error("Native mesh weights require the legacy strategy");
    if (options.flattenTolerance <= 0 || options.areaTolerance <= 0)
        throw std::runtime_error("Flattening and area tolerances must be positive");
    if (colorOptions.localWindow < 1 || colorOptions.contrastRetention > 1)
        throw std::runtime_error("Abstraction local window must be positive and contrast retention must be between 0 and 1");
    auto document = gui::extractSvgVectorObjects(input, size);
    if (!document.supportsObjectSelection() || document.objects.isEmpty())
        throw std::runtime_error(("Unsupported baseline: " + document.fallbackReason).toStdString());
    const auto baseline = svgRender(input, size);
    const auto capture = opaque::render(document.objects, size);
    const auto opacity = opacityAudit(document.objects, baseline);
    const bool opaqueFills = opacity.value("nonopaque_fills").toInt() == 0;
    const auto canonical = opaqueFills ? opaque::svg(document.objects, size) : input;
    const auto canonicalRender = svgRender(canonical, size);
    const auto captureError = opaque::imageError(baseline, capture);
    const auto roundtripError = opaque::imageError(baseline, canonicalRender);
    const auto conversion = opaque::imageError(reference, baseline);
    const auto beforePaths = opaque::pathMetrics(document.objects, options.flattenTolerance).json();
    QJsonObject report{
        {"schema", 4}, {"native_width", size.width()}, {"native_height", size.height()},
        {"captured_paths", document.objects.size()},
        {"svg_before", beforePaths}, {"svg_after", beforePaths},
        {"appearance_groups", appearanceGroups(opaque::makeItems(document.objects))},
        {"input_sha256", QString::fromLatin1(QCryptographicHash::hash(sourceInput, QCryptographicHash::Sha256).toHex())},
        {"reference_sha256", QString::fromLatin1(QCryptographicHash::hash(read(parser.value("reference")), QCryptographicHash::Sha256).toHex())},
        {"source_to_opaque", conversion}, {"svg_to_capture", captureError},
        {"structural_mode", parser.isSet("structural")},
        {"conversion_passed", conversion.value("rmse").toDouble() <= number(parser, "conversion-rmse")
            && conversion.value("max_channel_error").toDouble() <= number(parser, "conversion-max-error")},
        {"validation_renderer", options.strategy == "legacy" ? "native_mesh_and_svg" : "svg_paths"},
        {"closure_fill_rule_transform_roundtrip", roundtripError}, {"opacity", opacity},
        {"baseline_to_optimized", QJsonValue()}, {"input_to_topology_baseline", QJsonValue()},
        {"topology_baseline_to_optimized", QJsonValue()}, {"before", QJsonValue()}, {"after", QJsonValue()},
        {"options", QJsonObject{{"flatten_pixels", options.flattenTolerance}, {"intersection_area", options.areaTolerance},
            {"visible_color_area", options.visibleAreaTolerance > 0 ? options.visibleAreaTolerance : options.areaTolerance},
            {"silhouette_area", options.silhouetteTolerance}, {"render_rmse_8bit", options.renderRmse},
            {"render_max_error_8bit", options.renderMaxError}, {"conversion_rmse_8bit", number(parser, "conversion-rmse")},
            {"conversion_max_error_8bit", number(parser, "conversion-max-error")},
            {"weight_paths", options.pathWeight}, {"weight_segments", options.segmentWeight},
            {"weight_controls", options.controlPointWeight},
            {"weight_holes", options.holeWeight}, {"weight_components", options.componentWeight},
            {"weight_ordering", options.orderingWeight}, {"weight_native_items", options.nativeItemWeight},
            {"weight_native_segments", options.nativeSegmentWeight}}},
        {"objective", "Bucket-compatible connected paint targets, encoded boundary segments, holes and fixed-ownership ordering; exact fills may occupy several stack positions"},
        {"compact_fit", QJsonObject{{"measured", false}, {"placements", QJsonValue()}, {"runtime_seconds", QJsonValue()}}},
        {"cost_encoding", options.strategy == "legacy"
            ? "FLS polygon mesh, Square/Triangle, float32 native shape transforms; shape records exclude container framing"
            : "Emitted SVG paths, line/cubic commands, cubic control points, holes and components; Compact Fit placements measured separately"},
        {"approximate_color_merging", parser.isSet("abstract-colors") || parser.isSet("absorb-colors")}, {"accepted", 0}, {"status", "baseline_rejected"},
    };
    if (parser.isSet("raster-regions")) report.insert("raster_trace", raster.audit);
    auto reportedOptions = report.value("options").toObject();
    reportedOptions.insert("strategy", options.strategy);
    reportedOptions.insert("hard_point_weight", options.hardPointWeight);
    reportedOptions.insert("soft_point_weight", options.softPointWeight);
    reportedOptions.insert("concavity_weight", options.concavityWeight);
    reportedOptions.insert("lookahead_weight", options.lookaheadWeight);
    reportedOptions.insert("shortlist", options.shortlistSize);
    reportedOptions.insert("group_neighbors", options.groupNeighbors);
    reportedOptions.insert("completion_candidates", options.completionCandidates);
    reportedOptions.insert("component_limit", options.componentLimit);
    reportedOptions.insert("checkpoint_edits", options.checkpointEdits);
    reportedOptions.insert("boolean_coordinate_grid", opaque::booleanCoordinateGrid());
    reportedOptions.insert("color_absorption_enabled", parser.isSet("absorb-colors"));
    reportedOptions.insert("absorption_passes", absorptionPasses);
    reportedOptions.insert("absorption_source_growth", sourceGrowth);
    reportedOptions.insert("absorption_edge_contrast", sourceEdgeContrast);
    report.insert("options", reportedOptions);
    if (!opaqueFills) {
        auto forced = document.objects;
        for (auto &object : forced) object.color.setAlpha(255);
        report.insert("translucent_to_forced_opaque", opaque::imageError(baseline, opaque::render(forced, size)));
        report.insert("reference_to_forced_opaque", opaque::imageError(reference, opaque::render(forced, size)));
    }
    QImage qualityTarget = reference;
    if (parser.isSet("original")) {
        const QImage original = load(parser.value("original"));
        if (original.isNull() || original.size() != size)
            throw std::runtime_error("Original raster must match native dimensions");
        report.insert(parser.isSet("raster-regions") ? "original_to_posterized" : "original_to_splat", opaque::imageError(original, reference));
        report.insert("original_to_opaque", opaque::imageError(original, baseline));
        report.insert("original_sha256", QString::fromLatin1(QCryptographicHash::hash(read(parser.value("original")), QCryptographicHash::Sha256).toHex()));
        qualityTarget = original;
    }
    save(file("before.png"), baseline);
    save(file("source_to_opaque_diff.png"), opaque::difference(reference, baseline));
    const auto inputBucket = opaque::auditPenTargets(document.objects, options);
    report.insert("bucket_input", inputBucket);
    if (opaqueFills) {
        write(file("baseline.svg"), canonical);
        if (options.strategy == "legacy" || inputBucket.value("passed").toBool())
            write(file("optimized.svg"), canonical);
    }
    QString rejected;
    if (!opaqueFills)
        rejected = "Nonopaque baseline fills; explicit forced-opacity error recorded, optimization skipped";
    else if (captureError.value("max_channel_error").toDouble() > 1
        || roundtripError.value("max_channel_error").toDouble() > 1
        || captureError.value("max_alpha_error").toDouble() > 1
        || roundtripError.value("max_alpha_error").toDouble() > 1)
        rejected = "SVG capture or export roundtrip mismatch";
    else if (!parser.isSet("structural") && (conversion.value("rmse").toDouble() > number(parser, "conversion-rmse")
        || conversion.value("max_channel_error").toDouble() > number(parser, "conversion-max-error")))
        rejected = "Source-to-opaque conversion exceeds budget; structural optimization was skipped";
    if (rejected.isEmpty()) {
        const opaque::Renderer renderer = [size](const auto &objects) { return svgRender(opaque::svg(objects, size), size); };
        const auto windowed = opaque::clipToViewport(document.objects, size, options);
        const QRectF viewport{QPointF(), QSizeF(size)};
        const bool viewportChanged = std::any_of(document.objects.begin(), document.objects.end(), [&](const auto &object) {
            return !viewport.contains(object.path.boundingRect());
        });
        const auto viewportRender = renderer(windowed);
        const auto viewportError = opaque::imageError(baseline, viewportRender);
        const bool viewportPassed = viewportError.value("rmse").toDouble() <= options.renderRmse
            && viewportError.value("max_channel_error").toDouble() <= options.renderMaxError
            && viewportError.value("max_alpha_error").toDouble() <= options.renderMaxError;
        report.insert("viewport", QJsonObject{{"changed", viewportChanged}, {"passed", viewportPassed},
            {"before_objects", document.objects.size()}, {"after_objects", windowed.size()}, {"image_error", viewportError},
            {"geometry_scope", "Native SVG viewport; off-canvas paint is excluded from Bucket targets and fixed ownership"}});
        write(file("viewport.svg"), opaque::svg(windowed, size));
        save(file("viewport.png"), viewportRender);
        save(file("viewport_difference.png"), opaque::difference(baseline, viewportRender));
        if (viewportPassed) document.objects = windowed;
        else {
            rejected = "Viewport clipping exceeds rendering limits";
            QFile::remove(file("optimized.svg"));
        }
        opaque::ColorResult abstraction;
        abstraction.items = opaque::makeItems(document.objects);
        if (parser.isSet("abstract-colors") && rejected.isEmpty()) {
            abstraction = opaque::abstractColors(abstraction.items, options, colorOptions, renderer);
            report.insert("abstraction", abstraction.report);
            write(file("abstraction.json"), QJsonDocument(abstraction.report).toJson());
            write(file("abstracted.svg"), opaque::svg(opaque::objects(abstraction.items), size));
            write(file("palette_only.svg"), opaque::svg(opaque::objects(abstraction.paletteItems), size));
            const auto palette = renderer(opaque::objects(abstraction.paletteItems));
            save(file("palette_only.png"), palette);
            save(file("palette_difference.png"), opaque::difference(baseline, palette));
            write(file("regional_candidate.svg"), opaque::svg(opaque::objects(abstraction.regionalItems), size));
            const auto regional = renderer(opaque::objects(abstraction.regionalItems));
            save(file("regional_candidate.png"), regional);
            save(file("regional_difference.png"), opaque::difference(baseline, regional));
            const auto topology = renderer(opaque::objects(abstraction.topologyCandidate));
            const auto reconstructed = renderer(opaque::objects(abstraction.reconstructionCandidate));
            write(file("topology_candidate.svg"), opaque::svg(opaque::objects(abstraction.topologyCandidate), size));
            write(file("reconstruction_candidate.svg"), opaque::svg(opaque::objects(abstraction.reconstructionCandidate), size));
            save(file("topology_candidate.png"), topology);
            save(file("topology_candidate_difference.png"), opaque::difference(baseline, topology));
            save(file("reconstruction_candidate.png"), reconstructed);
            save(file("reconstruction_candidate_difference.png"), opaque::difference(baseline, reconstructed));
            const auto reduced = renderer(opaque::objects(abstraction.items));
            save(file("abstracted.png"), reduced);
            save(file("abstraction_difference.png"), opaque::difference(baseline, reduced));
        }
        auto optimizerInput = abstraction.items;
        QImage absorptionBaseline;
        QVector<opaque::Item> absorptionFitBaseline;
        QJsonObject absorptionReport;
        bool absorptionChanged = false;
        bool bucketChanged = viewportChanged;
        if (options.strategy != "legacy" && rejected.isEmpty()) {
            if (options.progress) std::fprintf(stderr, "Bucket baseline: validating %d source objects\n", int(optimizerInput.size()));
            const auto prepared = opaque::preparePenTargets(optimizerInput, options, renderer);
            if (options.progress) std::fprintf(stderr, "Bucket baseline: %d targets, %s\n", int(prepared.items.size()),
                prepared.error.isEmpty() ? "passed" : "rejected");
            report.insert("bucket_baseline", prepared.report);
            const auto candidate = opaque::svg(opaque::objects(prepared.items), size);
            write(file("bucket_candidate.svg"), candidate);
            const auto painted = svgRender(candidate, size);
            save(file("bucket_candidate.png"), painted);
            save(file("bucket_difference.png"), opaque::difference(renderer(opaque::objects(optimizerInput)), painted));
            rejected = prepared.error;
            bucketChanged |= prepared.changed;
            if (rejected.isEmpty()) {
                write(file("bucket_baseline.svg"), candidate);
                optimizerInput = prepared.items;
                write(file("optimized.svg"), candidate);
            } else if (viewportChanged) QFile::remove(file("optimized.svg"));
        }
        if (parser.isSet("absorb-colors") && rejected.isEmpty()) {
            if (integer(parser, "compact-fit-limit") > 0) absorptionFitBaseline = optimizerInput;
            absorptionBaseline = renderer(opaque::objects(optimizerInput));
            save(file("absorption_target.png"), qualityTarget);
            save(file("absorption_input.png"), absorptionBaseline);
            auto absorptionOptions = options;
            absorptionOptions.strategy = "incremental";
            absorptionOptions.maximumPasses = absorptionPasses;
            absorptionOptions.colorOwnership = false;
            absorptionOptions.onSupportTrial = {};
            auto limits = colorOptions;
            limits.renderRmse = options.renderRmse;
            limits.renderMaxError = options.renderMaxError;
            opaque::Optimizer absorber(optimizerInput, absorptionOptions, {}, renderer);
            absorber.setAppearanceGuard(baseline, limits);
            absorber.setColorAbsorption(qualityTarget, limits, sourceGrowth, sourceEdgeContrast);
            const auto absorbed = absorber.run();
            const auto image = renderer(opaque::objects(absorbed.items));
            const auto bucket = opaque::auditPenTargets(opaque::objects(absorbed.items), options);
            const auto detail = opaque::absorptionDetailError(qualityTarget, absorptionBaseline, image, limits, sourceEdgeContrast);
            absorptionChanged = absorbed.error.isEmpty() && bucket.value("passed").toBool()
                && detail.value("passed").toBool() && absorbed.accepted > 0;
            absorptionReport = {{"status", absorptionChanged ? "accepted" : absorbed.error.isEmpty() ? "unchanged" : "rejected"},
                {"error", absorbed.error}, {"accepted_merges", absorbed.accepted},
                {"before", absorbed.before.json()}, {"after", absorbed.after.json()},
                {"visible_regions_before", opaque::absorptionRegionMetrics(absorbed.ownershipBefore, options.flattenTolerance)},
                {"visible_regions_after", opaque::absorptionRegionMetrics(absorbed.ownershipAfter, options.flattenTolerance)},
                {"source_before", opaque::imageError(qualityTarget, absorptionBaseline)},
                {"source_after", opaque::imageError(qualityTarget, image)},
                {"baseline_to_absorbed", opaque::imageError(absorptionBaseline, image)},
                {"input_to_absorbed", opaque::appearanceError(baseline, image, limits.localWindow)},
                {"source_detail", detail}, {"source_rmse_growth_limit", sourceGrowth},
                {"quality_target", parser.isSet("original") ? "original_raster" : "checkpoint_render"},
                {"ordering", absorbed.graph.json()}, {"native_targets", bucket}, {"diagnostics", absorbed.diagnostics}};
            if (absorptionChanged) optimizerInput = absorbed.items;
            report.insert("color_absorption", absorptionReport);
            write(file("absorbed.svg"), opaque::svg(opaque::objects(optimizerInput), size));
            write(file("absorption_regions_before.svg"), opaque::svg(absorbed.ownershipBefore, size));
            write(file("absorption_regions_after.svg"), opaque::svg(absorbed.ownershipAfter, size));
            save(file("absorbed.png"), renderer(opaque::objects(optimizerInput)));
            save(file("absorption_difference.png"), opaque::difference(absorptionBaseline, renderer(opaque::objects(optimizerInput))));
            write(file("absorption.json"), QJsonDocument(absorptionReport).toJson());
            if (options.progress) std::fprintf(stderr, "absorption: %d accepted merges, %d -> %d connected paths\n",
                absorbed.accepted, absorbed.before.paths, absorbed.after.paths);
        }
        if (rejected.isEmpty()) {
            gui::PolygonMeshSources sources;
            opaque::Encoder encoder;
            if (options.strategy == "legacy") {
                gui::ShapeGeometryStore geometry;
                QString error;
                if (!geometry.loadDefault(&error)) throw std::runtime_error(error.toStdString());
                sources = gui::buildPolygonMeshSources(geometry);
                if (!sources.valid()) throw std::runtime_error("Native Square/Triangle geometry unavailable");
                encoder = opaque::meshEncoder(sources, options);
            }
            if (options.progress) std::fprintf(stderr, "Ownership: partitioning %d paint targets\n", int(optimizerInput.size()));
            opaque::Optimizer optimizer(optimizerInput, options, encoder, renderer);
            if (options.progress) std::fprintf(stderr, "Ownership: ready, starting %s search\n", qPrintable(options.strategy));
            if (parser.isSet("abstract-colors")) optimizer.setAppearanceGuard(baseline, colorOptions);
            else if (parser.isSet("absorb-colors")) {
                auto limits = colorOptions;
                limits.renderRmse = options.renderRmse;
                limits.renderMaxError = options.renderMaxError;
                limits.localRmse = options.renderMaxError;
                optimizer.setAppearanceGuard(baseline, limits);
            }
            else if (options.strategy == "supports" && bucketChanged) {
                opaque::ColorOptions limits;
                limits.renderRmse = options.renderRmse;
                limits.renderMaxError = limits.localRmse = options.renderMaxError;
                limits.localWindow = 1;
                optimizer.setAppearanceGuard(baseline, limits);
            }
            if (options.strategy == "legacy") report.insert("ownership", optimizer.ownershipDiagnostics());
            auto result = optimizer.run();
            if ((abstraction.changed || absorptionChanged || bucketChanged || parser.isSet("absorb-colors")) && result.error.isEmpty()) {
                const auto cumulative = opaque::appearanceError(baseline, renderer(opaque::objects(result.items)), colorOptions.localWindow);
                bool sourcePassed = true;
                if (parser.isSet("absorb-colors")) {
                    const auto actual = renderer(opaque::objects(result.items));
                    const auto source = opaque::imageError(qualityTarget, actual);
                    const auto initial = opaque::imageError(qualityTarget, absorptionBaseline);
                    const auto detail = opaque::absorptionDetailError(qualityTarget, absorptionBaseline, actual, colorOptions, sourceEdgeContrast);
                    const double absorbedSourceRmse = absorptionReport.value("source_after").toObject().value("rmse").toDouble();
                    const double sourceLimit = std::min(initial.value("rmse").toDouble(), absorbedSourceRmse) + sourceGrowth;
                    sourcePassed = source.value("rmse").toDouble() <= sourceLimit + 1e-9
                        && detail.value("passed").toBool();
                    report.insert("final_absorption_source_detail", detail);
                    report.insert("quality_target_to_final", source);
                }
                if (cumulative.value("rmse").toDouble() > (abstraction.changed ? colorOptions.renderRmse : options.renderRmse)
                    || cumulative.value("max_channel_error").toDouble() > (abstraction.changed ? colorOptions.renderMaxError : options.renderMaxError)
                    || (abstraction.changed && cumulative.value("max_local_rmse").toDouble() > colorOptions.localRmse)
                    || !sourcePassed
                    || cumulative.value("max_alpha_error").toDouble() > options.renderMaxError
                    || opaque::silhouetteDifference(document.objects, opaque::objects(result.items), options.flattenTolerance)
                        > options.silhouetteTolerance) {
                    result.items = optimizer.baselineItems();
                    result.after = result.before;
                    result.graph = optimizer.dependencies(result.items);
                    result.accepted = 0;
                    result.selectedStrategy = "baseline";
                    result.silhouetteDifference = 0;
                    result.benchmarks.clear();
                    result.diagnostics.append(QJsonObject{{"rejection", "Topology edits exceeded cumulative input limits; validated Bucket baseline retained"}});
                }
            }
            const auto exported = opaque::svg(opaque::objects(result.items), size);
            if (!result.candidateItems.isEmpty()) {
                const auto candidateSvg = opaque::svg(opaque::objects(result.candidateItems), size);
                const auto candidate = svgRender(candidateSvg, size);
                write(file("ordering_candidate.svg"), candidateSvg);
                save(file("ordering_candidate.png"), candidate);
                save(file("ordering_candidate_difference.png"), opaque::difference(baseline, candidate));
                auto candidateReport = result.candidateReport;
                candidateReport.insert("selected", result.accepted > 0 && candidateSvg == exported);
                candidateReport.insert("input_to_candidate", opaque::imageError(baseline, candidate));
                candidateReport.insert("bucket", opaque::auditPenTargets(gui::extractSvgVectorObjects(candidateSvg, size).objects, options));
                report.insert("ordering_candidate", candidateReport);
            }
            const auto exportedObjects = gui::extractSvgVectorObjects(exported, size);
            const auto bucketExport = opaque::auditPenTargets(exportedObjects.objects, options);
            report.insert("bucket_export", bucketExport);
            if (options.strategy != "legacy" && (!bucketExport.value("passed").toBool()
                || exportedObjects.objects.size() != result.items.size()))
                result.error = "Final SVG contains an incompatible Bucket target";
            rejected = result.error;
            report.insert("before", abstraction.changed ? abstraction.report.value("before").toObject()
                : absorptionChanged ? absorptionReport.value("before").toObject() : result.before.json());
            report.insert("topology_before", result.before.json());
            report.insert("after", result.after.json());
            report.insert("svg_after", opaque::pathMetrics(opaque::objects(result.items), options.flattenTolerance).json());
            report.insert("accepted", result.accepted);
            report.insert("silhouette_difference_area", abstraction.changed || absorptionChanged
                ? opaque::silhouetteDifference(document.objects, opaque::objects(result.items), options.flattenTolerance)
                : result.silhouetteDifference);
            report.insert("native_silhouette_difference_area", options.strategy == "legacy"
                ? QJsonValue(result.nativeSilhouetteDifference) : QJsonValue());
            report.insert("appearance_groups", appearanceGroups(result.items));
            report.insert("ordering", result.graph.json());
            report.insert("diagnostics", result.diagnostics);
            report.insert("strategies", result.strategies);
            report.insert("selected_strategy", abstraction.changed
                ? result.accepted ? "color-abstraction+incremental" : "color-abstraction"
                : absorptionChanged ? "color-absorption+" + result.selectedStrategy : result.selectedStrategy);
            if (bucketChanged) report.insert("bucket_normalization_changed", true);
            if (!result.decompositionError.isEmpty()) {
                report.insert("opaque_to_fls_baseline", result.decompositionError);
                if (!result.error.isEmpty()) {
                    const auto encoded = opaque::svg(result.nativeShapes, size);
                    const auto encodedRender = svgRender(encoded, size);
                    write(file("baseline_fls.svg"), encoded);
                    save(file("baseline_fls.png"), encodedRender);
                    save(file("opaque_to_fls_diff.png"), opaque::difference(baseline, encodedRender));
                }
            }
            write(file("ordering.json"), QJsonDocument(QJsonObject{{"graph", result.graph.json()},
                {"passes", result.diagnostics}}).toJson());
            if (result.error.isEmpty()) {
                const auto optimizedSvg = options.strategy != "legacy" || result.accepted || abstraction.changed ? exported : canonical;
                const auto after = svgRender(optimizedSvg, size);
                if (parser.isSet("absorb-colors")) {
                    report.insert("quality_target_to_final", opaque::imageError(qualityTarget, after));
                    report.insert("final_absorption_source_detail", opaque::absorptionDetailError(qualityTarget,
                        absorptionBaseline, after, colorOptions, sourceEdgeContrast));
                }
                if (parser.isSet("original")) report.insert("original_to_final", opaque::imageError(qualityTarget, after));
                const auto topologyBaseline = renderer(opaque::objects(optimizer.baselineItems()));
                report.insert("input_to_topology_baseline", opaque::imageError(baseline, topologyBaseline));
                report.insert("topology_baseline_to_optimized", opaque::imageError(topologyBaseline, after));
                save(file("optimization_difference.png"), opaque::difference(topologyBaseline, after));
                if (parser.isSet("abstract-colors")) {
                    const auto reduced = renderer(opaque::objects(abstraction.items));
                    const auto palette = renderer(opaque::objects(abstraction.paletteItems));
                    report.insert("opaque_to_palette", opaque::appearanceError(baseline, palette, colorOptions.localWindow));
                    const auto regional = renderer(opaque::objects(abstraction.regionalItems));
                    report.insert("palette_to_regional", opaque::appearanceError(palette, regional, colorOptions.localWindow));
                    report.insert("opaque_to_regional", opaque::appearanceError(baseline, regional, colorOptions.localWindow));
                    report.insert("opaque_to_topology_candidate", opaque::appearanceError(baseline,
                        renderer(opaque::objects(abstraction.topologyCandidate)), colorOptions.localWindow));
                    report.insert("opaque_to_reconstruction_candidate", opaque::appearanceError(baseline,
                        renderer(opaque::objects(abstraction.reconstructionCandidate)), colorOptions.localWindow));
                    report.insert("opaque_to_abstracted", opaque::appearanceError(baseline, reduced, colorOptions.localWindow));
                    report.insert("abstracted_to_optimized", opaque::imageError(reduced, after));
                    report.insert("opaque_to_final_appearance", opaque::appearanceError(baseline, after, colorOptions.localWindow));
                    save(file("topology_difference.png"), opaque::difference(reduced, after));
                }
                report.insert("baseline_to_optimized", opaque::imageError(baseline, after));
                report.insert("baseline_to_optimized_paths", opaque::imageError(baseline, after));
                report.insert("source_to_final", opaque::imageError(reference, after));
                write(file("optimized.svg"), optimizedSvg);
                save(file("after.png"), after);
                save(file("difference.png"), opaque::difference(baseline, after));
                report.insert("status", result.accepted || abstraction.changed || absorptionChanged ? "optimized" : bucketChanged ? "normalized" : "unchanged");
                const int compactLimit = integer(parser, "compact-fit-limit");
                if (compactLimit > 0) report.insert("compact_fit", benchmarkCompactFit(opaque::objects(
                    absorptionFitBaseline.isEmpty() ? optimizer.baselineItems() : absorptionFitBaseline),
                    opaque::objects(result.items), compactLimit, integer(parser, "compact-fit-evaluations"), size, output));
                QJsonArray sampled;
                const int sampleCount = std::min(integer(parser, "compact-fit-sample"), int(result.benchmarks.size()));
                for (int index = 0; index < sampleCount; ++index) {
                    const auto &sample = result.benchmarks[index];
                    const QString directory = QString("compact_sample_%1").arg(index, 2, 10, QChar('0'));
                    if (!output.mkpath(directory)) throw std::runtime_error("Cannot create Compact Fit sample directory");
                    const QDir target(output.filePath(directory));
                    write(target.filePath("before.svg"), opaque::svg(sample.before, size));
                    write(target.filePath("after.svg"), opaque::svg(sample.after, size));
                    auto measured = benchmarkCompactFit(sample.before, sample.after, std::max(sample.before.size(), sample.after.size()),
                        integer(parser, "compact-fit-evaluations"), size, target);
                    measured.insert("sources", sample.sources);
                    measured.insert("geometry_saving", sample.saving);
                    measured.insert("directory", directory);
                    write(target.filePath("report.json"), QJsonDocument(measured).toJson());
                    sampled.append(measured);
                }
                report.insert("compact_fit_samples", sampled);
                if (options.strategy == "legacy") {
                    const auto nativeAfter = svgRender(opaque::svg(result.nativeShapes, size), size);
                    QVector<gui::SvgVectorObject> nativeBefore;
                    auto encoder = opaque::meshEncoder(sources, options);
                    for (const auto &object : document.objects) nativeBefore += encoder(object.path, object.color).shapes;
                    const auto beforeEncoded = svgRender(opaque::svg(nativeBefore, size), size);
                    report.insert("baseline_to_optimized", opaque::imageError(beforeEncoded, nativeAfter));
                    report.insert("baseline_to_optimized_paths", opaque::imageError(baseline, after));
                    report.insert("opaque_to_fls_baseline", opaque::imageError(baseline, beforeEncoded));
                    report.insert("source_to_final", opaque::imageError(reference, nativeAfter));
                    write(file("optimized.svg"), optimizedSvg);
                    write(file("baseline_fls.svg"), opaque::svg(nativeBefore, size));
                    write(file("optimized_fls.svg"), opaque::svg(result.nativeShapes, size));
                    save(file("baseline_fls.png"), beforeEncoded);
                    save(file("optimized_fls.png"), nativeAfter);
                    save(file("after.png"), after);
                    save(file("difference.png"), opaque::difference(beforeEncoded, nativeAfter));
                }
            }
        }
    }
    if (!rejected.isEmpty()) {
        save(file("after.png"), baseline);
        save(file("difference.png"), opaque::difference(baseline, baseline));
        report.insert("rejection", rejected);
    }
    report.insert("runtime_seconds", clock.elapsed() / 1000.0);
    write(file("report.json"), QJsonDocument(report).toJson());
    const auto summary = QJsonDocument(report).toJson(QJsonDocument::Compact);
    std::fwrite(summary.constData(), 1, size_t(summary.size()), stdout);
    std::fputc('\n', stdout);
    return rejected.isEmpty() ? 0 : 3;
}

} // namespace

int main(int argc, char **argv) {
    const bool fillOnly = std::any_of(argv + 1, argv + argc, [](const char *value) {
        return QByteArray(value) == "--fill-only";
    });
#ifdef Q_OS_WIN
    qputenv("QT_QPA_PLATFORM", fillOnly ? "windows" : "minimal");
#else
    qputenv("QT_QPA_PLATFORM", fillOnly ? "offscreen" : "minimal");
#endif
    QGuiApplication application(argc, argv);
    QCoreApplication::setApplicationName("fls_opaque_regions");
    QCommandLineParser parser;
    parser.setApplicationDescription("Audit and simplify SVG path count, geometry and ordering with native FLS mesh diagnostics.");
    parser.addHelpOption();
    parser.addPositionalArgument("baseline.svg", "Opaque SVG exported from the same checkpoint as the reference.");
    parser.addOption({"self-test", "Run deterministic geometry, ordering and export tests."});
    parser.addOption({"fill-only", "Fill validated contours with Compact Fit and save a project and native render."});
    parser.addOption({"prepare-fill-targets", "Save exact Compact Fit target polygons for seed construction and conversion auditing."});
    parser.addOption({"recover-fill-curves", "Recover mixed line/cubic contours with the Bucket contour fitter."});
    parser.addOption({"compact-fit-cpu", "Explicitly use CPU fitting instead of the editor GPU pipeline."});
    parser.addOption({"compact-fit-cache", "Shared cache of actual per-region Compact Fit results.", "directory"});
    parser.addOption({"compact-fit-seeds", "Validated-input polygon seed manifest for failed Pen-core construction.", "json"});
    parser.addOption({"compact-fit-polygon-seed", "Start Compact Fit from provided polygon seeds while verifying the original contours."});
    parser.addOption({"scene-validated-fills", "Allow retained region quality warnings only when the complete native render passes its image and silhouette budgets."});
    parser.addOption({"reference", "Same-checkpoint splat render, at native resolution.", "png"});
    parser.addOption({"original", "Optional original source raster at native resolution.", "image"});
    parser.addOption({"output", "Artifact directory.", "directory"});
    parser.addOption({"structural", "Optimize the existing opaque SVG while separately reporting unresolved source conversion loss."});
    parser.addOption({"progress", "Print incremental checkpoints to standard error."});
    parser.addOption({"raster-regions", "Trace a native-resolution binary-mask paint manifest before the SVG audit."});
    parser.addOption({"abstract-colors", "Merge similar colors with original-color and local-detail guards before topology optimization; no region-count target."});
    parser.addOption({"absorb-colors", "Absorb adjacent colors through individually validated geometry transactions before topology optimization."});
    parser.addOption({"skip-region-reconstruction", "Retain recolored source supports before topology optimization."});
    parser.addOption({"strategy", "Color-wide supports, incremental contour/group transactions, bottom-up current/difficulty/savings, or legacy mesh optimization.", "name", "incremental"});
    parser.addOption({"compare-strategies", "Compare current order, difficulty order and savings with neighbor lookahead."});
    const QVector<QStringList> numeric{
        {"trace-tolerance", "Potrace optimization tolerance for raster-region masks, in native pixels.", "0.1"},
        {"trace-corner-threshold", "Potrace corner threshold for raster-region masks; larger values favor curves.", "1"},
        {"conversion-rmse", "Maximum source-to-opaque RMSE in 8-bit channels.", "2"},
        {"conversion-max-error", "Maximum source-to-opaque channel error.", "32"},
        {"render-rmse", "Maximum baseline-to-final RMSE in 8-bit channels.", "0.5"},
        {"render-max-error", "Maximum baseline-to-final channel error.", "16"},
        {"color-tolerance", "Oklab difference in 0-100 units for regional color abstraction.", "5"},
        {"absorption-passes", "Local color-absorption adjacency sweeps before topology optimization.", "2"},
        {"absorption-source-growth", "Maximum cumulative source RMSE increase over the validated opaque baseline.", "0"},
        {"absorption-edge-contrast", "Minimum native-pixel source gradient protected during color absorption.", "24"},
        {"detail-scale", "Native-pixel scale for ranking eligible similar-color boundary merges.", "24"},
        {"abstraction-boundary-tolerance", "Internal boundary simplification tolerance in native pixels; external silhouette stays locked.", "0.25"},
        {"abstraction-rmse", "Maximum cumulative opaque-to-final RMSE when color abstraction is enabled.", "12"},
        {"abstraction-max-error", "Maximum cumulative opaque-to-final channel error when color abstraction is enabled.", "64"},
        {"abstraction-local-rmse", "Maximum RMSE in any native-pixel sliding window against the opaque input.", "12"},
        {"abstraction-local-window", "Native-pixel window width and height for local appearance validation.", "16"},
        {"contrast-retention", "Minimum retained fraction of original Oklab contrast at significant region boundaries, from 0 to 1.", "0.75"},
        {"abstraction-overlap", "Hidden overlap in native pixels beneath later regions to preserve opaque boundary coverage.", "1"},
        {"flatten-tolerance", "Adaptive cubic flattening error in native pixels.", "0.05"},
        {"area-tolerance", "Positive-area intersection and containment tolerance.", "0.000001"},
        {"visible-area-tolerance", "Maximum symmetric difference area per original visible color; 0 uses area-tolerance.", "0"},
        {"silhouette-tolerance", "Maximum symmetric silhouette difference area.", "0.000001"},
        {"passes", "Maximum optimization rounds; also bounds forward color-order trials.", "20"},
        {"chord-spans", "Fine-search chord length; longer chains use a coarse search. Set 0 to disable chord proposals.", "8"},
        {"weight-paths", "Cost per selectable SVG path, independent of color groups.", "1"},
        {"weight-segments", "Cost per SVG line or Bezier boundary segment.", "1"},
        {"weight-controls", "Additional cost per encoded cubic control point; set 0 for the original objective.", "1"},
        {"weight-holes", "Cost per logical hole.", "4"},
        {"weight-components", "Cost per logical component.", "1"},
        {"weight-ordering", "Optional cost per ownership dependency edge; ordering is always validated.", "0"},
        {"weight-native-items", "Additional cost per native mesh placement; separate from Compact Fit.", "0"},
        {"weight-native-segments", "Additional cost per native mesh polygon edge.", "0"},
        {"hard-point-weight", "Difficulty per hard contour point.", "1"},
        {"soft-point-weight", "Difficulty per curved span.", "2"},
        {"concavity-weight", "Additional difficulty per concavity or curvature sign change.", "2"},
        {"lookahead-weight", "Penalty for restricting neighboring completion choices.", "1"},
        {"shortlist", "Candidate shortlist for neighbor lookahead.", "12"},
        {"group-neighbors", "Nearest same-fill partners considered in addition to whole-color groups.", "2"},
        {"completion-candidates", "Maximum initial stack proposals and retained incremental chord alternatives per contour.", "16"},
        {"component-limit", "Maximum ownership components for split and split/group transactions.", "8"},
        {"checkpoint-edits", "Accepted transactions between complete geometry and render checks.", "32"},
        {"compact-fit-sample", "Benchmark this many changed artwork groups with actual Compact Fit; 0 disables sampling.", "0"},
        {"compact-fit-limit", "Benchmark complete before/after scenes up to this path count; 0 disables the benchmark.", "0"},
        {"compact-fit-evaluations", "Actual Compact Fit evaluation budget per path, with empty leeway.", "60000"},
        {"curve-fit-tolerance", "Bucket contour fitting tolerance in native pixels.", "1"},
        {"compact-fit-shape-budget", "Compact Fit shape budget per region.", "3000"},
        {"compact-fit-total-shape-budget", "Hard Compact Fit shape limit for the complete image.", "3000"},
        {"compact-fit-workers", "Concurrent independent Compact Fit region workers.", "4"},
        {"compact-fit-render-samples", "Native renderer samples per axis for boundary antialiasing, from 1 to 4.", "4"},
        {"compact-fit-boundary", "Compact Fit outward boundary allowance in native pixels.", "2"},
        {"compact-fit-inward", "Compact Fit inward boundary allowance in native pixels.", "0.5"},
        {"compact-fit-area-ratio", "Compact Fit maximum relative area error.", "0.02"},
    };
    for (const auto &option : numeric) parser.addOption({option[0], option[1], "number", option[2]});
    parser.process(application);
    try {
        return parser.isSet("self-test") ? opaque::selfTest() : parser.isSet("recover-fill-curves")
            ? recoverFillCurves(parser) : parser.isSet("prepare-fill-targets")
            ? prepareFillTargets(parser) : fillOnly ? runFillOnly(parser) : run(parser);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
