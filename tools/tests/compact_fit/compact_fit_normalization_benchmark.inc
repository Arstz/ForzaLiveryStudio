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

void exportTransformationControls(const QString &directory, const QVector<gui::catalog::Primitive> &primitives) {
    QDir().mkpath(directory);
    const auto shape = [&](int id) -> const gui::PenPrimitive & {
        const auto found = std::find_if(primitives.begin(), primitives.end(), [&](const auto &candidate) {
            return candidate.shape.shapeId == id;
        });
        require(found != primitives.end(), QStringLiteral("Missing study primitive"));
        return found->shape;
    };
    const auto rectangle = [&](double x, double y, double width, double height) {
        const auto bounds = shape(101).bounds;
        gui::PenPlacement result;
        result.shapeId = 101;
        result.transform = gui::catalog::emittedTransform(QTransform(width / bounds.width(), 0, 0,
            height / bounds.height(), x - bounds.left() * width / bounds.width(), y - bounds.top() * height / bounds.height()));
        return result;
    };
    const auto circle = [&](double x, double y, double horizontal, double vertical) {
        gui::PenPlacement result;
        result.shapeId = 102;
        result.transform = gui::catalog::emittedTransform(QTransform(horizontal, 0, 0, vertical, x, y));
        return result;
    };
    const QMap<QString, QVector<gui::PenPlacement>> families{
        {"native_circle", {circle(0, 0, 1, 1)}},
        {"native_rectangle", {rectangle(-80, -40, 160, 80)}},
        {"native_circle_square", {circle(-30, 10, 1.2, 1.2), rectangle(0, -54, 128, 128)}},
        {"native_ellipse_square", {circle(-30, 10, 1.4, 0.7), rectangle(0, -54, 128, 128)}},
        {"native_circle_pair", {circle(-35, 0, 1, 1), circle(35, 0, 1, 1)}},
        {"native_L", {rectangle(0, 0, 128, 32), rectangle(0, 32, 32, 96)}},
        {"native_thin_elbow", {rectangle(0, 0, 192, 4), rectangle(0, 4, 4, 124)}},
        {"native_frame", {rectangle(0, 0, 128, 24), rectangle(0, 104, 128, 24),
            rectangle(0, 24, 24, 80), rectangle(104, 24, 24, 80)}}};
    for (auto iterator = families.cbegin(); iterator != families.cend(); ++iterator) {
        gui::catalog::Polygons polygons;
        QJsonArray oracle;
        for (const auto &placement : iterator.value()) {
            polygons += gui::catalog::mapped(shape(placement.shapeId), placement.transform);
            const auto &transform = placement.transform;
            oracle.push_back(QJsonObject{{"shapeId", placement.shapeId}, {"transform", QJsonArray{
                transform.m11(), transform.m12(), transform.m21(), transform.m22(), transform.dx(), transform.dy()}}});
        }
        QJsonArray loops;
        for (const auto &polygon : gui::catalog::unite(polygons)) {
            QJsonArray points;
            for (const auto &point : polygon)
                points.push_back(QJsonObject{{"position", QJsonArray{point.x(), point.y()}}, {"kind", "hard"},
                    {"explicitHandles", true}, {"incoming", QJsonArray{0, 0}}, {"outgoing", QJsonArray{0, 0}}});
            loops.push_back(QJsonObject{{"kind", gui::catalog::signedArea(polygon) > 0 ? "outer" : "cutout"}, {"points", points}});
        }
        QFile file(QDir(directory).filePath(iterator.key() + QStringLiteral(".json")));
        require(file.open(QIODevice::WriteOnly), file.errorString());
        file.write(QJsonDocument(QJsonObject{{"request", QJsonObject{{"curveModel", "cubic-anchors-v1"},
            {"boundaryTolerance", 0.1}, {"loops", loops}}}, {"oraclePlacements", oracle}}).toJson());
    }
}

