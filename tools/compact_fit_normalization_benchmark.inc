void verifyNormalizationPlacements(const QString &path,
                                   const QVector<gui::catalog::Primitive> &primitives) {
    const auto request = readRequest(path);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), file.errorString());
    const auto document = QJsonDocument::fromJson(file.readAll()).object();
    gui::catalog::FillResult result;
    for (const auto &entry : document.value("oraclePlacements").toArray()) {
        const auto object = entry.toObject();
        const auto coefficients = object.value("transform").toArray();
        require(coefficients.size() == 6, QStringLiteral("Expected six affine coefficients"));
        gui::PenPlacement placement;
        placement.shapeId = object.value("shapeId").toInt();
        placement.transform = gui::catalog::emittedTransform(QTransform(
            coefficients[0].toDouble(), coefficients[1].toDouble(), coefficients[2].toDouble(),
            coefficients[3].toDouble(), coefficients[4].toDouble(), coefficients[5].toDouble()));
        result.fill.placements.push_back(placement);
    }
    require(!result.fill.placements.isEmpty(), QStringLiteral("Expected oracle placements"));
    QElapsedTimer timer;
    timer.start();
    const bool verified = gui::compact::verifyEmittedFill(request, primitives, {}, &result);
    const QJsonObject output{{"verified", verified}, {"count", result.fill.placements.size()},
        {"total_ms", timer.elapsed()}, {"error", result.fill.error}, {"diagnostics", result.diagnostics}};
    QTextStream(stdout) << QJsonDocument(output).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
}

