"""Allocate contour complexity using changes in the complete painted image."""

from dataclasses import dataclass
import heapq
import itertools
import math
import time

import numpy as np
from PySide6.QtCore import Qt
from PySide6.QtGui import QColor, QImage, QPainter, QPainterPath
import shapely
from shapely.geometry import Polygon

from fair_curves import polygons
from paint_stack import canonical_geometry, geometry_metrics, polygon_svg
from simplify import application, image_error, render


def painter_path(geometry):
    path = QPainterPath()
    path.setFillRule(Qt.FillRule.OddEvenFill)
    for part in polygons(geometry):
        for ring in (part.exterior, *part.interiors):
            points = list(ring.coords)
            path.moveTo(*points[0])
            for point in points[1:]:
                path.lineTo(*point)
            path.closeSubpath()
    return path


@dataclass
class Item:
    color: str
    geometry: object
    version: int = 0

    def __post_init__(self):
        self.geometry = canonical_geometry(self.geometry)
        self.path = painter_path(self.geometry)
        self.metrics = geometry_metrics(self.geometry)


def crop_bounds(bounds, size):
    return (max(0, math.floor(bounds[0]) - 2), max(0, math.floor(bounds[1]) - 2),
            min(size[0], math.ceil(bounds[2]) + 2), min(size[1], math.ceil(bounds[3]) + 2))


def render_items(items, size, bounds=None, replacements=None):
    application()
    left, top, right, bottom = bounds or (0, 0, *size)
    image = QImage(right - left, bottom - top, QImage.Format.Format_RGBA8888)
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.translate(-left, -top)
    for index, original in enumerate(items):
        item = replacements.get(index, original) if replacements else original
        if item.geometry.is_empty:
            continue
        x0, y0, x1, y1 = item.geometry.bounds
        if x1 < left or y1 < top or x0 > right or y0 > bottom:
            continue
        painter.fillPath(item.path, QColor(item.color))
    painter.end()
    return np.frombuffer(image.constBits(), np.uint8).reshape(bottom - top, image.bytesPerLine())[
        :, :(right - left) * 4].reshape(bottom - top, right - left, 4).copy()


def proposals(geometry):
    if geometry.is_empty:
        return []
    raw = [('remove', Polygon())]
    for tolerance in (.125, .25, .5, 1., 2., 4., 8., 16.):
        raw.append((f'chords-{tolerance:g}', geometry.simplify(tolerance, preserve_topology=False)))
    filled = shapely.union_all([Polygon(part.exterior) for part in polygons(geometry)])
    raw += [('fill-holes', filled), ('hull', geometry.convex_hull), ('box', geometry.minimum_rotated_rectangle)]
    for tolerance in (.5, 1., 2., 4., 8.):
        raw.append((f'filled-chords-{tolerance:g}', filled.simplify(tolerance, preserve_topology=False)))
    for radius in (1., 2., 4., 8.):
        for label, seed in (('open', geometry), ('filled-open', filled)):
            opened = seed.buffer(-radius, quad_segs=1, join_style='mitre').buffer(
                radius, quad_segs=1, join_style='mitre')
            raw.append((f'{label}-{radius:g}', opened.simplify(radius / 2, preserve_topology=False)))
    parts = polygons(geometry)
    for part_index, part in enumerate(parts):
        if len(parts) > 1:
            raw.append((f'remove-component-{part_index}', shapely.union_all(parts[:part_index] + parts[part_index + 1:])))
        for hole_index in range(len(part.interiors)):
            replacement = Polygon(part.exterior, [ring.coords for index, ring in enumerate(part.interiors)
                                                 if index != hole_index])
            raw.append((f'fill-hole-{part_index}-{hole_index}',
                        shapely.union_all(parts[:part_index] + [replacement] + parts[part_index + 1:])))
    seen, output = set(), []
    for name, shape in raw:
        if not shape.is_valid or shape.geom_type not in ('Polygon', 'MultiPolygon'):
            continue
        key = shapely.normalize(shape).wkb
        if key not in seen:
            seen.add(key)
            output.append((name, shape))
    return output