void normalizationBenchmark(const QStringList &arguments,
                            const QVector<gui::catalog::Primitive> &primitives) {
    require(arguments.size() >= 6, QStringLiteral("Expected request, target width, target height and evaluation budget"));
    const auto request = readRequest(arguments[2]);
    QFile sourceFile(arguments[2]);
    require(sourceFile.open(QIODevice::ReadOnly), sourceFile.errorString());
    const auto sourceDocument = QJsonDocument::fromJson(sourceFile.readAll()).object();
    const auto benchmarkOptions = sourceDocument.value("benchmarkOptions").toObject();
    const auto originalSettings = sourceDocument.value("originalOptions").toObject();
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
    if (arguments[3].startsWith(QStringLiteral("matrix:"))) {
        QFile matrixFile(arguments[3].mid(7));
        require(matrixFile.open(QIODevice::ReadOnly), matrixFile.errorString());
        const auto matrix = QJsonDocument::fromJson(matrixFile.readAll()).array();
        require(matrix.size() == 6, QStringLiteral("Expected six frame coefficients"));
        normalize = QTransform(matrix[0].toDouble(), matrix[1].toDouble(), matrix[2].toDouble(),
            matrix[3].toDouble(), matrix[4].toDouble(), matrix[5].toDouble());
    }
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
    working.boundaryTolerance = benchmarkOptions.value("boundaryTolerance").toDouble(0.1);
    gui::compact::FillOptions options;
    options.evaluationBudget = budget;
    options.useGpu = !arguments.contains(QStringLiteral("cpu"));
    options.adaptiveBudget = !arguments.contains(QStringLiteral("legacy"));
    options.retainFailedFill = true;
    options.boundaryAllowance = benchmarkOptions.value("boundaryAllowance").toDouble(options.boundaryAllowance);
    options.inwardAllowance = benchmarkOptions.value("inwardAllowance").toDouble(options.inwardAllowance);
    options.observationScale = benchmarkOptions.value("observationScale").toDouble(options.observationScale);
    options.profileTrialBudget = benchmarkOptions.value("profileTrialBudget").toInt(0);
    options.seedOnly = benchmarkOptions.value("seedOnly").toBool();
    gui::compact::FillOptions originalOptions;
    originalOptions.boundaryAllowance = originalSettings.value("boundaryAllowance").toDouble(originalOptions.boundaryAllowance);
    originalOptions.inwardAllowance = originalSettings.value("inwardAllowance").toDouble(originalOptions.inwardAllowance);
    originalOptions.observationScale = originalSettings.value("observationScale").toDouble(originalOptions.observationScale);
    gui::catalog::FillResult oracle;
    QVector<gui::PenPlacement> workingOracle;
    for (const auto &entry : sourceDocument.value("oraclePlacements").toArray()) {
        const auto object = entry.toObject();
        const auto values = object.value("transform").toArray();
        gui::PenPlacement placement;
        placement.shapeId = object.value("shapeId").toInt();
        placement.transform = gui::catalog::emittedTransform(QTransform(values[0].toDouble(), values[1].toDouble(),
            values[2].toDouble(), values[3].toDouble(), values[4].toDouble(), values[5].toDouble()));
        const double clearance = benchmarkOptions.value("oracleClearance").toDouble();
        if (clearance > 0) {
            const auto primitive = std::find_if(primitives.begin(), primitives.end(), [&](const auto &candidate) {
                return candidate.shape.shapeId == placement.shapeId;
            });
            const auto bounds = placement.transform.mapRect(primitive->shape.bounds);
            const auto center = bounds.center();
            QTransform expansion;
            expansion.translate(center.x(), center.y());
            expansion.scale(1 + clearance * 2 / bounds.width(), 1 + clearance * 2 / bounds.height());
            expansion.translate(-center.x(), -center.y());
            placement.transform = gui::catalog::emittedTransform(placement.transform * expansion);
        }
        oracle.fill.placements.push_back(placement);
        placement.transform = gui::catalog::emittedTransform(placement.transform * normalize);
        workingOracle.push_back(placement);
        if (benchmarkOptions.value("oracleSeed").toBool())
            options.initialPlacements.push_back(placement);
    }
    if (benchmarkOptions.value("oracleSeed").toBool() && !options.initialPlacements.isEmpty())
        options.shapeBudget = options.initialPlacements.size();
    const bool oracleVerified = !oracle.fill.placements.isEmpty()
        && gui::compact::verifyEmittedFill(request, primitives, originalOptions, &oracle);
    gui::catalog::FillResult workingOracleResult;
    workingOracleResult.fill.placements = workingOracle;
    const bool workingOracleVerified = !workingOracle.isEmpty()
        && gui::compact::verifyEmittedFill(working, primitives, options, &workingOracleResult);
    QElapsedTimer timer;
    timer.start();
    gui::catalog::FillResult fitted;
    if (benchmarkOptions.value("oraclePool").toBool()) {
        auto seedOptions = options;
        seedOptions.seedOnly = true;
        const auto seed = gui::profile::fillRegion(working, primitives, seedOptions);
        auto refinement = options;
        refinement.initialPlacements = seed.fill.placements;
        refinement.boundaryAllowance = seed.diagnostics.value("selectedBoundaryAllowance").toDouble(options.boundaryAllowance);
        auto candidates = std::make_shared<QVector<gui::compact::ReusableCandidate>>();
        for (const auto &placement : workingOracle) {
            const auto primitive = std::find_if(primitives.begin(), primitives.end(), [&](const auto &candidate) {
                return candidate.shape.shapeId == placement.shapeId;
            });
            const auto polygons = gui::catalog::mapped(primitive->shape, placement.transform);
            candidates->push_back({placement, polygons, gui::catalog::painterPath(polygons).boundingRect()});
        }
        refinement.replacementCandidates = candidates;
        fitted = seed.fill.placements.isEmpty() ? seed : gui::compact::fillRegion(working, primitives, refinement);
        fitted.diagnostics.insert("profileSeed", seed.diagnostics);
    } else {
        fitted = gui::profile::fillRegion(working, primitives, options);
    }
    const qint64 searchMilliseconds = timer.elapsed();
    const bool workingVerified = fitted.diagnostics.value("approximationVerified").toBool();
    const auto workingDiagnostics = fitted.diagnostics;
    const int workingCount = fitted.fill.placements.size();
    for (auto &placement : fitted.fill.placements) {
        placement.transform = gui::catalog::emittedTransform(placement.transform * restore);
        placement.area /= std::abs(normalize.determinant());
    }
    const QString searchError = fitted.fill.error;
    bool verified = !fitted.fill.placements.isEmpty()
        && gui::compact::verifyEmittedFill(request, primitives, originalOptions, &fitted);
    const qint64 validationMilliseconds = timer.elapsed() - searchMilliseconds;
    const bool verifiedBeforeFinish = verified;
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
        verified = !fitted.fill.placements.isEmpty()
            && gui::compact::verifyEmittedFill(request, primitives, originalOptions, &fitted);
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
        {"seed_count", seed.value("count")}, {"stages", fitted.diagnostics.value("stageMilliseconds")},
        {"working_count", workingCount}, {"working_verified", workingVerified}, {"verified_before_finish", verifiedBeforeFinish},
        {"oracle_count", oracle.fill.placements.size()}, {"oracle_verified", oracleVerified},
        {"working_oracle_verified", workingOracleVerified},
        {"oracle_diagnostics", oracle.diagnostics}, {"working_diagnostics", workingDiagnostics}, {"diagnostics", fitted.diagnostics}};
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
}
