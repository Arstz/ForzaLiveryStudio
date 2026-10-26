"""Restore useful boundary vertices while retaining the region topology."""

import heapq
import itertools
import time

import numpy as np
from shapely.geometry import LineString, Point, Polygon

from fair_curves import line_distance
from rate_distortion import Item


def orientation(points):
    return np.sign(np.sum(points[:, 0] * np.roll(points[:, 1], -1)
                          - points[:, 1] * np.roll(points[:, 0], -1)))


def refine(model, triangle_budget, max_edits=256):
    model.budget = triangle_budget
    queue, serial, edits = [], itertools.count(), []

    def candidate(action):
        index, ring_index, start, end, reference = action
        item = model.items[index]
        if item.geometry.geom_type != 'Polygon':
            return None
        rings = [np.asarray(ring.coords)[:-1] for ring in (item.geometry.exterior, *item.geometry.interiors)]
        if ring_index >= len(rings):
            return None
        points = rings[ring_index]
        matches = np.flatnonzero(np.all(points == start, axis=1)
                                 & np.all(np.roll(points, -1, axis=0) == end, axis=1))
        if not len(matches):
            return None
        edge = int(matches[0])
        coordinates, positions, line, direction = reference
        length = np.linalg.norm(end - start)
        if length < .3 or line.distance(Point(start)) > max(3., length * .1) \
                or line.distance(Point(end)) > max(3., length * .1):
            return None
        a, b = line.project(Point(start)), line.project(Point(end))
        forward = orientation(points) == direction
        span = ((b - a) if forward else (a - b)) % line.length
        if span > max(4., 4 * length):
            return None
        offsets = ((positions - a) if forward else (a - positions)) % line.length
        samples = coordinates[(offsets > 1e-6) & (offsets < span - 1e-6)]
        if not len(samples):
            return None
        distances = line_distance(samples, start, end)
        point = samples[int(np.argmax(distances))]
        if distances.max() < .15 or np.any(np.all(points == point, axis=1)):
            return None
        rings[ring_index] = np.insert(points, edge + 1, point, axis=0)
        geometry = Polygon(rings[0], rings[1:])
        if not geometry.is_valid:
            return None
        result = model.assess({index: Item(item.color, geometry)}, 'reference-vertex', refinement=True)
        if result is None or result['delta'] >= -1e-6 or not model.local_valid(result):
            return None
        result['point'] = point
        return result

    def add(action):
        result = candidate(action)
        if result:
            heapq.heappush(queue, (result['delta'], next(serial), action))

    for index, item in enumerate(model.items[1:], 1):
        if item.geometry.is_empty or item.geometry.geom_type != 'Polygon':
            continue
        matching = []
        for color, shape in model.original_stack[1:]:
            if color != item.color or shape.geom_type != 'Polygon' or not shape.intersects(item.geometry):
                continue
            overlap = shape.intersection(item.geometry).area
            matching.append((overlap / max(1e-8, shape.area + item.geometry.area - overlap), shape))
        if not matching:
            continue
        source = max(matching, key=lambda row: row[0])[1]
        for ring_index, ring in enumerate((item.geometry.exterior, *item.geometry.interiors)):
            if ring_index == 0:
                reference = source.exterior
            elif source.interiors:
                center = Polygon(ring).centroid
                reference = min(source.interiors, key=lambda other: Polygon(other).centroid.distance(center))
            else:
                continue
            coordinates = np.asarray(reference.coords)
            positions = np.r_[0., np.cumsum(np.linalg.norm(np.diff(coordinates, axis=0), axis=1))][:-1]
            data = coordinates[:-1], positions, LineString(coordinates), orientation(coordinates[:-1])
            points = np.asarray(ring.coords)[:-1]
            for start, end in zip(points, np.roll(points, -1, axis=0)):
                add((index, ring_index, start, end, data))
        if index % 16 == 0:
            model.progress({'vertex_candidates': len(queue), 'region': index,
                            'seconds': time.monotonic() - model.started})
    while queue and len(edits) < max_edits:
        _, sequence, action = heapq.heappop(queue)
        result = candidate(action)
        if result is None:
            continue
        if queue and result['delta'] > queue[0][0] + 1e-6:
            heapq.heappush(queue, (result['delta'], sequence, action))
            continue
        index, ring_index, start, end, reference = action
        left, top, right, bottom = result['bounds']
        from rate_distortion import render_items
        model.image[top:bottom, left:right] = render_items(model.items, model.size, result['bounds'], result['changes'])
        item = result['changes'][index]
        item.version = model.items[index].version + 1
        model.items[index] = item
        for key in model.metrics:
            model.metrics[key] += result['after'][key] - result['before'][key]
        model.squared_error += result['delta']
        edits.append({'item': index, 'ring': ring_index, 'point': result['point'].tolist(),
                      'source_error_delta': result['delta'], **model.metrics})
        add((index, ring_index, start, result['point'], reference))
        add((index, ring_index, result['point'], end, reference))
        if len(edits) % 32 == 0:
            model.reconcile()
            model.progress({'restored_vertices': len(edits), **model.metrics,
                            'seconds': time.monotonic() - model.started})
    model.reconcile()
    from simplify import image_error
    return [(item.color, item.geometry) for item in model.items if not item.geometry.is_empty], {
        'geometry_budget': triangle_budget, 'within_geometry_budget': model.metrics['triangle_bound'] <= triangle_budget,
        **model.metrics, 'edits': edits, 'source_error': image_error(model.source, model.image),
        'seconds': time.monotonic() - model.started}
