"""Selective opaque support completion with fixed ownership dependencies."""

from dataclasses import dataclass, replace
import heapq
import time

import numpy as np
import shapely
from shapely.geometry import Polygon, box
from shapely.strtree import STRtree

from fair_curves import CostWeights, encode_geometry, polygons


BOOLEAN_GRID = 1e-8


def difference_area(first, second):
    return 0. if second.covers(first) else shapely.difference(first, second, grid_size=BOOLEAN_GRID).area


@dataclass
class Paint:
    key: int
    color: str
    owners: frozenset
    support: object


def topological_order(keys, edges):
    outgoing = {key: [] for key in keys}
    incoming = dict.fromkeys(keys, 0)
    for first, last in sorted(edges):
        if first == last:
            return None
        outgoing[first].append(last)
        incoming[last] += 1
    ready = [key for key, count in incoming.items() if count == 0]
    heapq.heapify(ready)
    result = []
    while ready:
        key = heapq.heappop(ready)
        result.append(key)
        for last in outgoing[key]:
            incoming[last] -= 1
            if not incoming[last]:
                heapq.heappush(ready, last)
    return result if len(result) == len(keys) else None


def clean(geometry):
    if geometry.geom_type in ('Polygon', 'MultiPolygon') and geometry.is_valid:
        return shapely.normalize(geometry)
    parts = polygons(shapely.make_valid(geometry))
    return shapely.normalize(shapely.union_all(parts)) if parts else Polygon()


def filled_holes(geometry):
    return shapely.union_all([Polygon(part.exterior) for part in polygons(geometry)])


def contour_proxy(geometry, tolerance, weights):
    parts = polygons(geometry)
    return (weights.paths + 1) * len(parts) + weights.holes * sum(len(part.interiors) for part in parts) + sum(
        len(ring.simplify(max(.1, tolerance), preserve_topology=True).coords)
        for part in parts for ring in [part.exterior, *part.interiors])


def minimum_cost(geometry, weights):
    parts = polygons(geometry)
    return (weights.paths + 4) * len(parts) + (weights.holes + 3) * sum(len(part.interiors) for part in parts)


