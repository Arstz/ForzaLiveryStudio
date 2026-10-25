struct RasterManifest {
    QByteArray svg;
    QJsonObject audit;
    QStringList masks;
};

RasterManifest traceRasterManifest(const QString &filename, const QByteArray &data, QSize size,
                                   double tolerance, double cornerThreshold) {
    QElapsedTimer clock;
    clock.start();
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    const auto root = document.object();
    if (error.error != QJsonParseError::NoError || root.value("version").toInt() != 1
        || root.value("width").toInt() != size.width() || root.value("height").toInt() != size.height()
        || root.value("fill_opacity").toDouble(-1) != 1 || root.value("items").toArray().isEmpty())
        throw std::runtime_error("Invalid native-resolution opaque raster manifest");
    RasterManifest result;
    QVector<gui::SvgVectorObject> objects;
    QJsonArray masks;
    const QDir directory = QFileInfo(filename).absoluteDir();
    gui::RegionExtractionParams parameters;
    parameters.traceSpeckle = 0;
    parameters.traceOptTolerance = tolerance;
    parameters.traceAlphaMax = cornerThreshold;
    for (const auto &value : root.value("items").toArray()) {
        const auto item = value.toObject();
        const QColor color(item.value("fill").toString());
        const QString maskPath = directory.absoluteFilePath(item.value("mask").toString());
        const QImage mask = load(maskPath).convertToFormat(QImage::Format_ARGB32);
        if (!color.isValid() || color.alpha() != 255 || mask.size() != size)
            throw std::runtime_error("Raster masks require solid opaque colors and matching native dimensions");
        std::vector<std::uint8_t> pixels(static_cast<size_t>(size.width()) * size.height());
        QRect bounds;
        qint64 count = 0;
        for (int y = 0; y < mask.height(); ++y) {
            const auto row = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
            for (int x = 0; x < mask.width(); ++x) {
                if (row[x] != qRgb(0, 0, 0) && row[x] != qRgb(255, 255, 255))
                    throw std::runtime_error("Raster trace masks must be binary and fully opaque");
                if (qRed(row[x]) == 0) continue;
                pixels[static_cast<size_t>(y) * mask.width() + x] = 1;
                bounds |= QRect(x, y, 1, 1);
                ++count;
            }
        }
        if (count == 0) throw std::runtime_error("Raster manifest contains an empty mask");
        QPainterPath path;
        if (count == qint64(bounds.width()) * bounds.height()) path.addRect(QRectF(bounds));
        else path = gui::traceMaskToPath(pixels, size.width(), size.height(), bounds, parameters);
        if (path.isEmpty()) throw std::runtime_error("Raster contour tracing failed");
        objects.push_back({path, color});
        result.masks.push_back(maskPath);
        masks.append(QJsonObject{{"file", maskPath}, {"fill", color.name()}, {"pixels", double(count)},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(read(maskPath), QCryptographicHash::Sha256).toHex())}});
    }
    result.svg = opaque::svg(objects, size);
    result.audit = {{"method", "Vendored Potrace on binary native-resolution paint masks"},
        {"trace_tolerance_pixels", tolerance}, {"corner_threshold", cornerThreshold},
        {"speckle_deletion", false}, {"resized", false},
        {"mask_count", objects.size()}, {"masks", masks}, {"runtime_seconds", clock.elapsed() / 1000.0}};
    return result;
}
