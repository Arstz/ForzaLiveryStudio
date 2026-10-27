"""Rebuild filled region contours inside fixed visible-ownership constraints."""

import json
import heapq
from functools import lru_cache
from pathlib import Path
import subprocess
import time

import numpy as np
import cv2
import shapely
from shapely.geometry import Polygon, box

from fair_curves import polygons
from paint_stack import canonical_geometry, geometry_metrics, polygon_svg
from rate_distortion import Item, RateDistortion, crop_bounds, render_items, window_sums
from simplify import image_error, render


def rings(geometry):
    geometry = shapely.orient_polygons(shapely.set_precision(geometry, 1e-6))
    return [list(ring.coords)[:-1] for part in polygons(geometry)
            for ring in (part.exterior, *part.interiors)]


def from_rings(values):
    outer, holes = [], []
    for points in values:
        shape = Polygon(points)
        (outer if shape.exterior.is_ccw else holes).append(shape)
    assigned = [[] for _ in outer]
    for hole in holes:
        parents = [index for index, shape in enumerate(outer) if shape.covers(hole)]
        if not parents:
            raise ValueError('A native contour hole has no enclosing outer boundary')
        assigned[min(parents, key=lambda index: outer[index].area)].append(hole.exterior.coords)
    return canonical_geometry(shapely.union_all([
        Polygon(shape.exterior, assigned[index]) for index, shape in enumerate(outer)])) if outer else Polygon()


@lru_cache(maxsize=8192)
def cost(geometry):
    total = 0.
    for part in polygons(shapely.orient_polygons(canonical_geometry(geometry))):
        for hole, ring in enumerate((part.exterior, *part.interiors)):
            points = np.asarray(ring.coords)[:-1]
            incoming, outgoing = points - np.roll(points, 1, axis=0), np.roll(points, -1, axis=0) - points
            cross = incoming[:, 0] * outgoing[:, 1] - incoming[:, 1] * outgoing[:, 0]
            total += len(points) + (256 if hole else 32)
            total += 2 * np.count_nonzero(cross * (-1 if hole else 1) < -1e-6)
    return float(total)


def simplify_batch(tool, constraints, output):
    output = Path(output)
    output.mkdir(parents=True)
    manifest = output / 'constraints.json'
    manifest.write_text(json.dumps({'version': 1, 'regions': constraints}))
    result = subprocess.run([str(tool), str(manifest), '--simplify-fill-contours',
        '--area-tolerance', '.0001', '--output', str(output)], capture_output=True, text=True)
    (output / 'stderr.log').write_text(result.stderr)
    if result.returncode:
        raise ValueError('Native outer-contour simplification failed: ' + result.stderr[-2000:])
    return json.loads((output / 'outer_contours.json').read_text())


def insignificant_components(solver, index, required):
    item = solver.items[index]
    selected = []
    for part in polygons(required):
        if part.area > 256:
            continue
        bounds = crop_bounds(part.bounds, solver.size)
        left, top, right, bottom = bounds
        if left >= right or top >= bottom:
            continue
        underneath = render_items(solver.items, solver.size, bounds,
                                  {index: Item(item.color, Polygon())})
        current = solver.image[top:bottom, left:right]
        x, y = np.meshgrid(np.arange(left, right) + .5, np.arange(top, bottom) + .5)
        mask = shapely.contains_xy(part, x, y)
        if not np.any(mask):
            selected.append(part)
            continue
        source = solver.source[top:bottom, left:right, :3].astype(float)
        delta = (np.square(underneath[:, :, :3].astype(float) - source)
                 - np.square(current[:, :, :3].astype(float) - source)).sum(axis=2)
        impact = float(delta[mask].sum())
        contrast = float(np.sqrt(np.square(underneath[:, :, :3].astype(float)
                            - current[:, :, :3].astype(float))[mask].mean()))
        if impact <= 0 or contrast <= 32:
            selected.append(part)
    return selected


def constraint(key, support, required, allowed):
    seeds = []
    if not required.is_empty:
        seeds += [required.convex_hull, required.envelope, required.minimum_rotated_rectangle,
                  shapely.union_all([Polygon(part.exterior) for part in polygons(required)])]
    seeds += [shapely.union_all([Polygon(part.exterior) for part in polygons(support)])]
    for tolerance in (.5, 2., 8., 32.):
        seeds.append(support.simplify(tolerance, preserve_topology=False))
    return {'key': key, 'support': rings(support), 'required': rings(required),
            'allowed': rings(allowed), 'seeds': [rings(seed) for seed in seeds if seed.is_valid]}


