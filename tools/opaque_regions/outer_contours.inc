gui::catalog::Polygons contourRings(const QJsonArray &rings) {
    gui::catalog::Polygons result;
    for (const auto &ring : rings) {
        QPolygonF polygon;
        for (const auto &entry : ring.toArray()) {
            const auto point = entry.toArray();
            if (point.size() != 2 || !point[0].isDouble() || !point[1].isDouble()
                || !std::isfinite(point[0].toDouble()) || !std::isfinite(point[1].toDouble()))
                throw std::runtime_error("Invalid contour coordinate");
            polygon.push_back(QPointF(point[0].toDouble(), point[1].toDouble()));
        }
        if (polygon.size() > 1 && polygon.front() == polygon.back()) polygon.removeLast();
        if (polygon.size() < 3) throw std::runtime_error("A contour needs three vertices");
        result.push_back(polygon);
    }
    return gui::catalog::unite(result);
}

QJsonArray contourRingsJson(const gui::catalog::Polygons &rings) {
    QJsonArray result;
    for (const auto &ring : rings) {
        QJsonArray points;
        for (const auto &point : ring) points.append(QJsonArray{point.x(), point.y()});
        result.append(points);
    }
    return result;
}

double outerContourCost(const gui::catalog::Polygons &rings) {
    double cost = 0;
    for (const auto &ring : rings) {
        const bool hole = gui::catalog::signedArea(ring) < 0;
        cost += ring.size() + (hole ? 256 : 32);
        const double orientation = hole ? -1 : 1;
        for (int index = 0; index < ring.size(); ++index) {
            const auto incoming = ring[index] - ring[(index + ring.size() - 1) % ring.size()];
            const auto outgoing = ring[(index + 1) % ring.size()] - ring[index];
            if (orientation * (incoming.x() * outgoing.y() - incoming.y() * outgoing.x()) < -1e-6)
                cost += 2;
        }
    }
    return cost;
}

