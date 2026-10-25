"""Shared tonal palettes for the native-resolution region experiment."""

import numpy as np
from scipy import ndimage, signal
from scipy.spatial import cKDTree


def perceptual(rgb):
    value = np.asarray(rgb, dtype=np.float32) / 255
    value = np.where(value <= .04045, value / 12.92, ((value + .055) / 1.055) ** 2.4)
    matrix = np.array([[.4122214708, .5363325363, .0514459929],
                       [.2119034982, .6806995451, .1073969566],
                       [.0883024619, .2817188376, .6299787005]], dtype=np.float32)
    roots = np.cbrt(value @ matrix.T)
    return 100 * (roots @ np.array([[.2104542553, .7936177850, -.0040720468],
        [1.9779984951, -2.4285922050, .4505937099],
        [.0259040371, .7827717662, -.8086757660]], dtype=np.float32).T)


def rgb_color(lab):
    value = np.asarray(lab, dtype=np.float64) / 100
    l, a, b = np.moveaxis(value, -1, 0)
    roots = np.stack([l + .3963377774 * a + .2158037573 * b,
                      l - .1055613458 * a - .0638541728 * b,
                      l - .0894841775 * a - 1.2914855480 * b], axis=-1)
    linear = roots ** 3 @ np.array([[4.0767416621, -3.3077115913, .2309699292],
        [-1.2684380046, 2.6097574011, -.3413193965],
        [-.0041960863, -.7034186147, 1.7076147010]]).T
    linear = np.clip(linear, 0, 1)
    srgb = np.where(linear <= .0031308, 12.92 * linear, 1.055 * linear ** (1 / 2.4) - .055)
    return np.rint(255 * srgb).clip(0, 255).astype(np.uint8)


def weighted_centers(points, weights, count):
    center = np.average(points, axis=0, weights=weights)
    centers = [points[np.argmin(np.sum((points - center) ** 2, axis=1))]]
    distance = np.full(len(points), np.inf)
    for _ in range(1, min(count, len(points))):
        distance = np.minimum(distance, np.sum((points - centers[-1]) ** 2, axis=1))
        if distance.max() < .0625:
            break
        centers.append(points[np.argmax(weights * distance)])
    centers = np.asarray(centers)
    for _ in range(32):
        assignment = cKDTree(centers).query(points, workers=1)[1]
        updated = centers.copy()
        for index in range(len(centers)):
            chosen = assignment == index
            if chosen.any():
                updated[index] = np.average(points[chosen], axis=0, weights=weights[chosen])
        if np.max(np.abs(updated - centers)) < 1e-4:
            centers = updated
            break
        centers = updated
    return centers[np.argsort(centers[:, 0], kind='stable')]


def learn_palette(rgb, domain, shades=4, hue_width=40, neutral_chroma=3, shade_error=1.5):
    pixels = rgb[domain]
    packed = ((pixels[:, 0].astype(np.uint32) >> 3) << 10) \
        | ((pixels[:, 1].astype(np.uint32) >> 3) << 5) | (pixels[:, 2].astype(np.uint32) >> 3)
    weights = np.bincount(packed, minlength=32768)
    occupied = np.flatnonzero(weights)
    colors = np.column_stack([np.bincount(packed, weights=pixels[:, channel], minlength=32768)[occupied]
                              / weights[occupied] for channel in range(3)])
    lab, mass = perceptual(colors), weights[occupied].astype(np.float64)
    chroma = np.linalg.norm(lab[:, 1:], axis=1)
    hue = np.mod(np.arctan2(lab[:, 2], lab[:, 1]), 2 * np.pi)
    colorful = chroma >= neutral_chroma
    bins = np.floor(hue * 180 / (2 * np.pi)).astype(int) % 180
    histogram = np.bincount(bins[colorful], weights=mass[colorful] * chroma[colorful] ** 2, minlength=180)
    smooth = ndimage.gaussian_filter1d(histogram, max(1, hue_width / 8), mode='wrap')
    repeated = np.tile(smooth, 3)
    peaks, _ = signal.find_peaks(repeated, distance=max(1, int(hue_width / 2)),
                                prominence=max(float(smooth.max()) * .015, 1e-8))
    peaks = peaks[(peaks >= 180) & (peaks < 360)] - 180
    if colorful.any() and not len(peaks):
        peaks = np.array([int(np.argmax(smooth))])
    if len(peaks):
        selected = []
        for index in sorted(peaks, key=lambda index: (-smooth[index], int(index))):
            if all(min(abs(index - other), 180 - abs(index - other)) >= hue_width / 2 for other in selected):
                selected.append(int(index))
        peaks = np.array(sorted(selected))
        angles = (peaks + .5) * (2 * np.pi / 180)
        while True:
            separation = np.min(np.abs(np.angle(np.exp(1j * (hue[:, None] - angles)))), axis=1)
            residual = chroma * np.sin(np.minimum(separation, np.pi / 2))
            eligible = colorful & (np.degrees(separation) >= hue_width) \
                & (residual > max(4 * shade_error, 2 * neutral_chroma))
            if not eligible.any():
                break
            score = np.where(eligible, residual * np.sqrt(mass), -1)
            angles = np.sort(np.append(angles, hue[int(np.argmax(score))]))
        separation = np.abs(np.angle(np.exp(1j * (hue[:, None] - angles))))
        families = np.argmin(separation, axis=1) + 1
    else:
        angles, families = np.empty(0), np.zeros(len(lab), dtype=int)
    families[~colorful] = 0
    palette, metadata = [], []
    for family in range(len(angles) + 1):
        chosen = families == family
        if not chosen.any():
            continue
        points, counts = lab[chosen], mass[chosen]
        centers = weighted_centers(points, counts, 1)
        for count in range(2, shades + 1):
            distance = cKDTree(centers).query(points, workers=1)[0]
            if np.sqrt(np.average(distance ** 2, weights=counts)) <= shade_error:
                break
            centers = weighted_centers(points, counts, count)
        begin = len(palette)
        seen = {tuple(value) for value in palette}
        for color in rgb_color(centers):
            if tuple(color) not in seen:
                palette.append(color)
                seen.add(tuple(color))
        if len(palette) > begin:
            metadata.append({'family': len(metadata), 'hue_degrees': None if family == 0
                             else float(np.degrees(angles[family - 1])),
                             'palette_indices': list(range(begin, len(palette))),
                             'source_pixels': int(counts.sum())})
    palette = np.asarray(palette, dtype=np.uint8)
    return palette, metadata
