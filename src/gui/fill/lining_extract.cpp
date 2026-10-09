#include "lining_extract.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

namespace gui {
namespace {

constexpr int kOpaqueAlpha = 128;
constexpr int kMinimumDefaultWidth = 3;
constexpr int kMinimumWidth = 1;
constexpr int kMaximumWidth = 24;
constexpr int kMaximumColors = 16;
constexpr int kColorIterations = 12;
constexpr qint64 kMaximumSourcePixels = 64LL * 1024LL * 1024LL;
constexpr int kJunctionMinimumNeighbors = 5;
constexpr double kStrongContrastMultiplier = 1.5;
constexpr double kPaletteMergeDistance = 24.0;
constexpr double kCoreColorQuantile = 0.3;
constexpr std::array<QPoint, 4> kNormals = {QPoint(1, 0), QPoint(0, 1), QPoint(1, 1), QPoint(1, -1)};

struct Component {
    std::vector<int> pixels;
    QRect bounds;
    QColor color;
};

bool stopped(const std::function<bool()> &cancelled) {

    return cancelled && cancelled();
}

int brightness(QRgb color) {

    return (54 * qRed(color) + 183 * qGreen(color) + 19 * qBlue(color)) / 256;
}

double colorDistance(QRgb a, QRgb b) {
    const int red = qRed(a) - qRed(b);
    const int green = qGreen(a) - qGreen(b);
    const int blue = qBlue(a) - qBlue(b);

    return red * red + green * green + blue * blue;
}

QColor coreColor(const Component &component, const QImage &image) {
    auto ordered = component.pixels;
    const auto sample = [&](int index) {
        return reinterpret_cast<const QRgb *>(image.constScanLine(index / image.width()))[index % image.width()];
    };
    const int count = std::max(1, static_cast<int>(ordered.size() * kCoreColorQuantile));
    std::nth_element(ordered.begin(), ordered.begin() + count - 1, ordered.end(),
        [&](int a, int b) { return brightness(sample(a)) < brightness(sample(b)); });
    qint64 red = 0;
    qint64 green = 0;
    qint64 blue = 0;
    for (int index = 0; index < count; ++index) {
        const QRgb color = sample(ordered[index]);
        red += qRed(color);
        green += qGreen(color);
        blue += qBlue(color);
    }

    return QColor(red / count, green / count, blue / count);
}

int nearestColor(const QColor &color, const QVector<QColor> &palette) {
    int best = 0;
    double distance = std::numeric_limits<double>::infinity();
    for (int index = 0; index < palette.size(); ++index) {
        const double trial = colorDistance(color.rgb(), palette[index].rgb());
        if (trial < distance) {
            best = index;
            distance = trial;
        }
    }

    return best;
}

QVector<QColor> linePalette(const std::vector<Component> &components, int maximumColors) {
    std::vector<int> order(components.size());
    QVector<QColor> palette;
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        if (components[a].pixels.size() != components[b].pixels.size())
            return components[a].pixels.size() > components[b].pixels.size();
        return a < b;
    });
    for (int index : order) {
        const auto color = components[index].color;
        if (palette.isEmpty() || colorDistance(color.rgb(), palette[nearestColor(color, palette)].rgb())
            > kPaletteMergeDistance * kPaletteMergeDistance) {
            palette.push_back(color);
            if (palette.size() == maximumColors)
                break;
        }
    }
    for (int iteration = 0; iteration < kColorIterations; ++iteration) {
        std::vector<std::array<qint64, 4>> sums(palette.size());
        for (const auto &component : components) {
            auto &sum = sums[nearestColor(component.color, palette)];
            const qint64 weight = component.pixels.size();
            sum[0] += component.color.red() * weight;
            sum[1] += component.color.green() * weight;
            sum[2] += component.color.blue() * weight;
            sum[3] += weight;
        }
        for (int index = 0; index < palette.size(); ++index)
            if (sums[index][3] > 0)
                palette[index] = QColor(sums[index][0] / sums[index][3],
                    sums[index][1] / sums[index][3], sums[index][2] / sums[index][3]);
    }

