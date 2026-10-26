"""Search opaque region geometry and grouping while constructing a paint stack."""

from dataclasses import dataclass
import hashlib
import math
import time

import numpy as np
import shapely
from shapely.geometry import Polygon, box

from fair_curves import polygons
from hidden_contours import contour_proposals, open_hidden_holes
from layer_completion import BOOLEAN_GRID, Completion, Paint, clean, filled_holes, topological_order


def fingerprint(geometry):
    return hashlib.sha256(geometry.wkb).digest()


def union(shapes):
    parts = [shape for shape in shapes if not shape.is_empty]
    if not parts:
        return Polygon()
    if len(parts) == 1:
        return parts[0]
    if len(parts) == 2:
        if parts[0].covers(parts[1]):
            return parts[0]
        if parts[1].covers(parts[0]):
            return parts[1]
    result = clean(shapely.union_all(parts, grid_size=BOOLEAN_GRID))
    shapely.prepare(result)
    return result


def subtract(first, second):
    if first.is_empty or second.is_empty:
        return first
    if second.covers(first):
        return Polygon()
    if first.disjoint(second):
        return first
    return clean(shapely.difference(first, second, grid_size=BOOLEAN_GRID))


def intersect(first, second):
    if first.is_empty or second.is_empty:
        return Polygon()
    if first.covers(second):
        return second
    if second.covers(first):
        return first
    return clean(shapely.intersection(first, second, grid_size=BOOLEAN_GRID))


def visible_masks(paints):
    visible, owned, covered = {}, [Polygon() for _ in paints], Polygon()
    for index in reversed(range(len(paints))):
        color, support = paints[index]
        owned[index] = subtract(support, covered)
        visible.setdefault(color, []).append(owned[index])
        covered = union((covered, support))
    return {color: union(parts) for color, parts in visible.items()}, owned, covered


@dataclass
class State:
    layers: tuple
    committed: dict
    wrong: dict
    cost: float = 0.
    estimate: float = 0.
    correct: dict | None = None


