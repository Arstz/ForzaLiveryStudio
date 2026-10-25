"""Original-source color fitting for fixed visible region labels."""

import numpy as np

from palette_model import perceptual


def color_metrics(colors, samples, weights):
    rgb = samples.astype(np.float64)
    lab = perceptual(samples).astype(np.float64)
    target = perceptual(colors).astype(np.float64)
    mass = float(weights.sum())
    rgb_squared, distances, lab_squared = [], [], []
    for color, value in zip(colors, target):
        delta = lab - value
        rgb_squared.append(float(np.sum(weights * np.sum((rgb - color) ** 2, axis=1))))
        lengths = np.linalg.norm(delta, axis=1)
        distances.append(float(np.sum(weights * lengths)))
        lab_squared.append(float(np.sum(weights * lengths ** 2)))
    return np.array(rgb_squared), np.array(distances), np.array(lab_squared), mass


def observed_candidates(samples, weights):
    points = perceptual(samples).astype(np.float64)
    center = np.average(points, axis=0, weights=weights)
    for _ in range(64):
        distance = np.maximum(np.linalg.norm(points - center, axis=1), 1e-4)
        updated = np.average(points, axis=0, weights=weights / distance)
        if np.linalg.norm(updated - center) < 1e-5:
            center = updated
            break
        center = updated
    mean_rgb = np.average(samples, axis=0, weights=weights)
    anchors = (center, perceptual(mean_rgb))
    chosen = set()
    for anchor in anchors:
        distance = np.sum((points - anchor) ** 2, axis=1)
        count = min(32, len(samples))
        indices = np.argpartition(distance, count - 1)[:count] if count < len(samples) else np.arange(count)
        chosen.update(map(int, indices))
    chosen.add(int(np.argmax(weights)))
    return samples[sorted(chosen)]


