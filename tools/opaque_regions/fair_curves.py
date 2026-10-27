"""Bounded mixed line/cubic fitting for closed region contours."""

from dataclasses import dataclass, field
from functools import lru_cache
import math
import re

import cv2
import numpy as np
from scipy.ndimage import gaussian_filter1d
import shapely
from shapely.geometry import LineString, MultiPolygon, Polygon


def polygons(geometry):
    if geometry.is_empty:
        return []
    if geometry.geom_type == 'Polygon':
        return [geometry]
    return [part for child in getattr(geometry, 'geoms', ()) for part in polygons(child)]


def flatten(curve, tolerance=.05):
    if len(curve) == 2:
        return curve
    first, control, second, last = curve
    chord = last - first
    length = np.linalg.norm(chord)
    if length:
        cross = lambda vector: abs(chord[0] * vector[1] - chord[1] * vector[0]) / length
        flat = max(cross(control - first), cross(second - first)) <= tolerance
        parameters = np.array([np.dot(control - first, chord), np.dot(second - first, chord)]) / length ** 2
        forward = 0 <= parameters[0] <= parameters[1] <= 1
    else:
        flat = max(np.linalg.norm(control - first), np.linalg.norm(second - first)) <= tolerance
        forward = True
    if flat and forward:
        return np.array([first, last])
    ab, bc, cd = (first + control) / 2, (control + second) / 2, (second + last) / 2
    abc, bcd = (ab + bc) / 2, (bc + cd) / 2
    middle = (abc + bcd) / 2
    return np.vstack((flatten(np.array([first, ab, abc, middle]), tolerance)[:-1],
                      flatten(np.array([middle, bcd, cd, last]), tolerance)))


def unit(vector):
    norm = np.linalg.norm(vector)
    return vector / norm if norm > 1e-12 else np.zeros(2)


def line_distance(points, start, end):
    chord = end - start
    squared = np.dot(chord, chord)
    if squared <= 1e-20:
        return np.linalg.norm(points - start, axis=1)
    parameter = np.clip((points - start) @ chord / squared, 0, 1)
    return np.linalg.norm(points - start - parameter[:, None] * chord, axis=1)


def fit_span(points, start_tangent, end_tangent, tolerance, fairness, depth=0, flatten_tolerance=.05):
    start, end = points[0], points[-1]
    distance = line_distance(points, start, end)
    if distance.max(initial=0) <= tolerance:
        return [np.array([start, end])]
    arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
    if len(points) <= 3 or depth >= 24 or arc[-1] <= 1e-8:
        return [np.array([a, b]) for a, b in zip(points[:-1], points[1:])]
    t = arc / arc[-1]
    first, second = 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t ** 2
    design = np.stack((first[:, None] * start_tangent, -second[:, None] * end_tangent), axis=2)
    base = ((1 - t) ** 3 + first)[:, None] * start + (second + t ** 3)[:, None] * end
    target = points - base
    regularizer = np.column_stack((3 * start_tangent, 3 * end_tangent))
    weight = fairness * len(points) / 81
    lhs = np.einsum('nki,nkj->ij', design, design) + weight * regularizer.T @ regularizer
    rhs = np.einsum('nki,nk->i', design, target) + weight * regularizer.T @ (2 * (end - start))
    lengths = np.linalg.lstsq(lhs, rhs, rcond=None)[0]
    lengths = np.clip(lengths, 0, arc[-1] * .8)
    curve = np.array([start, start + lengths[0] * start_tangent,
                      end - lengths[1] * end_tangent, end])
    fitted = ((1 - t) ** 3)[:, None] * start + first[:, None] * curve[1] \
        + second[:, None] * curve[2] + (t ** 3)[:, None] * end
    errors = np.linalg.norm(points - fitted, axis=1)
    reference, actual = LineString(points), LineString(flatten(curve, flatten_tolerance))
    if errors.max() <= tolerance and actual.is_simple \
            and reference.hausdorff_distance(actual) <= tolerance:
        return [curve]
    split = int(np.argmax(errors))
    split = min(len(points) - 2, max(1, split))
    tangent = unit(points[min(len(points) - 1, split + 2)] - points[max(0, split - 2)])
    return fit_span(points[:split + 1], start_tangent, tangent, tolerance, fairness, depth + 1, flatten_tolerance) \
        + fit_span(points[split:], tangent, end_tangent, tolerance, fairness, depth + 1, flatten_tolerance)