class Completion:
    def __init__(self, required, colors, domain, tolerance=1., fairness=1., area_tolerance=1e-6,
                 progress=None, background=None, flatten_tolerance=.05, source_supports=None, original_encodings=None,
                 path_weight=256, hole_weight=96, ring_bank=None):
        self.required, self.colors, self.domain = required, colors, domain
        self.tolerance, self.fairness, self.area_tolerance = tolerance, fairness, area_tolerance
        self.progress = progress or (lambda entry: None)
        self.background = background
        self.flatten_tolerance = flatten_tolerance
        self.source_supports = source_supports or {}
        self.original_encodings = original_encodings or {}
        self.weights = CostWeights(path_weight, hole_weight)
        self.ring_bank = ring_bank
        self.on_checkpoint = None
        self.tree = STRtree(required)
        self.required_array = np.asarray(required, dtype=object)
        self.color_owners = {color: frozenset(index for index, fill in enumerate(colors) if fill == color)
                             for color in set(colors)}
        self.bounds = domain.bounds if domain.equals(box(*domain.bounds)) else None
        self.encodings, self.requirements = {}, {}
        self.coverage, self.checked = {}, set()
        self.raw_signature, self.raw_shapes = None, None
        self.diagnostics = []
        self.started = time.perf_counter()

    def requirement(self, owners):
        if owners not in self.requirements:
            self.requirements[owners] = clean(shapely.union_all([self.required[index] for index in sorted(owners)]))
            self.coverage[self.requirements[owners].wkb] = [index for index in sorted(owners)
                if self.required[index].area > self.area_tolerance]
        return self.requirements[owners]

    def encoding(self, geometry):
        key = geometry.wkb
        if key not in self.encodings:
            self.encodings[key] = replace(encode_geometry(geometry, self.tolerance, self.fairness, self.flatten_tolerance,
                self.bounds, self.original_encodings, self.ring_bank), weights=self.weights)
        return self.encodings[key]

    def dependencies(self, items):
        assignment = {owner: item.key for item in items for owner in item.owners}
        if set(assignment) != set(range(len(self.required))) or sum(len(item.owners) for item in items) != len(assignment):
            raise ValueError('Every original ownership region must have exactly one draw position')
        edges = set()
        for item in items:
            required = self.requirement(item.owners)
            signature = item.support.wkb
            validation = (item.owners, signature)
            if validation not in self.checked:
                shapely.prepare(item.support)
                if difference_area(required, item.support) > self.area_tolerance:
                    raise ValueError('A support removed required visible territory')
                if difference_area(item.support, self.domain) > self.area_tolerance:
                    raise ValueError('A support extends outside the original silhouette')
                self.checked.add(validation)
            if signature not in self.coverage:
                candidates = self.tree.query(item.support, predicate='intersects')
                referenced = self.required_array[candidates]
                contained = shapely.covers(item.support, referenced)
                covered = contained & (shapely.area(referenced) > self.area_tolerance)
                partial = np.flatnonzero(~contained)
                covered[partial] = shapely.area(shapely.intersection(referenced[partial], item.support,
                    grid_size=BOOLEAN_GRID)) > self.area_tolerance
                self.coverage[signature] = list(map(int, candidates[covered]))
            for owner in self.coverage[signature]:
                following = assignment[owner]
                if following != item.key and self.colors[owner] != item.color:
                    edges.add((item.key, following))
        return edges

    def split_items(self, items):
        result = []
        next_key = max((item.key for item in items), default=-1) + 1
        for item in items:
            parts = polygons(item.support)
            tree = STRtree(parts)
            assignments = [set() for _ in parts]
            coupled = False
            for owner in sorted(item.owners):
                hits = tree.query(self.required[owner], predicate='intersects')
                if not len(hits):
                    coupled = True
                    break
                selected = max(map(int, hits), key=lambda index: (self.required[owner].intersection(parts[index]).area, -index))
                if difference_area(self.required[owner], parts[selected]) > self.area_tolerance:
                    coupled = True
                    break
                assignments[selected].add(owner)
            if coupled:
                result.append(replace(item, key=next_key))
                next_key += 1
                continue
            for part, assigned in zip(parts, assignments):
                owners = frozenset(assigned)
                if owners:
                    result.append(Paint(next_key, item.color, owners, part))
                    next_key += 1
        return result

    def improve_components(self, items, passes, shortlist):
        accepted = []
        next_key = max(item.key for item in items) + 1
        for sweep in range(passes):
            by_key = {item.key: item for item in items}
            colors = {}
            for item in items:
                colors.setdefault(item.color, []).append(item)
            foundations = {item.key for item in items if item.color == self.background and item.support.equals(self.domain)}
            sets = {tuple([item.key]) for item in items if item.key not in foundations}
            for peers in colors.values():
                peers = [item for item in peers if item.key not in foundations]
                if len(peers) > 1:
                    sets.add(tuple(sorted(item.key for item in peers)))
                for item in peers:
                    nearest = sorted((other for other in peers if other.key != item.key),
                        key=lambda other: (item.support.distance(other.support), other.key))[:2]
                    sets.update(tuple(sorted((item.key, other.key))) for other in nearest)
            def priority(keys):
                return (-sum(self.encoding(by_key[key].support).cost for key in keys), keys)
            changed = 0
            total_cost = sum(self.encoding(item.support).cost for item in items)
            self.progress({'component_sweep': sweep + 1, 'candidate_sets': len(sets),
                           'runtime_seconds': time.perf_counter() - self.started})
            for index, keys in enumerate(sorted(sets, key=priority)):
                if index % 8 == 0:
                    self.progress({'component_candidate': index, 'candidate_sets': len(sets),
                                   'runtime_seconds': time.perf_counter() - self.started})
                if any(key not in by_key for key in keys):
                    continue
                originals = [by_key[key] for key in keys]
                if index % 8 == 0:
                    self.progress({'component_fill': originals[0].color, 'merged_items': len(originals),
                                   'runtime_seconds': time.perf_counter() - self.started})
                owners = frozenset(owner for item in originals for owner in item.owners)
                current = clean(shapely.union_all([item.support for item in originals]))
                merged = Paint(next_key, originals[0].color, owners, self.requirement(owners))
                remaining = [item for item in items if item.key not in keys]
                base = remaining + [merged]
                base_edges = self.dependencies(base)
                ancestors = self.ancestors(next_key, base_edges)
                scope = self.domain if owners == self.color_owners[merged.color] else current.envelope
                relevant = set(map(int, self.tree.query(scope, predicate='intersects')))
                blocked_owners = frozenset(owner for item in remaining if item.key in ancestors
                                          for owner in item.owners if owner in relevant)
                blocked = self.requirement(blocked_owners)
                allowed = self.domain.difference(blocked)
                proposals = self.candidates(replace(merged, support=current), allowed)[:shortlist]
                proposals += self.candidates(replace(merged, support=current), self.domain)[:shortlist]
                unique = {}
                for proposal in proposals:
                    unique.setdefault(proposal[2].wkb, proposal)
                proposals = list(unique.values())
                before_cost = sum(self.encoding(item.support).cost for item in originals)
                best = None
                for _, kind, support in proposals:
                    if minimum_cost(support, self.weights) >= before_cost:
                        continue
                    encoding = self.encoding(support)
                    saving = before_cost - encoding.cost
                    if saving <= 0:
                        continue
                    candidate = replace(merged, support=support)
                    trial = remaining + [candidate]
                    try:
                        edges = self.dependencies(trial)
                    except ValueError as failure:
                        self.diagnostics.append({'items': keys, 'kind': kind, 'rejection': str(failure)})
                        continue
                    relocation = []
                    if topological_order([item.key for item in trial], edges) is None:
                        trial, edges, relocation = self.relocate_conflicts(trial, candidate, edges)
                        if trial is None:
                            self.diagnostics.append({'items': keys, 'kind': kind, 'rejection': 'dependency_cycle',
                                                     'edges': sorted(edges)})
                            continue
                        lower_cost = sum(self.encodings[item.support.wkb].cost
                            if item.support.wkb in self.encodings else minimum_cost(item.support, self.weights) for item in trial)
                        if lower_cost >= total_cost:
                            self.diagnostics.append({'items': keys, 'kind': kind,
                                'rejection': 'relocation_minimum_cost', 'lower_cost': lower_cost,
                                'current_cost': total_cost})
                            continue
                        saving = total_cost - sum(self.encoding(item.support).cost for item in trial)
                        if saving <= 0:
                            continue
                    rank = (-saving, max(0., support.area - self.requirement(owners).area), len(edges), kind)
                    if best is None or rank < best[0]:
                        best = (rank, trial, candidate, kind, saving, relocation)
                if best is not None:
                    _, items, candidate, kind, saving, relocation = best
                    by_key = {item.key: item for item in items}
                    next_key = max(by_key) + 1
                    total_cost -= saving
                    diagnostic = {'stage': 'components', 'items': keys, 'kind': kind, 'saving': saving,
                                  'owners': len(owners), 'after': self.encoding(candidate.support).metrics(),
                                  'relocated': relocation}
                    accepted.append(diagnostic)
                    changed += 1
                    self.progress({'local_completion': len(accepted), **diagnostic,
                        'runtime_seconds': time.perf_counter() - self.started})
                    if self.on_checkpoint:
                        self.on_checkpoint(items, 'components', accepted)
            if not changed:
                break
        return self.split_items(items), accepted

    def relocate_conflicts(self, items, candidate, edges):
        ancestors = self.ancestors(candidate.key, edges)
        assignment = {owner: item.key for item in items for owner in item.owners}
        hits = self.coverage[candidate.support.wkb]
        moved = {}
        for owner in map(int, hits):
            if assignment[owner] == candidate.key or assignment[owner] not in ancestors \
                    or self.colors[owner] == candidate.color:
                continue
            moved.setdefault(self.colors[owner], set()).add(owner)
        if not moved:
            return None, edges, []
        transferred = frozenset(owner for owners in moved.values() for owner in owners)
        result = [replace(item, owners=item.owners - transferred) for item in items if item.owners - transferred]
        key = max(item.key for item in items) + 1
        diagnostics = []
        for color, owners in sorted(moved.items()):
            assigned = frozenset(owners)
            result.append(Paint(key, color, assigned, self.requirement(assigned)))
            diagnostics.append({'item': key, 'fill': color, 'owners': sorted(owners)})
            key += 1
        final_edges = self.dependencies(result)
        if topological_order([item.key for item in result], final_edges) is None:
            return None, final_edges, diagnostics
        return result, final_edges, diagnostics

    def remove_redundant(self, items):
        accepted = []
        for original in list(reversed(items)):
            item = next((current for current in items if current.key == original.key), None)
            if item is None:
                continue
            recipients = {}
            for owner in sorted(item.owners):
                covering = [other for other in items if other.key != item.key and other.color == item.color
                            and other.support.covers(self.required[owner])]
                if not covering:
                    break
                key = min(other.key for other in covering)
                recipients.setdefault(key, set()).add(owner)
            else:
                trial = [replace(other, owners=other.owners | frozenset(recipients.get(other.key, ())))
                         for other in items if other.key != item.key]
                edges = self.dependencies(trial)
                if topological_order([other.key for other in trial], edges) is not None:
                    saving = self.encoding(item.support).cost
                    items = trial
                    accepted.append({'stage': 'redundant', 'item': item.key,
                                     'recipients': sorted(recipients), 'saving': saving})
                else:
                    self.diagnostics.append({'item': item.key, 'rejection': 'same_fill_layer_conflict',
                                             'recipients': sorted(recipients)})
        return items, accepted

    def ancestors(self, key, edges):
        found = set()
        pending = [key]
        while pending:
            following = pending.pop()
            for first, last in edges:
                if last == following and first not in found:
                    found.add(first)
                    pending.append(first)
        return found

    def candidates(self, item, allowed):
        required = self.requirement(item.owners)
        signature = (item.owners, item.color, item.support.wkb)
        if signature != self.raw_signature:
            raw = [('visible', required), ('fill_holes', filled_holes(item.support)),
                   ('required_holes', filled_holes(required)), ('rectangle', required.envelope),
                   ('convex_hull', required.convex_hull),
                   ('component_hulls', shapely.union_all([part.convex_hull for part in polygons(item.support)]))]
            if item.color in self.source_supports and item.owners == self.color_owners[item.color]:
                raw.append(('source_support', self.source_supports[item.color]))
            for radius in (2., 8., 32., 128.):
                if radius <= self.tolerance:
                    continue
                closed = item.support.buffer(radius, quad_segs=2, join_style='mitre', mitre_limit=2).buffer(
                    -radius, quad_segs=2, join_style='mitre', mitre_limit=2)
                raw.append((f'hidden_closing_{radius:g}', closed))
                raw.append((f'chords_{radius:g}', item.support.simplify(radius, preserve_topology=True)))
            self.raw_signature, self.raw_shapes = signature, raw
        seen = {item.support.wkb}
        output = []
        shapely.prepare(allowed)
        for name, shape in self.raw_shapes:
            try:
                clipped = clean(shape if allowed.covers(shape) else shape.intersection(allowed))
                candidate = clean(clipped if difference_area(required, clipped) <= self.area_tolerance
                                  else required.union(clipped))
            except shapely.errors.GEOSException as failure:
                self.diagnostics.append({'item': item.key, 'kind': name,
                    'rejection': 'invalid_boolean_geometry', 'detail': str(failure)})
                continue
            if candidate.wkb in seen or not candidate.is_valid:
                continue
            shapely.prepare(candidate)
            missing = difference_area(required, candidate)
            outside = difference_area(candidate, self.domain)
            if missing > self.area_tolerance or outside > self.area_tolerance:
                self.diagnostics.append({'item': item.key, 'kind': name,
                    'rejection': 'filled_geometry_constraint', 'missing_visible_area': missing, 'outside_area': outside})
                continue
            seen.add(candidate.wkb)
            output.append((contour_proxy(candidate, self.tolerance, self.weights), name, candidate))
        return sorted(output, key=lambda value: (value[0], value[1]))

    def run(self, passes=2, shortlist=3):
        groups = {}
        for index, color in enumerate(self.colors):
            groups.setdefault(color, []).append(index)
        items = []
        for color, owners in groups.items():
            sets = [frozenset(owners)]
            for assigned in sets:
                required = self.requirement(assigned)
                support = self.domain if color == self.background else self.source_supports.get(color, required)
                missing, outside = difference_area(required, support), difference_area(support, self.domain)
                if missing > self.area_tolerance or outside > self.area_tolerance:
                    self.progress({'source_support_fallback': color, 'missing_area': missing, 'outside_area': outside})
                    support = required
                items.append(Paint(len(items), color, assigned, support))
        accepted = []
        if self.background in groups:
            for item in items:
                if item.color == self.background:
                    self.progress({'foundation_item': item.key, 'metrics': self.encoding(item.support).metrics()})
                    break
        edges = self.dependencies(items)
        if topological_order([item.key for item in items], edges) is None:
            items = [replace(item, support=self.requirement(item.owners))
                     if item.color != self.background else item for item in items]
            edges = self.dependencies(items)
        revisions, proposals = {}, {}
        for step in range(max(0, passes) * len(items)):
            choices = []
            by_key = {item.key: item for item in items}
            for item in items:
                if revisions.get(item.key, 0) >= passes:
                    continue
                ancestors = self.ancestors(item.key, edges)
                signature = (item.support.wkb, tuple(sorted(ancestors)))
                if item.key in proposals:
                    candidate_rows = proposals[item.key][1]
                else:
                    blocked = shapely.union_all([self.requirement(by_key[key].owners) for key in sorted(ancestors)])
                    allowed = self.domain.difference(blocked)
                    candidate_rows = self.candidates(item, allowed)[:shortlist]
                    proposals[item.key] = (signature, candidate_rows)
                    self.progress({'proposal_item': item.key, 'owners': len(item.owners),
                        'shortlist': [row[1] for row in candidate_rows], 'runtime_seconds': time.perf_counter() - self.started})
                current = self.encoding(item.support)
                for _, kind, support in candidate_rows:
                    if minimum_cost(support, self.weights) >= current.cost:
                        continue
                    fitted = self.encoding(support)
                    saving = current.cost - fitted.cost
                    if saving <= 0:
                        continue
                    changed = replace(item, support=support)
                    trial = [changed if existing.key == item.key else existing for existing in items]
                    try:
                        trial_edges = self.dependencies(trial)
                    except ValueError as failure:
                        self.diagnostics.append({'item': item.key, 'kind': kind, 'rejection': str(failure)})
                        continue
                    order = topological_order(list(by_key), trial_edges)
                    if order is None:
                        self.diagnostics.append({'item': item.key, 'kind': kind, 'rejection': 'dependency_cycle',
                                                 'edges': sorted(trial_edges)})
                        continue
                    added = max(0., support.area - self.requirement(item.owners).area)
                    choices.append((trial, trial_edges, {'item': item.key, 'kind': kind, 'saving': saving,
                        'added_hidden_area': added, 'before': current.metrics(), 'after': fitted.metrics()}))
            if not choices:
                stale = [item for item in items if revisions.get(item.key, 0) < passes
                    and proposals.get(item.key, (None,))[0] != (
                        item.support.wkb, tuple(sorted(self.ancestors(item.key, edges))))]
                if not stale:
                    break
                item = min(stale, key=lambda candidate: (-self.encoding(candidate.support).cost, candidate.key))
                ancestors = self.ancestors(item.key, edges)
                blocked = shapely.union_all([self.requirement(by_key[key].owners) for key in sorted(ancestors)])
                candidate_rows = self.candidates(item, self.domain.difference(blocked))[:shortlist]
                proposals[item.key] = ((item.support.wkb, tuple(sorted(ancestors))), candidate_rows)
                self.progress({'refresh_item': item.key, 'shortlist': [row[1] for row in candidate_rows],
                               'runtime_seconds': time.perf_counter() - self.started})
                continue
            def rank(choice):
                _, candidate_edges, diagnostic = choice
                key = diagnostic['item']
                blocked = {}
                for _, other_edges, other in choices:
                    following = other['item']
                    if (key, following) in candidate_edges and (following, key) in other_edges:
                        blocked[following] = max(blocked.get(following, 0), other['saving'])
                diagnostic['blocked_neighbor_savings'] = sum(blocked.values())
                return (-diagnostic['saving'], diagnostic['blocked_neighbor_savings'],
                        diagnostic['added_hidden_area'], len(candidate_edges), key, diagnostic['kind'])
            items, edges, diagnostic = min(choices, key=rank)
            revisions[diagnostic['item']] = revisions.get(diagnostic['item'], 0) + 1
            accepted.append(diagnostic)
            self.progress({'completion': len(accepted), **diagnostic, 'runtime_seconds': time.perf_counter() - self.started})
        if self.on_checkpoint:
            self.on_checkpoint(items, 'global', accepted)
        return self.finish(items, passes, accepted, shortlist)

    def finish(self, items, passes=1, accepted=(), shortlist=3):
        accepted = list(accepted)
        split = self.split_items(items)
        if passes > 1:
            split, local = self.improve_components(split, passes - 1, shortlist)
            accepted.extend(local)
        split, redundant = self.remove_redundant(split)
        accepted.extend(redundant)
        edges = self.dependencies(split)
        order = topological_order([item.key for item in split], edges)
        if order is None:
            raise RuntimeError('Component decomposition introduced an ordering conflict')
        by_key = {item.key: item for item in split}
        items = [by_key[key] for key in order]
        self.progress({'completed_supports': len(items), 'dependency_edges': len(edges),
                       'runtime_seconds': time.perf_counter() - self.started})
        return items, {'accepted': accepted, 'rejections': self.diagnostics, 'ordering_edges': sorted(edges),
                       'order': order, 'appearance_groups': len(set(self.colors)), 'draw_items': len(items),
                       'items': [{'key': item.key, 'fill': item.color, 'owners': sorted(item.owners),
                                  'support_area': item.support.area,
                                  'required_visible_area': self.requirement(item.owners).area} for item in items],
                       'fixed_original_ownership': True, 'runtime_seconds': time.perf_counter() - self.started}
