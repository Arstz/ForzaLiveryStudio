struct NativeSampleShape {
    QPainterPath path;
    std::vector<int> winning;
    std::vector<int> coverage;
    int group = 0;
};

fls::Project selectNativeSamples(const QByteArray &bytes, const std::vector<bool> &keep) {
    auto result = fls::decodeProjectDocument(bytes);
    int source = 0;
    for (auto group = result.root->children.begin(); group != result.root->children.end();) {
        auto &region = static_cast<fls::scene::Group &>(**group);
        int count = 0;
        for (auto child = region.children.begin(); child != region.children.end();) {
            if ((*child)->kind() != fls::scene::LayerKind::Shape) {
                ++child;
            } else if (!keep[source++]) {
                child = region.children.erase(child);
            } else {
                ++count;
                ++child;
            }
        }
        if (count) {
            region.name = region.name.left(region.name.lastIndexOf('(')) + QString("(%1 shapes)").arg(count);
            ++group;
        } else group = result.root->children.erase(group);
    }
    return result;
}

QJsonObject compactNativeSamples(fls::Project &project, const QVector<gui::catalog::Primitive> &catalog,
                                 const gui::ShapeGeometryStore &geometry, QSize size, int samples) {
    const auto bytes = fls::encodeProjectDocument(project);
    auto identifiers = fls::decodeProjectDocument(bytes);
    std::vector<NativeSampleShape> shapes;
    std::vector<std::vector<int>> groups;
    QHash<QRgb, int> appearances;
    for (auto &node : identifiers.root->children) {
        auto &group = static_cast<fls::scene::Group &>(*node);
        for (auto &child : group.children) {
            if (child->kind() != fls::scene::LayerKind::Shape) continue;
            auto &shape = static_cast<fls::scene::Shape &>(*child);
            const auto primitive = std::find_if(catalog.begin(), catalog.end(), [&](const auto &item) {
                return item.shape.shapeId == shape.shapeId;
            });
            if (primitive == catalog.end()) throw std::runtime_error("Unknown sample-compaction primitive");
            const auto matrix = shape.worldMatrix();
            const QTransform transform(matrix.m[0][0], matrix.m[1][0], matrix.m[0][1],
                matrix.m[1][1], matrix.m[0][2], matrix.m[1][2]);
            const auto color = qRgb(shape.color[2], shape.color[1], shape.color[0]);
            if (!appearances.contains(color)) {
                appearances.insert(color, int(groups.size()));
                groups.emplace_back();
            }
            const int appearance = appearances.value(color);
            const unsigned id = unsigned(shapes.size()) + 1;
            shape.color = {quint8(id), quint8(id >> 8), quint8(id >> 16), 255};
            groups[appearance].push_back(int(shapes.size()));
            shapes.push_back({transform.map(primitive->shape.silhouette), {}, {}, appearance});
        }
    }
    if (shapes.empty() || shapes.size() >= 0xffffff)
        return {{"applied", false}, {"reason", "Unsupported identifier count"}};
    if (std::all_of(groups.begin(), groups.end(), [](const auto &group) { return group.size() <= 1; }))
        return {{"applied", false}, {"reason", "Each fill has one native placement"}};
    const auto ids = renderFilledProject(identifiers, geometry, size, samples, false);
    const int width = ids.width(), height = ids.height();
    const auto *pixels = ids.constBits();
    const auto ownerAt = [&](int pixel) {
        const auto *value = pixels + pixel * 4;
        return int((unsigned(value[0]) << 16) | (unsigned(value[1]) << 8) | value[2]) - 1;
    };
    std::vector<unsigned char> uncovered(size_t(width) * height, 0);
    std::vector<int> remaining(groups.size(), 0);
    for (int pixel = 0; pixel < width * height; ++pixel) {
        const int owner = ownerAt(pixel);
        if (owner >= int(shapes.size()) || (owner < 0 && pixels[pixel * 4 + 3] != 0))
            return {{"applied", false}, {"reason", "Identifier render contains blended samples"}};
        if (owner >= 0) {
            shapes[owner].winning.push_back(pixel);
            uncovered[pixel] = 1;
            ++remaining[shapes[owner].group];
        }
    }
    std::vector<bool> keep(shapes.size(), false);
    for (int group = 0; group < int(groups.size()); ++group) {
        if (!remaining[group]) continue;
        if (groups[group].size() == 1) {
            keep[groups[group][0]] = true;
            continue;
        }
        std::priority_queue<std::pair<int, int>> queue;
        for (int index : groups[group]) {
            auto &shape = shapes[index];
            const auto bounds = QTransform::fromScale(samples, samples).map(shape.path).boundingRect()
                .toAlignedRect().adjusted(-1, -1, 1, 1).intersected(QRect(0, 0, width, height));
            QImage mask(bounds.size(), QImage::Format_Grayscale8);
            mask.fill(0);
            if (!bounds.isEmpty()) {
                QPainter painter(&mask);
                painter.translate(-bounds.x(), -bounds.y());
                painter.scale(samples, samples);
                painter.fillPath(shape.path, Qt::white);
                painter.end();
                for (int y = 0; y < bounds.height(); ++y) {
                    const auto *row = mask.constScanLine(y);
                    for (int x = 0; x < bounds.width(); ++x) {
                        if (!row[x]) continue;
                        const int pixel = (bounds.y() + y) * width + bounds.x() + x;
                        const int owner = ownerAt(pixel);
                        if (owner >= 0 && shapes[owner].group == group) shape.coverage.push_back(pixel);
                    }
                }
            }
            for (int pixel : shape.winning) {
                const QPoint point(pixel % width, pixel / width);
                if (!bounds.contains(point) || !mask.constScanLine(point.y() - bounds.y())[point.x() - bounds.x()])
                    shape.coverage.push_back(pixel);
            }
            if (!shape.coverage.empty()) queue.push({int(shape.coverage.size()), -index});
        }
        while (remaining[group] && !queue.empty()) {
            const auto [previous, negative] = queue.top();
            queue.pop();
            const int index = -negative;
            int gain = 0;
            for (int pixel : shapes[index].coverage) gain += uncovered[pixel] != 0;
            if (!gain) continue;
            if (gain != previous) {
                queue.push({gain, negative});
                continue;
            }
            keep[index] = true;
            for (int pixel : shapes[index].coverage)
                if (uncovered[pixel]) {
                    uncovered[pixel] = 0;
                    --remaining[group];
                }
        }
        if (remaining[group])
            for (int index : groups[group]) keep[index] = true;
    }
    for (auto &shape : shapes) {
        shape.coverage.clear();
        shape.coverage.shrink_to_fit();
    }
    const auto before = renderFilledProject(project, geometry, size, samples, false);
    int restored = 0;
    for (int pass = 0; pass < 8; ++pass) {
        auto candidate = selectNativeSamples(bytes, keep);
        const auto after = renderFilledProject(candidate, geometry, size, samples, false);
        const auto *reference = before.constBits(), *actual = after.constBits();
        int changed = 0, added = 0;
        for (int pixel = 0; pixel < width * height; ++pixel) {
            const int offset = pixel * 4;
            if (reference[offset] == actual[offset] && reference[offset + 1] == actual[offset + 1]
                && reference[offset + 2] == actual[offset + 2] && reference[offset + 3] == actual[offset + 3]) continue;
            ++changed;
            const int owner = ownerAt(pixel);
            if (owner >= 0 && !keep[owner]) {
                keep[owner] = true;
                ++added;
            }
        }
        restored += added;
        if (!changed) {
            const int retained = int(std::count(keep.begin(), keep.end(), true));
            project = std::move(candidate);
            return {{"applied", true}, {"generated_placements", int(shapes.size())},
                {"final_placements", retained}, {"restored_placements", restored},
                {"verification_passes", pass + 1}, {"native_sample_differences", 0},
                {"samples_per_axis", samples}, {"method", "Visible native-sample set cover with exact renderer restoration"}};
        }
        if (!added) break;
    }
    return {{"applied", false}, {"reason", "Native sample verification retained the original scene"}};
}