    return palette;
}

} // namespace

LiningExtractionResult extractLining(const QImage &source, const LiningExtractionOptions &options,
                                     const std::function<bool()> &cancelled,
                                     const std::function<void(const QString &, int, int)> &progress) {
    LiningExtractionResult result;
    QElapsedTimer timer;
    timer.start();
    if (source.isNull() || qint64(source.width()) * source.height() > kMaximumSourcePixels
        || !std::isfinite(options.maximumWidth) || options.maximumWidth < 0.0
        || !std::isfinite(options.minimumContrast) || options.minimumContrast <= 0.0
        || options.minimumContrast >= 255.0 || options.minimumPixels < 1
        || options.maximumColors < 1 || options.maximumColors > kMaximumColors) {
        result.regions.error = QStringLiteral("Lining detection requires an image, positive contrast and valid width, area and color limits");
        return result;
    }
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    const int width = image.width();
    const int height = image.height();
    const int count = width * height;
    const int maximumWidth = static_cast<int>(std::ceil(std::clamp(options.maximumWidth > 0.0
        ? options.maximumWidth : std::max(static_cast<double>(kMinimumDefaultWidth),
            std::min(width, height) * kDefaultLiningWidthFraction),
        static_cast<double>(kMinimumWidth), static_cast<double>(kMaximumWidth))));
    std::vector<int> luminance(count, -1);
    std::vector<std::uint8_t> candidate(count, 0);
    std::vector<std::uint8_t> strong(count, 0);
    bool hasTransparency = false;
    for (int y = 0; y < height; ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            if (qAlpha(row[x]) >= kOpaqueAlpha)
                luminance[y * width + x] = brightness(row[x]);
            else
                hasTransparency = true;
        }
    }
    for (int y = 0; y < height; ++y) {
        if (stopped(cancelled)) {
            result.cancelled = true;
            return result;
        }
        if (progress && y % 32 == 0)
            progress(QStringLiteral("Detecting thin strokes"), y, height);
        for (int x = 0; x < width; ++x) {
            const int index = y * width + x;
            const int center = luminance[index];
            if (center < 0 || (!hasTransparency && center + options.minimumContrast >= 255.0))
                continue;
            double evidence = 0.0;
            for (const QPoint &normal : kNormals) {
                const double normalLength = std::hypot(normal.x(), normal.y());
                int leftDistance = 0;
                int rightDistance = 0;
                for (int radius = 1; radius <= maximumWidth / normalLength; ++radius) {
                    const QPoint first(x + normal.x() * radius, y + normal.y() * radius);
                    const QPoint second(x - normal.x() * radius, y - normal.y() * radius);
                    const int left = first.x() < 0 || first.y() < 0 || first.x() >= width || first.y() >= height
                        ? -1 : luminance[first.y() * width + first.x()];
                    const int right = second.x() < 0 || second.y() < 0 || second.x() >= width || second.y() >= height
                        ? -1 : luminance[second.y() * width + second.x()];
                    const int leftContrast = left < 0 ? 255 : left - center;
                    const int rightContrast = right < 0 ? 255 : right - center;
                    if (!leftDistance && leftContrast >= options.minimumContrast)
                        leftDistance = radius;
                    if (!rightDistance && rightContrast >= options.minimumContrast)
                        rightDistance = radius;
                    if (leftDistance && rightDistance
                        && (leftDistance + rightDistance - 1) * normalLength <= maximumWidth)
                        evidence = std::max(evidence, static_cast<double>(std::min(leftContrast, rightContrast)));
                }
            }
            candidate[index] = evidence >= options.minimumContrast;
            strong[index] = evidence >= options.minimumContrast * kStrongContrastMultiplier;
        }
    }
    auto joined = candidate;
    for (int y = 1; y + 1 < height; ++y)
        for (int x = 1; x + 1 < width; ++x) {
            const int index = y * width + x;
            if (candidate[index] || luminance[index] < 0)
                continue;
            int neighbors = 0;
            int neighborBrightness = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int neighbor = (y + dy) * width + x + dx;
                    if (candidate[neighbor]) {
                        ++neighbors;
                        neighborBrightness += luminance[neighbor];
                    }
                }
            if (neighbors >= kJunctionMinimumNeighbors
                && luminance[index] <= neighborBrightness / neighbors + options.minimumContrast)
                joined[index] = 1;
        }
    candidate = std::move(joined);
    const qint64 detectionMilliseconds = timer.elapsed();
    std::vector<Component> components;
    std::vector<int> stack;
    std::vector<std::uint8_t> visited(count, 0);
    auto raster = QSharedPointer<RegionRasterData>::create();
    raster->labels.assign(count, -1);
    raster->foreground.assign(count, 0);
    raster->lineart.assign(count, 0);
    int removedPixels = 0;
    for (int seed = 0; seed < count; ++seed) {
        if (!candidate[seed] || visited[seed])
            continue;
        if (stopped(cancelled)) {
            result.cancelled = true;
            return result;
        }
        Component component;
        bool supported = false;
        int minX = width;
        int minY = height;
        int maxX = 0;
        int maxY = 0;
        stack = {seed};
        visited[seed] = 1;
        while (!stack.empty()) {
            const int index = stack.back();
            const int x = index % width;
            const int y = index / width;
            stack.pop_back();
            component.pixels.push_back(index);
            supported = supported || strong[index];
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                        continue;
                    const int neighbor = ny * width + nx;
                    if (candidate[neighbor] && !visited[neighbor]) {
                        stack.push_back(neighbor);
                        visited[neighbor] = 1;
                    }
                }
        }
        if (!supported || component.pixels.size() < static_cast<size_t>(options.minimumPixels)) {
            removedPixels += component.pixels.size();
            continue;
        }
        component.bounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY));
        component.color = coreColor(component, image);
        components.push_back(std::move(component));
    }
    result.regions.imageSize = image.size();
    result.pixels = QImage(image.size(), QImage::Format_ARGB32);
    result.pixels.fill(Qt::transparent);
    const auto palette = linePalette(components, options.maximumColors);
    RegionExtractionParams traceOptions;
    traceOptions.traceSpeckle = 0;
    std::vector<std::uint8_t> mask(count, 0);
    int selectedPixels = 0;
    for (int index = 0; index < static_cast<int>(components.size()); ++index) {
        if (stopped(cancelled)) {
            result.cancelled = true;
            result.regions.regions.clear();
            return result;
        }
        if (progress)
            progress(QStringLiteral("Tracing lining contours"), index, components.size());
        const auto &component = components[index];
        const auto color = palette[nearestColor(component.color, palette)];
        for (int pixel : component.pixels) {
            mask[pixel] = 1;
            raster->labels[pixel] = index;
            raster->lineart[pixel] = 1;
            reinterpret_cast<QRgb *>(result.pixels.scanLine(pixel / width))[pixel % width] = color.rgba();
        }
        ExtractedRegion region;
        region.id = index;
        region.color = color;
        region.lineart = true;
        region.bounds = component.bounds;
        region.area = component.pixels.size();
        region.outline = traceMaskToPath(mask, width, height, component.bounds, traceOptions);
        selectedPixels += region.area;
        for (int pixel : component.pixels)
            mask[pixel] = 0;
        result.regions.regions.push_back(std::move(region));
    }
    for (int index = 0; index < count; ++index)
        raster->foreground[index] = luminance[index] >= 0;
    raster->traceParams = traceOptions;
    result.regions.raster = raster;
    result.regions.lineartRegionCount = components.size();
    result.diagnostics = QJsonObject{{QStringLiteral("maximumWidthPixels"), maximumWidth},
        {QStringLiteral("minimumContrast"), options.minimumContrast},
        {QStringLiteral("selectedPixels"), selectedPixels}, {QStringLiteral("removedSpecklePixels"), removedPixels},
        {QStringLiteral("components"), static_cast<int>(components.size())},
        {QStringLiteral("colors"), palette.size()}, {QStringLiteral("detectionMilliseconds"), detectionMilliseconds},
        {QStringLiteral("elapsedMilliseconds"), timer.elapsed()}};

    return result;
}

} // namespace gui
