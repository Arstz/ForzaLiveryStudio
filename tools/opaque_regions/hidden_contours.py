"""Bidirectional contour completion inside an occlusion envelope."""

import math

import numpy as np
import shapely
from shapely.geometry import LineString, Polygon
from shapely.ops import nearest_points, substring

from fair_curves import polygons
from layer_completion import BOOLEAN_GRID, clean, difference_area, filled_holes


def complete_contour(proposal, required, allowed, area_tolerance=1e-6):
    if proposal.is_empty:
        return Polygon() if required.is_empty else required
    shape = clean(shapely.intersection(proposal, allowed, grid_size=BOOLEAN_GRID))
    if not shape.covers(required):
        shape = clean(shapely.union_all([shape, required], grid_size=BOOLEAN_GRID))
    parts = [part for part in polygons(shape) if part.intersection(required).area > 0]
    shape = clean(shapely.union_all(parts)) if parts else Polygon()
    if not shape.is_valid or difference_area(required, shape) > area_tolerance \
            or difference_area(shape, allowed) > area_tolerance:
        raise ValueError('Contour completion changed required territory or escaped its envelope')
    return shape


def shortcut_chains(support, required, allowed, area_tolerance=1e-6):
    completed = []
    for part in polygons(support):
        needed = clean(shapely.intersection(required, part, grid_size=BOOLEAN_GRID))
        needed_parts = np.asarray(polygons(needed), dtype=object)
        needed_tree = shapely.STRtree(needed_parts)
        coordinates = [np.asarray(ring.coords)[:-1] for ring in [part.exterior, *part.interiors]]
        active = [np.ones(len(points), dtype=bool) for points in coordinates]
        current = part
        for ring_index, points in enumerate(coordinates):
            free = shapely.distance(shapely.points(points), needed) > 1e-6
            locked = np.flatnonzero(~free)
            if not len(locked):
                locked = np.array([0, len(points) // 3, 2 * len(points) // 3])
                free[locked] = False
            pivot = int(locked[0])
            sequence = (np.arange(len(points) + 1) + pivot) % len(points)
            boundaries = [position for position in range(len(points) + 1)
                          if position in (0, len(points)) or not free[sequence[position]]]

            def shorten(chain, depth=0):
                nonlocal current
                if len(chain) < 3 or len(points) - np.count_nonzero(~active[ring_index]) - len(chain) + 2 < 3:
                    return
                trial = active[ring_index].copy()
                trial[chain[1:-1]] = False
                rings = [vertices[trial if index == ring_index else active[index]]
                         for index, vertices in enumerate(coordinates)]
                candidate = Polygon(rings[0], rings[1:])
                valid = False
                if candidate.is_valid and not candidate.is_empty:
                    try:
                        swept = clean(Polygon(points[chain]))
                        nearby = needed_parts[needed_tree.query(swept, predicate='intersects')]
                        loss = float(shapely.area(shapely.intersection(nearby, swept,
                            grid_size=BOOLEAN_GRID)).sum()) if len(nearby) else 0.
                        valid = loss <= area_tolerance and difference_area(swept, allowed) <= area_tolerance
                    except shapely.errors.GEOSException:
                        pass
                if valid:
                    active[ring_index][:] = trial
                    current = candidate
                    return
                if len(chain) <= 3:
                    return
                start, end = points[chain[0]], points[chain[-1]]
                chord = end - start
                length = np.linalg.norm(chord)
                delta = points[chain[1:-1]] - start
                distances = np.abs(delta[:, 0] * chord[1] - delta[:, 1] * chord[0]) / max(length, 1e-12)
                split = int(np.argmax(distances)) + 1 if depth < 32 else len(chain) // 2
                shorten(chain[:split + 1], depth + 1)
                shorten(chain[split:], depth + 1)

            for start, stop in zip(boundaries[:-1], boundaries[1:]):
                shorten(sequence[start:stop + 1])
        completed.append(current)
    return complete_contour(clean(shapely.union_all(completed, grid_size=BOOLEAN_GRID)),
                            required, allowed, area_tolerance)


def open_hidden_holes(support, required, covered, score, tolerance=1., area_tolerance=1e-6):
    current = support
    holes = [Polygon(ring) for part in polygons(support) for ring in part.interiors]
    holes.sort(key=lambda hole: (-hole.length, hole.bounds))
    for hole in holes:
        exterior = shapely.union_all([part.exterior for part in polygons(current)])
        if hole.boundary.distance(exterior) <= 1e-7:
            continue
        free = clean(shapely.intersection(current, covered, grid_size=BOOLEAN_GRID))
        routes = []
        for part in polygons(free):
            boundary = LineString(part.exterior.coords)
            first, start = nearest_points(hole.boundary, boundary)
            last, stop = nearest_points(exterior, boundary)
            if first.distance(start) > 1e-7 or last.distance(stop) > 1e-7:
                continue
            a, b = sorted((boundary.project(start), boundary.project(stop)))
            if b - a <= 1e-7:
                continue
            routes.append(LineString([start, stop]))
            routes.append(substring(boundary, a, b))
            points = list(substring(boundary, b, boundary.length).coords) + list(substring(boundary, 0, a).coords)
            if len(points) >= 2:
                routes.append(LineString(points))
        best, old_cost = None, score(current)
        old_holes = sum(len(part.interiors) for part in polygons(current))
        for route in routes:
            for scale in (tolerance, 4 * tolerance, 16 * tolerance):
                line = route.simplify(scale, preserve_topology=False)
                cut = clean(shapely.intersection(line.buffer(scale, quad_segs=1, join_style='mitre'),
                    covered, grid_size=BOOLEAN_GRID))
                candidate = clean(shapely.difference(current, cut, grid_size=BOOLEAN_GRID))
                if sum(len(part.interiors) for part in polygons(candidate)) >= old_holes:
                    continue
                if difference_area(required, candidate) > area_tolerance:
                    continue
                cost = score(candidate)
                if cost < old_cost - 1e-6 and (best is None or cost < best[0]):
                    best = cost, candidate
        if best is not None:
            current = best[1]
    return current


def contour_proposals(support, required, allowed, tolerance=1., area_tolerance=1e-6):
    if tolerance <= 0 or not math.isfinite(tolerance):
        raise ValueError('Contour tolerance must be finite and positive')
    if required.is_empty:
        return [('remove-hidden', Polygon())]
    raw = [('visible', required), ('filled', filled_holes(support)),
           ('hull', required.convex_hull), ('box', required.envelope),
           ('oriented-box', required.minimum_rotated_rectangle)]
    try:
        raw.insert(0, ('hidden-chain-chords', shortcut_chains(support, required, allowed, area_tolerance)))
    except (ValueError, shapely.errors.GEOSException):
        pass
    extent = max(support.bounds[2] - support.bounds[0], support.bounds[3] - support.bounds[1])
    scales = [tolerance * 2 ** index for index in range(8) if tolerance * 2 ** index <= max(tolerance, extent)]
    for scale in scales:
        for preserve in (False, True):
            shape = support.simplify(scale, preserve_topology=preserve)
            raw.append((f'chords-{scale:g}-{int(preserve)}', shape))
        opened = support.buffer(-scale, quad_segs=2, join_style='mitre').buffer(
            scale, quad_segs=2, join_style='mitre')
        raw.append((f'prune-{scale:g}', opened.simplify(scale, preserve_topology=True)))
    found = {}
    for name, proposal in raw:
        try:
            shape = complete_contour(proposal, required, allowed, area_tolerance)
        except (ValueError, shapely.errors.GEOSException):
            continue
        if shape.equals(support):
            continue
        signature = shapely.normalize(shape).wkb
        found.setdefault(signature, (name, shape))
    return list(found.values())