def source_locks(solver):
    squared = np.square(solver.image[:, :, :3].astype(float) - solver.source[:, :, :3]).sum(axis=2)
    scale = 3 * solver.window ** 2
    current = np.sqrt(window_sums(squared, solver.window) / scale)
    permitted = np.sqrt(solver.local_limit / scale)
    risky = (permitted - current < 4.).astype(np.uint8)
    count, _, stats, _ = cv2.connectedComponentsWithStats(risky, connectivity=8)
    return shapely.union_all([box(max(0, x - 2), max(0, y - 2),
        min(solver.size[0], x + width + solver.window + 2),
        min(solver.size[1], y + height + solver.window + 2))
        for x, y, width, height, _ in stats[1:count]]) if count > 1 else Polygon()


def apply(solver, candidate, details):
    left, top, right, bottom = candidate['bounds']
    solver.image[top:bottom, left:right] = render_items(
        solver.items, solver.size, candidate['bounds'], candidate['changes'])
    solver.squared_error += candidate['delta']
    for index, item in candidate['changes'].items():
        item.version = solver.items[index].version + 1
        solver.items[index] = item
    for key in solver.metrics:
        solver.metrics[key] += candidate['after'][key] - candidate['before'][key]
    solver.edits.append(details)
    solver.visibility_cache = None


def transfer_weak_islands(solver, source_limit, tool, output):
    _, owned = solver.visibility()
    positions = sorted(owned)
    tree = shapely.STRtree([owned[index] for index in positions])
    colors = {index: np.array([int(solver.items[index].color[offset:offset + 2], 16)
                              for offset in (1, 3, 5)], dtype=float) for index in positions}
    upper = {}
    covered = Polygon()
    for index in reversed(range(len(solver.items))):
        upper[index] = covered
        covered = shapely.union_all([covered, solver.items[index].geometry])

    def receiver_shape(index, additions):
        original = solver.items[index].geometry
        geometry = shapely.union_all([original, additions], grid_size=1e-6)
        old, new = geometry_metrics(original), geometry_metrics(geometry)
        if new['holes'] > old['holes']:
            filled = shapely.union_all([Polygon(part.exterior) for part in polygons(geometry)])
            if filled.difference(geometry).difference(upper[index]).area > .0001:
                return None
            geometry = filled
            new = geometry_metrics(geometry)
        if new['holes'] > old['holes'] or new['regions'] > old['regions']:
            return None
        return geometry

    bundles, constraints = {}, []
    for index in positions:
        if not index:
            continue
        parts = polygons(owned[index])
        eligible = []
        for part in parts:
            if part.area > 256:
                continue
            neighbors = []
            for hit in tree.query(part.buffer(.05), predicate='intersects'):
                recipient = positions[int(hit)]
                if recipient <= index:
                    continue
                contrast = float(np.sqrt(np.square(colors[index] - colors[recipient]).mean()))
                if contrast <= 48:
                    geometry = receiver_shape(recipient, part)
                    if geometry is not None:
                        extra = cost(geometry) - cost(solver.items[recipient].geometry)
                        neighbors.append((extra, contrast, recipient))
            if neighbors:
                _, contrast, recipient = min(neighbors)
                eligible.append((part, recipient, contrast))
        signatures = set()
        for threshold in (4., 16., 64., 256.):
            patches, chosen = {}, []
            for part, recipient, contrast in eligible:
                if part.area > threshold:
                    continue
                addition = shapely.union_all([patches.get(recipient, Polygon()), part])
                if receiver_shape(recipient, addition) is None:
                    continue
                patches[recipient] = addition
                chosen.append((part, recipient, contrast))
            if not chosen:
                continue
            signature = tuple(part.wkb for part, _, _ in chosen)
            if signature in signatures:
                continue
            signatures.add(signature)
            key = f'{index}:transfer-{threshold:g}'
            core = owned[index].difference(shapely.union_all([part for part, _, _ in chosen]))
            constraints.append(constraint(key, solver.items[index].geometry, core,
                shapely.union_all([solver.items[index].geometry, upper[index]])))
            bundles[key] = index, patches, chosen
    if not constraints:
        return []
    proposals = simplify_batch(tool, constraints, output)
    by_donor = {}
    for row in proposals['regions']:
        index, patches, chosen = bundles[row['key']]
        for candidate in row['candidates']:
            by_donor.setdefault(index, []).append((from_rings(candidate['rings']), patches, chosen))
    history = []
    incoming = set()
    for index in sorted(by_donor):
        if index in incoming:
            continue
        accepted = []
        for geometry, patches, chosen in by_donor[index]:
            changes = {index: Item(solver.items[index].color, geometry)}
            for recipient, addition in patches.items():
                shape = receiver_shape(recipient, addition)
                if shape is None:
                    break
                changes[recipient] = Item(solver.items[recipient].color, shape)
            if len(changes) != len(patches) + 1:
                continue
            saving = sum(cost(solver.items[key].geometry) - cost(item.geometry) for key, item in changes.items())
            if saving <= 0 or sum(item.metrics['holes'] for item in changes.values()) \
                    > sum(solver.items[key].metrics['holes'] for key in changes):
                continue
            candidate = solver.assess(changes, 'joint-neighbor-island-reconstruction', refinement=True)
            if candidate is None or solver.squared_error + candidate['delta'] > source_limit \
                    or not solver.local_valid(candidate):
                continue
            accepted.append((-saving, candidate['delta'], candidate, chosen, patches))
        if not accepted:
            continue
        negative_saving, _, candidate, chosen, patches = min(accepted, key=lambda row: row[:2])
        details = {'from': index, 'to': sorted(patches), 'islands': len(chosen),
                   'area': sum(part.area for part, _, _ in chosen), 'complexity_saved': -negative_saving,
                   'source_error_delta': candidate['delta']}
        apply(solver, candidate, details)
        history.append(details)
        incoming.update(patches)
    return history