def fit_loop(coordinates, tolerance=1., fairness=1., flatten_tolerance=.05):
    points = np.asarray(coordinates, dtype=float)
    if np.linalg.norm(points[0] - points[-1]) < 1e-9:
        points = points[:-1]
    points = points[np.r_[True, np.linalg.norm(np.diff(points, axis=0), axis=1) > 0]]
    if len(points) < 3:
        raise ValueError('A closed contour needs three distinct points')
    closed = np.vstack((points, points[0]))
    perimeter = np.r_[0, np.cumsum(np.linalg.norm(np.diff(closed, axis=0), axis=1))]
    if perimeter[-1] <= 4 * tolerance:
        return [np.array([a, b]) for a, b in zip(closed[:-1], closed[1:])]
    count = max(12, int(math.ceil(perimeter[-1] / max(.25, min(1., tolerance / 2)))))
    positions = np.arange(count) * perimeter[-1] / count
    samples = np.column_stack([np.interp(positions, perimeter, closed[:, axis]) for axis in range(2)])
    coarse = cv2.approxPolyDP(points.astype(np.float32), max(tolerance, .1), True).reshape(-1, 2)
    corners = []
    for index, point in enumerate(coarse):
        incoming, outgoing = point - coarse[index - 1], coarse[(index + 1) % len(coarse)] - point
        angle = math.acos(float(np.clip(unit(incoming) @ unit(outgoing), -1, 1)))
        if angle >= math.radians(48) and min(np.linalg.norm(incoming), np.linalg.norm(outgoing)) >= 2 * tolerance:
            nearest = int(np.argmin(np.linalg.norm(samples - point, axis=1)))
            samples[nearest] = point
            corners.append(nearest)
    knots = sorted(set(corners))
    if len(knots) < 3:
        knots = sorted(set(knots + [int(count * fraction / 4) for fraction in range(4)]))
    smoothing = min(count / 8, max(.5, fairness * tolerance * count / perimeter[-1]))
    smooth = gaussian_filter1d(samples, smoothing, axis=0, mode='wrap')
    for index in corners:
        smooth[index] = samples[index]
    tangent_radius = min(count // 4, max(1, int(round(max(2., 3 * tolerance) * count / perimeter[-1]))))
    curves = []
    for first, last in zip(knots, knots[1:] + [knots[0] + count]):
        indices = np.arange(first, last + 1) % count
        reference = samples[indices]
        start_tangent = unit(smooth[(first + tangent_radius) % count] - smooth[(first - tangent_radius) % count])
        end_tangent = unit(smooth[(last + tangent_radius) % count] - smooth[(last - tangent_radius) % count])
        if first in corners:
            start_tangent = unit(reference[min(len(reference) - 1, tangent_radius)] - reference[0])
        if last % count in corners:
            end_tangent = unit(reference[-1] - reference[max(0, len(reference) - 1 - tangent_radius)])
        curves.extend(fit_span(reference, start_tangent, end_tangent, tolerance * .8, fairness,
                               flatten_tolerance=flatten_tolerance))
    actual = LineString(np.vstack([flatten(curve, flatten_tolerance)[:-1] for curve in curves] + [curves[-1][-1:]]))
    if not actual.is_simple or LineString(closed).hausdorff_distance(actual) > tolerance:
        simplified = LineString(closed).simplify(tolerance * .5)
        result = np.asarray(simplified.coords)
        return [np.array([a, b]) for a, b in zip(result[:-1], result[1:])]
    return curves


@dataclass
class CostWeights:
    paths: float = 256
    holes: float = 96


@dataclass
class Encoding:
    d: str
    geometry: object
    paths: int
    holes: int
    lines: int
    cubics: int
    weights: CostWeights = field(default_factory=CostWeights)

    @property
    def segments(self):
        return self.lines + self.cubics

    @property
    def cost(self):
        return (self.weights.paths + 1) * self.paths + self.weights.holes * self.holes + self.lines + 3 * self.cubics

    def metrics(self):
        return {'paths': self.paths, 'components': self.paths, 'holes': self.holes,
                'segments': self.segments, 'lines': self.lines, 'cubics': self.cubics,
                'bezier_controls': 2 * self.cubics, 'cost': self.cost}


def encode_ring(coordinates, tolerance, fairness, flatten_tolerance, bounds):
    curves = fit_loop(coordinates, tolerance, fairness, flatten_tolerance) if tolerance > 0 else [
        np.array([a, b]) for a, b in zip(coordinates[:-1], coordinates[1:])]
    if bounds is not None:
        curves = [np.clip(curve, bounds[:2], bounds[2:]) for curve in curves]
    return encode_curves(curves, flatten_tolerance)


def encode_curves(curves, flatten_tolerance):
    point = curves[0][0]
    commands = [f'M{point[0]:.17g} {point[1]:.17g}']
    lines = cubics = 0
    for curve in curves:
        if len(curve) == 2:
            commands.append(f'L{curve[-1,0]:.17g} {curve[-1,1]:.17g}')
            lines += 1
        else:
            commands.append('C' + ' '.join(f'{value:.17g}' for value in curve[1:].reshape(-1)))
            cubics += 1
    commands.append('Z')
    ring = np.vstack([flatten(curve, flatten_tolerance)[:-1] for curve in curves] + [curves[-1][-1:]])
    return ''.join(commands), ring, lines, cubics


def path_curves(data):
    tokens = re.findall(r'[MLCZ]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?', data)
    position, current, first = 0, None, None
    while position < len(tokens):
        command = tokens[position]
        count = {'M': 2, 'L': 2, 'C': 6, 'Z': 0}[command]
        values = np.array(list(map(float, tokens[position + 1:position + count + 1]))).reshape(-1, 2)
        position += count + 1
        if command == 'M':
            current = first = values[0]
        elif command == 'Z':
            if not np.array_equal(current, first):
                yield np.vstack((current, first))
            current = first
        else:
            curve = np.vstack((current, values))
            current = curve[-1]
            yield curve


def path_counts(data):
    kinds = [len(curve) for curve in path_curves(data)]
    return kinds.count(2), kinds.count(4)


class RingBank:
    def __init__(self, flatten_tolerance=.05, coordinate_digits=8):
        self.rings, self.spans = {}, {}
        self.flatten_tolerance = flatten_tolerance
        self.coordinate_digits = coordinate_digits

    def key(self, geometry):
        snapped = shapely.set_precision(geometry, 10. ** -self.coordinate_digits)
        if snapped.is_empty or snapped.geom_type != 'Polygon':
            return None
        return shapely.normalize(snapped.simplify(0)).wkb

    def add(self, data, geometry):
        encoded = (data, np.asarray(geometry.exterior.coords), *path_counts(data))
        key = self.key(geometry)
        if key is None:
            return
        previous = self.rings.get(key)
        if previous is None or encoded[2] + 3 * encoded[3] < previous[2] + 3 * previous[3]:
            self.rings[key] = encoded
        for curve in path_curves(data):
            if len(curve) != 4:
                continue
            samples = np.round(flatten(curve, self.flatten_tolerance), self.coordinate_digits)
            chain = tuple(map(tuple, LineString(samples).simplify(0).coords))
            if len(chain) >= 4:
                self.spans.setdefault(chain[0], {}).setdefault(chain, curve)
                self.spans.setdefault(chain[-1], {}).setdefault(tuple(reversed(chain)), curve[::-1])

    def find(self, coordinates):
        return self.rings.get(self.key(Polygon(coordinates)))

    def restore(self, coordinates):
        raw = np.asarray(coordinates)
        rounded = np.round(raw, self.coordinate_digits)
        originals = {}
        for point, snapped in zip(raw, rounded):
            originals.setdefault(tuple(snapped), point)
        keys = list(map(tuple, LineString(rounded).simplify(0).coords))[:-1]
        starts = [index for index, point in enumerate(keys) if point in self.spans]
        if not starts:
            return None
        start = starts[0]
        keys = keys[start:] + keys[:start]
        keys.append(keys[0])
        points = np.asarray([originals[point] for point in keys])
        curves, position, recovered = [], 0, 0
        while position + 1 < len(keys):
            selected = None
            choices = self.spans.get(keys[position], {})
            for chain, curve in sorted(choices.items(), key=lambda value: (-len(value[0]), value[0])):
                if tuple(keys[position:position + len(chain)]) == chain:
                    selected = curve.copy()
                    selected[0], selected[-1] = points[position], points[position + len(chain) - 1]
                    position += len(chain) - 1
                    recovered += 1
                    break
            if selected is None:
                selected = points[position:position + 2]
                position += 1
            curves.append(selected)
        return encode_curves(curves, self.flatten_tolerance) if recovered else None


@lru_cache(maxsize=4096)
def encode_polygon(data, tolerance, fairness, flatten_tolerance=.05, bounds=None, ring_bank=None):
    part = shapely.from_wkb(data)
    boundaries = [np.asarray(boundary.coords) for boundary in [part.exterior, *part.interiors]]
    rings = list(boundaries)
    encodings = []
    for index, boundary in enumerate(boundaries):
        local_tolerance = tolerance
        original = ring_bank.find(boundary) if ring_bank is not None else None
        restored = ring_bank.restore(boundary) if ring_bank is not None else None
        while True:
            encoded = encode_ring(boundary, local_tolerance, fairness, flatten_tolerance, bounds)
            alternatives = sorted([item for item in (original, restored, encoded) if item is not None],
                                  key=lambda item: item[2] + 3 * item[3])
            selected = None
            for alternative in alternatives:
                trial = list(rings)
                trial[index] = alternative[1]
                actual = Polygon(trial[0], trial[1:])
                if actual.is_valid and not actual.is_empty:
                    selected, rings = alternative, trial
                    break
            if selected is not None:
                encodings.append(selected)
                break
            if local_tolerance == 0:
                raise ValueError('The complete filled geometry is invalid')
            local_tolerance = local_tolerance / 2 if local_tolerance > .2 else 0
    actual = Polygon(rings[0], rings[1:])
    if tolerance > 0 and (part.difference(actual.buffer(tolerance)).area > 1e-6
                         or actual.difference(part.buffer(tolerance)).area > 1e-6):
        return encode_polygon(data, 0, fairness, flatten_tolerance, bounds)
    return Encoding(''.join(item[0] for item in encodings), actual, 1, len(part.interiors),
                    sum(item[2] for item in encodings), sum(item[3] for item in encodings))


def encode_geometry(geometry, tolerance=1., fairness=1., flatten_tolerance=.05, bounds=None, originals=None,
                    ring_bank=None):
    parts = polygons(geometry)
    originals = originals or {}
    def unchanged(index):
        return originals.get(shapely.normalize(parts[index]).wkb) or encode_polygon(
            parts[index].wkb, 0, fairness, flatten_tolerance, bounds)
    encoded = [encode_polygon(part.wkb, tolerance, fairness, flatten_tolerance, bounds, ring_bank) for part in parts]
    if originals:
        for index, part in enumerate(parts):
            original = originals.get(shapely.normalize(part).wkb)
            if original is not None and original.cost <= encoded[index].cost:
                encoded[index] = original
    for attempt in range(3 if len(encoded) > 1 else 0):
        tree = shapely.STRtree([item.geometry for item in encoded])
        conflicts = set()
        for index, item in enumerate(encoded):
            for other in tree.query(item.geometry, predicate='intersects'):
                if other > index and item.geometry.intersection(encoded[other].geometry).area > 1e-7:
                    conflicts.update((index, int(other)))
        for index in conflicts:
            encoded[index] = unchanged(index)
        if not conflicts:
            break
    else:
        if len(encoded) > 1:
            encoded = [unchanged(index) for index in range(len(parts))]
    actual = shapely.union_all([item.geometry for item in encoded]) if encoded else Polygon()
    if not actual.is_valid:
        raise ValueError('The complete fitted region is invalid')
    return Encoding(''.join(item.d for item in encoded), actual, len(encoded),
                    sum(item.holes for item in encoded), sum(item.lines for item in encoded),
                    sum(item.cubics for item in encoded))


def boundary_measurements(geometries, arm=4., angle_degrees=3.):
    length, eligible, reversals = 0., 0., 0
    for geometry in geometries:
        for part in polygons(geometry):
            for boundary in [part.exterior, *part.interiors]:
                points = np.asarray(boundary.coords)
                arc = np.r_[0, np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))]
                keep = np.r_[True, np.diff(arc) > 1e-8]
                points, arc = points[keep], arc[keep]
                length += arc[-1]
                if arc[-1] < 8 * arm:
                    continue
                count = int(math.ceil(arc[-1]))
                positions = np.arange(count) * arc[-1] / count
                samples = np.column_stack([np.interp(positions, arc, points[:, axis]) for axis in range(2)])
                step = max(1, int(round(arm * count / arc[-1])))
                before = samples - np.roll(samples, step, axis=0)
                after = np.roll(samples, -step, axis=0) - samples
                angles = np.arctan2(before[:, 0] * after[:, 1] - before[:, 1] * after[:, 0], np.sum(before * after, axis=1))
                active = np.sign(angles[np.abs(angles) >= math.radians(angle_degrees)])
                reversals += int(np.count_nonzero(active != np.roll(active, 1))) if len(active) > 1 else 0
                eligible += arc[-1]
    return {'boundary_length_native_pixels': float(length), 'eligible_boundary_length_native_pixels': float(eligible),
            'turning_direction_reversals': reversals, 'reversals_per_1000_boundary_pixels': 1000 * reversals / max(1., eligible),
            'arm_native_pixels': arm, 'angle_threshold_degrees': angle_degrees,
            'includes_hidden_boundaries_and_genuine_features': True}
