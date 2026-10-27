"""Recover independent boundary chains after whole-contour curve rejection."""

import numpy as np
from shapely.geometry import LineString, Polygon

from fair_curves import encode_curves, fit_span, path_curves, unit


def chains(points):
    incoming = points - np.roll(points, 1, axis=0)
    outgoing = np.roll(points, -1, axis=0) - points
    length = np.linalg.norm(incoming, axis=1) * np.linalg.norm(outgoing, axis=1)
    angles = np.arccos(np.clip(np.sum(incoming * outgoing, axis=1) / np.maximum(length, 1e-20), -1, 1))
    knots = list(np.flatnonzero(angles >= np.deg2rad(48)))
    if len(knots) < 2:
        knots = sorted(set(knots + [index * len(points) // 4 for index in range(4)]))
    result = []
    for first, last in zip(knots, knots[1:] + [knots[0] + len(points)]):
        while last - first > 24:
            result.append((first, first + 16))
            first += 16
        result.append((first, last))
    return result


class ChainRecovery:
    def __init__(self, data, tolerance, flatten_tolerance=.05):
        self.points = []
        self.parts = []
        self.directions = []
        self.tolerance = tolerance
        self.flatten_tolerance = flatten_tolerance
        for ring, encoded in enumerate(data.split('M')[1:]):
            spans = list(path_curves('M' + encoded))
            if not spans:
                continue
            points = np.asarray([span[0] for span in spans])
            self.points.append(points)
            self.directions.append([(unit(spans[index - 1][-1] - spans[index - 1][-2]),
                                     unit(span[1] - span[0])) for index, span in enumerate(spans)])
            curved = [index for index, span in enumerate(spans) if len(span) == 4]
            if not curved:
                for first, last in chains(points):
                    sample = points[np.arange(first, last + 1) % len(points)]
                    self.parts.append((ring, sample, [np.asarray([a, b]) for a, b in zip(sample[:-1], sample[1:])]))
                continue
            pivot = curved[0]
            sequence = spans[pivot:] + spans[:pivot]
            position = 0
            while position < len(sequence):
                if len(sequence[position]) == 4:
                    self.parts.append((ring, None, [sequence[position]]))
                    position += 1
                    continue
                last = position
                while last < len(sequence) and len(sequence[last]) == 2:
                    last += 1
                sample = np.asarray([sequence[position][0], *[span[-1] for span in sequence[position:last]]])
                corners = [0]
                for index in range(1, len(sample) - 1):
                    if unit(sample[index] - sample[index - 1]) @ unit(sample[index + 1] - sample[index]) \
                            <= np.cos(np.deg2rad(48)):
                        corners.append(index)
                corners.append(len(sample) - 1)
                for first, stop in zip(corners[:-1], corners[1:]):
                    while first < stop:
                        end = min(stop, first + 24)
                        reference = sample[first:end + 1]
                        self.parts.append((ring, reference,
                            [np.asarray([a, b]) for a, b in zip(reference[:-1], reference[1:])]))
                        first = end
                position = last

    def encode(self, replacement=None):
        rings, commands = [], []
        for ring in range(len(self.points)):
            curves = [curve for index, (owner, _, original) in enumerate(self.parts) if owner == ring
                      for curve in (replacement[1] if replacement and index == replacement[0] else original)]
            data, boundary, _, _ = encode_curves(curves, self.flatten_tolerance)
            if not LineString(boundary).is_simple:
                return None
            rings.append(boundary)
            commands.append(data)
        if not rings or not Polygon(rings[0], rings[1:]).is_valid:
            return None
        return ''.join(commands)

    def proposals(self):
        for index, (ring, points, original) in enumerate(self.parts):
            if points is None or len(points) < 4:
                continue
            boundary = self.points[ring]
            def tangent(point, forward):
                position = int(np.argmin(np.linalg.norm(boundary - point, axis=1)))
                incoming, outgoing = self.directions[ring][position]
                return unit(incoming + outgoing) if incoming @ outgoing > np.cos(np.deg2rad(48)) \
                    else outgoing if forward else incoming
            for tolerance in (self.tolerance, self.tolerance * .5, self.tolerance * .25):
                curves = fit_span(points, tangent(points[0], True), tangent(points[-1], False), tolerance, .5,
                                  flatten_tolerance=self.flatten_tolerance)
                if not any(len(curve) == 4 for curve in curves) \
                        or sum(2 if len(curve) == 4 else 1 for curve in curves) \
                            >= sum(2 if len(curve) == 4 else 1 for curve in self.parts[index][2]):
                    continue
                data = self.encode((index, curves))
                if data:
                    yield index, curves, data

    def accept(self, index, curves):
        ring, points, _ = self.parts[index]
        self.parts[index] = ring, points, curves