def regroup(stack, source, baseline, domain, *, source_growth=8., local_growth=32.,
            max_edits=64, progress=None):
    from bottom_up import visible_masks
    progress = progress or (lambda entry: None)
    _, original, _ = visible_masks(stack)
    required = [shapely.set_precision(shape, 1e-6) for shape in original]
    tree = shapely.STRtree(required)
    items = {index: {'color': color, 'support': shape, 'owners': frozenset([index])}
             for index, (color, shape) in enumerate(stack)}
    size = source.shape[1], source.shape[0]
    window = min(16, *size)
    reference_error = np.square(baseline[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
    local_limit = np.sqrt(window_sums(reference_error, window) / (3 * window ** 2)) + local_growth
    initial = render(polygon_svg(stack, size), size)
    initial_error = np.square(initial[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
    local_limit = np.maximum(local_limit, np.sqrt(window_sums(initial_error, window) / (3 * window ** 2)))
    global_limit = image_error(source, baseline)['rmse'] + source_growth
    history, rejected = [], {}
    intersections = {}

    def touched(shape):
        key = shape.wkb
        if key not in intersections:
            working = shapely.set_precision(shape, 1e-6)
            intersections[key] = [int(owner) for owner in tree.query(working, predicate='intersects')
                if working.intersection(required[int(owner)]).area > .0001]
        return intersections[key]

    def ordered(scene):
        assignment = {owner: key for key, item in scene.items() for owner in item['owners']}
        edges = set()
        for key, item in scene.items():
            for owner in touched(item['support']):
                other = assignment[owner]
                if other != key and scene[other]['color'] != item['color']:
                    edges.add((key, other))
        outgoing = {key: [] for key in scene}
        incoming = {key: 0 for key in scene}
        for first, last in sorted(edges):
            outgoing[first].append(last)
            incoming[last] += 1
        ready = [(cost(item['support']), key) for key, item in scene.items() if not incoming[key]]
        heapq.heapify(ready)
        order = []
        while ready:
            _, key = heapq.heappop(ready)
            order.append(key)
            for other in outgoing[key]:
                incoming[other] -= 1
                if not incoming[other]:
                    heapq.heappush(ready, (cost(scene[other]['support']), other))
        if len(order) != len(scene):
            return None
        return [(scene[key]['color'], scene[key]['support']) for key in order]

    @lru_cache(maxsize=2048)
    def seeds(shape):
        return [shape.convex_hull, shape.envelope, shape.minimum_rotated_rectangle,
                shapely.union_all([Polygon(part.exterior) for part in polygons(shape)]), shape]

    @lru_cache(maxsize=2048)
    def ownership(owners):
        return shapely.union_all([required[index] for index in sorted(owners)])

    def transaction(members, support, name):
        owners = frozenset().union(*(items[key]['owners'] for key in members))
        core = ownership(owners)
        if not support.is_valid or support.difference(domain).area > 1e-6 \
                or core.difference(support).area > .0001:
            return None
        color = items[members[0]]['color']
        scene = {key: dict(item) for key, item in items.items() if key not in members}
        scene[members[0]] = {'color': color, 'support': support, 'owners': owners}
        for key, item in list(scene.items()):
            if key == 0 or key == members[0] or item['color'] == color:
                continue
            own = ownership(item['owners'])
            if own.intersection(support).area <= .0001 or item['support'].intersection(core).area <= .0001:
                continue
            alternatives = [shape for shape in seeds(own) if shape.is_valid
                and shape.difference(domain).area <= 1e-6
                and shape.intersection(core).area <= .0001 and own.difference(shape).area <= .0001]
            if alternatives:
                item['support'] = min(alternatives, key=lambda shape: (cost(shape), shape.area, shape.wkb))
        saving = sum(cost(item['support']) for item in items.values()) \
            - sum(cost(item['support']) for item in scene.values())
        if saving <= 0:
            return None
        candidate = ordered(scene)
        if candidate is None:
            rejected['dependency_cycle'] = rejected.get('dependency_cycle', 0) + 1
            return None
        return saving, name, members, scene, candidate

    for iteration in range(max_edits):
        proposals = []
        colors = sorted({item['color'] for item in items.values()})
        for color in colors:
            members = [key for key, item in items.items() if item['color'] == color and key != 0]
            groups = [members] if len(members) > 1 else []
            pairs = set()
            for key in members:
                neighbors = sorted((other for other in members if other != key),
                    key=lambda other: (items[key]['support'].distance(items[other]['support']), other))[:2]
                pairs.update(tuple(sorted((key, other))) for other in neighbors)
            groups += [list(pair) for pair in sorted(pairs) if list(pair) not in groups]
            for group in groups:
                core = ownership(frozenset().union(*(items[key]['owners'] for key in group)))
                for variant, shape in enumerate(seeds(core)[:4]):
                    proposal = transaction(group, shape, f'same-fill-outer-{variant}')
                    if proposal:
                        proposals.append(proposal)
        for key, item in items.items():
            if key == 0 or not any(part.interiors for part in polygons(item['support'])):
                continue
            filled = shapely.union_all([Polygon(part.exterior) for part in polygons(item['support'])])
            proposal = transaction([key], filled, 'outer-only-and-promote-owners')
            if proposal:
                proposals.append(proposal)
        if not proposals:
            break
        selected = None
        for proposal in sorted(proposals, key=lambda entry: (-entry[0], entry[1], entry[2])):
            saving, name, members, scene, candidate = proposal
            image = render(polygon_svg(candidate, size), size)
            error = image_error(source, image)
            squared = np.square(image[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
            if error['rmse'] <= global_limit and error['alpha_max_error'] == 0 \
                    and np.all(np.sqrt(window_sums(squared, window) / (3 * window ** 2)) <= local_limit + 1e-5):
                selected = proposal
                break
            rejected['source_error'] = rejected.get('source_error', 0) + 1
        if selected is None:
            break
        saving, name, members, items, candidate = selected
        details = {'kind': name, 'merged_positions': members, 'complexity_saved': saving,
                   'draw_items': len(candidate), 'points': sum(geometry_metrics(shape)['points'] for _, shape in candidate)}
        history.append(details)
        progress({'outer_group_transaction': details})
    result = ordered(items)
    if result is None:
        raise ValueError('Outer-region grouping lost its fixed ownership order')
    return result, {'edits': history, 'rejected': rejected, 'fixed_original_ownership': True,
                    'draw_items_before': len(stack), 'draw_items_after': len(result)}


def run(tool, stack, source, baseline, domain, output, *, passes=2, boundary_tolerance=2.,
        source_growth=8., local_growth=32., progress=None, ownership=True):
    started = time.monotonic()
    output = Path(output)
    output.mkdir(parents=True)
    progress = progress or (lambda entry: None)
    before = [(color, shape) for color, geometry in stack for shape in polygons(geometry)]
    reconstruction = None
    if ownership and passes:
        from ownership_regions import rebuild
        stack, reconstruction = rebuild(before, source, baseline, domain, output / 'ownership',
            source_growth=source_growth, local_growth=local_growth, progress=progress)
    solver = RateDistortion(stack, source, domain, local_growth=local_growth,
        reference_image=baseline, reference_stack=stack, topology_transactions=True, complete_background=False)
    solver.budget = float('inf')
    strict_local_limit = solver.local_limit.copy()
    provisional = bool(reconstruction and reconstruction['accepted'])
    if reconstruction and reconstruction['accepted']:
        initial_error = np.square(solver.image[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
        solver.local_limit = np.maximum(solver.local_limit, window_sums(initial_error, solver.window))
    source_limit = (image_error(source, baseline)['rmse'] + source_growth) ** 2 * source.shape[0] * source.shape[1] * 3
    history = []
    transfers = transfer_weak_islands(solver, source_limit, tool, output / 'island_transfers') \
        if passes and not ownership else []
    progress({'neighbor_island_transfers': sum(row['islands'] for row in transfers),
              'joint_transactions': len(transfers), 'transferred_area': sum(row['area'] for row in transfers)})
    grouping = []
    for iteration in range(passes):
        _, owned = solver.visibility()
        locked = source_locks(solver)
        upper = Polygon()
        constraints = []
        removed = {}
        for index in reversed(range(1, len(solver.items))):
            item = solver.items[index]
            if item.geometry.is_empty:
                continue
            required = owned.get(index, Polygon())
            allowed = shapely.union_all([item.geometry, upper]).intersection(domain)
            constraints.append(constraint(f'{index}:0', item.geometry, required, allowed))
            insignificant = insignificant_components(solver, index, required)
            signatures = {required.wkb}
            for threshold in (4., 16., 64., 256.):
                omitted = [part for part in insignificant if part.area <= threshold]
                if not omitted:
                    continue
                core = required.difference(shapely.union_all(omitted))
                if core.wkb in signatures:
                    continue
                signatures.add(core.wkb)
                key = f'{index}:{threshold:g}'
                constraints.append(constraint(key, item.geometry, core, allowed))
                removed[key] = {'eligible_islands': len(omitted), 'eligible_area': sum(part.area for part in omitted)}
            if boundary_tolerance:
                protected = [part for part in polygons(required)
                    if part.buffer(-boundary_tolerance, quad_segs=1, join_style='mitre', mitre_limit=2).is_empty
                    and not any(part.equals(noise) for noise in insignificant)]
                for tolerance in (boundary_tolerance * .25, boundary_tolerance * .5, boundary_tolerance):
                    core = required.buffer(-tolerance, quad_segs=1, join_style='mitre', mitre_limit=2)
                    core = shapely.union_all([core, required.intersection(locked), *protected])
                    envelope = allowed.buffer(tolerance, quad_segs=1, join_style='mitre', mitre_limit=2).intersection(domain)
                    envelope = envelope.difference(locked.difference(allowed))
                    key = f'{index}:boundary-{tolerance:g}'
                    constraints.append(constraint(key, item.geometry, core, envelope))
            upper = shapely.union_all([upper, item.geometry])
        progress({'outer_contour_pass': iteration, 'constraint_variants': len(constraints),
                  'locked_source_area': locked.area})
        proposals = simplify_batch(tool, constraints, output / f'pass_{iteration}')
        by_index = {}
        for row in proposals['regions']:
            index = int(row['key'].split(':')[0])
            for entry in row['candidates']:
                geometry = from_rings(entry['rings'])
                if not geometry.is_valid or geometry.difference(domain).area > 1e-6 \
                        or geometry_metrics(geometry)['holes'] > solver.items[index].metrics['holes']:
                    continue
                saving = cost(solver.items[index].geometry) - cost(geometry)
                if saving <= 0:
                    continue
                by_index.setdefault(index, []).append((cost(geometry), row['key'], geometry, saving))
        accepted = 0
        for index in sorted(by_index):
            choices = []
            for target_cost, key, geometry, saving in by_index[index]:
                candidate = solver.assess({index: Item(solver.items[index].color, geometry)},
                                          'outer-region-reconstruction', refinement=True)
                if candidate is None or solver.squared_error + candidate['delta'] > source_limit \
                        or not solver.local_valid(candidate):
                    continue
                choices.append((target_cost, candidate['delta'], key, saving, candidate))
            if not choices:
                continue
            target_cost, delta, key, saving, candidate = min(choices, key=lambda entry: entry[:3])
            details = {'region': index, 'constraint': key, 'complexity_saved': saving,
                       'source_error_delta': delta, **removed.get(key, {})}
            apply(solver, candidate, details)
            history.append(details)
            accepted += 1
        solver.reconcile()
        progress({'outer_contours_changed': accepted, 'pass': iteration,
                  'points': solver.metrics['points'], 'holes': solver.metrics['holes'],
                  'source_error': image_error(source, solver.image)})
        (output / 'progress.json').write_text(json.dumps({'pass': iteration, 'edits': history,
            'source_error': image_error(source, solver.image)}, indent=2))
        current_stack = [(item.color, item.geometry) for item in solver.items if not item.geometry.is_empty]
        grouped, group_report = regroup(current_stack, source, baseline, domain,
            source_growth=source_growth, local_growth=local_growth, max_edits=32, progress=progress)
        grouping.append(group_report)
        if group_report['edits']:
            solver = RateDistortion(grouped, source, domain, local_growth=local_growth,
                reference_image=baseline, reference_stack=grouped, topology_transactions=True, complete_background=False)
            solver.budget = float('inf')
            if provisional:
                initial_error = np.square(solver.image[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
                solver.local_limit = np.maximum(solver.local_limit, window_sums(initial_error, solver.window))
        if not accepted and not group_report['edits']:
            break
    result = [(item.color, item.geometry) for item in solver.items if not item.geometry.is_empty]
    data = polygon_svg(result, solver.size)
    actual = render(data, solver.size)
    errors = np.square(actual[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
    actual_windows = window_sums(errors, solver.window)
    strict_local_passed = bool(np.all(actual_windows <= strict_local_limit + 1e-5))
    planning_local_passed = bool(np.all(actual_windows <= solver.local_limit + 1e-5))
    source_error = image_error(source, actual)
    valid = float(errors.sum()) <= source_limit + 1e-5 \
        and (planning_local_passed or provisional) \
        and image_error(baseline, actual)['alpha_max_error'] == 0
    (output / 'candidate.svg').write_bytes(data)
    if not valid:
        result = before
    proxy_reduced = sum(cost(shape) for _, shape in result) <= sum(cost(shape) for _, shape in before)
    if not proxy_reduced and not (reconstruction and reconstruction['accepted']):
        result = before
        valid = False
    report = {'method': 'Outer-region reconstruction with native Bucket corridor constraints',
        'accepted': bool(valid), 'changed_regions': len(history) if valid else 0,
        'vector_source_local_passed': strict_local_passed,
        'vector_planning_local_passed': planning_local_passed,
        'requires_final_source_validation': bool(valid and not strict_local_passed),
        'candidate_source_error': source_error,
        'vector_source_local_excess': float(np.max(np.sqrt(actual_windows / (3 * solver.window ** 2))
            - np.sqrt(strict_local_limit / (3 * solver.window ** 2)))),
        'geometry_proxy_reduced': proxy_reduced, 'final_selection_uses_native_counts': True,
        'edits': history, 'ownership_reconstruction': reconstruction,
        'island_transfers': transfers, 'grouping': grouping, 'boundary_tolerance': boundary_tolerance,
        'seconds': time.monotonic() - started,
        'before': {name: sum(geometry_metrics(shape)[name] for _, shape in before)
                   for name in ('regions', 'holes', 'points', 'triangle_bound')},
        'after': {name: sum(geometry_metrics(shape)[name] for _, shape in result)
                  for name in ('regions', 'holes', 'points', 'triangle_bound')},
        'source_error': image_error(source, render(polygon_svg(result, solver.size), solver.size))}
    (output / 'report.json').write_text(json.dumps(report, indent=2))
    (output / 'contours.svg').write_bytes(polygon_svg(result, solver.size))
    return result, report
