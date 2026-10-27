struct CompactOutputOptions {
    QString cacheDirectory;
    QVector<QVector<QPolygonF>> seedTriangles;
    QVector<QPolygonF> allowedDomain;
    int evaluations = gui::compact::kDefaultEvaluationBudget;
    int shapeBudget = gui::compact::kDefaultShapeBudget;
    int totalShapeBudget = gui::compact::kDefaultShapeBudget;
    int workers = 1;
    int renderSamples = 4;
    double boundary = gui::compact::kDefaultBoundaryAllowance;
    double inward = gui::compact::kDefaultInwardAllowance;
    double areaRatio = gui::compact::kDefaultAreaErrorRatio;
    double flattenTolerance = 0.05;
    bool nativeRender = false;
    bool polygonSeed = false;
    bool useGpu = true;
    bool allowSceneWarnings = false;
};

QByteArray hash(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QJsonArray placementJson(const QVector<gui::PenPlacement> &placements) {
    QJsonArray result;
    for (const auto &placement : placements) {
        const auto &matrix = placement.transform;
        result.append(QJsonObject{{"shape_id", placement.shapeId}, {"transform", QJsonArray{
            matrix.m11(), matrix.m12(), matrix.m21(), matrix.m22(), matrix.dx(), matrix.dy()}}});
    }
    return result;
}

QVector<gui::PenPlacement> placementsFromJson(const QJsonArray &data) {
    QVector<gui::PenPlacement> result;
    for (const auto &entry : data) {
        const auto item = entry.toObject();
        const auto values = item.value("transform").toArray();
        if (values.size() != 6 || item.value("shape_id").toInt() <= 0)
            throw std::runtime_error("Invalid cached placement");
        for (const auto &value : values)
            if (!value.isDouble() || !std::isfinite(value.toDouble()))
                throw std::runtime_error("Invalid cached placement transform");
        const QTransform matrix(values[0].toDouble(), values[1].toDouble(), values[2].toDouble(),
            values[3].toDouble(), values[4].toDouble(), values[5].toDouble());
        if (!matrix.isInvertible()) throw std::runtime_error("Singular cached placement");
        result.push_back({item.value("shape_id").toInt(), matrix});
    }
    return result;
}

QVector<gui::PenPlacement> polygonSeed(const gui::PenFillRequest &request,
                                     const QVector<gui::catalog::Primitive> &catalog,
                                     const QVector<QPolygonF> &triangles, double boundary) {
    QVector<gui::PenPlacement> result;
    auto region = gui::catalog::buildRegion(request, {});
    region.permitted = gui::catalog::expanded(region.required, boundary);
    if (triangles.isEmpty()) {
        for (const auto &piece : gui::catalog::completeCover(region, catalog, {})) result.push_back(piece.placement);
        return result;
    }
    for (const auto &piece : gui::catalog::completeTriangleCover(region, catalog, triangles, {}))
        result.push_back(piece.placement);
    return result;
}

QJsonObject fitCompactRegion(const gui::SvgVectorObject &object,
                            const QVector<gui::catalog::Primitive> &catalog,
                            const CompactOutputOptions &settings, const QByteArray &engineKey,
                            const QVector<QPolygonF> &seedTriangles = {}) {
    QByteArray pathBytes;
    QDataStream stream(&pathBytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << object.path << settings.evaluations << settings.shapeBudget
           << settings.boundary << settings.inward << settings.areaRatio << settings.polygonSeed << settings.useGpu
           << settings.allowedDomain << seedTriangles;
    const auto key = QString::fromLatin1(hash(engineKey + pathBytes));
    const auto cachePath = QDir(settings.cacheDirectory).filePath(key + ".json");
    if (!settings.cacheDirectory.isEmpty() && QFile::exists(cachePath)) {
        auto cached = QJsonDocument::fromJson(read(cachePath)).object();
        const auto checksum = cached.take("checksum").toString().toLatin1();
        if (cached.value("key").toString() == key
            && hash(QJsonDocument(cached).toJson(QJsonDocument::Compact)) == checksum) {
            try {
                placementsFromJson(cached.value("placements_data").toArray());
                cached.insert("cache_hit", true);
                return cached;
            } catch (const std::exception &) {
            }
        }
    }
    QElapsedTimer timer;
    timer.start();
    gui::RegionPenLoopConversionOptions conversionOptions;
    conversionOptions.preserveInputCurves = true;
    const auto conversion = gui::regionOutlineToPenLoops(object.path, conversionOptions);
    gui::PenFillRequest request;
    request.loops = conversion.loops;
    for (const auto &primitive : catalog) request.primitives.push_back(primitive.shape);
    gui::compact::FillOptions options;
    options.useGpu = settings.useGpu;
    options.evaluationBudget = settings.evaluations;
    options.shapeBudget = settings.shapeBudget;
    options.boundaryAllowance = settings.boundary;
    options.inwardAllowance = settings.inward;
    options.areaErrorRatio = settings.areaRatio;
    options.retainFailedFill = true;
    options.allowedDomain = settings.allowedDomain;
    QPainterPath rectangle;
    rectangle.addRect(object.path.boundingRect());
    const bool exactRectangle = rectangle.subtracted(object.path).isEmpty()
        && object.path.subtracted(rectangle).isEmpty();
    if (exactRectangle) {
        options.boundaryAllowance = std::min(options.boundaryAllowance, 1e-5);
        options.inwardAllowance = std::min(options.inwardAllowance, 1e-5);
    }
    gui::catalog::FillResult fitted;
    QString seedFailure;
    try {
      if (conversion.valid()) {
        if (settings.polygonSeed && !seedTriangles.isEmpty())
            options.initialPlacements = polygonSeed(request, catalog, seedTriangles, settings.boundary);
        fitted = settings.polygonSeed ? gui::compact::fillRegion(request, catalog, options)
            : gui::profile::fillRegion(request, catalog, options);
        if (settings.polygonSeed && options.initialPlacements.isEmpty() && !fitted.fill.error.isEmpty()
            && !fitted.diagnostics.contains("seedCount")) {
            seedFailure = fitted.fill.error;
            try {
                options.initialPlacements = polygonSeed(request, catalog, seedTriangles, settings.boundary);
                if (!options.initialPlacements.isEmpty())
                    fitted = gui::compact::fillRegion(request, catalog, options);
            } catch (const std::exception &failure) {
                seedFailure += "; polygon seed: " + QString::fromUtf8(failure.what());
            }
        }
      } else fitted.fill.error = conversion.error;
    } catch (const std::exception &failure) {
        fitted.fill.error = QString::fromUtf8(failure.what());
    }
    const bool complete = conversion.valid() && fitted.fill.error.isEmpty()
        && !fitted.fill.cancelled && !fitted.fill.timedOut && fitted.fill.unfilled.isEmpty()
        && !fitted.fill.placements.isEmpty();
    const bool verified = conversion.valid() && fitted.fill.error.isEmpty()
        && !fitted.fill.cancelled && !fitted.fill.timedOut && !fitted.fill.placements.isEmpty()
        && fitted.diagnostics.value("approximationVerified").toBool();
    const bool retained = conversion.valid() && !fitted.fill.cancelled && !fitted.fill.timedOut
        && !fitted.fill.placements.isEmpty() && fitted.fill.placements.size() <= settings.shapeBudget
        && fitted.fill.error.startsWith("Compact fit failed:");
    QJsonObject result{{"key", key}, {"complete", complete}, {"verified", verified},
        {"error", fitted.fill.error}, {"zero_residual", fitted.fill.unfilled.isEmpty()},
        {"placements", fitted.fill.placements.size()}, {"placements_data", placementJson(fitted.fill.placements)},
        {"seconds", timer.elapsed() / 1000.0}, {"cache_hit", false}, {"seed_failure", seedFailure},
        {"polygon_seed_used", !options.initialPlacements.isEmpty()}, {"gpu_requested", settings.useGpu},
        {"pipeline", settings.polygonSeed ? "explicit polygon seed" : "editor curve-first Compact Fit"},
        {"diagnostics", fitted.diagnostics}};
    result.insert("scene_eligible", complete || verified || retained);
    if (!settings.cacheDirectory.isEmpty()) {
        if (!QDir().mkpath(settings.cacheDirectory)) throw std::runtime_error("Cannot create fill cache");
        auto cached = result;
        cached.insert("checksum", QString::fromLatin1(hash(QJsonDocument(result).toJson(QJsonDocument::Compact))));
        write(cachePath, QJsonDocument(cached).toJson());
    }
    return result;
}

std::unique_ptr<fls::scene::Group> placementGroup(int index, QColor color,
                                                const QVector<gui::PenPlacement> &placements,
                                                const QPainterPath &target) {
    auto group = std::make_unique<fls::scene::Group>();
    group->id = QString("region_%1").arg(index);
    group->name = QString("Region %1 %2 (%3 shapes)").arg(index + 1).arg(color.name()).arg(placements.size());
    for (const auto &placement : placements) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->id = QString("shape_%1_%2").arg(index).arg(group->children.size());
        shape->name = QString("%1 [%2]").arg(group->children.size() + 1).arg(placement.shapeId);
        shape->setVectorShape(placement.shapeId);
        shape->color = {quint8(color.blue()), quint8(color.green()), quint8(color.red()), 255};
        const auto &matrix = placement.transform;
        shape->transform = fls::decomposeTransform2D(fls::affine(matrix.m11(), matrix.m21(),
            matrix.dx(), matrix.m12(), matrix.m22(), matrix.dy()));
        group->append(std::move(shape));
    }
    gui::RegionPenLoopConversionOptions conversionOptions;
    conversionOptions.preserveInputCurves = true;
    const auto converted = gui::regionOutlineToPenLoops(target, conversionOptions);
    if (!converted.valid()) throw std::runtime_error("Cannot retain the fitted region contour");
    auto contour = std::make_unique<fls::scene::ContourFigure>();
    contour->id = QString("contour_%1").arg(index);
    contour->name = "Region contour";
    contour->visible = false;
    contour->data.closed = true;
    contour->data.fillColor = color;
    for (const auto &loop : converted.loops) {
        if (loop.kind == gui::PenLoopKind::Outer) contour->data.points = loop.points;
        else contour->data.cutouts.push_back(loop.points);
    }
    group->append(std::move(contour));
    return group;
}

QVector<gui::SvgVectorObject> projectObjects(const fls::Project &project,
                                          const QVector<gui::catalog::Primitive> &catalog) {
    QVector<gui::SvgVectorObject> result;
    for (const auto &node : project.root->children) {
        const auto &group = static_cast<const fls::scene::Group &>(*node);
        for (const auto &child : group.children) {
            if (child->kind() != fls::scene::LayerKind::Shape) continue;
            const auto &shape = static_cast<const fls::scene::Shape &>(*child);
            const auto primitive = std::find_if(catalog.begin(), catalog.end(), [&](const auto &item) {
                return item.shape.shapeId == shape.shapeId;
            });
            if (primitive == catalog.end()) throw std::runtime_error("Unknown placement primitive");
            const auto matrix = shape.worldMatrix();
            const QTransform transform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
                matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]);
            result.push_back({transform.map(primitive->shape.silhouette),
                QColor(shape.color[2], shape.color[1], shape.color[0], shape.color[3])});
        }
    }
    return result;
}