int simplifyOuterContours(const QCommandLineParser &parser) {
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || !parser.isSet("output"))
        throw std::runtime_error("Provide a contour constraint manifest and output directory");
    QJsonParseError parseError;
    const auto manifest = QJsonDocument::fromJson(read(arguments.front()), &parseError).object();
    if (parseError.error != QJsonParseError::NoError || manifest.value("version").toInt() != 1
        || !manifest.value("regions").isArray())
        throw std::runtime_error("Invalid contour constraint manifest");
    QDir output(parser.value("output"));
    if (!QDir().mkpath(output.absolutePath())) throw std::runtime_error("Cannot create contour output directory");
    for (const auto &name : {"outer_contours.json", "outer_progress.json"})
        if (QFileInfo(output.filePath(name)).absoluteFilePath().compare(
                QFileInfo(arguments.front()).absoluteFilePath(), Qt::CaseInsensitive) == 0)
            throw std::runtime_error("Contour artifacts would overwrite the input");
    const double tolerance = number(parser, "area-tolerance");
    const auto regions = manifest.value("regions").toArray();
    QJsonArray results;
    for (const auto &entry : regions) {
        const auto region = entry.toObject();
        const auto support = contourRings(region.value("support").toArray());
        const auto required = contourRings(region.value("required").toArray());
        const auto allowed = contourRings(region.value("allowed").toArray());
        const auto freePath = gui::catalog::painterPath(gui::catalog::expanded(
            gui::catalog::subtract(allowed, required), 1e-6));
        int checks = 0;
        const auto valid = [&](const gui::catalog::Polygons &candidate) {
            ++checks;
            return gui::catalog::area(gui::catalog::subtract(required, candidate)) <= tolerance
                && gui::catalog::area(gui::catalog::subtract(candidate, allowed)) <= tolerance;
        };
        QVector<gui::catalog::Polygons> seeds{support};
        for (const auto &seed : region.value("seeds").toArray())
            seeds.push_back(contourRings(seed.toArray()));
        QVector<gui::catalog::Polygons> candidates;
        const auto consider = [&](const gui::catalog::Polygons &candidate) {
            const auto normalized = gui::catalog::unite(candidate);
            if (valid(normalized)) candidates.push_back(normalized);
        };
        if (required.isEmpty()) consider({});
        for (const auto &seed : seeds) {
            consider(seed);
            gui::catalog::Polygons filled;
            for (const auto &ring : seed)
                if (gui::catalog::signedArea(ring) > 0) filled.push_back(ring);
            consider(filled);
            if (!valid(seed)) continue;
            auto current = seed;
            for (int ringIndex = 0; ringIndex < current.size(); ++ringIndex) {
                const auto original = current[ringIndex];
                for (double epsilon : {0.0, .25, .5, 1., 2., 4., 8., 16., 32.}) {
                    if (ringIndex >= current.size()) break;
                    const auto ring = gui::simplifyClosedPolygonCorridor(original, epsilon,
                        [&](const QPointF &first, const QPointF &last) {
                            QPainterPath chord;
                            chord.moveTo(first);
                            chord.lineTo(last);
                            return freePath.contains(chord);
                        });
                    auto trial = current;
                    trial[ringIndex] = ring;
                    trial = gui::catalog::unite(trial);
                    if (outerContourCost(trial) < outerContourCost(current) && valid(trial)) current = trial;
                }
            }
            candidates.push_back(current);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
            const double first = outerContourCost(left), last = outerContourCost(right);
            return first < last || (first == last && gui::catalog::area(left) < gui::catalog::area(right));
        });
        if (!candidates.isEmpty()) {
            auto current = candidates.front();
            for (int span : {64, 32, 16, 8, 4, 2, 1}) {
                for (int ringIndex = 0; ringIndex < current.size() && checks < 6000; ++ringIndex) {
                    int start = 0;
                    while (start < current[ringIndex].size() && checks < 6000) {
                        const auto ring = current[ringIndex];
                        const int removed = std::min(span, int(ring.size()) - 3);
                        if (removed < 1) break;
                        QPolygonF shortened;
                        for (int offset = 0; offset < ring.size(); ++offset)
                            if (offset == 0 || offset > removed)
                                shortened.push_back(ring[(start + offset) % ring.size()]);
                        auto trial = current;
                        trial[ringIndex] = shortened;
                        trial = gui::catalog::unite(trial);
                        if (trial.size() == current.size() && outerContourCost(trial) < outerContourCost(current)
                            && valid(trial)) {
                            current = trial;
                            start = 0;
                        } else ++start;
                    }
                }
            }
            candidates.prepend(current);
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
            const double first = outerContourCost(left), last = outerContourCost(right);
            return first < last || (first == last && gui::catalog::area(left) < gui::catalog::area(right));
        });
        QSet<QByteArray> seen;
        QJsonArray selected;
        for (const auto &candidate : candidates) {
            const auto rings = contourRingsJson(candidate);
            const auto key = QJsonDocument(rings).toJson(QJsonDocument::Compact);
            if (seen.contains(key)) continue;
            seen.insert(key);
            selected.append(QJsonObject{{"rings", rings}, {"cost", outerContourCost(candidate)}});
            if (selected.size() == 4) break;
        }
        results.append(QJsonObject{{"key", region.value("key")}, {"candidates", selected}, {"geometry_checks", checks}});
        tryWrite(output.filePath("outer_progress.json"), QJsonDocument(QJsonObject{
            {"completed_regions", results.size()}, {"total_regions", regions.size()}}).toJson());
    }
    write(output.filePath("outer_contours.json"), QJsonDocument(QJsonObject{
        {"method", "Bucket corridor simplification with filled-geometry constraints"},
        {"regions", results}, {"area_tolerance", tolerance}}).toJson());
    return 0;
}
