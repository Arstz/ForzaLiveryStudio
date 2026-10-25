namespace {

ColorVector compositeColor(QRgb pixel) {
    const double alpha = qAlpha(pixel) / 255.0;
    return {qRed(pixel) * alpha + 255 * (1 - alpha),
        qGreen(pixel) * alpha + 255 * (1 - alpha), qBlue(pixel) * alpha + 255 * (1 - alpha)};
}

QVector<gui::SvgVectorObject> absorptionOwnership(const QVector<Item> &items,
                                                const QVector<QPainterPath> &ownership, double tolerance) {
    std::map<QRgb, PathsD> colors;
    for (const auto &item : items) for (const int owner : item.owners) {
        const auto shape = polygons(ownership[owner], tolerance);
        auto &combined = colors[item.color.rgb()];
        combined.insert(combined.end(), shape.begin(), shape.end());
    }
    QVector<gui::SvgVectorObject> result;
    for (const auto &[color, shape] : colors) {
        const auto combined = Union(shape, FillRule::NonZero, kBooleanPrecision);
        if (!combined.empty()) result.push_back({fromPolygons(combined), QColor(color)});
    }
    return result;
}

} // namespace

QJsonObject absorptionRegionMetrics(const QVector<gui::SvgVectorObject> &regions, double tolerance) {
    const auto metrics = pathMetrics(regions, tolerance);
    const double thresholds[]{1, 4, 16, 64};
    int counts[4]{};
    double areas[4]{}, total = 0;
    for (const auto &region : regions) for (const auto &part : components(polygons(region.path, tolerance))) {
        const double value = std::abs(Area(part));
        total += value;
        for (int index = 0; index < 4; ++index) if (value < thresholds[index]) {
            ++counts[index];
            areas[index] += value;
        }
    }
    QJsonArray histogram;
    for (int index = 0; index < 4; ++index) histogram.append(QJsonObject{{"below_area_pixels", thresholds[index]},
        {"components", counts[index]}, {"area_pixels", areas[index]}});
    return {{"components", metrics.components}, {"holes", metrics.holes}, {"colors", metrics.fills},
        {"total_visible_area_pixels", total}, {"small_region_histogram", histogram}};
}

QJsonObject absorptionDetailError(const QImage &target, const QImage &baseline, const QImage &actual,
                                 const ColorOptions &limits, double edgeContrast) {
    if (target.size() != baseline.size() || target.size() != actual.size() || target.isNull())
        throw std::runtime_error("Absorption detail images must have matching native dimensions");
    const auto source = target.convertToFormat(QImage::Format_ARGB32);
    const auto before = baseline.convertToFormat(QImage::Format_ARGB32);
    const auto after = actual.convertToFormat(QImage::Format_ARGB32);
    const int stride = source.width() + 1;
    std::vector<double> integral(stride * (source.height() + 1));
    int protectedEdges = 0, lostEdges = 0;
    double minimumRetention = 1, maximumLocal = 0;
    const auto edge = [&](QRgb sourceA, QRgb sourceB, QRgb beforeA, QRgb beforeB, QRgb afterA, QRgb afterB) {
        if (beforeA == afterA && beforeB == afterB) return;
        const auto a = compositeColor(sourceA), b = compositeColor(sourceB);
        const auto c = compositeColor(beforeA), d = compositeColor(beforeB);
        const auto e = compositeColor(afterA), f = compositeColor(afterB);
        double strength = 0, oldProjection = 0, newProjection = 0;
        for (int channel = 0; channel < 3; ++channel) {
            const double direction = b[channel] - a[channel];
            strength += direction * direction;
            oldProjection += (d[channel] - c[channel]) * direction;
            newProjection += (f[channel] - e[channel]) * direction;
        }
        if (strength < 3 * edgeContrast * edgeContrast || strength <= 1e-12) return;
        const double length = std::sqrt(strength);
        oldProjection /= length;
        newProjection /= length;
        if (oldProjection < edgeContrast * .5) return;
        ++protectedEdges;
        const double required = std::min(oldProjection, length);
        const double fraction = (newProjection + 2 * std::sqrt(3.0)) / required;
        minimumRetention = std::min(minimumRetention, fraction);
        lostEdges += fraction + 1e-9 < limits.contrastRetention;
    };
    for (int y = 0; y < source.height(); ++y) {
        const auto *a = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        const auto *b = reinterpret_cast<const QRgb *>(before.constScanLine(y));
        const auto *c = reinterpret_cast<const QRgb *>(after.constScanLine(y));
        const auto *nextA = y + 1 < source.height() ? reinterpret_cast<const QRgb *>(source.constScanLine(y + 1)) : nullptr;
        const auto *nextB = y + 1 < source.height() ? reinterpret_cast<const QRgb *>(before.constScanLine(y + 1)) : nullptr;
        const auto *nextC = y + 1 < source.height() ? reinterpret_cast<const QRgb *>(after.constScanLine(y + 1)) : nullptr;
        double row = 0;
        for (int x = 0; x < source.width(); ++x) {
            double added = 0;
            if (b[x] != c[x]) {
                const auto targetRgb = compositeColor(a[x]), oldRgb = compositeColor(b[x]), newRgb = compositeColor(c[x]);
                for (int channel = 0; channel < 3; ++channel)
                    added += std::pow(targetRgb[channel] - newRgb[channel], 2)
                        - std::pow(targetRgb[channel] - oldRgb[channel], 2);
            }
            row += std::max(0.0, added);
            integral[(y + 1) * stride + x + 1] = integral[y * stride + x + 1] + row;
            if (x + 1 < source.width()) edge(a[x], a[x + 1], b[x], b[x + 1], c[x], c[x + 1]);
            if (nextA) edge(a[x], nextA[x], b[x], nextB[x], c[x], nextC[x]);
        }
    }
    const int width = std::min(limits.localWindow, source.width());
    const int height = std::min(limits.localWindow, source.height());
    for (int y = height; y <= source.height(); ++y) for (int x = width; x <= source.width(); ++x) {
        const double added = integral[y * stride + x] - integral[(y - height) * stride + x]
            - integral[y * stride + x - width] + integral[(y - height) * stride + x - width];
        maximumLocal = std::max(maximumLocal, added / (width * height * 3.0));
    }
    maximumLocal = std::sqrt(maximumLocal);
    return {{"protected_changed_source_edges", protectedEdges}, {"lost_source_edges", lostEdges},
        {"minimum_source_edge_retention", minimumRetention}, {"maximum_added_local_rmse", maximumLocal},
        {"local_window_pixels", limits.localWindow}, {"source_edge_contrast", edgeContrast},
        {"passed", lostEdges == 0 && maximumLocal <= limits.localRmse + 1e-9}};
}

void Optimizer::setColorAbsorption(QImage target, ColorOptions limits, double sourceGrowth, double edgeContrast) {
    absorptionReference_ = std::move(target);
    absorptionLimits_ = limits;
    absorptionSourceGrowth_ = sourceGrowth;
    absorptionEdgeContrast_ = edgeContrast;
    absorptionEnabled_ = true;
}