QImage renderFilledProject(const fls::Project &project, const gui::ShapeGeometryStore &geometry, QSize size, int samples,
                           bool resolve = true) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    QOpenGLContext context;
    context.setFormat(surface.format());
    if (!surface.isValid() || !context.create() || !context.makeCurrent(&surface))
        throw std::runtime_error("Cannot create the native output renderer OpenGL context");
    const QSize renderedSize(size.width() * samples, size.height() * samples);
    QImage image(renderedSize, QImage::Format_RGBA8888);
    if (image.isNull()) throw std::runtime_error("Cannot allocate the native comparison render");
    {
        gui::NativeShapeRenderer renderer;
        renderer.initialize();
        if (!renderer.isInitialized()) throw std::runtime_error("Cannot initialize native output renderer");
        renderer.uploadGeometry(geometry);
        const auto texture = renderer.renderSceneToTexture(project, geometry, QTransform::fromScale(samples, samples), renderedSize);
        if (!texture) throw std::runtime_error("Native output renderer produced no texture");
        auto *functions = context.extraFunctions();
        GLuint framebuffer = 0;
        functions->glGenFramebuffers(1, &framebuffer);
        functions->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        functions->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        const bool valid = functions->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        if (valid) functions->glReadPixels(0, 0, renderedSize.width(), renderedSize.height(), GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
        functions->glBindFramebuffer(GL_FRAMEBUFFER, 0);
        functions->glDeleteFramebuffers(1, &framebuffer);
        if (!valid) throw std::runtime_error("Cannot read native output renderer framebuffer");
    }
    context.doneCurrent();
    return resolve ? image.mirrored().scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : image.mirrored();
}