def fit_palette(rgb, labels, palette, families, method='source', source_rmse_growth=0.):
    if method not in ('source', 'mean') or source_rmse_growth < 0 or not np.isfinite(source_rmse_growth):
        raise ValueError('Invalid palette fitting method or source-error budget')
    if rgb.shape != (*labels.shape, 3) or palette.ndim != 2 or palette.shape[1] != 3 \
            or np.any(labels < -1) or np.any(labels >= len(palette)):
        raise ValueError('Invalid source dimensions or palette labels')
    membership = [index for family in families for index in family['palette_indices']]
    if sorted(membership) != list(range(len(palette))) or any(len(family['palette_indices']) > 4 for family in families):
        raise ValueError('Each palette slot must belong to one family of at most four shades')
    if len({tuple(color) for color in palette}) != len(palette):
        raise ValueError('Input palette slots must have distinct RGB values')
    packed = (rgb[:, :, 0].astype(np.uint32) << 16) | (rgb[:, :, 1].astype(np.uint32) << 8) | rgb[:, :, 2]
    tables, old_rgb, old_perceptual, old_lab_squared, total_mass, floor_squared = [], 0., 0., 0., 0., 0.
    for index, original in enumerate(palette):
        codes, count = np.unique(packed[labels == index], return_counts=True)
        if not len(codes):
            tables.append({'colors': original[None], 'rgb': np.zeros(1), 'distance': np.zeros(1),
                           'lab_squared': np.zeros(1), 'mass': 0., 'source_codes': codes})
            continue
        samples = np.column_stack((codes >> 16, (codes >> 8) & 255, codes & 255)).astype(np.uint8)
        weights = count.astype(np.float64)
        mean = np.rint(np.average(samples, axis=0, weights=weights)).astype(np.uint8)
        proposed = observed_candidates(samples, weights) if method == 'source' else mean[None]
        colors = np.unique(np.concatenate((original[None], proposed)), axis=0)
        squared, distance, lab_squared, mass = color_metrics(colors, samples, weights)
        original_position = int(np.flatnonzero(np.all(colors == original, axis=1))[0])
        old_rgb += squared[original_position]
        old_perceptual += distance[original_position]
        old_lab_squared += lab_squared[original_position]
        total_mass += mass
        floor_squared += color_metrics(mean[None], samples, weights)[0][0]
        tables.append({'colors': colors, 'rgb': squared, 'distance': distance, 'lab_squared': lab_squared,
                       'mass': mass, 'source_codes': codes, 'original': original_position})
    if not total_mass:
        raise ValueError('The input contains no visible labeled pixels')
    old_rmse = np.sqrt(old_rgb / (3 * total_mass))
    maximum_squared = (old_rmse + source_rmse_growth) ** 2 * 3 * total_mass

    def choose(price):
        positions = []
        for table in tables:
            objective = table['distance'] + price * table['rgb'] if method == 'source' else table['rgb']
            positions.append(int(np.lexsort((table['rgb'], objective))[0]))
        fitted = np.array([table['colors'][position] for table, position in zip(tables, positions)])
        squared = sum(table['rgb'][position] for table, position in zip(tables, positions))
        distance = sum(table['distance'][position] for table, position in zip(tables, positions))
        return fitted, positions, float(squared), float(distance)

    selected = choose(0.)
    price = 0.
    if selected[2] > maximum_squared + 1e-6:
        low, high = 0., 1.
        for _ in range(64):
            if choose(high)[2] <= maximum_squared + 1e-6:
                break
            high *= 2
        selected = choose(high)
        for _ in range(48):
            middle = (low + high) / 2
            trial = choose(middle)
            if trial[2] <= maximum_squared + 1e-6:
                high, selected = middle, trial
            else:
                low = middle
        price = high
    fitted, positions, squared, distance = selected
    accepted = squared <= maximum_squared + 1e-6 and (method == 'mean' or distance <= old_perceptual + 1e-6)
    if not accepted:
        fitted = palette.copy()
        positions = [table.get('original', 0) for table in tables]
        squared, distance = old_rgb, old_perceptual
    per_color, lab_squared_total = [], 0.
    for index, (table, position) in enumerate(zip(tables, positions)):
        code = (int(fitted[index, 0]) << 16) | (int(fitted[index, 1]) << 8) | int(fitted[index, 2])
        lab_squared_total += table['lab_squared'][position]
        per_color.append({'palette_index': index, 'source_pixels': int(table['mass']),
            'before_rgb': list(map(int, palette[index])), 'after_rgb': list(map(int, fitted[index])),
            'observed_in_its_source_region': bool(np.any(table['source_codes'] == code)),
            'mean_oklab_distance_after': float(table['distance'][position] / table['mass']) if table['mass'] else 0.})
    merged_colors = {}
    for index, color in enumerate(fitted):
        merged_colors.setdefault(tuple(map(int, color)), []).append(index)
    return fitted, {'method': method, 'accepted': bool(accepted), 'source_rmse_growth_limit': source_rmse_growth,
        'changed_colors': int(np.sum(np.any(fitted != palette, axis=1))), 'palette_slots': len(fitted),
        'color_count': len(merged_colors),
        'source_rgb_rmse_before': float(old_rmse), 'source_rgb_rmse_after': float(np.sqrt(squared / (3 * total_mass))),
        'source_mean_oklab_distance_before': float(old_perceptual / total_mass),
        'source_mean_oklab_distance_after': float(distance / total_mass),
        'source_rms_oklab_distance_before': float(np.sqrt(old_lab_squared / total_mass)),
        'source_rms_oklab_distance_after': float(np.sqrt(lab_squared_total / total_mass)),
        'fixed_regions_rgb_rmse_floor': float(np.sqrt(floor_squared / (3 * total_mass))),
        'rgb_error_price': price,
        'shared_appearance_colors': [{'rgb': list(color), 'palette_indices': indices}
            for color, indices in merged_colors.items() if len(indices) > 1], 'per_color': per_color}
