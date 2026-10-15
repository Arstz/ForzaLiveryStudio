import gzip
import json
import heapq
import math
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SAMPLES = 65
ENDPOINT_INSET = 0.00001


def distance(first, second):
    return math.hypot(first[0] - second[0], first[1] - second[1])


def sample_polyline(points, count):
    lengths = [0.0]
    for first, last in zip(points, points[1:]):
        lengths.append(lengths[-1] + distance(first, last))
    sampled = []
    index = 0
    for sample in range(count):
        along = lengths[-1] * sample / (count - 1)
        while index + 2 < len(points) and lengths[index + 1] < along:
            index += 1
        fraction = (along - lengths[index]) / max(lengths[index + 1] - lengths[index], 1e-12)
        sampled.append(tuple(points[index][axis] * (1 - fraction) + points[index + 1][axis] * fraction
                             for axis in (0, 1)))
    return sampled


def paired_boundary_centerline(vertices, triangles, backbone):
    counts = {}
    for triangle in triangles:
        for first, last in zip(triangle, triangle[1:] + triangle[:1]):
            edge = tuple(sorted((first, last)))
            counts[edge] = counts.get(edge, 0) + 1
    boundary = {}
    for (first, last), count in counts.items():
        if count == 1:
            boundary.setdefault(first, []).append(last)
            boundary.setdefault(last, []).append(first)
    if any(len(neighbors) != 2 for neighbors in boundary.values()):
        return backbone
    loops = []
    remaining = set(boundary)
    while remaining:
        start = min(remaining)
        loop = [start]
        previous = None
        current = start
        while True:
            remaining.discard(current)
            following = next(index for index in boundary[current] if index != previous)
            if following == start:
                break
            loop.append(following)
            previous, current = current, following
        loops.append([vertices[index] for index in loop])

    def closest(loop, point):
        choices = []
        for index, (first, last) in enumerate(zip(loop, loop[1:] + loop[:1])):
            delta = tuple(last[axis] - first[axis] for axis in (0, 1))
            fraction = max(0.0, min(1.0, sum((point[axis] - first[axis]) * delta[axis] for axis in (0, 1))
                                       / max(sum(value * value for value in delta), 1e-12)))
            projected = tuple(first[axis] + fraction * delta[axis] for axis in (0, 1))
            choices.append((distance(point, projected), index + fraction, projected))
        return min(choices)

    loop = min(loops, key=lambda loop: closest(loop, backbone[0])[0] + closest(loop, backbone[-1])[0])
    _, first_index, first = closest(loop, backbone[0])
    _, last_index, last = closest(loop, backbone[-1])
    if distance(first, last) < 1e-8:
        return backbone
    if last_index < first_index:
        last_index += len(loop)
    forward = [first] + [loop[index % len(loop)] for index in range(math.floor(first_index) + 1, math.ceil(last_index))] + [last]
    backward = [first] + [loop[index % len(loop)] for index in range(math.ceil(first_index) - 1, math.floor(last_index - len(loop)), -1)] + [last]
    left = sample_polyline(forward, SAMPLES)
    right = sample_polyline(backward, SAMPLES)
    paired = [tuple((a[axis] + b[axis]) / 2 for axis in (0, 1)) for a, b in zip(left, right)]

    def inside(point):
        winding = False
        for first, last in zip(loop, loop[1:] + loop[:1]):
            if (first[1] > point[1]) != (last[1] > point[1]):
                crossing = first[0] + (point[1] - first[1]) * (last[0] - first[0]) / (last[1] - first[1])
                if point[0] < crossing:
                    winding = not winding
        return winding

    return paired if all(inside(point) for point in paired[1:-1]) else backbone