QJsonObject cullHiddenPlacements(fls::Project &project, const QVector<gui::catalog::Primitive> &catalog,
                                double clearance = 0) {
    gui::catalog::Polygons covered;
    QJsonArray retained;
    QJsonObject counts;
    int removed = 0, finalCount = 0;
    for (int groupIndex = int(project.root->children.size()) - 1; groupIndex >= 0; --groupIndex) {
        auto &group = static_cast<fls::scene::Group &>(*project.root->children[groupIndex]);
        int count = 0;
        for (int index = int(group.children.size()) - 1; index >= 0; --index) {
            if (group.children[index]->kind() != fls::scene::LayerKind::Shape) continue;
            const auto &shape = static_cast<const fls::scene::Shape &>(*group.children[index]);
            const auto primitive = std::find_if(catalog.begin(), catalog.end(), [&](const auto &item) {
                return item.shape.shapeId == shape.shapeId;
            });
            if (primitive == catalog.end()) throw std::runtime_error("Unknown culling primitive");
            const auto matrix = shape.worldMatrix();
            const QTransform transform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
                matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]);
            const auto polygons = gui::catalog::mapped(primitive->shape, transform);
            const auto support = clearance > 0 ? gui::catalog::expanded(polygons, clearance) : polygons;
            if (gui::catalog::subtract(support, covered).isEmpty()) {
                group.children.erase(group.children.begin() + index);
                ++removed;
            } else {
                covered = gui::catalog::unite(covered + polygons);
                ++count;
            }
        }
        const int source = group.id.mid(7).toInt();
        group.name = group.name.left(group.name.lastIndexOf('(')) + QString("(%1 shapes)").arg(count);
        counts.insert(QString::number(source), count);
        finalCount += count;
        if (count) retained.prepend(source);
        else project.root->children.erase(project.root->children.begin() + groupIndex);
    }
    return {{"removed_placements", removed}, {"final_placements", finalCount},
        {"retained_region_paths", retained}, {"placements_per_path", counts}};
}

