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
            source_growth=8., maximum_boxes=None):
    size = source.shape[1], source.shape[0]
    guide = render(data, size)
    window = min(16, *size)
    scale = 3 * window ** 2

    def errors(first, second):
        squared = np.square(first[:, :, :3].astype(float) - second[:, :, :3]).sum(axis=2)
        return np.sqrt(window_sums(squared, window) / scale)

    fit_excess = errors(guide, actual) - local_rmse
    reference_error = errors(source, baseline)
    source_excess = errors(source, actual) - reference_error - local_growth
    guide_source_excess = errors(source, guide) - reference_error - local_growth
    planning_limit = reference_error + local_growth + np.maximum(guide_source_excess, 0)
    bad = (fit_excess > 1e-5) | (source_excess > 1e-5)
    if not np.any(bad):
        return None
    count, labels, stats, _ = cv2.connectedComponentsWithStats(bad.astype(np.uint8), connectivity=8)
    candidates = []
    for label in range(1, count):
        x, y, width, height, area = map(int, stats[label])
        component = labels[y:y + height, x:x + width] == label
        guide_bad = bool(np.any(guide_source_excess[y:y + height, x:x + width][component] > 1e-5))
        fit_bad = bool(np.any(fit_excess[y:y + height, x:x + width][component] > 1e-5))
        source_bad = guide_bad or not fit_bad and bool(np.any(
            source_excess[y:y + height, x:x + width][component] > 1e-5))
        excess = max(float(fit_excess[y:y + height, x:x + width][component].max()),
                     float(source_excess[y:y + height, x:x + width][component].max()))
        bounds = (max(0, x - 4), max(0, y - 4), min(size[0], x + width + window + 4),
                  min(size[1], y + height + window + 4))
        candidates.append((excess * area, bounds, source_bad))
    candidates.sort(key=lambda entry: (-entry[0], entry[1]))
    current = read_svg(data, size)
    patches, diagnostics = [], []

    def clipped_patches(bounds, reference):
        clip = box(*bounds)
        clipped = [(color, shape.intersection(clip)) for color, shape in reference if shape.intersects(clip)]
        owned, covered = {}, Polygon()
        for color, shape in reversed(clipped):
            visible = shape.difference(covered)
            if visible.area > .01:
                owned.setdefault(color, []).append(visible)
            covered = shapely.union_all([covered, shape])
        local = []
        for color, shapes in sorted(owned.items()):
            for part in polygons(shapely.union_all(shapes)):
                if part.area >= 1:
                    outer = Polygon(part.exterior)
                    shape = outer if outer.difference(covered).area <= .0001 else part
                    local.append((color, canonical_geometry(shape)))
        return sorted(local, key=lambda item: (-item[1].area, item[0], item[1].wkb))

    for _, bounds, source_bad in candidates[:maximum_boxes]:
        reference = original if source_bad else current
        local = clipped_patches(bounds, reference)
        patches.extend(local)
        diagnostics.append({'bounds': bounds, 'regions': len(local),
                            'reference': 'original opaque source' if source_bad else 'derived guide'})
    if not patches:
        return None
    document = ET.fromstring(data)
    for path in ET.fromstring(polygon_svg(patches, size)):
        document.append(path)
    restoration_batches = 0
    for iteration in range(3):
        target = render(ET.tostring(document), size)
        excess = errors(source, target) - planning_limit
        if not np.any(excess > 1e-5):
            break
        count, _, stats, _ = cv2.connectedComponentsWithStats((excess > 1e-5).astype(np.uint8), connectivity=8)
        additions = []
        for label in range(1, count):
            x, y, width, height, _ = map(int, stats[label])
            bounds = (max(0, x - 4), max(0, y - 4), min(size[0], x + width + window + 4),
                      min(size[1], y + height + window + 4))
            local = clipped_patches(bounds, original)
            additions.extend(local)
            diagnostics.append({'bounds': bounds, 'regions': len(local), 'reference': 'original opaque source',
                                'source_restoration_batch': iteration + 1})
        if not additions:
            break
        patches.extend(additions)
        for path in ET.fromstring(polygon_svg(additions, size)):
            document.append(path)
        restoration_batches += 1
    proposed = ET.tostring(document)
    target = render(proposed, size)
    source_error = image_error(source, target)
    target_windows = errors(source, target)
    growth = float(np.max(target_windows - reference_error))
    planning_excess = float(np.max(target_windows - planning_limit))
    output = Path(output)
    if source_error['rmse'] > image_error(source, baseline)['rmse'] + source_growth \
            or planning_excess > 1e-5 or source_error['alpha_max_error']:
        output.with_suffix('.rejected.svg').write_bytes(proposed)
        output.with_suffix('.rejected.json').write_text(json.dumps({'reason': 'Source-relative image error budget exceeded',
            'source_error': source_error, 'maximum_source_local_growth': growth, 'boxes': diagnostics}, indent=2))
        return None
    details = {'boxes': diagnostics, 'added_regions': len(patches), 'source_error': source_error,
               'maximum_source_local_growth': growth, 'preserved_prefix_paths': len(current),
               'source_restoration_batches': restoration_batches,
               'requires_final_source_validation': growth > local_growth + 1e-5,
               'planning_local_excess': planning_excess}
    output.with_suffix('.svg').write_bytes(proposed)
    output.with_suffix('.json').write_text(json.dumps(details, indent=2))
    return proposed, details