def centerline(shape):
    vertices = [tuple(vertex[:2]) for vertex in shape["vertices"]]
    triangles = [triangle for triangle in shape["triangles"]
                 if any(shape["vertices"][index][2] > 0 for index in triangle)]
    centers = [tuple(sum(vertices[index][axis] for index in triangle) / 3 for axis in (0, 1))
               for triangle in triangles]
    owners = {}
    graph = [[] for _ in triangles]
    for index, triangle in enumerate(triangles):
        for first, last in zip(triangle, triangle[1:] + triangle[:1]):
            edge = tuple(sorted((first, last)))
            if edge in owners:
                other = owners[edge]
                weight = distance(centers[index], centers[other])
                graph[index].append((other, weight, edge))
                graph[other].append((index, weight, edge))
            else:
                owners[edge] = index

    def farthest(start):
        distances = {start: 0.0}
        previous = {}
        queue = [(0.0, start)]
        while queue:
            cost, index = heapq.heappop(queue)
            if cost != distances[index]:
                continue
            for other, weight, edge in graph[index]:
                trial = cost + weight
                if trial < distances.get(other, math.inf):
                    distances[other] = trial
                    previous[other] = (index, edge)
                    heapq.heappush(queue, (trial, other))
        return max(distances, key=distances.get), distances, previous

    visited = set()
    best = None
    for index in range(len(triangles)):
        if index in visited:
            continue
        first, component, _ = farthest(index)
        visited.update(component)
        last, distances, previous = farthest(first)
        if best is None or distances[last] > best[0]:
            best = (distances[last], first, last, previous)
    _, first, last, previous = best
    chain = [last]
    edges = []
    while chain[-1] != first:
        parent, edge = previous[chain[-1]]
        edges.append(edge)
        chain.append(parent)
    chain.reverse()
    edges.reverse()
    if not edges:
        points = [vertices[index] for index in triangles[first]]
        endpoints = max(((a, b) for a in points for b in points), key=lambda pair: distance(*pair))
        points = list(endpoints)
    else:
        portals = [tuple((vertices[a][axis] + vertices[b][axis]) / 2 for axis in (0, 1)) for a, b in edges]

        def cap(index, portal, shared, direction):
            length = math.hypot(*direction)
            triangle = triangles[index]
            tip = vertices[next(vertex for vertex in triangle if vertex not in shared)]
            sides = [tuple(vertices[vertex][axis] - tip[axis] for axis in (0, 1)) for vertex in shared]
            cosine = sum(sides[0][axis] * sides[1][axis] for axis in (0, 1)) / max(math.hypot(*sides[0]) * math.hypot(*sides[1]), 1e-12)
            if cosine > math.cos(math.pi / 6):
                return tip
            choices = []
            for a, b in zip(triangle, triangle[1:] + triangle[:1]):
                if tuple(sorted((a, b))) == shared:
                    continue
                delta = tuple(vertices[b][axis] - vertices[a][axis] for axis in (0, 1))
                denominator = math.hypot(*delta) * length
                cosine = abs(sum(delta[axis] * direction[axis] for axis in (0, 1))) / max(denominator, 1e-12)
                choices.append((cosine, tuple((vertices[a][axis] + vertices[b][axis]) / 2 for axis in (0, 1))))
            cosine, point = min(choices)
            if cosine > 0.85:
                return vertices[next(vertex for vertex in triangle if vertex not in shared)]
            return point

        first_direction = tuple(centers[chain[0]][axis] - centers[chain[1]][axis] for axis in (0, 1))
        last_direction = tuple(centers[chain[-1]][axis] - centers[chain[-2]][axis] for axis in (0, 1))
        points = [cap(chain[0], portals[0], edges[0], first_direction)] + portals
        points.append(cap(chain[-1], portals[-1], edges[-1], last_direction))
    points = paired_boundary_centerline(vertices, triangles, points)
    lengths = [0.0]
    for a, b in zip(points, points[1:]):
        lengths.append(lengths[-1] + distance(a, b))
    sampled = []
    index = 0
    for sample in range(SAMPLES):
        along = lengths[-1] * (ENDPOINT_INSET + (1 - 2 * ENDPOINT_INSET) * sample / (SAMPLES - 1))
        while index + 2 < len(points) and lengths[index + 1] < along:
            index += 1
        fraction = (along - lengths[index]) / max(lengths[index + 1] - lengths[index], 1e-12)
        sampled.append(tuple(points[index][axis] * (1 - fraction) + points[index + 1][axis] * fraction for axis in (0, 1)))
    ink_vertices = [vertices[index] for index in {index for triangle in triangles for index in triangle}]
    minimum = [min(vertex[axis] for vertex in ink_vertices) for axis in (0, 1)]
    extent = [max(vertex[axis] for vertex in ink_vertices) - minimum[axis] for axis in (0, 1)]
    return [[round(max(0.0, min(1.0, (point[axis] - minimum[axis]) / extent[axis])), 8) for axis in (0, 1)]
            for point in sampled]


def main():
    config_path = ROOT / "assets/lining_shapes.json"
    config = json.loads(config_path.read_text(encoding="utf-8"))
    geometry = json.loads(gzip.decompress((ROOT / "assets/vector/shape_geometry.json.gz").read_bytes()))
    definitions = {str(shape_id): centerline(geometry["shapes"][str(shape_id)])
                   for shape_id in config["shape_ids"] + config["reserve_shape_ids"]}
    if "101" in definitions:
        definitions["101"] = [[round(sample / (SAMPLES - 1), 8), 0.5] for sample in range(SAMPLES)]
    config.pop("centerlines", None)
    text = json.dumps(config, indent=2).rstrip()[:-1].rstrip() + ',\n  "centerlines": {\n'
    text += ",\n".join(f'    "{shape_id}": {json.dumps(points, separators=(",", ":"))}'
                       for shape_id, points in definitions.items())
    config_path.write_text(text + "\n  }\n}\n", encoding="utf-8")
    print(f"Defined centerlines for {len(definitions)} lining shapes")


if __name__ == "__main__":
    main()