void normalizationBenchmark(const QStringList &arguments,
                            const QVector<gui::catalog::Primitive> &primitives) {
    require(arguments.size() >= 6, QStringLiteral("Expected request, target width, target height and evaluation budget"));
    const auto request = readRequest(arguments[2]);
    const auto region = gui::catalog::buildRegion(request, {});
    const bool hardFrame = arguments[3].startsWith(QStringLiteral("hardaffine"));
    const bool affineFrame = hardFrame || arguments[3].startsWith(QStringLiteral("affine"));
    const bool uniformFrame = arguments[3].startsWith(QStringLiteral("uniform"));
    const double width = affineFrame ? arguments[3].mid(hardFrame ? 10 : 6).toDouble()
        : uniformFrame ? arguments[3].mid(7).toDouble() : arguments[3].toDouble();
    const double height = arguments[4].toDouble();
    const int budget = arguments[5].toInt();
    const int finishBudget = arguments.size() > 6 ? arguments[6].toInt() : 0;
    require(budget > 0 && width >= 0 && height >= 0, QStringLiteral("Invalid normalization trial settings"));
    const double horizontal = width > 0 ? width / (uniformFrame
        ? std::max(region.bounds.width(), region.bounds.height()) : region.bounds.width()) : 1.0;
    const double vertical = !uniformFrame && height > 0 ? height / region.bounds.height() : horizontal;
    QTransform normalize(horizontal, 0, 0, vertical,
        -region.bounds.center().x() * horizontal, -region.bounds.center().y() * vertical);
    if (affineFrame) {
        QVector<QPointF> controls;
        if (hardFrame)
            for (const auto &point : request.loops.front().points)
                if (point.kind == gui::PenPointKind::Hard)
                    controls.push_back(point.position);
        if (controls.size() < 3) {
            controls.clear();
            for (const auto &segment : gui::penSegments(request.loops.front().points)) {
                controls.push_back(segment.start);
                if (segment.curved) {
                    controls.push_back(segment.control);
                    controls.push_back(segment.control2);
                }
            }
        }
        std::array<QPointF, 3> anchors;
        double largest = 0;
        for (int first = 0; first < controls.size(); ++first)
            for (int second = first + 1; second < controls.size(); ++second)
                for (int third = second + 1; third < controls.size(); ++third) {
                    const auto a = controls[second] - controls[first];
                    const auto b = controls[third] - controls[first];
                    const double area = std::abs(a.x() * b.y() - a.y() * b.x());
                    if (area > largest * (1 + 1e-8)) {
                        largest = area;
                        anchors = {controls[first], controls[second], controls[third]};
                    }
                }
        require(largest > 0, QStringLiteral("Cannot construct an affine fitting frame"));
        normalize = gui::catalog::affineFromAnchors(anchors,
            {QPointF(-width / 2, -width / 2), QPointF(width / 2, -width / 2), QPointF(-width / 2, width / 2)});
    }
    const QTransform restore = normalize.inverted();
    auto working = request;
    for (auto &loop : working.loops) {
        gui::materializePenHandles(loop.points);
        for (auto &point : loop.points)
            gui::transformPenPoint(point, normalize);
    }
    working.boundaryTolerance = 0.1;
    gui::compact::FillOptions options;
    options.evaluationBudget = budget;
    options.useGpu = !arguments.contains(QStringLiteral("cpu"));
    options.adaptiveBudget = !arguments.contains(QStringLiteral("legacy"));
    options.retainFailedFill = true;
    QElapsedTimer timer;
    timer.start();
    auto fitted = gui::profile::fillRegion(working, primitives, options);
    const qint64 searchMilliseconds = timer.elapsed();
    for (auto &placement : fitted.fill.placements) {
        placement.transform = gui::catalog::emittedTransform(placement.transform * restore);
        placement.area /= std::abs(normalize.determinant());
    }
    const QString searchError = fitted.fill.error;
    gui::compact::FillOptions originalOptions;
    bool verified = !fitted.fill.placements.isEmpty()
        && gui::compact::verifyEmittedFill(request, primitives, originalOptions, &fitted);
    const qint64 validationMilliseconds = timer.elapsed() - searchMilliseconds;
    const int beforeFinish = fitted.fill.placements.size();
    qint64 finishMilliseconds = 0;
    if ((!verified || arguments.contains(QStringLiteral("always"))) && finishBudget > 0 && !fitted.fill.placements.isEmpty()) {
        const auto checkpoint = fitted;
        const bool checkpointVerified = verified;
        originalOptions.evaluationBudget = finishBudget;
        originalOptions.initialPlacements = fitted.fill.placements;
        if (arguments.size() > 7 && arguments[7] == QStringLiteral("fixed"))
            originalOptions.shapeBudget = fitted.fill.placements.size();
        originalOptions.retainFailedFill = true;
        const qint64 start = timer.elapsed();
        fitted = gui::compact::fillRegion(request, primitives, originalOptions);
        finishMilliseconds = timer.elapsed() - start;
        verified = fitted.fill.error.isEmpty() && fitted.diagnostics.value("approximationVerified").toBool();
        if (checkpointVerified && (!verified || fitted.fill.placements.size() >= checkpoint.fill.placements.size())) {
            fitted = checkpoint;
            verified = true;
        }
    }
    QJsonObject counts;
    QJsonArray placements;
    for (const auto &placement : fitted.fill.placements) {
        const auto key = QString::number(placement.shapeId);
        counts.insert(key, counts.value(key).toInt() + 1);
        const auto &transform = placement.transform;
        placements.push_back(QJsonObject{{"shapeId", placement.shapeId},
            {"transform", QJsonArray{transform.m11(), transform.m12(), transform.m21(), transform.m22(), transform.dx(), transform.dy()}}});
    }
    const auto seed = fitted.diagnostics.value("profileSeed").toObject();
    const QJsonObject result{{"verified", verified}, {"count", fitted.fill.placements.size()},
        {"before_finish", beforeFinish}, {"shape_counts", counts}, {"placements", placements},
        {"budget", budget}, {"finish_budget", finishBudget}, {"evaluations", fitted.diagnostics.value("evaluations")},
        {"search_ms", searchMilliseconds}, {"validation_ms", validationMilliseconds},
        {"finish_ms", finishMilliseconds}, {"total_ms", timer.elapsed()}, {"search_error", searchError},
        {"error", fitted.fill.error}, {"source_width", region.bounds.width()}, {"source_height", region.bounds.height()},
        {"target_width", normalize.mapRect(region.bounds).width()}, {"target_height", normalize.mapRect(region.bounds).height()},
        {"normalization", arguments[3]}, {"axis_scale_x", horizontal}, {"axis_scale_y", vertical},
        {"boundary", fitted.diagnostics.value("boundaryQuality")},
        {"missing_interior", fitted.diagnostics.value("missingInteriorArea")},
        {"outside_envelope", fitted.diagnostics.value("outsideEnvelope")},
        {"area_error", fitted.diagnostics.value("areaError")},
        {"seed_count", seed.value("count")}, {"stages", fitted.diagnostics.value("stageMilliseconds")}};
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
}
