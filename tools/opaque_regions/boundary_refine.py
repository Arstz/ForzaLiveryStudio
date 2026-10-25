"""Source-guided boundary motion with fixed labels and paint-mask topology."""

import time

import cv2
import numpy as np
from scipy import ndimage

from palette_model import perceptual


DEFAULT_SWEEPS = 12
DEFAULT_BAND = 8.
DEFAULT_BOUNDARY_WEIGHT = 12.
DEFAULT_EDGE_SCALE = 6.
NEIGHBORS = ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1))
AXIAL = np.array([1, 3, 4, 6])
BITS = 1 << np.arange(8, dtype=np.uint16)


def simple_patterns():
    result = np.zeros(256, dtype=bool)
    for pattern in range(256):
        foreground = np.zeros((3, 3), dtype=bool)
        for bit, (y, x) in enumerate(NEIGHBORS):
            foreground[y + 1, x + 1] = bool(pattern & (1 << bit))
        found, _ = ndimage.label(foreground)
        touching = np.unique(found[[0, 1, 1, 2], [1, 0, 2, 1]])
        foreground_count = np.count_nonzero(touching)
        background = ~foreground
        background[1, 1] = False
        background_count = ndimage.label(background, np.ones((3, 3)))[1]
        diagonal_foreground_count = ndimage.label(foreground, np.ones((3, 3)))[1]
        found, _ = ndimage.label(background)
        touching = np.unique(found[[0, 1, 1, 2], [1, 0, 2, 1]])
        diagonal_background_count = np.count_nonzero(touching)
        result[pattern] = foreground_count == background_count == diagonal_foreground_count == diagonal_background_count == 1
    return result


SIMPLE = simple_patterns()


def patterns(values):
    return np.sum(values * BITS, axis=1, dtype=np.uint16)


def source_edge_agreement(rgb, actual, domain, sigma=1., contrast=24.):
    def gradient(image):
        smoothed = cv2.GaussianBlur(image.astype(np.float32), (0, 0), sigma)
        return np.concatenate((cv2.Sobel(smoothed, -1, 1, 0, ksize=3, scale=.125),
                               cv2.Sobel(smoothed, -1, 0, 1, ksize=3, scale=.125)), axis=2)
    source = gradient(rgb)
    power = np.sum(source ** 2, axis=2)
    radius = int(np.ceil(3 * sigma)) + 1
    interior = cv2.erode(domain.astype(np.uint8), np.ones((2 * radius + 1, 2 * radius + 1), dtype=np.uint8)) > 0
    selected = interior & (power >= 3 * contrast ** 2)
    count = int(selected.sum())
    ratios = np.sum(source[selected] * gradient(actual)[selected], axis=1) / power[selected]
    return {'strong_source_gradient_pixels': count, 'sigma_native_pixels': sigma, 'rms_gradient_threshold': contrast,
            'mean_aligned_gradient_ratio': float(ratios.mean()) if count else 0.,
            'below_half_fraction': float(np.mean(ratios < .5)) if count else 0.}


