"""Rebuild local layer positions from fixed visible ownership fragments."""

from copy import deepcopy
from dataclasses import dataclass, replace
import hashlib
import heapq
import itertools
import json
from pathlib import Path
import time
import xml.etree.ElementTree as ET

import numpy as np
import shapely
from shapely.geometry import Polygon

from bottom_up import visible_masks
from fair_curves import RingBank, encode_geometry, path_counts, polygons
from outer_regions import constraint, cost, from_rings, simplify_batch, source_locks
from paint_stack import DEFAULT_INTERSECTION_AREA, DEFAULT_NATIVE_SHAPE_WEIGHT, geometry_metrics
from rate_distortion import window_sums
from simplify import image_error, read_svg, render


GRID = 1e-6
AREA_TOLERANCE = .0001


@dataclass(frozen=True)
class Paint:
    key: int
    color: str
    support: object
    required: object
    element: object = None


def clean(shape):
    return shapely.set_precision(shape, GRID)


def union(shapes):
    return shapely.union_all(list(shapes), grid_size=GRID)


def paint_cost(item):
    if item.element is None or 'C' not in item.element.attrib['d']:
        return cost(item.support)
    lines, curves = path_counts(item.element.attrib['d'])
    return lines + 2 * curves + 32 + 256 * sum(len(part.interiors) for part in polygons(item.support))


def order(items, domain, area_tolerance=DEFAULT_INTERSECTION_AREA):
    live = sorted((item for item in items if not item.required.is_empty), key=lambda item: item.key)
    if any(not item.support.is_valid or item.required.difference(item.support).area > AREA_TOLERANCE
           or item.support.difference(domain).area > AREA_TOLERANCE for item in live):
        return None, {'reason': 'geometry'}
    tree = shapely.STRtree([item.required for item in live])
    outgoing = {item.key: [] for item in live}
    incoming = {item.key: 0 for item in live}
    by_key = {item.key: item for item in live}
    edges = []
    for item in live:
        for hit in tree.query(item.support, predicate='intersects'):
            other = live[int(hit)]
            if item.key == other.key or item.color == other.color:
                continue
            if item.support.intersection(other.required).area > area_tolerance:
                outgoing[item.key].append(other.key)
                incoming[other.key] += 1
                edges.append((item.key, other.key))
    ready = [(item.key, paint_cost(item)) for item in live if not incoming[item.key]]
    heapq.heapify(ready)
    result = []
    while ready:
        key, _ = heapq.heappop(ready)
        result.append(by_key[key])
        for other in sorted(outgoing[key]):
            incoming[other] -= 1
            if not incoming[other]:
                heapq.heappush(ready, (other, paint_cost(by_key[other])))
    if len(result) != len(live):
        return None, {'reason': 'dependency_cycle',
            'blocked_items': sorted(key for key, degree in incoming.items() if degree), 'edges': sorted(edges)}
    return result, {'fixed_original_ownership': True, 'order': [item.key for item in result], 'edges': sorted(edges)}