#include "native_scene_compaction.inc"

QJsonObject fitCompactScene(const QVector<gui::SvgVectorObject> &objects, QSize size,
                           const CompactOutputOptions &settings, const QDir &output, const QString &name) {
    gui::ShapeGeometryStore geometry;
    QString error;
    if (!geometry.loadDefault(&error)) throw std::runtime_error(error.toStdString());
    const auto catalog = gui::catalog::buildCatalog(geometry, &error);
    if (catalog.isEmpty()) throw std::runtime_error(error.toStdString());
    QByteArray catalogBytes;
    QDataStream catalogStream(&catalogBytes, QIODevice::WriteOnly);
    catalogStream.setVersion(QDataStream::Qt_6_0);
    for (const auto &primitive : catalog)
        catalogStream << primitive.shape.shapeId << primitive.shape.silhouette << primitive.shape.contours;
    const auto engineKey = hash(read(QCoreApplication::applicationFilePath())) + hash(catalogBytes)
        + (catalog.front().configuration ? QJsonDocument(*catalog.front().configuration).toJson(QJsonDocument::Compact) : QByteArray());
    auto sceneSettings = settings;
    QVector<QPolygonF> domain;
    for (const auto &object : objects) {
        const auto converted = gui::regionOutlineToPenLoops(object.path);
        if (!converted.valid()) throw std::runtime_error(converted.error.toStdString());
        gui::PenFillRequest request;
        request.loops = converted.loops;
        domain += gui::catalog::buildRegion(request, {}).required;
    }
    sceneSettings.allowedDomain = gui::catalog::unite(domain);
    QElapsedTimer timer;
    timer.start();
    fls::Project project;
    project.name = name + " Compact Fit";
    project.root->id = "root";
    QJsonArray paths;
    QSet<QRgb> colors;
    int placements = 0, completed = 0, verified = 0, eligible = 0;
    std::atomic_int next{0};
    std::vector<std::promise<QJsonObject>> promises(objects.size());
    std::vector<std::future<QJsonObject>> results;
    for (auto &promise : promises) results.push_back(promise.get_future());
    std::vector<std::future<void>> workers;
    for (int worker = 0; worker < std::min(settings.workers, int(objects.size())); ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (int index = next.fetch_add(1); index < objects.size(); index = next.fetch_add(1)) {
                try {
                    auto fitted = fitCompactRegion(objects[index], catalog, sceneSettings, engineKey,
                        index < settings.seedTriangles.size() ? settings.seedTriangles[index] : QVector<QPolygonF>());
                    std::fprintf(stderr, "Compact Fit %s: region %d/%d, %d shapes, %s%s\n", qPrintable(name), index + 1,
                        int(objects.size()), fitted.value("placements").toInt(),
                        fitted.value("verified").toBool() || fitted.value("complete").toBool() ? "verified" : "FAILED",
                        fitted.value("cache_hit").toBool() ? " (cached)" : "");
                    promises[index].set_value(std::move(fitted));
                } catch (...) {
                    promises[index].set_exception(std::current_exception());
                }
            }
        }));
    }
    for (int index = 0; index < objects.size(); ++index) {
        auto fitted = results[index].get();
        fitted.insert("path", index);
        fitted.insert("fill", objects[index].color.name());
        const auto data = fitted.take("placements_data").toArray();
        const bool accepted = fitted.value("verified").toBool() || fitted.value("complete").toBool();
        const bool retained = fitted.value("scene_eligible").toBool();
        completed += fitted.value("complete").toBool();
        verified += accepted;
        eligible += retained;
        placements += fitted.value("placements").toInt();
        if (retained) project.root->append(placementGroup(index, objects[index].color,
            placementsFromJson(data), objects[index].path));
        colors.insert(objects[index].color.rgb());
        paths.append(fitted);
        write(output.filePath(name + "_compact_progress.json"), QJsonDocument(QJsonObject{
            {"completed_regions", index + 1}, {"total_regions", objects.size()}, {"verified_regions", verified},
            {"attempted_placements", placements}, {"per_path", paths}}).toJson());
    }
    for (auto &worker : workers) worker.get();
    const int generatedPlacements = placements;
    const bool valid = !objects.isEmpty() && eligible == objects.size();
    QByteArray projectBytes = valid ? fls::encodeProjectDocument(project) : QByteArray();
    QJsonObject culling{{"applied", false}, {"removed_placements", 0}};
    int finalRegions = objects.size();
    QSet<QRgb> finalColors = colors;
    if (valid) {
        auto candidate = fls::decodeProjectDocument(projectBytes);
        const auto sampleCompaction = settings.nativeRender
            ? compactNativeSamples(candidate, catalog, geometry, size, settings.renderSamples) : QJsonObject();
        const auto sampleBytes = fls::encodeProjectDocument(candidate);
        culling = cullHiddenPlacements(candidate, catalog);
        culling.insert("removed_placements", generatedPlacements - culling.value("final_placements").toInt());
        culling.insert("sample_compaction", sampleCompaction);
        if (culling.value("removed_placements").toInt() > 0) {
            const auto renderProject = [&](const fls::Project &scene) {
                return settings.nativeRender ? renderFilledProject(scene, geometry, size, settings.renderSamples)
                    : svgRender(opaque::svg(projectObjects(scene, catalog), size), size);
            };
            const auto before = renderProject(project);
            auto after = renderProject(candidate);
            auto error = opaque::imageError(before, after);
            if (error.value("max_channel_error").toDouble() != 0 || error.value("max_alpha_error").toDouble() != 0) {
                const auto firstError = error;
                candidate = fls::decodeProjectDocument(sampleBytes);
                culling = cullHiddenPlacements(candidate, catalog, .001);
                culling.insert("removed_placements", generatedPlacements - culling.value("final_placements").toInt());
                culling.insert("sample_compaction", sampleCompaction);
                after = renderProject(candidate);
                error = opaque::imageError(before, after);
                culling.insert("clearance", .001);
                culling.insert("initial_image_error", firstError);
            }
            const bool unchanged = error.value("max_channel_error").toDouble() == 0
                && error.value("max_alpha_error").toDouble() == 0;
            culling.insert("image_error", error);
            culling.insert("applied", unchanged);
            save(output.filePath(name + "_unculled.png"), before);
            save(output.filePath(name + "_culling_difference.png"), opaque::difference(before, after));
            if (unchanged) {
                placements = culling.value("final_placements").toInt();
                projectBytes = fls::encodeProjectDocument(candidate);
                finalRegions = candidate.root->children.size();
                finalColors.clear();
                for (const auto &value : culling.value("retained_region_paths").toArray())
                    finalColors.insert(objects[value.toInt()].color.rgb());
            }
        }
    }
    if (culling.value("applied").toBool()) {
        const auto counts = culling.value("placements_per_path").toObject();
        for (int index = 0; index < paths.size(); ++index) {
            auto path = paths[index].toObject();
            path.insert("final_placements", counts.value(QString::number(index)).toInt());
            paths[index] = path;
        }
    }
    const bool budgetPassed = placements <= settings.totalShapeBudget;
    const bool strict = !objects.isEmpty() && verified == objects.size() && budgetPassed;
    QJsonObject report{{"measured", true}, {"complete", completed == objects.size()}, {"verified", strict},
        {"scene_candidate", valid && budgetPassed}, {"regions_with_placements", eligible},
        {"paths", objects.size()}, {"input_region_count", objects.size()}, {"region_count", finalRegions},
        {"input_color_count", colors.size()}, {"color_count", finalColors.size()},
        {"verified_paths", verified}, {"completed_paths", completed}, {"failed_regions", objects.size() - verified},
        {"verified_placements", strict ? QJsonValue(placements) : QJsonValue()},
        {"shape_count", valid ? QJsonValue(placements) : QJsonValue()}, {"placements", completed == objects.size() ? QJsonValue(placements) : QJsonValue()},
        {"attempted_placements", generatedPlacements}, {"per_path", paths}, {"leeway_used", false},
        {"placement_culling", culling},
        {"evaluation_budget_per_path", settings.evaluations}, {"shape_budget_per_path", settings.shapeBudget},
        {"total_shape_budget", settings.totalShapeBudget}, {"shape_budget_passed", budgetPassed},
        {"over_budget_by", std::max(0, placements - settings.totalShapeBudget)}};
    report.insert("workers", settings.workers);
    if (valid && budgetPassed && (strict || settings.allowSceneWarnings)) {
        const auto filename = name + "_compact.3so";
        write(output.filePath(filename), projectBytes);
        const auto loaded = fls::decodeProjectDocument(read(output.filePath(filename)));
        const auto fitted = projectObjects(loaded, catalog);
        if (fitted.size() != placements) throw std::runtime_error("Saved project lost placements");
        const auto image = settings.nativeRender ? renderFilledProject(loaded, geometry, size, settings.renderSamples)
            : svgRender(opaque::svg(fitted, size), size);
        save(output.filePath(name + "_compact.png"), image);
        const auto target = svgRender(opaque::svg(objects, size), size);
        save(output.filePath(name + "_compact_difference.png"), opaque::difference(target, image));
        report.insert("project", filename);
        report.insert("render", name + "_compact.png");
        report.insert("renderer", settings.nativeRender ? "NativeShapeRenderer" : "QtSvg native primitive silhouettes");
        report.insert("render_samples", settings.nativeRender ? settings.renderSamples : 1);
        report.insert("image_error", opaque::imageError(target, image));
        report.insert("appearance_error", opaque::appearanceError(target, image, 16));
        report.insert("silhouette_difference_area", opaque::silhouetteDifference(objects, fitted, settings.flattenTolerance));
    }
    report.insert("seconds", timer.elapsed() / 1000.0);
    return report;
}
