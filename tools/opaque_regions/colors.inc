namespace {

using ColorVector = std::array<double, 3>;

ColorVector perceptualColor(QColor color) {
    const auto linear = [](double value) {
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double r = linear(color.redF()), g = linear(color.greenF()), b = linear(color.blueF());
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

double colorDistance(const ColorVector &a, const ColorVector &b) {
    double squared = 0;
    for (int channel = 0; channel < 3; ++channel) squared += std::pow(a[channel] - b[channel], 2);
    return squared;
}

struct ColorRegion {
    PathsD shape;
    QPainterPath path;
    ColorVector mean;
    ColorVector rgb;
    double area = 0;
    int source = 0;
    double coverage = 1;
};

int maximumCoverage(const QPainterPath &path, QRect rectangle) {
    QImage mask(rectangle.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-rectangle.topLeft());
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(path);
    }
    int alpha = 0;
    for (int y = 0; y < mask.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        for (int x = 0; x < mask.width(); ++x) {
            alpha = std::max(alpha, qAlpha(row[x]));
            if (alpha == 255) return alpha;
        }
    }
    return alpha;
}

#include "color_plan.inc"
#include "color_reconstruct.inc"
#include "color_merge.inc"

} // namespace

ColorResult abstractColors(const QVector<Item> &baseline, const Options &options,
                          const ColorOptions &colors, const Renderer &renderer) {
    ColorResult result;
    result.items = baseline;
    result.paletteItems = result.regionalItems = baseline;
    result.topologyCandidate = result.reconstructionCandidate = baseline;
    const auto started = std::chrono::steady_clock::now();
    const auto reference = renderer(objects(baseline));
    auto before = pathMetrics(objects(baseline), options.flattenTolerance);
    before.cost = geometryCost(before, options);
    before.costMeasured = true;
    result.report = {{"enabled", true}, {"status", "unchanged"}, {"color_space", "Oklab"},
        {"color_tolerance", colors.tolerance}, {"detail_scale_pixels", colors.detailScale},
        {"boundary_tolerance_pixels", colors.boundaryTolerance},
        {"render_rmse_limit_8bit", colors.renderRmse}, {"render_max_error_limit_8bit", colors.renderMaxError},
        {"local_rmse_limit_8bit", colors.localRmse}, {"local_window_pixels", colors.localWindow},
        {"contrast_retention", colors.contrastRetention}, {"component_deletion_enabled", false},
        {"reconstruction_enabled", colors.reconstruct},
        {"before", before.json()}};
    if (colors.tolerance == 0 || before.fills < 2) {
        result.report.insert("after", before.json());
        result.report.insert("image_error", imageError(reference, reference));
        return result;
    }

    PathsD domain;
    std::vector<ColorRegion> regions;
    const auto appendRegion = [&](PathsD part, int source) {
        const double weight = std::abs(Area(part));
        if (weight <= 1e-12) return;
        const auto color = baseline[source].color;
        regions.push_back({part, fromPolygons(part), perceptualColor(color),
            {double(color.red()), double(color.green()), double(color.blue())}, weight, source});
    };
    const auto cacheKey = QCryptographicHash::hash(svg(objects(baseline), reference.size())
        + QByteArray::number(options.flattenTolerance, 'g', 17) + "color-partition-2", QCryptographicHash::Sha256);
    bool cached = false;
    if (!colors.cachePath.isEmpty()) {
        QFile file(colors.cachePath);
        if (file.open(QIODevice::ReadOnly)) {
            QDataStream stream(&file);
            QByteArray key;
            QPainterPath outline;
            qint32 count = 0;
            stream >> key >> outline >> count;
            if (key == cacheKey && count > 0 && count < 0xffffff) {
                domain = polygons(outline, options.flattenTolerance);
                for (int region = 0; region < count; ++region) {
                    QPainterPath path;
                    qint32 source = -1;
                    stream >> source >> path;
                    if (source < 0 || source >= baseline.size()) break;
                    appendRegion(polygons(path, options.flattenTolerance), source);
                }
                cached = stream.status() == QDataStream::Ok && regions.size() == size_t(count);
            }
        }
    }
    if (!cached) { domain.clear(); regions.clear(); }
    for (int source = cached ? -1 : int(baseline.size()) - 1; source >= 0; --source) {
        const auto normalized = polygons(baseline[source].path, options.flattenTolerance);
        const auto visible = Difference(normalized, domain, FillRule::NonZero, kBooleanPrecision);
        for (auto &part : components(visible)) appendRegion(std::move(part), source);
        domain = Union(domain, normalized, FillRule::NonZero, kBooleanPrecision);
        if (options.progress && source % 512 == 0)
            std::fprintf(stderr, "colors: ownership %d/%lld, %zu visible regions\n",
                int(baseline.size()) - source, static_cast<long long>(baseline.size()), regions.size());
    }
    result.report.insert("partition_cache_hit", cached);
    if (!cached && !colors.cachePath.isEmpty()) {
        QSaveFile file(colors.cachePath);
        if (file.open(QIODevice::WriteOnly)) {
            QDataStream stream(&file);
            stream << cacheKey << fromPolygons(domain) << qint32(regions.size());
            for (const auto &region : regions) stream << qint32(region.source) << region.path;
            if (stream.status() == QDataStream::Ok) file.commit();
        }
    }
    if (regions.empty()) return result;
    if (regions.size() >= 0xffffff) throw std::runtime_error("Too many visible regions for the adjacency index");

    QImage ownership(reference.size(), QImage::Format_RGB32);
    ownership.fill(Qt::black);
    {
        QPainter painter(&ownership);
        painter.setPen(Qt::NoPen);
        for (size_t index = 0; index < regions.size(); ++index) {
            painter.setBrush(QColor::fromRgb(static_cast<QRgb>(index + 1)));
            painter.drawPath(regions[index].path);
        }
    }
    std::vector<std::map<int, double>> adjacency(regions.size());
    std::vector<int> pixels(regions.size());
    const auto connect = [&](int a, int b, double length) {
        if (a < 0 || b < 0 || a == b) return;
        adjacency[a][b] += length;
        adjacency[b][a] += length;
    };
    for (int y = 0; y < ownership.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(ownership.constScanLine(y));
        const auto *next = y + 1 < ownership.height()
            ? reinterpret_cast<const QRgb *>(ownership.constScanLine(y + 1)) : nullptr;
        for (int x = 0; x < ownership.width(); ++x) {
            const int owner = int(row[x] & 0xffffff) - 1;
            if (owner >= 0) ++pixels[owner];
            if (x + 1 < ownership.width()) connect(owner, int(row[x + 1] & 0xffffff) - 1, 1);
            if (next) connect(owner, int(next[x] & 0xffffff) - 1, 1);
        }
    }
    QVector<QPainterPath> paths;
    for (const auto &region : regions) paths.push_back(region.path);
    const OwnerIndex index(paths, fromPolygons(domain).boundingRect());
    for (int owner = 0; owner < int(regions.size()); ++owner) {
        if (pixels[owner] && !adjacency[owner].empty()) continue;
        QPainterPathStroker stroker;
        const double width = std::max(options.flattenTolerance * 4, 1e-5);
        stroker.setWidth(width);
        const auto contact = stroker.createStroke(regions[owner].path);
        const auto band = polygons(contact, options.flattenTolerance);
        for (const int neighbor : index.query(contact.boundingRect()))
            if (neighbor != owner && !adjacency[owner].contains(neighbor)
                && contact.intersects(regions[neighbor].path)) {
                const double overlap = std::abs(Area(Intersect(band, regions[neighbor].shape,
                    FillRule::NonZero, kBooleanPrecision)));
                connect(owner, neighbor, std::max(1.0, overlap / (width / 2)));
            }
    }
    result.report.insert("initial_visible_regions", int(regions.size()));
    result.report.insert("subpixel_regions", int(std::count(pixels.begin(), pixels.end(), 0)));
    int weighted = 0;
    for (auto &region : regions) {
        const auto box = region.path.boundingRect().adjusted(-1, -1, 1, 1).toAlignedRect().intersected(reference.rect());
        if (box.isEmpty()) continue;
        const auto probe = QRect(box.center() - QPoint(1, 1), QSize(3, 3)).intersected(box);
        int alpha = maximumCoverage(region.path, probe);
        if (alpha != 255) alpha = maximumCoverage(region.path, box);
        region.coverage = std::min(1.0, (alpha + 2.0) / 255);
        weighted += region.coverage < 1;
    }
    result.report.insert("coverage_weighted_regions", weighted);
    result.report.insert("coverage_weight_source", "Conservative native antialiased visible-region coverage; output fills stay opaque");
    return mergeColors(baseline, options, colors, renderer, regions, adjacency,
                       std::move(result), reference, before, started);
}