class Search:
    def __init__(self, tool, data, source, baseline, domain, output, *, source_growth=8., local_growth=32.,
                 progress=None, native_score=None, shortlist=3, shape_weight=DEFAULT_NATIVE_SHAPE_WEIGHT,
                 flatten_tolerance=.05):
        self.tool, self.output = Path(tool), Path(output)
        self.output.mkdir(parents=True)
        self.source, self.baseline, self.domain = source, baseline, domain
        self.size = source.shape[1], source.shape[0]
        self.progress = progress or (lambda event: None)
        self.document = ET.fromstring(data)
        self.flatten_tolerance = flatten_tolerance
        stack = read_svg(data, self.size, flatten_tolerance)
        _, required, _ = visible_masks(stack)
        self.items = [Paint(index, color, shape, clean(core), deepcopy(element))
            for index, ((color, shape), core, element) in enumerate(zip(stack, required, self.document))]
        if order(self.items, domain)[0] is None:
            raise ValueError('The fixed ownership graph must represent the input paint order')
        self.next_key = len(self.items)
        self.bank = RingBank(flatten_tolerance, coordinate_digits=6)
        for item in self.items:
            for ring in item.element.attrib['d'].split('M')[1:]:
                encoded = 'M' + ring
                from simplify import parse_path
                self.bank.add(encoded, parse_path(encoded, 'evenodd', flatten_tolerance))
        self.image = render(data, self.size)
        self.window = min(16, *self.size)
        self.scale = 3 * self.window ** 2
        self.strict = self.windows(baseline) + local_growth
        self.limit = np.maximum(self.strict, self.windows(self.image))
        self.local_limit = self.limit ** 2 * self.scale
        self.global_limit = image_error(source, baseline)['rmse'] + source_growth
        self.history, self.rejected = [], []
        self.serial = 0
        self.encodings = {}
        self.constraints = {}
        self.native_score, self.shortlist = native_score, shortlist
        self.shape_weight = shape_weight
        self.native_current = None
        self.checkpoints = []

    def windows(self, image):
        error = np.square(image[:, :, :3].astype(float) - self.source[:, :, :3]).sum(axis=2)
        return np.sqrt(window_sums(error, self.window) / self.scale)

    def emit(self, items):
        document = deepcopy(self.document)
        document.clear()
        document.attrib.update({'width': str(self.size[0]), 'height': str(self.size[1]),
                                'viewBox': f'0 0 {self.size[0]} {self.size[1]}'})
        for item in items:
            if item.element is not None:
                document.append(deepcopy(item.element))
                continue
            for element in self.encode(item):
                document.append(deepcopy(element))
        return ET.tostring(document)

    def encode(self, item):
        key = item.color, item.support.wkb
        if key not in self.encodings:
            self.encodings[key] = [ET.Element('{http://www.w3.org/2000/svg}path',
                {'fill': item.color, 'fill-rule': 'evenodd',
                 'd': encode_geometry(part, tolerance=0, flatten_tolerance=self.flatten_tolerance,
                                      ring_bank=self.bank).d})
                for part in polygons(item.support)]
        return self.encodings[key]

    def assess(self, items, details, *, require_saving=True, allow_relabel=False, relabel_domain=None):
        if any(not item.support.is_valid for item in items):
            self.rejected.append(dict(details, reason='invalid_geometry'))
            return None
        try:
            items = [replace(item, element=self.encode(item)[0])
                     if item.element is None and len(polygons(item.support)) == 1 else item for item in items]
        except (ValueError, shapely.errors.GEOSException) as failure:
            self.rejected.append(dict(details, reason='invalid_encoding', error=str(failure)))
            return None
        if not allow_relabel or relabel_domain is not None:
            before, after = {item.key: item for item in self.items}, {item.key: item for item in items}
            changed = [key for key in before.keys() | after.keys() if key not in before or key not in after
                       or before[key].required.wkb != after[key].required.wkb or before[key].color != after[key].color]
            colors = {mapping[key].color for key in changed for mapping in (before, after) if key in mapping}
            for color in colors:
                old = union(before[key].required for key in changed if key in before and before[key].color == color)
                new = union(after[key].required for key in changed if key in after and after[key].color == color)
                changed_shape = old.symmetric_difference(new)
                outside = changed_shape.difference(relabel_domain) if relabel_domain is not None else changed_shape
                if outside.area > (DEFAULT_INTERSECTION_AREA if allow_relabel else AREA_TOLERANCE):
                    self.rejected.append(dict(details, reason='fixed_ownership'))
                    return None
        ordered, diagnostics = order(items, self.domain)
        if ordered is None:
            self.rejected.append(dict(details, **diagnostics))
            return None
        saving = sum(paint_cost(item) for item in self.items) - sum(paint_cost(item) for item in ordered)
        if require_saving and saving <= 0:
            self.rejected.append(dict(details, reason='encoded_complexity', complexity_saved=saving))
            return None
        data = self.emit(ordered)
        image = render(data, self.size)
        error = image_error(self.source, image)
        window_excess = self.windows(image) - self.limit
        excess = float(np.max(window_excess))
        relabel_excess = excess
        if relabel_domain is not None:
            x, y = np.meshgrid(np.arange(self.size[0]) + .5, np.arange(self.size[1]) + .5)
            mask = shapely.contains_xy(relabel_domain.buffer(2), x, y)
            affected = window_sums(mask.astype(float), self.window) > 0
            relabel_excess = float(np.max(window_excess[affected], initial=-float('inf')))
        if error['rmse'] > self.global_limit or allow_relabel and relabel_excess > 1e-5 \
                or np.any(image[:, :, 3] != self.baseline[:, :, 3]):
            self.rejected.append(dict(details, reason='source_error', source_error=error,
                                      local_budget_excess=excess))
            return None
        return {'items': ordered, 'data': data, 'image': image, 'saving': saving,
                'details': dict(details, complexity_saved=saving, source_error=error,
                                fixed_original_ownership=not allow_relabel,
                                requires_final_source_validation=excess > 1e-5,
                                source_local_excess=excess), 'ordering': diagnostics}

    def apply(self, proposal):
        self.items, self.image = proposal['items'], proposal['image']
        self.history.append(proposal['details'])
        (self.output / f'step_{len(self.history)}.svg').write_bytes(proposal['data'])
        self.checkpoints.append({'items': self.items, 'data': proposal['data'], 'history': len(self.history),
                                 'cost': sum(paint_cost(item) for item in self.items)})
        self.progress({'joint_region_transaction': proposal['details'], 'draw_items': len(self.items)})

    def simplify(self, requests):
        if not requests:
            return {}
        keys = {request['key']: hashlib.sha256(json.dumps({key: value for key, value in request.items()
            if key != 'key'}, sort_keys=True).encode()).hexdigest() for request in requests}
        pending = [request for request in requests if keys[request['key']] not in self.constraints]
        if pending:
            directory = self.output / f'constraints_{self.serial}'
            self.serial += 1
            result = simplify_batch(self.tool, pending, directory)
            for row in result['regions']:
                shapes = []
                for candidate in row['candidates']:
                    try:
                        shape = from_rings(candidate['rings'])
                        if not shape.is_valid:
                            raise ValueError('The filled native candidate is invalid')
                        shapes.append(shape)
                    except (ValueError, shapely.errors.GEOSException) as failure:
                        self.rejected.append({'kind': 'native-constraint-result', 'constraint': row['key'],
                                              'reason': 'invalid_native_geometry', 'error': str(failure)})
                self.constraints[keys[row['key']]] = shapes
        return {key: self.constraints[signature] for key, signature in keys.items()}

    def absorb(self, maximum=64):
        from rate_distortion import Item, RateDistortion, proposals
        from outer_regions import apply
        solver = RateDistortion([(item.color, item.support) for item in self.items], self.source,
            self.domain, local_growth=0., reference_image=self.baseline, complete_background=False)
        solver.local_limit = self.limit ** 2 * self.scale
        planning_error = np.square(solver.image[:, :, :3].astype(float) - self.source[:, :, :3]).sum(axis=2)
        solver.local_limit = np.maximum(solver.local_limit, window_sums(planning_error, self.window))
        solver.budget = float('inf')
        limit = self.global_limit ** 2 * self.size[0] * self.size[1] * 3
        transactions = []
        replay = []
        origins = list(self.items)
        for sweep in range(2):
            self.progress({'joint_absorption_sweep': sweep, 'draw_items': len(self.items)})
            _, owned = solver.visibility()
            positions = sorted(owned)
            tree = shapely.STRtree([owned[index] for index in positions])
            choices = []
            for donor in positions:
                for patch in polygons(owned[donor]):
                    if patch.area > 256 and 2 * patch.area / max(patch.length, GRID) > 2:
                        continue
                    for hit in tree.query(patch.buffer(.1), predicate='intersects'):
                        recipient = positions[int(hit)]
                        if recipient <= donor or solver.items[recipient].color == solver.items[donor].color:
                            continue
                        receiver = solver.items[recipient]
                        merged = union([receiver.geometry, patch])
                        saving = cost(receiver.geometry) - cost(merged)
                        if saving <= 0 or geometry_metrics(merged)['holes'] > receiver.metrics['holes']:
                            continue
                        changes = {recipient: Item(receiver.color, merged)}
                        candidate = solver.assess(changes, 'absorb-any-color-visible-component', refinement=True)
                        if candidate and solver.squared_error + candidate['delta'] <= limit and solver.local_valid(candidate):
                            choices.append((-saving, candidate['delta'], donor, 'complete-neighbor-notch',
                                            candidate, patch.area, [recipient]))
            for index in positions:
                item = solver.items[index]
                if index < len(self.items) and self.items[index].element is not None \
                        and 'C' in self.items[index].element.attrib['d']:
                    continue
                if item.metrics['points'] <= 4 and owned[index].area > 256:
                    continue
                shapes = [(name, shape) for name, shape in proposals(item.geometry)
                          if name == 'remove' or name.startswith(('chords-', 'open-', 'filled-open-', 'remove-component-'))]
                for name, shape in shapes:
                    missing = owned[index].difference(shape)
                    if missing.is_empty:
                        continue
                    patches, keep = {}, []
                    for patch in polygons(missing):
                        thickness = 2 * patch.area / max(patch.length, GRID)
                        if patch.area > 256 and thickness > 2:
                            keep.append(patch)
                            continue
                        x, y, right, bottom = patch.bounds
                        left, top = max(0, int(x)), max(0, int(y))
                        right, bottom = min(self.size[0], int(np.ceil(right))), min(self.size[1], int(np.ceil(bottom)))
                        xx, yy = np.meshgrid(np.arange(left, right) + .5, np.arange(top, bottom) + .5)
                        samples = self.source[top:bottom, left:right, :3][shapely.contains_xy(patch, xx, yy)]
                        neighbors = []
                        for hit in tree.query(patch.buffer(.1), predicate='intersects'):
                            recipient = positions[int(hit)]
                            if recipient == index or solver.items[recipient].color == item.color:
                                continue
                            original = solver.items[recipient]
                            combined = union([original.geometry, patch, patches.get(recipient, Polygon())])
                            if geometry_metrics(combined)['holes'] > original.metrics['holes']:
                                continue
                            rgb = np.array([int(original.color[n:n + 2], 16) for n in (1, 3, 5)])
                            error = float(np.square(samples.astype(float) - rgb).mean()) if len(samples) else 0.
                            extra = cost(combined) - cost(original.geometry)
                            neighbors.append((error + 4 * extra, extra, recipient))
                        if not neighbors:
                            keep.append(patch)
                            continue
                        recipient = min(neighbors)[2]
                        patches[recipient] = union([patches.get(recipient, Polygon()), patch])
                    if not patches:
                        continue
                    donor = union([shape, *keep])
                    changes = {index: Item(item.color, donor)}
                    for recipient, patch in patches.items():
                        receiver = solver.items[recipient]
                        merged = union([receiver.geometry, patch])
                        if not merged.equals(receiver.geometry):
                            changes[recipient] = Item(receiver.color, merged)
                    saving = sum(cost(solver.items[key].geometry) - cost(value.geometry)
                                 for key, value in changes.items())
                    if saving <= 0 or sum(value.metrics['holes'] for value in changes.values()) \
                            > sum(solver.items[key].metrics['holes'] for key in changes):
                        continue
                    candidate = solver.assess(changes, 'absorb-any-color-' + name, refinement=True)
                    if candidate and solver.squared_error + candidate['delta'] <= limit and solver.local_valid(candidate):
                        choices.append((-saving, candidate['delta'], index, name, candidate,
                                        sum(patch.area for patch in patches.values()), sorted(patches)))
            if not choices:
                break
            changed = set()
            for negative_saving, delta, index, name, candidate, area, recipients in sorted(choices, key=lambda row: row[:4]):
                if changed.intersection(candidate['changes']):
                    continue
                candidate = solver.assess(candidate['changes'], candidate['name'], refinement=True)
                if candidate is None or solver.squared_error + candidate['delta'] > limit or not solver.local_valid(candidate):
                    continue
                details = {'kind': 'absorb-any-color', 'donor': index, 'recipients': recipients,
                           'complexity_saved': -negative_saving, 'area': area, 'source_error_delta': delta}
                replay.append((details, {key: solver.items[key].geometry for key in candidate['changes']},
                               {key: value.geometry for key, value in candidate['changes'].items()}))
                apply(solver, candidate, details)
                transactions.append(details)
                changed.update(candidate['changes'])
                if len(transactions) >= maximum:
                    break
            solver.reconcile()
            if len(transactions) >= maximum:
                break
        if not transactions:
            self.progress({'joint_absorption_transactions': 0})
            return
        self.progress({'joint_absorption_transactions': len(transactions)})
        stack = [(item.color, item.geometry) for item in solver.items[:len(self.items)]]
        _, required, _ = visible_masks(stack)
        items = [replace(old, support=shape, required=clean(core),
                    element=old.element if old.support.equals(shape) else None)
                 for old, (_, shape), core in zip(self.items, stack, required)]
        proposal = self.assess(items, {'kind': 'source-validated-any-color-absorption', 'edits': transactions},
                               allow_relabel=True)
        if proposal:
            self.apply(proposal)
            return
        self.progress({'joint_absorption_batch_rejected': self.rejected[-1]['reason'],
                       'replaying_transactions': len(replay)})
        for details, expected, changes in replay:
            by_key = {item.key: item for item in self.items}
            if any(origins[index].key not in by_key or not by_key[origins[index].key].support.equals(shape)
                   for index, shape in expected.items()):
                continue
            replacements = {origins[index].key: shape for index, shape in changes.items()}
            current = [replace(item, support=replacements[item.key], element=None)
                       if item.key in replacements else item for item in self.items]
            _, required, _ = visible_masks([(item.color, item.support) for item in current])
            current = [replace(item, required=clean(core)) for item, core in zip(current, required)]
            proposal = self.assess(current, {'kind': 'source-validated-any-color-absorption',
                                            'edits': [details], 'replayed_independently': True}, allow_relabel=True)
            if proposal:
                self.apply(proposal)

    def hole_proposals(self, item, broad=False):
        current = self.items
        position = next(index for index, paint in enumerate(current) if paint.key == item.key)
        holes = [Polygon(ring) for part in polygons(item.support) for ring in part.interiors]
        completions = [('hole', union([item.support, hole])) for hole in holes]
        if len(holes) > 1:
            completions.append(('all-holes', union([item.support, *holes])))
        if broad:
            completions.extend([('hull', clean(item.required.convex_hull.intersection(self.domain))),
                ('box', clean(item.required.minimum_rotated_rectangle.intersection(self.domain)))])
        seen = set()
        for completion_index, (completion_kind, completed) in enumerate(completions):
            if completed.wkb in seen or completed.equals(item.support) \
                    or item.required.difference(completed).area > AREA_TOLERANCE:
                continue
            seen.add(completed.wkb)
            self.progress({'joint_completion_item': item.key, 'hole_variant': completion_index})
            gap = clean(completed.difference(item.support))
            promoted = {}
            scene = []
            for index, other in enumerate(current):
                fragment = clean(other.required.intersection(gap)) if index < position and other.color != item.color else Polygon()
                if fragment.area > AREA_TOLERANCE:
                    promoted.setdefault(other.color, []).append((other, fragment))
                    scene.append(replace(other, required=clean(other.required.difference(gap))))
                else:
                    scene.append(replace(other, support=completed, element=None) if other.key == item.key else other)
            colors = list(promoted)
            variants = [tuple(colors), tuple(reversed(colors)),
                        tuple(sorted(colors, key=lambda color: (cost(union(core for _, core in promoted[color])), color)))]
            if len(colors) <= 3:
                variants.extend(itertools.permutations(colors))
            upper = union(paint.support for paint in current[position + 1:])
            cores = {color: union(core for _, core in pieces) for color, pieces in promoted.items()}
            requests, arrangements = [], []
            for variant, arrangement in enumerate(dict.fromkeys(variants)):
                arrangements.append(arrangement)
                forbidden = Polygon()
                for color_index, color in enumerate(arrangement):
                    allowed = clean(union([gap, upper]).difference(forbidden))
                    support = clean(union(paint.support for paint, _ in promoted[color]).intersection(allowed))
                    key = f'{variant}:{color_index}'
                    requests.append(constraint(key, support, cores[color], allowed))
                    forbidden = union([forbidden, cores[color]])
            candidates = self.simplify(requests)
            if not colors:
                arrangements = [()]
            for variant, arrangement in enumerate(arrangements):
                additions = []
                for color_index, color in enumerate(arrangement):
                    shapes = candidates.get(f'{variant}:{color_index}', [])
                    if not shapes:
                        break
                    support = min(shapes, key=lambda shape: (cost(shape), shape.area, shape.wkb))
                    additions.append(Paint(self.next_key + color_index, color, support, cores[color]))
                if len(additions) != len(arrangement):
                    continue
                kind = 'complete-hole-and-split-layer-positions' if completion_kind in ('hole', 'all-holes') \
                    else f'complete-{completion_kind}-and-split-layer-positions'
                proposal = self.assess(scene + additions, {'kind': kind,
                    'completed_item': item.key, 'hole_variant': completion_index,
                    'promoted_colors': list(arrangement),
                    'split_items': sorted(paint.key for pieces in promoted.values() for paint, _ in pieces),
                    'promoted_items': len(additions)})
                if proposal:
                    yield proposal

    def group(self):
        requests, transactions = [], {}
        for color in sorted({item.color for item in self.items}):
            members = [item for item in self.items if item.color == color]
            if len(members) < 2:
                continue
            groups = [members]
            for member in members:
                nearest = sorted((other for other in members if other.key != member.key),
                    key=lambda other: (member.support.distance(other.support), other.key))[:2]
                groups.extend([[member, other] for other in nearest if member.key < other.key])
            for group in groups:
                key = ','.join(str(value) for value in sorted(item.key for item in group))
                if key in transactions:
                    continue
                core = union(item.required for item in group)
                allowed = self.domain.difference(union(item.required for item in self.items
                    if item.color != color and any(item.support.intersection(member.required).area > AREA_TOLERANCE
                                                   for member in group)))
                support = union(item.support for item in group)
                requests.append(constraint(key, core, core, allowed))
                transactions[key] = group, core, support
        proposals = []
        self.progress({'joint_group_proposals': len(requests)})
        for key, shapes in self.simplify(requests).items():
            members, core, _ = transactions[key]
            keys = {item.key for item in members}
            for shape in shapes[:2]:
                item = Paint(min(keys), members[0].color, shape, core)
                proposal = self.assess([other for other in self.items if other.key not in keys] + [item],
                    {'kind': 'group-fixed-fragments', 'merged_items': sorted(keys)})
                if proposal:
                    proposals.append(proposal)
        return proposals

    def contours(self):
        from hidden_contours import open_hidden_holes
        from ownership_regions import split_support
        requests, originals, immediate = [], {}, []
        upper = Polygon()
        for item in reversed(self.items):
            holes = sum(len(part.interiors) for part in polygons(item.support))
            if holes:
                opened = open_hidden_holes(item.support, item.required, upper, cost,
                                           area_tolerance=DEFAULT_INTERSECTION_AREA)
                if sum(len(part.interiors) for part in polygons(opened)) < holes:
                    proposal = self.assess([replace(other, support=opened, element=None)
                        if other.key == item.key else other for other in self.items],
                        {'kind': 'open-hole-through-hidden-coverage', 'item': item.key})
                    if proposal:
                        immediate.append(proposal)
                    key = f'{item.key}:open'
                    requests.append(constraint(key, opened, item.required, union([item.support, upper])))
                    originals[key] = item, 'open-hole-through-hidden-coverage'
                pieces = split_support(item.support)
                replacements = [Paint(self.next_key + index, item.color, part,
                                      clean(item.required.intersection(part))) for index, part in enumerate(pieces)]
                proposal = self.assess([other for other in self.items if other.key != item.key] + replacements,
                    {'kind': 'split-hole-into-filled-pieces', 'item': item.key, 'pieces': len(pieces)})
                if proposal:
                    immediate.append(proposal)
            if item.element is None or sum(path_counts(item.element.attrib['d'])) > 12:
                key = f'{item.key}:simplify'
                requests.append(constraint(key, item.support, item.required, union([item.support, upper])))
                originals[key] = item, 'simplify-released-hidden-boundaries'
            upper = union([upper, item.support])
        self.progress({'joint_contour_proposals': len(requests)})
        for key, shapes in self.simplify(requests).items():
            item, kind = originals[key]
            for shape in shapes[:2]:
                replacement = replace(item, support=shape, element=None)
                proposal = self.assess([replacement if other.key == item.key else other for other in self.items],
                    {'kind': kind, 'item': item.key})
                if proposal:
                    immediate.append(proposal)
        return immediate

    def reassign_components(self):
        requests, changes = [], {}
        positions = {item.key: index for index, item in enumerate(self.items)}
        owned = [item.required for item in self.items]
        tree = shapely.STRtree(owned)
        upper = {}
        covered = Polygon()
        for item in reversed(self.items):
            upper[item.key] = covered
            covered = union([covered, item.support])
        locked = source_locks(self)
        donors = sorted((item for item in self.items if item.key and paint_cost(item) > 48),
                        key=lambda item: (-paint_cost(item), item.key))[:self.shortlist * 2]
        for item in donors:
            core = item.required
            seeds = [('small-components', union(part for part in polygons(core) if part.area > 16))]
            for radius in (.5, 1., 2.):
                opened = core.buffer(-radius, quad_segs=1, join_style='mitre').buffer(
                    radius, quad_segs=1, join_style='mitre')
                seeds.append((f'visible-opening-{radius:g}', opened))
                seeds.append((f'visible-chords-{radius:g}', core.simplify(radius, preserve_topology=False)))
            signatures = set()
            for name, seed in seeds:
                removed = core.difference(seed).difference(locked)
                patches, territory = {}, []
                for part in polygons(removed):
                    if part.area <= AREA_TOLERANCE or part.area > 256 and 2 * part.area / max(part.length, GRID) > 2:
                        continue
                    left, top, right, bottom = part.bounds
                    left, top = max(0, int(left)), max(0, int(top))
                    right, bottom = min(self.size[0], int(np.ceil(right))), min(self.size[1], int(np.ceil(bottom)))
                    xx, yy = np.meshgrid(np.arange(left, right) + .5, np.arange(top, bottom) + .5)
                    samples = self.source[top:bottom, left:right, :3][shapely.contains_xy(part, xx, yy)]
                    options = []
                    for hit in tree.query(part.buffer(.1), predicate='intersects'):
                        recipient = self.items[int(hit)]
                        if recipient.key == item.key or recipient.color == item.color:
                            continue
                        merged = union([recipient.support, part, patches.get(recipient.key, Polygon())])
                        if sum(len(p.interiors) for p in polygons(merged)) \
                                > sum(len(p.interiors) for p in polygons(recipient.support)):
                            continue
                        rgb = np.array([int(recipient.color[n:n + 2], 16) for n in (1, 3, 5)])
                        error = float(np.square(samples.astype(float) - rgb).mean()) if len(samples) else 0.
                        options.append((error + 4 * (cost(merged) - cost(recipient.support)), recipient.key))
                    if not options:
                        continue
                    recipient = min(options)[1]
                    patches[recipient] = union([patches.get(recipient, Polygon()), part])
                    territory.append(part)
                if not territory:
                    continue
                removed = union(territory)
                signature = removed.wkb
                if signature in signatures:
                    continue
                signatures.add(signature)
                lower = union(patch for key, patch in patches.items() if positions[key] < positions[item.key])
                allowed = clean(union([item.support, upper[item.key]]).difference(lower))
                support = clean(item.support.intersection(allowed))
                required = clean(core.difference(removed))
                key = f'{item.key}:{name}'
                requests.append(constraint(key, support, required, allowed))
                changes[key] = item, patches, removed
        self.progress({'joint_visible_ownership_proposals': len(requests), 'locked_source_area': locked.area})
        proposals = []
        for key, shapes in self.simplify(requests).items():
            donor, patches, removed = changes[key]
            for shape in shapes[:2]:
                replacements = {donor.key: shape}
                for recipient, patch in patches.items():
                    original = next(item for item in self.items if item.key == recipient)
                    replacements[recipient] = union([original.support, patch])
                items = [replace(item, support=replacements[item.key], element=None)
                    if item.key in replacements and not item.support.equals(replacements[item.key]) else item
                    for item in self.items]
                _, required, _ = visible_masks([(item.color, item.support) for item in items])
                items = [replace(item, required=clean(core)) for item, core in zip(items, required)]
                proposal = self.assess(items, {'kind': 'rebuild-after-visible-component-reassignment',
                    'item': donor.key, 'variant': key, 'reassigned_area': removed.area,
                    'recipients': sorted(patches)}, allow_relabel=True, relabel_domain=removed)
                if proposal:
                    proposals.append(proposal)
        return proposals

    def measured_key(self, measured, items):
        if not measured.get('accepted'):
            return (float('inf'),) * 4
        encoded = measured.get('encoding_cost')
        if encoded is None:
            encoded = sum(paint_cost(item) for item in items)
        return (self.shape_weight * measured['shape_count'] + encoded, measured['shape_count'], encoded,
                measured['region_count'])

    def run(self, maximum=16, absorb=True):
        if self.native_score:
            self.native_current = self.native_score(self.emit(self.items), {'kind': 'unchanged-joint-input'})
        original, history_start = self.items, len(self.history)
        if absorb:
            self.absorb()
        if self.native_score and self.history:
            candidates = [self.checkpoints[-1]]
            if len(self.checkpoints) > 2:
                candidates.append(self.checkpoints[len(self.checkpoints) // 2])
            best, measured = None, self.native_current
            best_key = self.measured_key(measured, original)
            for checkpoint in candidates:
                actual = self.native_score(checkpoint['data'], {'kind': 'measure-color-absorption',
                                          'accepted_transactions': checkpoint['history']})
                key = self.measured_key(actual, checkpoint['items'])
                if key < best_key:
                    best, measured, best_key = checkpoint, actual, key
            self.items = best['items'] if best else original
            self.history = self.history[:best['history'] if best else history_start]
            self.image = render(self.emit(self.items), self.size)
            self.native_current = measured
        for iteration in range(maximum):
            proposals = self.reassign_components() + self.contours() + self.group()
            broad = {item.key for item in sorted(self.items, key=lambda item: (-paint_cost(item), item.key))
                     [:self.shortlist] if paint_cost(item) > 48}
            for item in self.items:
                if item.key in broad or any(part.interiors for part in polygons(item.support)):
                    proposals.extend(self.hole_proposals(item, broad=item.key in broad))
            if not proposals:
                break
            proposals.sort(key=lambda entry: (-entry['saving'], len(entry['items']), entry['data']))
            if self.native_score:
                measured = []
                seen, families, candidates = set(), set(), []
                for proposal in proposals:
                    kind = proposal['details']['kind']
                    if kind not in families:
                        candidates.append(proposal)
                        families.add(kind)
                candidates.sort(key=lambda proposal: proposal['details']['kind'] != 'group-fixed-fragments')
                included = {id(proposal) for proposal in candidates}
                candidates += [proposal for proposal in proposals if id(proposal) not in included]
                for proposal in candidates:
                    details = proposal['details']
                    signature = details['kind'], details.get('item', details.get('completed_item',
                        tuple(details.get('merged_items', []))))
                    if signature in seen:
                        continue
                    seen.add(signature)
                    actual = self.native_score(proposal['data'], details)
                    key = self.measured_key(actual, proposal['items'])
                    if key < self.measured_key(self.native_current, self.items):
                        measured.append((key, proposal, actual))
                    else:
                        self.rejected.append(dict(details, reason='native_encoded_cost',
                            measured_native=actual, incumbent_native=self.native_current))
                    if len(seen) >= self.shortlist:
                        break
                if not measured:
                    break
                _, best, self.native_current = min(measured, key=lambda entry: entry[0])
            else:
                best = proposals[0]
            self.next_key = max(item.key for item in best['items']) + 1
            self.apply(best)
        data = self.emit(self.items)
        final = render(data, self.size)
        report = {'method': 'Fixed ownership fragments with joint completion and split draw positions',
            'edits': self.history, 'rejected': self.rejected,
            'fixed_original_ownership': True, 'source_error': image_error(self.source, final),
            'maximum_source_local_excess': float(np.max(self.windows(final) - self.strict)),
            'requires_final_source_validation': bool(np.any(self.windows(final) > self.strict + 1e-5)),
            'encoded_paths': len(list(ET.fromstring(data))),
            'flatten_tolerance': self.flatten_tolerance,
            'intersection_area_tolerance': DEFAULT_INTERSECTION_AREA,
            'final_selection_uses_native_counts': True, 'native_shape_weight': self.shape_weight,
            'native_measurement': self.native_current}
        return data, report


def run(tool, data, source, baseline, domain, output, *, source_growth=8., local_growth=32.,
        maximum=16, absorb=True, progress=None, native_score=None, shortlist=3,
        shape_weight=DEFAULT_NATIVE_SHAPE_WEIGHT, flatten_tolerance=.05):
    started = time.monotonic()
    search = Search(tool, data, source, baseline, domain, output,
        source_growth=source_growth, local_growth=local_growth, progress=progress,
        native_score=native_score, shortlist=shortlist, shape_weight=shape_weight,
        flatten_tolerance=flatten_tolerance)
    result, report = search.run(maximum=maximum, absorb=absorb)
    report['seconds'] = time.monotonic() - started
    Path(output, 'report.json').write_text(json.dumps(report, indent=2))
    Path(output, 'contours.svg').write_bytes(result)
    return result, read_svg(result, search.size, flatten_tolerance), report
