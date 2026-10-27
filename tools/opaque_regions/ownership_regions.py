"""Construct a paint hierarchy from connected visible ownership and solid outer loops."""

import heapq
import json
from pathlib import Path

import numpy as np
import shapely
from shapely.geometry import Polygon, LineString
from shapely.ops import split

from bottom_up import visible_masks
from fair_curves import polygons
from outer_regions import apply, cost
from paint_stack import canonical_geometry, geometry_metrics, polygon_svg
from rate_distortion import Item, RateDistortion, window_sums
from simplify import image_error, render


def split_support(geometry):
    pending, result = polygons(geometry), []
    while pending:
        shape = pending.pop()
        if not shape.interiors:
            result.append(shape)
            continue
        extent = max(shape.bounds[2] - shape.bounds[0], shape.bounds[3] - shape.bounds[1]) * 4 + 1
        holes = sorted((Polygon(ring) for ring in shape.interiors), key=lambda hole: -hole.area)[:8]
        choices = []
        for hole in holes:
            center = np.array(hole.representative_point().coords[0])
            for angle in (0., np.pi / 4, np.pi / 2, 3 * np.pi / 4):
                offset = extent * np.array([np.cos(angle), np.sin(angle)])
                pieces = polygons(split(shape, LineString([center - offset, center + offset])))
                remaining = sum(len(part.interiors) for part in pieces)
                if len(pieces) < 2 or remaining >= len(shape.interiors):
                    continue
                if shapely.union_all(pieces).symmetric_difference(shape).area > .0001:
                    continue
                score = sum(cost(part) - 256 * len(part.interiors) for part in pieces)
                choices.append((remaining, score, len(pieces), pieces))
        if not choices:
            pending.extend(polygons(shapely.constrained_delaunay_triangles(shape)))
        else:
            pending.extend(min(choices, key=lambda entry: entry[:3])[3])
    return [canonical_geometry(shape) for shape in result]


