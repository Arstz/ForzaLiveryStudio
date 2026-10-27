"""Polygon costs and internal contour encoding for opaque paint stacks."""

from fair_curves import polygons
import heapq

import numpy as np
import shapely
from shapely.geometry import MultiPolygon, Polygon


DEFAULT_INTERSECTION_AREA = .01
DEFAULT_NATIVE_SHAPE_WEIGHT = 4.


def canonical_geometry(geometry):
    parts = []
    for part in polygons(geometry.simplify(0, preserve_topology=True)):
        rings = []
        for ring in (part.exterior, *part.interiors):
            points = np.asarray(ring.coords)[:-1]
            while True:
                incoming, outgoing = points - np.roll(points, 1, axis=0), np.roll(points, -1, axis=0) - points
                cross = incoming[:, 0] * outgoing[:, 1] - incoming[:, 1] * outgoing[:, 0]
                scale = np.linalg.norm(incoming, axis=1) + np.linalg.norm(outgoing, axis=1)
                keep = (np.abs(cross) > 1e-8 * scale) | (np.sum(incoming * outgoing, axis=1) < 0)
                count = np.count_nonzero(keep)
                if count < 3 or count == len(points):
                    break
                points = points[keep]
            rings.append(points)
        parts.append(Polygon(rings[0], rings[1:]))
    return parts[0] if len(parts) == 1 else MultiPolygon(parts) if parts else Polygon()


def geometry_metrics(geometry):
    geometry = canonical_geometry(geometry)
    parts = polygons(geometry)
    holes = sum(len(part.interiors) for part in parts)
    points = sum(len(ring.coords) - 1 for part in parts for ring in (part.exterior, *part.interiors))
    triangles = points + 2 * holes - 2 * len(parts)
    rectangles = sum(not part.interiors and len(part.exterior.coords) == 5
                     and part.symmetric_difference(part.minimum_rotated_rectangle).area < 1e-6 for part in parts)
    return {'regions': len(parts), 'holes': holes, 'points': points,
            'triangle_bound': triangles - rectangles,
            'cost': triangles - rectangles + 32 * len(parts) + 64 * holes}


def polygon_svg(stack, size):
    paths = []
    for color, geometry in stack:
        geometry = canonical_geometry(geometry)
        for part in polygons(geometry):
            commands = []
            for ring in (part.exterior, *part.interiors):
                points = list(ring.coords)[:-1]
                commands.append('M' + 'L'.join(f'{x:.8f} {y:.8f}' for x, y in points) + 'Z')
            paths.append(f'<path fill="{color}" fill-rule="evenodd" d="{"".join(commands)}"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size[0]}" height="{size[1]}" '
            f'viewBox="0 0 {size[0]} {size[1]}">' + ''.join(paths) + '</svg>').encode()


def order_stack(stack, area_tolerance=DEFAULT_INTERSECTION_AREA, ownership_grid=1e-6, retain_input_order=False):
    stack = [(color, canonical_geometry(part)) for color, shape in stack for part in polygons(shape)]
    working = [shapely.set_precision(shape, ownership_grid) for _, shape in stack]
    required = [Polygon() for _ in stack]
    covered = Polygon()
    for index in reversed(range(len(stack))):
        shape = working[index]
        required[index] = shape.difference(covered)
        covered = shapely.union_all([covered, shape], grid_size=ownership_grid)
    tree = shapely.STRtree(required)
    edges = set()
    for index, (color, shape) in enumerate(stack):
        added = working[index].difference(required[index])
        for owner in tree.query(added, predicate='intersects'):
            owner = int(owner)
            if owner != index and color != stack[owner][0] \
                    and added.intersection(required[owner]).area > area_tolerance:
                edges.add((index, owner))
    incoming = [0] * len(stack)
    outgoing = [[] for _ in stack]
    for first, last in sorted(edges):
        incoming[last] += 1
        outgoing[first].append(last)
    costs = [geometry_metrics(shape) for _, shape in stack]
    def priority(index):
        metric = costs[index]
        return (index if retain_input_order else metric['cost']), metric['points'], metric['holes'], index
    ready = [priority(index) for index, degree in enumerate(incoming) if not degree]
    heapq.heapify(ready)
    order = []
    while ready:
        *_, index = heapq.heappop(ready)
        order.append(index)
        for other in outgoing[index]:
            incoming[other] -= 1
            if not incoming[other]:
                heapq.heappush(ready, priority(other))
    if len(order) != len(stack):
        backward = [(first, last) for first, last in sorted(edges) if first > last]
        raise ValueError(f'Fixed ownership produced an ordering cycle; backward edges: {backward}')
    result = [stack[index] for index in order]
    report = {'fixed_ownership': True, 'area_tolerance': area_tolerance, 'ownership_grid': ownership_grid,
        'retained_input_order': retain_input_order,
        'original_order': list(range(len(stack))), 'order': order, 'edges': sorted(edges),
        'appearance_groups': len({color for color, _ in stack}), 'draw_items': len(stack),
        'layers': [{'source_index': index, 'fill': stack[index][0], **costs[index],
            'support_area': stack[index][1].area, 'visible_area': required[index].area} for index in order]}
    return result, report