def refine_boundaries(rgb, labels, palette, order, sweeps=DEFAULT_SWEEPS, band=DEFAULT_BAND,
                      boundary=DEFAULT_BOUNDARY_WEIGHT, edge_scale=DEFAULT_EDGE_SCALE, progress=None):
    started = time.perf_counter()
    progress = progress or (lambda entry: None)
    if sweeps < 0 or band < 0 or boundary < 0 or edge_scale <= 0 \
            or not all(np.isfinite(value) for value in (band, boundary, edge_scale)):
        raise ValueError('Invalid boundary refinement parameters')
    used = np.unique(labels[labels >= 0])
    if sorted(order) != list(used) or len(set(order)) != len(order):
        raise ValueError('The fixed paint order must contain each used label exactly once')
    original = labels.copy()
    padded = np.pad(labels, 1, constant_values=-1)
    height, width = labels.shape
    stride = width + 2
    values = padded.reshape(-1)
    offsets = np.array([y * stride + x for y, x in NEIGHBORS])
    borders = np.zeros(labels.shape, dtype=bool)
    for first, second in ((original[:, :-1], original[:, 1:]), (original[:-1], original[1:])):
        different = (first != second) & (first >= 0) & (second >= 0)
        if first.shape[0] == height:
            borders[:, :-1] |= different
            borders[:, 1:] |= different
        else:
            borders[:-1] |= different
            borders[1:] |= different
    active = (original >= 0) & (ndimage.distance_transform_edt(~borders) <= band) if borders.any() \
        else np.zeros(labels.shape, dtype=bool)
    y, x = np.indices(labels.shape)
    phases = [np.flatnonzero(active & ((y % 2) == py) & ((x % 2) == px))
              for py in range(2) for px in range(2)]
    rank = np.full(len(palette) + 1, -1, dtype=np.int32)
    rank[np.array(order) + 1] = np.arange(len(order))
    lab = perceptual(rgb).reshape(-1, 3)
    colors = perceptual(palette).astype(np.float64)
    source_rgb = rgb.reshape(-1, 3).astype(np.float64)
    palette_rgb = palette.astype(np.float64)
    initial_rgb_squared = float(np.sum((source_rgb[original.reshape(-1) >= 0]
        - palette_rgb[original[original >= 0]]) ** 2))
    completed, changed, topology_rejections = [], 0, 0
    for sweep in range(sweeps):
        updated = 0
        for phase in phases:
            indices = (phase // width + 1) * stride + phase % width + 1
            adjacent = values[indices[:, None] + offsets[AXIAL]]
            current = values[indices]
            selected = np.any((adjacent >= 0) & (adjacent != current[:, None]), axis=1)
            source_indices, indices = phase[selected], indices[selected]
            if not len(indices):
                continue
            patches = values[indices[:, None] + offsets]
            adjacent = patches[:, AXIAL]
            current = values[indices]
            source = lab[source_indices].astype(np.float64)
            old_error = np.sum((source - colors[current]) ** 2, axis=1)
            new_error = np.sum((source[:, None] - colors[np.maximum(adjacent, 0)]) ** 2, axis=2)
            neighbors = source_indices[:, None] + np.array([-width, -1, 1, width])
            neighbor_valid = adjacent >= 0
            neighbor_lab = lab[np.clip(neighbors, 0, len(lab) - 1)].astype(np.float64)
            weights = np.exp(-np.sum((source[:, None] - neighbor_lab) ** 2, axis=2) / (2 * edge_scale ** 2))
            weights *= neighbor_valid
            old_boundary = np.sum(weights * (adjacent != current[:, None]), axis=1)
            new_boundary = np.sum(weights[:, None] * (adjacent[:, :, None] != adjacent[:, None, :]), axis=2)
            gain = old_error[:, None] - new_error + boundary * (old_boundary[:, None] - new_boundary)
            old_rgb_error = np.sum((source_rgb[source_indices] - palette_rgb[current]) ** 2, axis=1)
            new_rgb_error = np.sum((source_rgb[source_indices, None] - palette_rgb[np.maximum(adjacent, 0)]) ** 2, axis=2)
            gain[(adjacent < 0) | (adjacent == current[:, None]) | (new_rgb_error >= old_rgb_error[:, None] - 1e-7)] = -np.inf
            old_simple = SIMPLE[patterns(patches == current[:, None])]
            candidate_order = np.argsort(-gain, axis=1, kind='stable')
            accepted = np.zeros(len(indices), dtype=bool)
            ranks = rank[patches + 1]
            for alternative in range(4):
                direction = candidate_order[:, alternative]
                rows = np.flatnonzero(~accepted & old_simple & (gain[np.arange(len(indices)), direction] > 1e-7))
                if not len(rows):
                    continue
                color = adjacent[rows, direction[rows]]
                keep = SIMPLE[patterns(patches[rows] == color[:, None])]
                low = np.minimum(rank[current[rows] + 1], rank[color + 1])
                high = np.maximum(rank[current[rows] + 1], rank[color + 1])
                for level in range(1, len(order)):
                    probes = np.flatnonzero(keep & (low < level) & (high >= level))
                    if len(probes):
                        keep[probes] &= SIMPLE[patterns(ranks[rows[probes]] >= level)]
                topology_rejections += int(np.count_nonzero(~keep))
                chosen = rows[keep]
                values[indices[chosen]] = color[keep]
                accepted[chosen] = True
            updated += int(accepted.sum())
        completed.append(updated)
        changed += updated
        progress({'boundary_sweep': sweep + 1, 'pixel_moves': updated})
        if not updated:
            break
    result = padded[1:-1, 1:-1].copy()
    final_rgb_squared = float(np.sum((source_rgb[result.reshape(-1) >= 0] - palette_rgb[result[result >= 0]]) ** 2))
    if not np.array_equal(result >= 0, original >= 0) or final_rgb_squared > initial_rgb_squared + 1e-6:
        raise RuntimeError('Boundary refinement violated its domain or original-image error contract')
    return result, {'method': 'Original-source boundary motion with fixed visible-label and cumulative paint-mask topology',
                    'sweeps': len(completed), 'moves_per_sweep': completed, 'pixel_moves': changed,
                    'changed_pixels': int(np.count_nonzero(result != original)),
                    'topology_rejections': topology_rejections, 'band_native_pixels': band,
                    'boundary_weight': boundary, 'edge_scale_oklab': edge_scale,
                    'palette_order': list(map(int, order)), 'palette_changed': False,
                    'original_rgb_squared_error_before': initial_rgb_squared,
                    'original_rgb_squared_error_after': final_rgb_squared,
                    'runtime_seconds': time.perf_counter() - started}