def rebuild(stack, source, baseline, domain, output, *, source_growth=8., local_growth=32.,
            max_merges=512, tiny_area=16., progress=None):
    output = Path(output)
    output.mkdir(parents=True)
    progress = progress or (lambda entry: None)
    colors, _, _ = visible_masks(stack)
    original_components = [(color, part) for color in sorted(colors) for part in polygons(colors[color])
                           if part.area > .0001]
    solver = RateDistortion(stack, source, domain, local_growth=local_growth,
        reference_image=baseline, reference_stack=stack)
    solver.budget = float('inf')
    global_limit = image_error(source, baseline)['rmse'] + source_growth
    squared_limit = global_limit ** 2 * source.shape[0] * source.shape[1] * 3
    tree = shapely.STRtree([part for _, part in original_components])
    rgb = {color: np.array([int(color[offset:offset + 2], 16) for offset in (1, 3, 5)], dtype=float)
           for color in colors}
    transfers = []
    for index in sorted(range(len(original_components)), key=lambda item: (original_components[item][1].area, item)):
        color, part = original_components[index]
        if part.area > 256:
            break
        choices = []
        for hit in tree.query(part.buffer(.05), predicate='intersects'):
            other, neighbor = original_components[int(hit)]
            if other == color or neighbor.area <= part.area:
                continue
            contrast = float(np.sqrt(np.square(rgb[color] - rgb[other]).mean()))
            if contrast <= 48 or part.area <= tiny_area:
                choices.append((contrast, -neighbor.area, other))
        if not choices:
            continue
        slot = len(solver.items)
        solver.items.append(Item(color, Polygon()))
        candidates = []
        for contrast, area, other in sorted(set(choices)):
            candidate = solver.assess({slot: Item(other, part)}, 'visible-component-relabel', refinement=True)
            if candidate is not None and solver.squared_error + candidate['delta'] <= squared_limit \
                    and solver.local_valid(candidate):
                candidates.append((candidate['delta'], contrast, area, other, candidate))
        if not candidates:
            continue
        delta, contrast, _, other, candidate = min(candidates, key=lambda entry: entry[:4])
        details = {'from': color, 'to': other, 'area': part.area,
                   'color_distance': contrast, 'source_error_delta': delta}
        apply(solver, candidate, details)
        colors[color] = colors[color].difference(part)
        colors[other] = shapely.union_all([colors[other], part])
        transfers.append(details)
    components = [(color, canonical_geometry(part)) for color in sorted(colors)
                  for part in polygons(shapely.set_precision(colors[color], 1e-6)) if part.area > .0001]
    required = np.array([part for _, part in components], dtype=object)
    fixed_colors = {color: shapely.union_all([part for fill, part in components if fill == color])
                    for color in colors}
    snapping_error = sum(fixed_colors[color].symmetric_difference(shape).area for color, shape in colors.items())
    tree = shapely.STRtree(required)
    touched_cache = {}

    def touched(shape):
        key = shape.wkb
        if key not in touched_cache:
            candidates = tree.query(shape, predicate='intersects')
            areas = shapely.area(shapely.intersection(required[candidates], shape, grid_size=1e-6))
            touched_cache[key] = [int(owner) for owner in candidates[areas > .0001]]
        return touched_cache[key]

    def ordered(scene):
        assignment = {owner: key for key, item in scene.items() for owner in item['owners']}
        outgoing, incoming = {key: set() for key in scene}, {key: 0 for key in scene}
        for key, item in scene.items():
            for owner in touched(item['support']):
                other = assignment[owner]
                if other != key and scene[other]['color'] != item['color'] and other not in outgoing[key]:
                    outgoing[key].add(other)
                    incoming[other] += 1
        ready = [(cost(scene[key]['support']), key) for key in scene if not incoming[key]]
        heapq.heapify(ready)
        order = []
        while ready:
            _, key = heapq.heappop(ready)
            order.append(key)
            for other in sorted(outgoing[key]):
                incoming[other] -= 1
                if not incoming[other]:
                    heapq.heappush(ready, (cost(scene[other]['support']), other))
        return order if len(order) == len(scene) else None

    layouts = []
    for strategy in ('solid-outer', 'open-pieces'):
        items = {-1: {'color': stack[0][0], 'support': domain, 'owners': frozenset()}}
        for index, (color, part) in enumerate(components):
            filled = Polygon(part.exterior)
            support = filled if strategy == 'solid-outer' and filled.difference(domain).area <= .0001 else part
            items[index] = {'color': color, 'support': support, 'owners': frozenset([index])}
        if ordered(items) is None:
            raise ValueError('Connected ownership produced a dependency cycle')
        queue, serial, generated = [], 0, set()

        def planning_cost(shape):
            holes = sum(len(part.interiors) for part in polygons(shape))
            return cost(shape) - (224 * holes if strategy == 'open-pieces' else 0)

        def propose(members):
            nonlocal serial
            members = tuple(sorted(members))
            signature = tuple(items[key]['owners'] for key in members)
            if signature in generated:
                return
            generated.add(signature)
            merged = shapely.union_all([items[key]['support'] for key in members])
            owners = frozenset().union(*(items[key]['owners'] for key in members))
            core = shapely.union_all(required[list(owners)]) if owners else Polygon()
            old_cost = sum(planning_cost(items[key]['support']) for key in members)
            assignment = {owner: key for key, item in items.items() for owner in item['owners']}
            for shape in (merged, merged.convex_hull, merged.envelope, merged.minimum_rotated_rectangle):
                shape = canonical_geometry(shapely.set_precision(shape, 1e-6))
                saving = old_cost - planning_cost(shape)
                if saving <= 0 or shape.difference(domain).area > .0001 or core.difference(shape).area > .0001:
                    continue
                foreign = {assignment[owner] for owner in touched(shape)
                           if items[assignment[owner]]['color'] != items[members[0]]['color']}
                priority = saving / (1 + len(foreign)) ** 1.5 if strategy == 'open-pieces' else saving
                heapq.heappush(queue, (-priority, shape.area - merged.area, serial,
                    members, signature, shape))
                serial += 1

        groups = {}
        for key, item in items.items():
            groups.setdefault(item['color'], []).append(key)
            propose([key])
        for members in groups.values():
            if len(members) > 1:
                propose(members)
            for key in members:
                nearest = sorted((other for other in members if other != key),
                    key=lambda other: (items[key]['support'].distance(items[other]['support']), other))[:2]
                for other in nearest:
                    propose([key, other])
        progress({'ownership_layout': strategy, 'ownership_components_before': len(original_components),
                  'after_color_relabel': len(components), 'relabeled_components': len(transfers),
                  'completion_proposals': len(queue)})
        history, rejected = [], 0
        while queue and len(history) < max_merges:
            _, added, _, members, signature, shape = heapq.heappop(queue)
            if any(key not in items for key in members) or tuple(items[key]['owners'] for key in members) != signature:
                continue
            saving = sum(planning_cost(items[key]['support']) for key in members) - planning_cost(shape)
            if saving <= 0:
                continue
            owners = frozenset().union(*(items[key]['owners'] for key in members))
            candidate = {key: item for key, item in items.items() if key not in members}
            key = members[0]
            candidate[key] = {'color': items[key]['color'], 'support': shape, 'owners': owners}
            if ordered(candidate) is None:
                rejected += 1
                continue
            items = candidate
            history.append({'members': members, 'planning_reduction': saving,
                            'draw_items': len(items), 'added_area': added})
            propose([key])
            for other, item in items.items():
                if other != key and item['color'] == items[key]['color']:
                    propose([key, other])
            if len(history) % 16 == 0:
                progress({'ownership_layout': strategy, 'ownership_completions': len(history),
                          'draw_items': len(items), 'cycle_rejections': rejected})
        result = []
        for key in ordered(items):
            item = items[key]
            pieces = [item['support']] if key == -1 else split_support(item['support'])
            result.extend((item['color'], piece) for piece in pieces)
        image = render(polygon_svg(result, solver.size), solver.size)
        error = image_error(source, image)
        squared = np.square(image[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
        actual_colors, _, _ = visible_masks(result)
        color_difference = sum(actual_colors.get(color, Polygon()).symmetric_difference(shape).area
                               for color, shape in fixed_colors.items())
        source_passed = bool(error['rmse'] <= global_limit
            and np.all(window_sums(squared, solver.window) <= solver.local_limit + 1e-5))
        accepted = bool(error['rmse'] <= global_limit and image_error(baseline, image)['alpha_max_error'] == 0
            and color_difference <= .0001 * max(1, len(components)))
        details = {'strategy': strategy, 'accepted': bool(accepted), 'draw_items': len(result),
            'source_limits_passed': source_passed, 'requires_final_source_validation': True,
            'visible_color_difference_area': color_difference,
            'ownership_snapping_difference_area': snapping_error,
            'source_error': error, 'cost': sum(cost(shape) for _, shape in result),
            'completions': history, 'cycle_rejections': rejected,
            'geometry': {name: sum(geometry_metrics(shape)[name] for _, shape in result)
                         for name in ('regions', 'holes', 'points', 'triangle_bound')}}
        (output / f'{strategy}.svg').write_bytes(polygon_svg(result, solver.size))
        layouts.append((result, details))
        progress({'ownership_layout_result': details['strategy'], 'accepted': accepted,
                  'geometry': details['geometry'], 'source_error': error})
    result, selected = min(layouts, key=lambda entry: (not entry[1]['accepted'], entry[1]['cost']))
    accepted = selected['accepted']
    report = {'method': 'Connected visible ownership, solid outer contours and expansion dependencies',
        'accepted': bool(accepted), 'visible_components_before': len(original_components),
        'visible_components_after_relabel': len(components), 'draw_items': len(result),
        'island_transfers': transfers, 'selected_strategy': selected['strategy'],
        'layouts': [entry[1] for entry in layouts], 'source_error': selected['source_error'],
        'geometry': {name: sum(geometry_metrics(shape)[name] for _, shape in result)
                     for name in ('regions', 'holes', 'points', 'triangle_bound')}}
    (output / 'report.json').write_text(json.dumps(report, indent=2))
    (output / 'candidate.svg').write_bytes(polygon_svg(result, solver.size))
    return (result if accepted else stack), report