def window_sums(error, window=16):
    integral = np.pad(error, ((1, 0), (1, 0))).cumsum(0).cumsum(1)
    return integral[window:, window:] - integral[:-window, window:] \
        - integral[window:, :-window] + integral[:-window, :-window]


class RateDistortion:
    def __init__(self, stack, source, domain, progress=None, local_growth=None, reference_image=None,
                 checkpoint=None, reference_stack=None, topology_transactions=False, complete_background=True):
        self.size = source.shape[1], source.shape[0]
        self.source = source
        self.domain = domain
        self.items = [Item(color, shape) for color, shape in stack]
        if complete_background:
            self.items[0] = Item(self.items[0].color, domain)
        self.original_stack = stack if reference_stack is None else reference_stack
        self.reference = None
        self.reservations = {}
        for color in sorted({color for color, _ in stack}):
            self.reservations[color] = len(self.items)
            self.items.append(Item(color, Polygon()))
        self.image = render_items(self.items, self.size)
        self.squared_error = float(np.square(self.image[:, :, :3].astype(float) - source[:, :, :3]).sum())
        self.metrics = {key: sum(item.metrics[key] for item in self.items)
                        for key in ('regions', 'holes', 'points', 'triangle_bound', 'cost')}
        self.topology_transactions = topology_transactions
        self.hole_surcharge = 192 if topology_transactions else 0
        self.metrics['cost'] += self.metrics['holes'] * self.hole_surcharge
        self.progress = progress or (lambda event: None)
        self.checkpoint = checkpoint
        self.render_drift = []
        self.window = min(16, self.size[0], self.size[1])
        self.local_limit = None
        if local_growth is not None:
            reference = self.image if reference_image is None else reference_image
            error = np.square(reference[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
            scale = 3 * self.window ** 2
            self.local_limit = (np.sqrt(window_sums(error, self.window) / scale) + local_growth) ** 2 * scale
        self.queue, self.serial = [], itertools.count()
        self.edits = []
        self.undo_bank = []
        self.visibility_cache = None
        self.started = time.monotonic()

    def assess(self, changes, name, refinement=False):
        if any(not item.geometry.is_valid for item in changes.values()):
            return None
        old = [self.items[index] for index in changes]
        if any(index == 0 for index in changes):
            return None
        before = {key: sum(item.metrics[key] for item in old) for key in self.metrics}
        after = {key: sum(item.metrics[key] for item in changes.values()) for key in self.metrics}
        before['cost'] += before['holes'] * self.hole_surcharge
        after['cost'] += after['holes'] * self.hole_surcharge
        saving = before['cost'] - after['cost']
        if (saving <= 0 and not refinement) or (before['triangle_bound'] < after['triangle_bound']
                and self.metrics['triangle_bound'] + after['triangle_bound'] - before['triangle_bound'] > self.budget):
            return None
        if any(not self.domain.covers(item.geometry) for item in changes.values() if not item.geometry.is_empty):
            return None
        geometries = [(shapely.union_all([self.items[index].geometry, item.geometry])
                      if self.items[index].color != item.color else
                      shapely.symmetric_difference(self.items[index].geometry, item.geometry))
                      for index, item in changes.items()]
        geometries = [shape for shape in geometries if not shape.is_empty]
        if not geometries:
            return None
        bounds = crop_bounds(shapely.total_bounds(geometries), self.size)
        if bounds[0] >= bounds[2] or bounds[1] >= bounds[3]:
            return None
        actual = render_items(self.items, self.size, bounds, changes)
        left, top, right, bottom = bounds
        previous = self.image[top:bottom, left:right]
        target = self.source[top:bottom, left:right, :3].astype(float)
        delta = float(np.square(actual[:, :, :3].astype(float) - target).sum()
                      - np.square(previous[:, :, :3].astype(float) - target).sum())
        ratio = delta / saving if saving else delta
        return {'name': name, 'changes': changes, 'bounds': bounds, 'delta': delta, 'ratio': ratio,
                'saving': saving, 'before': before, 'after': after,
                'versions': {index: self.items[index].version for index in changes}}

    def add(self, changes, name):
        candidate = self.assess(changes, name)
        if candidate:
            heapq.heappush(self.queue, (candidate['ratio'], next(self.serial), candidate))

    def add_local(self, index):
        item = self.items[index]
        for name, shape in proposals(item.geometry):
            self.add({index: Item(item.color, shape)}, name)
        if self.topology_transactions:
            if item.metrics['points'] > 8:
                self.add_hidden_shortcuts(index)
            if item.metrics['holes']:
                self.add_hole_promotions(index)
            if item.metrics['points'] > 4:
                self.add_neighbor_absorption(index)
        elif item.metrics['holes'] or item.metrics['points'] > 32:
            self.add_completion_restoration(index)

    def add_hidden_shortcuts(self, index):
        from hidden_contours import shortcut_chains
        item = self.items[index]
        _, owned = self.visibility()
        required = owned.get(index, Polygon())
        if required.is_empty:
            self.add({index: Item(item.color, Polygon())}, 'remove-hidden-support')
            return
        upper = shapely.union_all([other.geometry for other in self.items[index + 1:]])
        allowed = shapely.union_all([item.geometry, upper])
        for name, seed in (('current', item.geometry), ('hull', required.convex_hull.intersection(allowed))):
            try:
                shape = shortcut_chains(seed, required, allowed, area_tolerance=.01)
            except (ValueError, shapely.errors.GEOSException):
                continue
            self.add({index: Item(item.color, shape)}, 'shortcut-hidden-' + name)

    def add_hole_promotions(self, index):
        item = self.items[index]
        _, owned = self.visibility()
        for part in polygons(item.geometry):
            for ring in part.interiors:
                hole = Polygon(ring)
                filled = shapely.union_all([item.geometry, hole])
                patches = []
                for owner in range(index):
                    visible = owned.get(owner, Polygon()).intersection(hole)
                    if self.items[owner].color == item.color or visible.area <= .01:
                        continue
                    patch = self.items[owner].geometry.intersection(hole)
                    if not patch.is_empty:
                        patches.append((self.items[owner].color, patch))
                if not patches:
                    self.add({index: Item(item.color, filled)}, 'complete-hidden-hole')
                    continue
                upper = shapely.union_all([other.geometry for other in self.items[index + 1:]])
                simplified = []
                covered = upper
                for color, patch in reversed(patches):
                    pieces = []
                    for component in polygons(patch):
                        keep = [boundary.coords for boundary in component.interiors
                                if Polygon(boundary).difference(covered).area > .01]
                        pieces.append(Polygon(component.exterior, keep))
                    patch = shapely.union_all(pieces)
                    simplified.append(Item(color, patch))
                    covered = shapely.union_all([covered, patch])
                simplified.reverse()
                last_live = max(position for position, value in enumerate(self.items) if not value.geometry.is_empty)
                additions = len(simplified)
                while len(self.items) <= last_live + additions:
                    self.items.append(Item(item.color, Polygon()))
                changes = {index: Item(item.color, filled)}
                for position in range(index + 1, last_live + 1):
                    original = self.items[position]
                    changes[position + additions] = Item(original.color, original.geometry)
                for offset, patch in enumerate(simplified, index + 1):
                    changes[offset] = patch
                self.add(changes, 'fill-hole-and-promote-current-owners')

    def add_neighbor_absorption(self, index):
        item = self.items[index]
        _, owned = self.visibility()
        visible = owned.get(index, Polygon())
        if visible.is_empty:
            return
        owners = sorted(position for position, shape in owned.items()
                        if position != index and self.items[position].color != item.color and not shape.is_empty)
        if not owners:
            return
        tree = shapely.STRtree([owned[position] for position in owners])
        variants = [(name, shape) for name, shape in proposals(item.geometry)
                    if name in ('chords-0.5', 'chords-1', 'chords-2', 'chords-4', 'open-1', 'open-2', 'open-4')]
        for name, shape in variants:
            missing = polygons(visible.difference(shape))
            if not missing:
                continue
            additions = {}
            retained = []
            for patch in missing:
                if patch.area <= .01:
                    continue
                if patch.area > 256:
                    retained.append(patch)
                    continue
                bounds = crop_bounds(patch.bounds, self.size)
                left, top, right, bottom = bounds
                x, y = np.meshgrid(np.arange(left, right) + .5, np.arange(top, bottom) + .5)
                mask = shapely.contains_xy(patch, x, y)
                samples = self.source[top:bottom, left:right, :3][mask].astype(float)
                if not len(samples):
                    point = patch.representative_point()
                    samples = self.source[min(self.size[1] - 1, max(0, int(point.y))),
                                          min(self.size[0] - 1, max(0, int(point.x))), :3][None].astype(float)
                original = np.array([int(item.color[n:n + 2], 16) for n in (1, 3, 5)])
                original_error = np.square(samples - original).sum(axis=1).mean()
                choices = []
                for neighbor in tree.query(patch.buffer(.1), predicate='intersects'):
                    recipient = owners[int(neighbor)]
                    color = np.array([int(self.items[recipient].color[n:n + 2], 16) for n in (1, 3, 5)])
                    delta = float(np.square(samples - color).sum(axis=1).mean() - original_error)
                    impact = delta * patch.area
                    if impact <= 3 * (8 * self.window) ** 2:
                        merged = shapely.union_all([self.items[recipient].geometry, patch])
                        metric = geometry_metrics(merged)
                        extra = metric['cost'] - self.items[recipient].metrics['cost'] \
                            + self.hole_surcharge * (metric['holes'] - self.items[recipient].metrics['holes'])
                        choices.append((extra, impact, recipient))
                if not choices:
                    retained.append(patch)
                    continue
                recipient = min(choices)[2]
                additions.setdefault(recipient, []).append(patch)
            if additions:
                shape = shapely.union_all([shape, *retained])
                changes = {index: Item(item.color, shape)}
                for recipient, patches in additions.items():
                    merged = shapely.union_all([self.items[recipient].geometry, *patches])
                    changes[recipient] = Item(self.items[recipient].color, merged)
                self.add(changes, 'absorb-neighbor-fragments-' + name)

    def add_completion_restoration(self, index):
        if self.reference is None:
            from bottom_up import visible_masks
            self.reference, _, _ = visible_masks(self.original_stack)
        item = self.items[index]
        upper = shapely.union_all([other.geometry for other in self.items[index + 1:]
                                  if not other.geometry.is_empty])
        filled = shapely.union_all([Polygon(part.exterior) for part in polygons(item.geometry)])
        completions = [('filled', filled)]
        if item.metrics['points'] > 32:
            completions += [('hull', item.geometry.convex_hull), ('box', item.geometry.minimum_rotated_rectangle)]
        seen = set()
        for kind, completed in completions:
            completed = completed.intersection(self.domain)
            key = shapely.normalize(completed).wkb
            if key in seen:
                continue
            seen.add(key)
            exposed = completed.difference(item.geometry).difference(upper)
            if exposed.is_empty:
                continue
            patches = {color: owned.intersection(exposed) for color, owned in self.reference.items()
                       if color != item.color and owned.intersects(exposed)}
            for tolerance in (.5, 2., 4., 8.):
                changes = {index: Item(item.color, completed)}
                for color, patch in patches.items():
                    patch = patch.simplify(tolerance, preserve_topology=False)
                    if patch.is_empty:
                        continue
                    above = [position for position in range(index + 1, len(self.items))
                             if self.items[position].color == color and not self.items[position].geometry.is_empty]
                    if above:
                        position = above[-1]
                    else:
                        empty = [position for position in range(index + 1, len(self.items))
                                 if self.items[position].color == color and self.items[position].geometry.is_empty]
                        if empty:
                            position = empty[0]
                        else:
                            position = len(self.items)
                            self.items.append(Item(color, Polygon()))
                    combined = shapely.union_all([self.items[position].geometry, patch])
                    changes[position] = Item(color, combined)
                self.add(changes, f'complete-{kind}-restore-{tolerance:g}')

    def add_groups(self, global_completions=False):
        colors = sorted({item.color for item in self.items[1:] if not item.geometry.is_empty})
        priorities = sorted(colors, key=lambda color: (-sum(item.metrics['cost'] for item in self.items[1:]
            if item.color == color), color))[:4] if global_completions else []
        for color in colors:
            members = [index for index, item in enumerate(self.items) if index and item.color == color
                       and not item.geometry.is_empty]
            if sum(self.items[index].metrics['regions'] for index in members) < 2:
                continue
            merged = shapely.union_all([self.items[index].geometry for index in members])
            alternatives = [('same-fill-union', merged), ('same-fill-filled',
                shapely.union_all([Polygon(part.exterior) for part in polygons(merged)])),
                ('same-fill-hull', merged.convex_hull)]
            for position in sorted({members[0], members[-1], members[len(members) // 2]}):
                for name, shape in alternatives:
                    changes = {index: Item(color, Polygon()) for index in members}
                    changes[position] = Item(color, shape)
                    self.add(changes, name)
            if color not in priorities:
                continue
            self.progress({'distortion_group_trial': color, 'members': len(members),
                           'seconds': time.monotonic() - self.started})
            _, current_owned = self.visibility()
            required = shapely.union_all([current_owned.get(index, Polygon()) for index in members])
            if required.is_empty:
                continue
            for position in sorted({members[0], members[-1]}):
                for name, shape in (('hull', required.convex_hull),
                                    ('box', required.minimum_rotated_rectangle)):
                    changes = {index: Item(color, Polygon()) for index in members}
                    changes[position] = Item(color, shape.intersection(self.domain))
                    self.restore_group(changes, f'same-fill-{name}-restore')

    def visibility(self, replacements=None):
        if replacements is None and self.visibility_cache and self.visibility_cache[0] == len(self.edits):
            return self.visibility_cache[1]
        from bottom_up import visible_masks
        indices, stack = [], []
        for index, original in enumerate(self.items):
            item = replacements.get(index, original) if replacements else original
            if not item.geometry.is_empty:
                indices.append(index)
                stack.append((item.color, item.geometry))
        colors, owned, _ = visible_masks(stack)
        result = colors, dict(zip(indices, owned))
        if replacements is None:
            self.visibility_cache = len(self.edits), result
        return result

    def restore_group(self, changes, name):
        if self.reference is None:
            from bottom_up import visible_masks
            self.reference, _, _ = visible_masks(self.original_stack)
        before, _ = self.visibility()
        after, owned = self.visibility(changes)
        changed = shapely.union_all([shapely.symmetric_difference(before.get(color, Polygon()),
                                    after.get(color, Polygon())) for color in set(before) | set(after)])
        if changed.is_empty:
            self.add(changes, name)
            return
        patches = {color: required.intersection(changed).difference(after.get(color, Polygon()))
                   for color, required in self.reference.items() if required.intersects(changed)}
        patches = {color: shape for color, shape in patches.items() if not shape.is_empty and shape.area > 1e-8}
        last = max(owned, default=0)
        positions = {}
        for color in patches:
            empty = [position for position in range(last + 1, len(self.items))
                     if self.items[position].color == color and self.items[position].geometry.is_empty]
            if empty:
                positions[color] = empty[0]
            else:
                positions[color] = len(self.items)
                self.items.append(Item(color, Polygon()))
        for tolerance in (.5, 2., 4., 8.):
            trial = {index: Item(item.color, item.geometry) for index, item in changes.items()}
            corrections = []
            for color, patch in patches.items():
                patch = patch.simplify(tolerance, preserve_topology=False)
                if not patch.is_empty:
                    trial[positions[color]] = Item(color, patch)
                    corrections.append(patch)
            covered = shapely.union_all(corrections)
            for index, required in owned.items():
                if index and (required.is_empty or covered.covers(required)):
                    item = trial.get(index, self.items[index])
                    trial[index] = Item(item.color, Polygon())
            self.add(trial, f'{name}-{tolerance:g}')

    def add_order_pair(self, first, second):
        a, b = self.items[first], self.items[second]
        if a.color == b.color or a.geometry.is_empty or b.geometry.is_empty \
                or not a.geometry.envelope.intersects(b.geometry.envelope):
            return
        def completions(item):
            shapes = [item.geometry]
            if item.metrics['holes']:
                shapes.append(shapely.union_all([Polygon(part.exterior) for part in polygons(item.geometry)]))
            if item.metrics['points'] > 4:
                shapes.append(item.geometry.convex_hull)
            return shapes
        for first_shape in completions(a):
            for second_shape in completions(b):
                self.add({first: Item(b.color, second_shape), second: Item(a.color, first_shape)},
                         'reorder-and-complete')

    def add_ordering(self, changed=None):
        live = [index for index, item in enumerate(self.items) if index and not item.geometry.is_empty]
        for first, second in zip(live[:-1], live[1:]):
            if changed is None or first in changed or second in changed:
                self.add_order_pair(first, second)

    def local_valid(self, candidate):
        if self.local_limit is None:
            return True
        left, top, right, bottom = candidate['bounds']
        margin = self.window - 1
        a, b, c, d = max(0, left - margin), max(0, top - margin), \
            min(self.size[0], right + margin), min(self.size[1], bottom + margin)
        image = self.image[b:d, a:c].copy()
        image[top - b:bottom - b, left - a:right - a] = render_items(
            self.items, self.size, candidate['bounds'], candidate['changes'])
        error = np.square(image[:, :, :3].astype(float) - self.source[b:d, a:c, :3]).sum(axis=2)
        local = window_sums(error, self.window)
        allowed = self.local_limit[b:d - self.window + 1, a:c - self.window + 1]
        return bool(np.all(local <= allowed + 1e-5))

    def reconcile(self):
        image = render_items(self.items, self.size)
        encoded = render(polygon_svg([(item.color, item.geometry) for item in self.items
                                     if not item.geometry.is_empty], self.size), self.size)
        export_error = image_error(image, encoded)
        changed = np.abs(image.astype(np.int16) - self.image.astype(np.int16))
        self.render_drift.append({'edits': len(self.edits), 'maximum_channel_drift': int(changed.max()),
                                  'changed_pixels': int(np.count_nonzero(np.any(changed, axis=2))),
                                  'export_error': export_error})
        self.image = encoded
        self.squared_error = float(np.square(encoded[:, :, :3].astype(float) - self.source[:, :, :3]).sum())
        if self.checkpoint:
            self.checkpoint([(item.color, item.geometry) for item in self.items if not item.geometry.is_empty],
                {**self.metrics, 'edits': self.edits, 'source_error': image_error(self.source, encoded),
                 'render_drift': self.render_drift, 'seconds': time.monotonic() - self.started})

    def refine(self, triangle_budget, max_edits=128):
        self.budget = triangle_budget
        remaining = list(self.undo_bank[-96:])
        edits = []
        while remaining and len(edits) < max_edits:
            candidates = []
            for changes in remaining:
                candidate = self.assess(changes, 'restore-contour', refinement=True)
                if candidate and candidate['delta'] < -1e-6 and self.local_valid(candidate):
                    density = -candidate['delta'] / max(1., -candidate['saving'])
                    candidates.append((density, candidate, changes))
            if not candidates:
                break
            _, candidate, changes = max(candidates, key=lambda row: row[0])
            remaining = [entry for entry in remaining if entry is not changes]
            left, top, right, bottom = candidate['bounds']
            self.image[top:bottom, left:right] = render_items(self.items, self.size, candidate['bounds'], changes)
            for index, item in changes.items():
                self.items[index] = Item(item.color, item.geometry, self.items[index].version + 1)
            for key in self.metrics:
                self.metrics[key] += candidate['after'][key] - candidate['before'][key]
            self.squared_error += candidate['delta']
            edits.append({'source_error_delta': candidate['delta'], **self.metrics})
        self.reconcile()
        return [(item.color, item.geometry) for item in self.items if not item.geometry.is_empty], {
            'geometry_budget': triangle_budget, 'within_geometry_budget': self.metrics['triangle_bound'] <= triangle_budget,
            **self.metrics, 'edits': self.edits, 'refinements': edits,
            'source_error': image_error(self.source, self.image), 'render_drift': self.render_drift,
            'seconds': time.monotonic() - self.started}

    def refine_from_reference(self, triangle_budget, max_edits=16):
        self.budget = triangle_budget
        ranked = []
        for index, item in enumerate(self.items[1:], 1):
            if item.geometry.is_empty:
                continue
            matching = []
            for color, shape in self.original_stack[1:]:
                if color != item.color or not shape.intersects(item.geometry):
                    continue
                overlap = shape.intersection(item.geometry).area
                matching.append((overlap / max(1e-8, shape.area + item.geometry.area - overlap), shape))
            matching.sort(key=lambda row: -row[0])
            seen = set()
            for _, reference in matching[:2]:
                for tolerance in (.25, .5, 1., 2., 4., 8., 16.):
                    shape = reference.simplify(tolerance, preserve_topology=False)
                    if not shape.is_valid or shape.is_empty or shape.wkb in seen:
                        continue
                    seen.add(shape.wkb)
                    changes = {index: Item(item.color, shape)}
                    candidate = self.assess(changes, 'reference-contour', refinement=True)
                    if candidate and candidate['delta'] < -1e-6 and self.local_valid(candidate):
                        density = -candidate['delta'] / max(1., -candidate['saving'])
                        ranked.append((density, changes))
            if index % 16 == 0:
                self.progress({'reference_contours': index, 'candidates': len(ranked),
                               'seconds': time.monotonic() - self.started})
        ranked.sort(key=lambda row: row[0])
        self.undo_bank = [changes for _, changes in ranked[-96:]]
        return self.refine(triangle_budget, max_edits)

    def run(self, triangle_budget, max_edits=2000, source_rmse_limit=None, continue_below_budget=False):
        if triangle_budget < 1 or max_edits < 0:
            raise ValueError('Use a positive geometry budget and nonnegative search effort')
        self.budget = triangle_budget
        for index in range(1, len(self.items)):
            self.add_local(index)
            if index % 32 == 0:
                self.progress({'distortion_initialized': index, 'items': len(self.items),
                               'seconds': time.monotonic() - self.started})
        self.add_groups(global_completions=not self.topology_transactions)
        self.add_ordering()
        while self.queue and len(self.edits) < max_edits:
            _, serial, candidate = heapq.heappop(self.queue)
            if any(self.items[index].version != version for index, version in candidate['versions'].items()):
                continue
            current = self.assess(candidate['changes'], candidate['name'])
            if current is None:
                continue
            if self.queue and current['ratio'] > self.queue[0][0] + 1e-8:
                heapq.heappush(self.queue, (current['ratio'], serial, current))
                continue
            if not continue_below_budget and self.metrics['triangle_bound'] <= triangle_budget and current['delta'] > 0:
                break
            new_error = self.squared_error + current['delta']
            if source_rmse_limit is not None and new_error > source_rmse_limit ** 2 * self.source.shape[0] * self.source.shape[1] * 3:
                continue
            if not self.local_valid(current):
                continue
            left, top, right, bottom = current['bounds']
            self.undo_bank.append({index: self.items[index] for index in current['changes']})
            self.image[top:bottom, left:right] = render_items(self.items, self.size, current['bounds'], current['changes'])
            self.squared_error = new_error
            changed = []
            for index, item in current['changes'].items():
                item.version = self.items[index].version + 1
                self.items[index] = item
                changed.append(index)
            for key in self.metrics:
                self.metrics[key] += current['after'][key] - current['before'][key]
            self.edits.append({'kind': current['name'], 'items': sorted(changed),
                'source_error_delta': current['delta'], 'complexity_reduction': current['saving'],
                **self.metrics})
            if len(self.edits) % 64 == 0:
                self.reconcile()
            if len(self.edits) % 16 == 0 or self.metrics['triangle_bound'] <= triangle_budget:
                self.progress({'distortion_edits': len(self.edits), **self.metrics,
                    'source_rmse': math.sqrt(max(0., self.squared_error) / (self.source.shape[0] * self.source.shape[1] * 3)),
                    'seconds': time.monotonic() - self.started})
            for index in changed:
                if not self.items[index].geometry.is_empty:
                    self.add_local(index)
            self.add_ordering(set(changed))
            if len(self.edits) % 64 == 0:
                self.add_groups(global_completions=not self.topology_transactions and len(self.edits) % 256 == 0)
        stack = [(item.color, item.geometry) for item in self.items if not item.geometry.is_empty]
        self.reconcile()
        return stack, {'geometry_budget': triangle_budget, 'within_geometry_budget': self.metrics['triangle_bound'] <= triangle_budget,
            **self.metrics, 'edits': self.edits, 'source_error': image_error(self.source, self.image),
            'render_drift': self.render_drift,
            'seconds': time.monotonic() - self.started}
