namespace {

PathsD connectedColorBridges(const PathsD &required, const PathsD &allowed, const Options &options) {
    const auto parts = components(required);
    if (parts.size() < 2) return required;
    const int count = int(parts.size());
    QVector<QRectF> bounds;
    QVector<int> parent;
    for (int index = 0; index < count; ++index) {
        bounds.push_back(fromPolygons(parts[index]).boundingRect());
        parent.push_back(index);
    }
    const auto root = [&](int index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    const auto distance = [&](int a, int b) {
        const double x = std::max({0.0, bounds[a].left() - bounds[b].right(), bounds[b].left() - bounds[a].right()});
        const double y = std::max({0.0, bounds[a].top() - bounds[b].bottom(), bounds[b].top() - bounds[a].bottom()});
        return x * x + y * y;
    };
    std::set<std::pair<int, int>> pairs;
    const int neighbors = std::min(count - 1, std::max(1, options.groupNeighbors));
    for (int a = 0; a < count; ++a) {
        std::vector<std::pair<double, int>> nearest;
        for (int b = 0; b < count; ++b) if (a != b) nearest.push_back({distance(a, b), b});
        std::partial_sort(nearest.begin(), nearest.begin() + neighbors, nearest.end());
        for (int index = 0; index < neighbors; ++index)
            pairs.insert(std::minmax(a, nearest[index].second));
    }
    std::vector<std::tuple<double, int, int>> edges;
    for (const auto &[a, b] : pairs) edges.push_back({distance(a, b), a, b});
    std::stable_sort(edges.begin(), edges.end());
    const auto closest = [](const PathD &loop, QPointF target) {
        QPointF result;
        double best = std::numeric_limits<double>::infinity();
        for (size_t index = 0; index < loop.size(); ++index) {
            const auto &a = loop[index], &b = loop[(index + 1) % loop.size()];
            const QPointF start(a.x, a.y), delta(b.x - a.x, b.y - a.y);
            const double squared = delta.x() * delta.x() + delta.y() * delta.y();
            const auto relative = target - start;
            const double t = squared > 0 ? std::clamp((relative.x() * delta.x() + relative.y() * delta.y()) / squared, 0.0, 1.0) : 0;
            const auto point = start + delta * t;
            const double value = (point.x() - target.x()) * (point.x() - target.x())
                + (point.y() - target.y()) * (point.y() - target.y());
            if (value < best) { result = point; best = value; }
        }
        return result;
    };
    const PolygonIndex permitted(allowed);
    PathsD bridges;
    double spent = 0;
    int remaining = count;
    const double limit = std::min(options.areaTolerance, options.silhouetteTolerance);
    for (const auto &[score, a, b] : edges) {
        if (root(a) == root(b)) continue;
        const auto start = closest(parts[a].front(), bounds[b].center());
        const auto end = closest(parts[b].front(), start);
        const auto anchor = closest(parts[a].front(), end);
        const auto delta = end - anchor;
        const double length = std::hypot(delta.x(), delta.y());
        if (length <= 1e-12) continue;
        const QPointF direction = delta / length;
        for (const double width : {std::max(0.01, options.flattenTolerance * 2),
                                   std::max(0.002, options.flattenTolerance * .5), 0.002}) {
            const QPointF normal(-direction.y() * width * .5, direction.x() * width * .5);
            const auto cap = direction * (width * .5);
            const QPointF points[]{anchor - cap + normal, end + cap + normal, end + cap - normal, anchor - cap - normal};
            PathD rectangle;
            for (const auto &point : points) rectangle.push_back({point.x(), point.y()});
            if (Area(rectangle) < 0) std::reverse(rectangle.begin(), rectangle.end());
            const PathsD bridge{rectangle};
            const auto path = fromPolygons(bridge);
            const double outside = permitted.differenceArea(bridge, path.boundingRect());
            if (spent + outside > limit || !penTargetError(path, options).isEmpty()) continue;
            bridges.push_back(std::move(rectangle));
            spent += outside;
            parent[root(b)] = root(a);
            --remaining;
            break;
        }
        if (remaining == 1) break;
    }
    return bridges.empty() ? required : Union(required, bridges, FillRule::NonZero, kBooleanPrecision);
}

} // namespace