class StackSearch:
    def __init__(self, paints, start=0, stop=None, beam_width=6, max_states=192,
                 tolerance=1., area_tolerance=1e-6, path_weight=256., hole_weight=512.,
                 angle_weight=4., progress=None, measured_selection=False, ownership_tolerance=0.):
        self.paints = paints
        self.start, self.stop = start, len(paints) if stop is None else stop
        if not 0 <= self.start < self.stop <= len(paints) or beam_width < 1 or max_states < 1:
            raise ValueError('Invalid search window or search effort')
        self.beam_width, self.max_states = beam_width, max_states
        self.tolerance, self.area_tolerance = tolerance, area_tolerance
        self.path_weight, self.hole_weight, self.angle_weight = path_weight, hole_weight, angle_weight
        self.progress = progress or (lambda entry: None)
        self.measured_selection = measured_selection
        if not math.isfinite(ownership_tolerance) or ownership_tolerance < 0:
            raise ValueError('Ownership tolerance must be finite and nonnegative')
        self.ownership_tolerance = ownership_tolerance
        self.progress({'bottom_up_initializing': 'visible ownership'})
        self.reference, self.source_owned, self.domain = visible_masks(paints)
        footprint = union(shape for _, shape in paints[self.start:self.stop]).convex_hull
        self.allowed_domain = intersect(footprint, self.domain)
        self.targets = {color: intersect(union(self.source_owned[index] for index in range(self.stop)
                                    if paints[index][0] == color), self.allowed_domain)
                        for color in sorted({color for color, _ in paints[:self.stop]})}
        self.targets = {color: shape for color, shape in self.targets.items() if not shape.is_empty}
        self.free_upper = union(shape for _, shape in paints[self.stop:])
        self.progress({'bottom_up_initializing': 'source groups', 'colors': len(self.targets)})
        self.sources = {color: [(intersect(self.source_owned[index], self.allowed_domain), paints[index][1])
            for index in range(self.stop) if paints[index][0] == color
            and self.source_owned[index].intersects(self.allowed_domain)] for color in self.targets}
        self.source_supports = {color: union(support for _, support in sources)
                                for color, sources in self.sources.items()}
        self.scores, self.proposal_cache, self.raw_cache, self.cores, self.impact = {}, {}, {}, {}, {}
        self.rejected = {}
        self.started = time.perf_counter()
        self.expanded, self.generated = 0, 0
        self.visible_area = sum(shape.area for shape in self.targets.values())
        self.visible_domain = union(self.targets.values())
        self.visible_cost = sum(self.metrics(shape)['cost'] for shape in self.targets.values())
        self.error_density = 0.

    def reject(self, reason):
        self.rejected[reason] = self.rejected.get(reason, 0) + 1

    def visible_difference(self, color, actual):
        expected = self.reference.get(color, Polygon())
        changed = clean(shapely.symmetric_difference(expected, actual, grid_size=BOOLEAN_GRID))
        if not self.ownership_tolerance:
            return changed.area
        return subtract(changed, expected.boundary.buffer(self.ownership_tolerance)).area

    def metrics(self, geometry):
        key = fingerprint(geometry)
        if key in self.scores:
            return self.scores[key]
        components = polygons(geometry)
        holes = points = reflex = reversals = 0
        angle_error = 0.
        preferred = np.array([0., 30., 45., 60., 90., 120., 135., 150., 180.])
        for part in components:
            holes += len(part.interiors)
            for ring in [part.exterior, *part.interiors]:
                coordinates = np.asarray(ring.simplify(max(.1, self.tolerance * .5)).coords)[:-1]
                original = np.asarray(ring.coords)[:-1]
                def signed_area(points):
                    return np.sum(points[:, 0] * np.roll(points[:, 1], -1)
                                  - points[:, 1] * np.roll(points[:, 0], -1))
                if len(coordinates) < 3 or abs(signed_area(coordinates) - signed_area(original)) \
                        > .05 * abs(signed_area(original)):
                    coordinates = original
                points += len(coordinates)
                incoming = coordinates - np.roll(coordinates, 1, axis=0)
                outgoing = np.roll(coordinates, -1, axis=0) - coordinates
                cross = incoming[:, 0] * outgoing[:, 1] - incoming[:, 1] * outgoing[:, 0]
                turns = np.arctan2(cross, np.sum(incoming * outgoing, axis=1))
                orientation = np.sign(np.sum(turns)) or 1
                reflex += int(np.count_nonzero(turns * orientation < -math.radians(5)))
                active = np.sign(turns[np.abs(turns) > math.radians(5)])
                reversals += int(np.count_nonzero(active != np.roll(active, 1))) if len(active) > 1 else 0
                degrees = np.degrees(np.abs(turns))
                angle_error += float(np.min(np.abs(degrees[:, None] - preferred), axis=1).sum() / 15)
        cost = ((self.path_weight + 1) * len(components) + self.hole_weight * holes + points
                + self.angle_weight * angle_error + 2 * reflex + reversals)
        result = {'regions': len(components), 'holes': holes, 'polygon_points': points,
                  'reflex_corners': reflex, 'turn_reversals': reversals,
                  'angle_error': angle_error, 'cost': cost}
        if len(self.scores) > 4096:
            self.scores.clear()
        self.scores[key] = result
        return result

    def allowed(self, state, color):
        blocked = union(shape for fill, shape in state.committed.items() if fill != color)
        return subtract(self.allowed_domain, blocked)

    def proposals(self, target, allowed, source=None, detailed=True):
        signature = (fingerprint(target), fingerprint(allowed), fingerprint(source) if source is not None else None, detailed)
        if signature in self.proposal_cache:
            return self.proposal_cache[signature]
        raw_signature = (signature[0], signature[2], detailed)
        if raw_signature in self.raw_cache:
            raw = self.raw_cache[raw_signature] + [('available-domain', allowed)]
        else:
            raw = self.raw_proposals(target, source, detailed)
            if len(self.raw_cache) > 256:
                self.raw_cache.clear()
            self.raw_cache[raw_signature] = raw
            raw = raw + [('available-domain', allowed)]
        found = {}
        for name, geometry in raw:
            try:
                shape = geometry if allowed.covers(geometry) else intersect(geometry, allowed)
                if name.startswith(('outer-', 'closing-', 'source')):
                    shape = union((shape, target))
                parts = polygons(shape)
                retained = [part for part in parts if part.intersects(target)]
                shape = clean(shapely.MultiPolygon(retained)) if len(retained) > 1 else (
                    clean(retained[0]) if retained else Polygon())
                if shape.is_empty or not shape.is_valid:
                    self.reject('invalid_or_empty_geometry')
                    continue
                if shape.wkb not in found:
                    found[shape.wkb] = (self.metrics(shape)['cost'], name, shape)
            except shapely.errors.GEOSException:
                self.reject('boolean_operation_failed')
                continue
        result = sorted(found.values(), key=lambda row: (round(row[0], 8),
            max(0., row[2].area - target.area), row[1], row[2].wkb))
        if len(self.proposal_cache) > 512:
            self.proposal_cache.clear()
        self.proposal_cache[signature] = result
        return result

    def raw_proposals(self, target, source, detailed):
        raw = [('visible', target), ('filled', filled_holes(target)),
               ('hull', target.convex_hull), ('box', target.envelope),
               ('oriented-box', target.minimum_rotated_rectangle)]
        if source is not None:
            raw.extend([('source', source), ('source-filled', filled_holes(source))])
        part_count = len(polygons(target))
        coarse = target.simplify(max(.25, self.tolerance), preserve_topology=True)
        for radius in ((2., 8., 32.) if detailed and part_count <= 64 else ()):
            expanded = coarse.buffer(radius, quad_segs=2, join_style='mitre', mitre_limit=2)
            raw.append((f'outer-{radius:g}', expanded.simplify(radius * .5, preserve_topology=True)))
            raw.append((f'closing-{radius:g}', expanded.buffer(-radius, quad_segs=2,
                join_style='mitre', mitre_limit=2)))
        return raw

    def seeds(self, wrong, color):
        result = [(wrong, None)]
        parts = sorted(polygons(wrong), key=lambda part: (-part.area, part.bounds))
        result += [(part, None) for part in parts[:2]]
        if parts:
            part = parts[0]
            signature = fingerprint(part)
            if signature not in self.cores:
                if hasattr(shapely, 'maximum_inscribed_circle'):
                    axis = shapely.maximum_inscribed_circle(part, tolerance=max(.25, self.tolerance))
                    center = axis.coords[0]
                    radius = axis.length / math.sqrt(2) * .999
                    core = box(center[0] - radius, center[1] - radius,
                               center[0] + radius, center[1] + radius)
                else:
                    core = Polygon()
                if len(self.cores) > 256:
                    self.cores.clear()
                self.cores[signature] = core if part.covers(core) else Polygon()
            core = self.cores[signature]
            if core.area > self.area_tolerance:
                result.append((core, part.convex_hull))
        sources = sorted(self.sources[color], key=lambda pair: -intersect(pair[0], wrong).area)
        for owned, support in sources[:2]:
            needed = intersect(owned, wrong)
            if not needed.is_empty:
                result.append((needed, support))
        found = {}
        for shape, source in result:
            if shape.area > self.area_tolerance:
                found.setdefault(shape.wkb, (shape, source))
        return list(found.values())

    def advance(self, state, color, promise, support):
        if state.correct is None:
            state.correct = {fill: subtract(reference, state.wrong[fill]) for fill, reference in self.targets.items()}
        wrong, correct = {}, {}
        for fill, reference in self.targets.items():
            correct[fill] = (union((state.correct[fill], intersect(reference, support))) if fill == color else
                             subtract(state.correct[fill], support))
            wrong[fill] = subtract(reference, correct[fill])
        committed = dict(state.committed)
        committed[color] = union((committed[color], promise))
        layers = state.layers + ((color, support, promise),)
        cost = state.cost + self.metrics(support)['cost']
        if state.layers and state.layers[-1][0] == color:
            _, previous, previous_promise = state.layers[-1]
            combined = union((previous, support))
            layers = state.layers[:-1] + ((color, combined, union((previous_promise, promise))),)
            cost = state.cost - self.metrics(previous)['cost'] + self.metrics(combined)['cost']
        return State(layers, committed, wrong, cost, correct=correct)

    def estimate(self, state):
        return ((self.path_weight + 4) * sum(shape.area > self.area_tolerance for shape in state.wrong.values())
                + self.error_density * sum(shape.area for shape in state.wrong.values()))

    def signature(self, state):
        last = (state.layers[-1][0], fingerprint(state.layers[-1][1])) if state.layers else ('', b'')
        return (last, tuple((color, fingerprint(state.committed[color]), fingerprint(state.wrong[color]))
                            for color in sorted(self.targets)))

    def complete(self, state):
        return all(shape.area <= self.area_tolerance for shape in state.wrong.values())

    def foreign_area(self, support, color):
        signature = (fingerprint(support), color)
        if signature not in self.impact:
            if len(self.impact) > 512:
                self.impact.clear()
            self.impact[signature] = max(0., intersect(support, self.visible_domain).area
                - intersect(support, self.targets[color]).area)
        return self.impact[signature]

    def candidates(self, state):
        rows = []
        for color, wrong in state.wrong.items():
            if wrong.area <= self.area_tolerance:
                continue
            allowed = self.allowed(state, color)
            previous = state.layers[-1][1] if state.layers and state.layers[-1][0] == color else Polygon()
            base_cost = state.cost - self.metrics(previous)['cost']
            seen = set()
            for target, source in self.seeds(wrong, color):
                coverage = union((previous, target))
                proposals = self.proposals(coverage, allowed, source)
                shortlisted = proposals[:2]
                for prefix in ('source', 'outer-', 'closing-'):
                    extra = next((proposal for proposal in proposals if proposal[1].startswith(prefix)), None)
                    if extra is not None:
                        shortlisted.append(extra)
                for _, kind, support in shortlisted:
                    key = (target.wkb, support.wkb)
                    if key in seen:
                        continue
                    seen.add(key)
                    pending = sum(shape.area > self.area_tolerance for shape in state.wrong.values())
                    progress = intersect(support, wrong).area / self.visible_area
                    commitment = self.metrics(target)['cost'] / self.visible_cost
                    repaint = self.foreign_area(support, color) - progress * self.visible_area
                    priority = base_cost + self.metrics(support)['cost'] + (self.path_weight + 4) * pending \
                        + self.path_weight * (commitment - progress) + self.error_density * repaint
                    rows.append((priority, color, kind, target, support))
        self.generated += len(rows)
        rows.sort(key=lambda row: (row[0], row[1], row[2], row[3].wkb, row[4].wkb))
        return [(priority, color, kind, self.advance(state, color, target, support))
                for priority, color, kind, target, support in rows[:self.beam_width * 3]]

    def finish(self, state, baseline_cost, savings=False):
        for _ in self.targets:
            if self.complete(state):
                return state
            choices = []
            for color, wrong in state.wrong.items():
                if wrong.area <= self.area_tolerance:
                    continue
                proposals = self.proposals(wrong, self.allowed(state, color),
                    self.source_supports[color], detailed=False)
                if proposals:
                    cost, kind, support = min(proposals, key=lambda row: (
                        row[0] + self.error_density * self.foreign_area(row[2], color), row[1]))
                    candidate = self.advance(state, color, wrong, support)
                    increment = candidate.cost - state.cost
                    priority = increment + self.error_density * (self.foreign_area(support, color) - wrong.area)
                    if savings:
                        priority -= self.metrics(wrong)['cost']
                    choices.append((priority, increment, color, kind, candidate))
            if not choices:
                return None
            _, cost, color, kind, candidate = min(choices, key=lambda row: row[:4])
            if state.cost + cost >= baseline_cost:
                self.progress({'bottom_up_finishing_rejected': 'no_search_cost_reduction',
                    'fill': color, 'stack_cost': state.cost, 'added_cost': cost,
                    'remaining_visible_area': sum(shape.area for shape in state.wrong.values()),
                    'remaining_components': sum(len(polygons(shape)) for shape in state.wrong.values()),
                    'baseline_cost': baseline_cost})
                return None
            state = candidate
            state.committed[color] = self.targets[color]
        return state if self.complete(state) else None

    def finish_source(self, state, baseline_cost):
        for index in range(self.stop):
            color, source = self.paints[index]
            if color not in self.targets:
                continue
            needed = intersect(self.source_owned[index], state.wrong[color])
            if needed.area <= self.area_tolerance:
                continue
            allowed = self.allowed(state, color)
            support = union((intersect(source, allowed), needed))
            choices = [(self.metrics(shape)['cost'], shape) for shape in (needed, support)]
            candidates = [self.advance(state, color, needed, shape) for _, shape in choices]
            candidate = min(candidates, key=lambda row: row.cost)
            if candidate.cost >= baseline_cost:
                self.reject('source_finishing_exceeded_cost')
                return None
            state = candidate
        return state if self.complete(state) else None

    def cutout_candidates(self, baseline_cost):
        candidates = []
        suffix, covered = [Polygon() for _ in self.paints], Polygon()
        for index in reversed(range(len(self.paints))):
            suffix[index] = covered
            covered = union((covered, self.paints[index][1]))
        for index in range(self.start, self.stop):
            color, support = self.paints[index]
            groups = {}
            for part in polygons(support):
                for ring in part.interiors:
                    hole = Polygon(ring)
                    exposed = subtract(hole, suffix[index])
                    fills = [fill for fill, required in self.reference.items()
                             if intersect(exposed, required).area > self.area_tolerance]
                    if len(fills) <= 1:
                        groups.setdefault(fills[0] if fills else color, []).append(hole)
            for fill, holes in sorted(groups.items()):
                added = union(holes)
                expanded = union((support, added))
                patches = []
                if fill != color:
                    allowed = intersect(union((self.reference[fill], suffix[index])), self.domain)
                    proposals = self.proposals(added, allowed, detailed=False)
                    if not proposals:
                        continue
                    patches.append((fill, proposals[0][2]))
                cost = baseline_cost - self.metrics(support)['cost'] + self.metrics(expanded)['cost'] \
                    + sum(self.metrics(shape)['cost'] for _, shape in patches)
                result = self.paints[:index] + [(color, expanded)] + patches + self.paints[index + 1:]
                frozen = {position: position for position in range(index)}
                frozen.update({position + len(patches): position for position in range(index + 1, len(self.paints))})
                _, owned, _ = visible_masks(result)
                retained = [position for position, shape in enumerate(owned) if not shape.is_empty]
                removed = [frozen[position] for position in frozen if position not in retained]
                cost -= sum(self.metrics(self.paints[position][1])['cost'] for position in removed)
                if cost >= baseline_cost:
                    continue
                frozen = {new: frozen[old] for new, old in enumerate(retained) if old in frozen}
                result = [result[position] for position in retained]
                block = [(color, expanded)] + patches
                candidates.append((cost, result, {'search_cost': cost, 'baseline_search_cost': baseline_cost,
                    'kind': 'cutout_completion', 'source_position': index, 'filled_holes': len(holes),
                    'removed_hidden_source_positions': removed,
                    'frozen_sources': frozen,
                    'layers': [{'fill': fill, **self.metrics(shape), 'support_area': shape.area,
                        'committed_visible_area': intersect(shape, self.reference[fill]).area} for fill, shape in block]}))
        return sorted(candidates, key=lambda row: (row[0], row[2]['source_position']))

    def group_sources(self, baseline_cost):
        contract = Completion(self.source_owned, [color for color, _ in self.paints], self.domain,
            area_tolerance=self.area_tolerance, path_weight=self.path_weight, hole_weight=self.hole_weight)
        items = [Paint(index, color, frozenset([index]), support)
                 for index, (color, support) in enumerate(self.paints)]
        edges = contract.dependencies(items)
        attempts, accepted, tried = 0, [], set()
        cost = baseline_cost
        while attempts < self.max_states:
            by_key = {item.key: item for item in items}
            groups = {}
            for item in items:
                if all(self.start <= owner < self.stop for owner in item.owners):
                    groups.setdefault(item.color, []).append(item.key)
            pairs = []
            for color, keys in sorted(groups.items()):
                keys.sort()
                for offset, first in enumerate(keys):
                    for second in keys[offset + 1:offset + 3]:
                        a, b = by_key[first], by_key[second]
                        signature = (a.owners, b.owners, fingerprint(a.support), fingerprint(b.support))
                        if signature not in tried:
                            potential = self.metrics(a.support)['cost'] + self.metrics(b.support)['cost']
                            pairs.append((-potential, first, second, signature))
            if not pairs:
                break
            _, first, second, signature = min(pairs, key=lambda row: row[:3])
            tried.add(signature)
            attempts += 1
            a, b = by_key[first], by_key[second]
            owners = a.owners | b.owners
            ancestors = (contract.ancestors(first, edges) | contract.ancestors(second, edges)) - {first, second}
            required = contract.requirement(owners)
            source = union((a.support, b.support))
            old_cost = self.metrics(a.support)['cost'] + self.metrics(b.support)['cost']
            best = None
            released = [key for key in ancestors if all(self.start <= owner < self.stop for owner in by_key[key].owners)
                        and by_key[key].color != a.color and not by_key[key].support.equals(self.domain)]
            hull = required.convex_hull
            released.sort(key=lambda key: (-intersect(contract.requirement(by_key[key].owners), hull).area, key))
            for lifted in [None, *released[:self.beam_width]]:
                blocked = union(contract.requirement(by_key[key].owners) for key in sorted(ancestors)
                                if key != lifted and by_key[key].color != a.color)
                allowed = subtract(self.domain, blocked)
                old_repair_cost = self.metrics(by_key[lifted].support)['cost'] if lifted is not None else 0.
                for proposed_cost, kind, support in self.proposals(required, allowed, source, detailed=False):
                    if proposed_cost >= old_cost + old_repair_cost:
                        continue
                    replacement = Paint(first, a.color, owners, support)
                    trial = [item for item in items if item.key not in (first, second, lifted)] + [replacement]
                    repairs, repair_cost = [], 0.
                    if lifted is not None:
                        item = by_key[lifted]
                        repair_ancestors = contract.ancestors(lifted, edges) - {first, second}
                        repair_blocked = union([required, *(contract.requirement(by_key[key].owners)
                            for key in sorted(repair_ancestors) if by_key[key].color != item.color)])
                        repair_proposals = self.proposals(contract.requirement(item.owners),
                            subtract(self.domain, repair_blocked), item.support, detailed=False)
                        if not repair_proposals:
                            continue
                        repair_cost, repair_kind, repair_support = repair_proposals[0]
                        trial.append(Paint(lifted, item.color, item.owners, repair_support))
                        repairs.append({'sources': sorted(item.owners), 'kind': repair_kind,
                                        'before_cost': old_repair_cost, 'after': self.metrics(repair_support)})
                    combined_cost = proposed_cost + repair_cost - old_repair_cost
                    if old_cost - combined_cost <= max(1e-6, old_cost * 1e-9):
                        continue
                    old_shapes = [a.support, b.support] + ([by_key[lifted].support] if lifted is not None else [])
                    new_shapes = [support] + ([repair_support] if lifted is not None else [])
                    if not any(sum(self.metrics(shape)[metric] for shape in new_shapes)
                               < sum(self.metrics(shape)[metric] for shape in old_shapes)
                               for metric in ('regions', 'holes', 'polygon_points', 'reflex_corners', 'turn_reversals')):
                        continue
                    try:
                        dependencies = contract.dependencies(trial)
                    except ValueError as failure:
                        self.reject('grouping_geometry_constraint')
                        self.progress({'bottom_up_grouping_rejected': str(failure),
                            'source_positions': sorted(owners), 'kind': kind})
                        continue
                    if topological_order([item.key for item in trial], dependencies) is None:
                        self.reject('grouping_dependency_cycle')
                        self.progress({'bottom_up_grouping_rejected': 'dependency_cycle',
                            'source_positions': sorted(owners), 'kind': kind,
                            'incident_edges': sorted(edge for edge in dependencies if first in edge)})
                        continue
                    rank = (combined_cost, max(0., support.area - required.area), len(dependencies), kind)
                    if best is None or rank < best[0]:
                        best = (rank, trial, dependencies, kind, replacement, repairs)
            if best is not None:
                rank, items, edges, kind, replacement, repairs = best
                cost += rank[0] - old_cost
                accepted.append({'sources': sorted(owners), 'kind': kind, 'saving': old_cost - rank[0],
                    'before_regions': len(polygons(a.support)) + len(polygons(b.support)),
                    'after': self.metrics(replacement.support), 'upper_repairs': repairs})
            self.progress({'bottom_up_grouping_attempt': attempts, 'accepted_merges': len(accepted),
                'source_positions': sorted(owners), 'search_cost': cost,
                'paint_regions': sum(len(polygons(item.support)) for item in items),
                'runtime_seconds': time.perf_counter() - self.started})
        if not accepted:
            return []
        by_key = {item.key: item for item in items}
        ranked = sorted(by_key, key=lambda key: (self.metrics(by_key[key].support)['cost'], key))
        positions = {key: index for index, key in enumerate(ranked)}
        order = topological_order(range(len(ranked)), {(positions[a], positions[b]) for a, b in edges})
        items = [by_key[ranked[index]] for index in order]
        result = [(item.color, item.support) for item in items]
        frozen = {position: next(iter(item.owners)) for position, item in enumerate(items)
                  if len(item.owners) == 1 and item.support.equals(self.paints[next(iter(item.owners))][1])}
        return [(cost, result, {'search_cost': cost, 'baseline_search_cost': baseline_cost,
            'kind': 'source_grouping', 'grouping_attempts': attempts, 'accepted_merges': accepted,
            'frozen_sources': frozen, 'ordering_edges': sorted(edges),
            'layers': [{'fill': item.color, **self.metrics(item.support), 'support_area': item.support.area,
                'committed_visible_area': contract.requirement(item.owners).area} for item in items if len(item.owners) > 1]})]

    def simplify_hidden(self, baseline_cost):
        contract = Completion(self.source_owned, [color for color, _ in self.paints], self.domain,
            area_tolerance=self.area_tolerance, path_weight=self.path_weight, hole_weight=self.hole_weight)
        items = [Paint(index, color, frozenset([index]), support)
                 for index, (color, support) in enumerate(self.paints)]
        suffix, covered = {}, Polygon()
        for item in reversed(items):
            suffix[item.key] = covered
            covered = union((covered, item.support))
        targets = sorted(range(self.start, self.stop),
            key=lambda index: (-self.metrics(items[index].support)['cost'], index))[:self.max_states]
        accepted = []
        for index in targets:
            item = items[index]
            required = self.source_owned[index]
            if required.is_empty:
                continue
            allowed = union((item.support, suffix[index]))
            old_metrics = self.metrics(item.support)
            candidates = []
            alternatives = contour_proposals(item.support, required, allowed,
                self.tolerance, self.area_tolerance)
            try:
                opened = open_hidden_holes(item.support, required, suffix[index],
                    lambda shape: self.metrics(shape)['cost'], self.tolerance, self.area_tolerance)
                if not opened.equals(item.support):
                    alternatives.append(('hidden-hole-channels', opened))
            except shapely.errors.GEOSException:
                self.reject('hidden_hole_boolean_failed')
            for kind, shape in alternatives:
                metrics = self.metrics(shape)
                if metrics['cost'] >= old_metrics['cost'] - max(1e-6, old_metrics['cost'] * 1e-9):
                    continue
                trial = items.copy()
                trial[index] = Paint(item.key, item.color, item.owners, shape)
                try:
                    edges = contract.dependencies(trial)
                    if topological_order([entry.key for entry in trial], edges) is None:
                        self.reject('hidden_contour_ordering_cycle')
                        continue
                except ValueError:
                    self.reject('hidden_contour_invalid_ownership')
                    continue
                candidates.append((metrics['cost'], kind, shape, metrics))
            if not candidates:
                continue
            _, kind, shape, metrics = min(candidates, key=lambda row: (row[0], row[1], fingerprint(row[2])))
            items[index] = Paint(item.key, item.color, item.owners, shape)
            accepted.append({'source_position': index, 'kind': kind, 'before': old_metrics, 'after': metrics,
                'removed_hidden_area': subtract(item.support, shape).area,
                'added_hidden_area': subtract(shape, item.support).area})
            self.progress({'hidden_contour': accepted[-1]})
        if not accepted:
            return []
        edges = contract.dependencies(items)
        ranked = sorted(items, key=lambda item: (self.metrics(item.support)['cost'], item.key))
        positions = {item.key: index for index, item in enumerate(ranked)}
        order = topological_order(range(len(ranked)), {(positions[a], positions[b]) for a, b in edges})
        if order is None:
            self.reject('hidden_contour_final_cycle')
            return []
        ordered = [ranked[index] for index in order]
        result = [(item.color, item.support) for item in ordered]
        frozen = {position: item.key for position, item in enumerate(ordered)
                  if item.support.equals(self.paints[item.key][1])}
        saving = sum(row['before']['cost'] - row['after']['cost'] for row in accepted)
        return [(baseline_cost - saving, result, {'kind': 'bidirectional_hidden_contours',
            'search_cost': baseline_cost - saving, 'baseline_search_cost': baseline_cost,
            'contour_edits': accepted, 'frozen_sources': frozen, 'ordering_edges': sorted(edges),
            'layers': [{'fill': item.color, **self.metrics(item.support)} for item in ordered]})]

    def promote_completion(self, baseline_cost, kinds=None):
        original_cost = sum(self.metrics(shape)['cost'] for _, shape in self.paints)
        suffix, covered = {}, Polygon()
        for index in reversed(range(len(self.paints))):
            suffix[index] = covered
            covered = union((covered, self.paints[index][1]))
        candidates = []
        targets = sorted(range(self.start, self.stop),
            key=lambda index: (-self.metrics(self.paints[index][1])['cost'], index))[:self.max_states]
        for index in targets:
            color, support = self.paints[index]
            required = self.source_owned[index]
            if required.is_empty:
                continue
            shapes = [('filled', filled_holes(support)), ('hull', required.convex_hull),
                      ('box', required.envelope), ('oriented-box', required.minimum_rotated_rectangle)]
            seen = set()
            for kind, raw in shapes:
                if kinds is not None and kind not in kinds:
                    continue
                expanded = intersect(raw, self.domain)
                if expanded.wkb in seen or subtract(required, expanded).area > self.area_tolerance:
                    continue
                seen.add(expanded.wkb)
                added = subtract(expanded, support)
                if added.is_empty:
                    continue
                exposed = subtract(added, suffix[index])
                patches = {fill: intersect(exposed, owned) for fill, owned in self.reference.items()
                           if fill != color and owned.intersects(exposed)}
                patches = {fill: shape for fill, shape in patches.items() if shape.area > self.area_tolerance}
                if not patches:
                    continue
                ascending = sorted(patches, key=lambda fill: (self.metrics(patches[fill])['cost'], fill))
                by_area = sorted(patches, key=lambda fill: (-patches[fill].area, fill))
                orders = []
                for offset in range(len(ascending)):
                    for base in (ascending, ascending[::-1], by_area, by_area[::-1]):
                        order = tuple(base[offset:] + base[:offset])
                        if order not in orders:
                            orders.append(order)
                self.progress({'promoted_completion': index, 'kind': kind, 'colors': len(patches)})
                for order in orders[:self.max_states]:
                    upper = suffix[index]
                    block = []
                    for fill in reversed(order):
                        target = patches[fill]
                        own = self.reference[fill]
                        if self.ownership_tolerance:
                            own = intersect(clean(own.buffer(self.ownership_tolerance, quad_segs=2)), self.domain)
                        allowed = union((upper, own))
                        proposals = self.proposals(target, allowed, source=target, detailed=False)
                        if not proposals:
                            break
                        shape = proposals[0][2]
                        block.append((fill, shape))
                        upper = union((upper, shape))
                    if len(block) != len(order):
                        continue
                    block.reverse()
                    result = self.paints[:index] + [(color, expanded)] + block + self.paints[index + 1:]
                    frozen = {position: position for position in range(index)}
                    frozen.update({position + len(block): position
                        for position in range(index + 1, len(self.paints))})
                    _, owned, _ = visible_masks(result)
                    retained = [position for position, shape in enumerate(owned) if not shape.is_empty]
                    removed = [frozen[position] for position in frozen if position not in retained]
                    frozen = {new: frozen[old] for new, old in enumerate(retained) if old in frozen}
                    result = [result[position] for position in retained]
                    cost = sum(self.metrics(shape)['cost'] for _, shape in result)
                    if cost >= original_cost - max(1e-6, original_cost * 1e-9) and not self.measured_selection:
                        self.reject('promoted_completion_exceeded_total_cost')
                        continue
                    adjusted = baseline_cost + cost - original_cost
                    candidates.append((adjusted, result, {'kind': 'promoted_color_completion',
                        'completion_kind': kind, 'source_position': index,
                        'ownership_tolerance': self.ownership_tolerance,
                        'filled_holes': max(0, sum(len(part.interiors) for part in polygons(support))
                            - sum(len(part.interiors) for part in polygons(expanded))),
                        'search_cost': adjusted, 'baseline_search_cost': baseline_cost,
                        'promoted_colors': list(order), 'removed_hidden_source_positions': removed,
                        'replaced_result_position': retained.index(index),
                        'frozen_sources': frozen, 'layers': [{'fill': fill, **self.metrics(shape),
                            'support_area': shape.area} for fill, shape in [(color, expanded)] + block]}))
        candidates.sort(key=lambda row: (row[0], row[2]['source_position'],
            row[2]['completion_kind'], row[2]['promoted_colors']))
        if self.measured_selection:
            families = {}
            for row in candidates:
                families.setdefault((row[2]['source_position'], row[2]['completion_kind']), row)
            candidates = list(families.values())
        else:
            candidates = candidates[:self.beam_width]
        for candidate_index, (cost, result, details) in enumerate(candidates):
            position = details['replaced_result_position']
            fill, support = result[position]
            before = self.metrics(support)
            if before['polygon_points'] <= 4 and before['holes'] == 0:
                continue
            allowed = union((support, union(shape for _, shape in result[position + 1:])))
            needed = self.source_owned[details['source_position']]
            alternatives = [(self.metrics(shape)['cost'], name, shape)
                for name, shape in contour_proposals(support, needed, allowed, self.tolerance, self.area_tolerance)]
            if not alternatives:
                continue
            improved_cost, name, shape = min(alternatives, key=lambda row: (row[0], row[1], fingerprint(row[2])))
            if improved_cost >= before['cost'] - 1e-6:
                continue
            result = list(result)
            result[position] = fill, shape
            cost += improved_cost - before['cost']
            details = dict(details, search_cost=cost, hidden_contour_completion=name)
            details['layers'] = [dict(details['layers'][0], **self.metrics(shape), support_area=shape.area)] + details['layers'][1:]
            candidates[candidate_index] = cost, result, details
        return candidates

    def run(self):
        lower, _, _ = visible_masks(self.paints[:self.start])
        wrong = {color: subtract(target, lower.get(color, Polygon())) for color, target in self.targets.items()}
        initial = State((), {color: Polygon() for color in self.targets}, wrong)
        baseline_cost = sum(self.metrics(shape)['cost'] for _, shape in self.paints[self.start:self.stop])
        self.error_density = baseline_cost / max(1., self.visible_area)
        direct = (self.promote_completion(baseline_cost) + self.simplify_hidden(baseline_cost)
                  + self.group_sources(baseline_cost) + self.cutout_candidates(baseline_cost))
        self.progress({'bottom_up_cutout_proposals': len(direct)})
        if self.start == 0 and self.paints[0][1].equals(self.domain):
            color, support = self.paints[0]
            if color in self.targets:
                initial = self.advance(initial, color, Polygon(), support)
        beam, finished, seen = [initial], [], {}
        while beam and self.expanded < self.max_states:
            children = []
            for state in beam:
                if self.complete(state):
                    finished.append(state)
                    continue
                if self.expanded >= self.max_states:
                    break
                self.expanded += 1
                self.progress({'bottom_up_expanding': self.expanded, 'depth': len(state.layers),
                    'runtime_seconds': time.perf_counter() - self.started})
                candidates = self.candidates(state)[:self.beam_width * 3]
                self.progress({'bottom_up_proposals': self.expanded, 'shortlisted': len(candidates),
                    'runtime_seconds': time.perf_counter() - self.started})
                for _, color, kind, candidate in candidates:
                    if candidate.cost >= baseline_cost:
                        self.reject('no_search_cost_reduction')
                        continue
                    signature = self.signature(candidate)
                    if seen.get(signature, math.inf) <= candidate.cost:
                        continue
                    seen[signature] = candidate.cost
                    candidate.estimate = self.estimate(candidate)
                    children.append(candidate)
            children.sort(key=lambda state: (state.cost + state.estimate, state.cost, self.signature(state)))
            beam = children[:self.beam_width]
            self.progress({'bottom_up_expanded': self.expanded, 'generated': self.generated,
                'beam': len(beam), 'complete_arrangements': len(finished),
                'best_estimate': beam[0].cost + beam[0].estimate if beam else None,
                'best_layers': [{'fill': color, **self.metrics(shape)} for color, shape, _ in beam[0].layers] if beam else [],
                'runtime_seconds': time.perf_counter() - self.started})
        finished += [state for state in beam if self.complete(state)]
        finishing_attempts = 0
        if not finished and not direct:
            for state in beam[:2] + [initial]:
                for savings in (False, True):
                    finishing_attempts += 1
                    self.progress({'bottom_up_finishing': finishing_attempts,
                        'starting_layers': len(state.layers), 'prefer_savings': savings})
                    complete = self.finish(state, baseline_cost, savings)
                    if complete is not None:
                        finished.append(complete)
                finishing_attempts += 1
                self.progress({'bottom_up_finishing': finishing_attempts,
                    'starting_layers': len(state.layers), 'source_positions': True})
                complete = self.finish_source(state, baseline_cost)
                if complete is not None:
                    finished.append(complete)
        finished.sort(key=lambda state: (state.cost, len(state.layers), self.signature(state)))
        proposals = []
        for state in finished[:self.beam_width]:
            if state.cost >= baseline_cost:
                continue
            layers = [(color, shape) for color, shape, _ in state.layers]
            result = self.paints[:self.start] + layers + self.paints[self.stop:]
            visible, _, domain = visible_masks(result)
            violations = {color: self.visible_difference(color, visible.get(color, Polygon()))
                          for color in self.reference}
            differences = {color: clean(shapely.symmetric_difference(shape, visible.get(color, Polygon()),
                grid_size=BOOLEAN_GRID)).area for color, shape in self.reference.items()}
            silhouette = clean(shapely.symmetric_difference(domain, self.domain, grid_size=BOOLEAN_GRID)).area
            if max(violations.values(), default=0) > self.area_tolerance or silhouette > self.area_tolerance:
                self.reject('complete_filled_geometry_changed')
                continue
            proposals.append((result, {'search_cost': state.cost, 'baseline_search_cost': baseline_cost,
                'layers': [{'fill': color, **self.metrics(shape), 'support_area': shape.area,
                    'committed_visible_area': promise.area} for color, shape, promise in state.layers],
                'visible_color_difference_area': differences, 'visible_color_difference_outside_tolerance_area': violations,
                'silhouette_difference_area': silhouette}))
        for cost, result, details in direct:
            visible, _, domain = visible_masks(result)
            violations = {color: self.visible_difference(color, visible.get(color, Polygon()))
                          for color in self.reference}
            differences = {color: clean(shapely.symmetric_difference(shape, visible.get(color, Polygon()),
                grid_size=BOOLEAN_GRID)).area for color, shape in self.reference.items()}
            silhouette = clean(shapely.symmetric_difference(domain, self.domain, grid_size=BOOLEAN_GRID)).area
            if max(violations.values(), default=0) > self.area_tolerance or silhouette > self.area_tolerance:
                self.reject('cutout_filled_geometry_changed')
                continue
            details.update(visible_color_difference_area=differences,
                visible_color_difference_outside_tolerance_area=violations, silhouette_difference_area=silhouette)
            proposals.append((result, details))
        proposals.sort(key=lambda row: (row[1]['search_cost'], len(row[0]), row[1].get('source_position', -1)))
        if self.measured_selection:
            promoted = [row for row in proposals if row[1].get('kind') == 'promoted_color_completion']
            promoted.sort(key=lambda row: (row[1]['search_cost'], row[1]['layers'][0]['cost'],
                row[1]['source_position'], row[1]['completion_kind']))
            ordinary = [row for row in proposals if row[1].get('kind') != 'promoted_color_completion']
            proposals = promoted[:1] + ordinary[:1] + promoted[1:] + ordinary[1:]
        else:
            proposals = proposals[:self.beam_width]
        return proposals, {'method': 'Bottom-up joint region, geometry and order beam search',
            'window': [self.start, self.stop], 'beam_width': self.beam_width,
            'ownership_tolerance': self.ownership_tolerance,
            'max_states': self.max_states, 'expanded_states': self.expanded,
            'generated_candidates': self.generated, 'completed_arrangements': len(finished) + len(direct),
            'finishing_attempts': finishing_attempts,
            'direct_cutout_proposals': len(direct),
            'rejections': self.rejected,
            'validated_arrangements': len(proposals), 'fixed_palette': sorted(self.targets),
            'window_domain_area': self.allowed_domain.area,
            'promoted_lower_visible_area': {color: union(intersect(self.source_owned[index], self.allowed_domain)
                for index in range(self.start) if self.paints[index][0] == color).area for color in self.targets},
            'region_count_quota': None, 'baseline_search_cost': baseline_cost,
            'runtime_seconds': time.perf_counter() - self.started}
