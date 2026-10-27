"""Restore bounded contour patches at measured native-render error windows."""

import json
from pathlib import Path
import xml.etree.ElementTree as ET

import cv2
import numpy as np
import shapely
from shapely.geometry import Polygon, box

from fair_curves import polygons
from paint_stack import canonical_geometry, polygon_svg
from rate_distortion import window_sums
from simplify import image_error, read_svg, render


def propose(data, original, source, baseline, actual, output, *, local_rmse=24., local_growth=32.,
            source_growth=8., maximum_boxes=3):
    size = source.shape[1], source.shape[0]
    guide = render(data, size)
    window = min(16, *size)
    scale = 3 * window ** 2

    def errors(first, second):
        squared = np.square(first[:, :, :3].astype(float) - second[:, :, :3]).sum(axis=2)
        return np.sqrt(window_sums(squared, window) / scale)

    fit_excess = errors(guide, actual) - local_rmse
    source_excess = errors(source, actual) - errors(source, baseline) - local_growth
    bad = (fit_excess > 1e-5) | (source_excess > 1e-5)
    if not np.any(bad):
        return None
    count, labels, stats, _ = cv2.connectedComponentsWithStats(bad.astype(np.uint8), connectivity=8)
    candidates = []
    for label in range(1, count):
        x, y, width, height, area = map(int, stats[label])
        component = labels[y:y + height, x:x + width] == label
        source_bad = bool(np.any(source_excess[y:y + height, x:x + width][component] > 1e-5))
        excess = max(float(fit_excess[y:y + height, x:x + width][component].max()),
                     float(source_excess[y:y + height, x:x + width][component].max()))
        bounds = (max(0, x - 4), max(0, y - 4), min(size[0], x + width + window + 4),
                  min(size[1], y + height + window + 4))
        candidates.append((excess * area, bounds, source_bad))
    candidates.sort(key=lambda entry: (-entry[0], entry[1]))
    current = read_svg(data, size)
    patches, diagnostics = [], []
    for _, bounds, source_bad in candidates[:maximum_boxes]:
        clip = box(*bounds)
        reference = original if source_bad else current
        clipped = [(color, shape.intersection(clip)) for color, shape in reference if shape.intersects(clip)]
        retained, covered = [], Polygon()
        for color, shape in reversed(clipped):
            visible = shape.difference(covered)
            if visible.area > .01:
                retained.append((color, shape))
            covered = shapely.union_all([covered, shape])
        for color, shape in reversed(retained):
            for part in polygons(shape):
                if part.area >= 1:
                    patches.append((color, canonical_geometry(part)))
        diagnostics.append({'bounds': bounds, 'reference': 'original opaque source' if source_bad else 'derived guide'})
    if not patches:
        return None
    document = ET.fromstring(data)
    for path in ET.fromstring(polygon_svg(patches, size)):
        document.append(path)
    proposed = ET.tostring(document)
    target = render(proposed, size)
    source_error = image_error(source, target)
    growth = float(np.max(errors(source, target) - errors(source, baseline)))
    if source_error['rmse'] > image_error(source, baseline)['rmse'] + source_growth \
            or growth > local_growth + 1e-5 or source_error['alpha_max_error']:
        return None
    details = {'boxes': diagnostics, 'added_regions': len(patches), 'source_error': source_error,
               'maximum_source_local_growth': growth, 'preserved_prefix_paths': len(current)}
    output = Path(output)
    output.with_suffix('.svg').write_bytes(proposed)
    output.with_suffix('.json').write_text(json.dumps(details, indent=2))
    return proposed, details
