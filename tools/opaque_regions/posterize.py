"""Experimental shared-shade labeling and opaque raster paint stacks."""

import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

import cv2
import numpy as np
from PIL import Image
from scipy import ndimage

from palette_model import learn_palette, perceptual
from raster_regions import components, label_sweep, merge_regions, nearest_labels, source_locks
from structure_filter import DEFAULT_GUIDANCE_SCALE, rolling_guidance


def error(source, actual, domain):
    delta = source[domain].astype(np.float64) - actual[domain]
    return {'rmse': float(np.sqrt(np.mean(delta ** 2))),
            'max_channel_error': int(np.max(np.abs(delta), initial=0))}


def topology(labels):
    regions, count = components(labels)
    areas = np.bincount(regions[regions >= 0], minlength=count)
    holes = 0
    for color in np.unique(labels[labels >= 0]):
        complement, _ = ndimage.label(labels != color, structure=np.ones((3, 3)))
        outside = np.unique(np.concatenate((complement[0], complement[-1], complement[:, 0], complement[:, -1])))
        holes += len(np.setdiff1d(np.unique(complement), np.append(outside, 0)))
    return {'components': count, 'holes': holes, 'colors': int(len(np.unique(labels[labels >= 0]))),
            'small_region_histogram': [{'below_area_pixels': limit, 'components': int(np.sum(areas < limit)),
                                      'area_pixels': int(areas[areas < limit].sum())} for limit in (1, 4, 16, 64)]}


def energy(lab, labels, palette, boundary, region_cost):
    valid = labels >= 0
    data = float(np.sum((lab[valid].astype(np.float64) - palette[labels[valid]]) ** 2))
    length = 0
    for a, b in ((labels[:, :-1], labels[:, 1:]), (labels[:-1], labels[1:])):
        length += int(np.sum((a >= 0) & (b >= 0) & (a != b)))
    count = components(labels)[1]
    return {'source_squared_perceptual_error': data, 'boundary_pixels': length, 'components': count,
            'total': data + boundary * length + region_cost * count}


def save_rgb(path, rgb, domain):
    Image.fromarray(np.dstack((rgb, domain.astype(np.uint8) * 255))).save(path)


def mask_geometry(mask):
    contours, hierarchy = cv2.findContours(mask.astype(np.uint8), cv2.RETR_CCOMP, cv2.CHAIN_APPROX_SIMPLE)
    holes = int(np.sum(hierarchy[0, :, 3] >= 0)) if hierarchy is not None else 0
    count = int(cv2.connectedComponents(mask.astype(np.uint8), connectivity=4)[0] - 1)
    segments = sum(len(contour) for contour in contours)
    return {'components': count, 'holes': holes, 'raster_corners': segments,
            'cost_proxy': 64 * count + segments + 96 * holes}


def build_stack(labels, order):
    rank = np.full(int(labels.max()) + 1, -1, dtype=np.int32)
    rank[order] = np.arange(len(order))
    levels = np.full(labels.shape, -1, dtype=np.int32)
    domain = labels >= 0
    levels[domain] = rank[labels[domain]]
    masks, metrics = [], {'components': 0, 'holes': 0, 'raster_corners': 0, 'cost_proxy': 0}
    actual = np.full(labels.shape, -1, dtype=np.int32)
    for position, color in enumerate(order):
        legal = levels >= position
        _, connected = cv2.connectedComponents(legal.astype(np.uint8), connectivity=4)
        needed = np.unique(connected[labels == color])
        keep = np.zeros(int(connected.max()) + 1, dtype=bool)
        keep[needed] = True
        keep[0] = False
        mask = keep[connected]
        assert np.all(mask[labels == color]) and not np.any(mask & ~legal)
        geometry = mask_geometry(mask)
        for key in metrics:
            metrics[key] += geometry[key]
        masks.append((int(color), mask, geometry))
        actual[mask] = color
    assert np.array_equal(actual, labels)
    return masks, metrics


def write_manifest(folder, masks, palette, size, reference):
    folder.mkdir(parents=True, exist_ok=True)
    entries = []
    for index, (color, mask, geometry) in enumerate(masks):
        filename = f'mask_{index:02d}.png'
        Image.fromarray(mask.astype(np.uint8) * 255).save(folder / filename)
        entries.append({'mask': filename, 'fill': '#' + ''.join(f'{int(v):02x}' for v in palette[color]),
                        'palette_index': color, 'raster_geometry': geometry})
    manifest = {'version': 1, 'width': size[0], 'height': size[1],
                'reference': str(reference.resolve()), 'items': entries,
                'fill_opacity': 1, 'pixel_coverage_verified': True}
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2))


def make_stacks(labels, palette, output, progress):
    used, areas = np.unique(labels[labels >= 0], return_counts=True)
    border = np.concatenate((labels[0], labels[-1], labels[:, 0], labels[:, -1]))
    border = border[border >= 0]
    background = int(np.argmax(np.bincount(border, minlength=len(palette)))) if len(border) else int(used[np.argmax(areas)])
    lab = perceptual(palette)
    quantities = dict(zip(used, areas))
    orders = [('dark_first', sorted(used, key=lambda c: (lab[c, 0], int(c)))),
              ('light_first', sorted(used, key=lambda c: (-lab[c, 0], int(c)))),
              ('large_first', sorted(used, key=lambda c: (-quantities[c], int(c)))),
              ('small_first', sorted(used, key=lambda c: (quantities[c], int(c))))]
    trials, seen = [], set()
    best = None
    for name, order in orders:
        order = [background] + [int(color) for color in order if color != background]
        if tuple(order) in seen:
            continue
        seen.add(tuple(order))
        masks, metric = build_stack(labels, order)
        entry = {'name': name, 'palette_order': order, **metric, 'pixel_mismatch': 0}
        trials.append(entry)
        progress({'stack_trial': entry})
        if best is None or (metric['cost_proxy'], metric['components'], name) < best[0]:
            best = ((metric['cost_proxy'], metric['components'], name), masks, entry)
    write_manifest(output / 'stacked', best[1], palette, (labels.shape[1], labels.shape[0]), output / 'posterized.png')
    flat = [(int(color), labels == color, mask_geometry(labels == color)) for color in used]
    write_manifest(output / 'flat', flat, palette, (labels.shape[1], labels.shape[0]), output / 'posterized.png')
    return {'selected': best[2], 'trials': trials, 'score': 'Raster contour proxy; native encoding remains unmeasured',
            'appearance_groups': len(used), 'compound_svg_objects_are_not_bucket_paths': True}


def run(source, output, shades=4, hue_width=40., neutral_chroma=3., shade_error=1.5,
        boundary=6., region_cost=64., edge_contrast=24., sweeps=2, stacks=True, progress=None,
        guidance_scale=DEFAULT_GUIDANCE_SCALE, guidance_range=.075, guidance_iterations=3):
    started = time.perf_counter()
    progress = progress or (lambda entry: None)
    rgba = np.asarray(Image.open(source).convert('RGBA'))
    if np.any((rgba[:, :, 3] != 0) & (rgba[:, :, 3] != 255)):
        raise ValueError('The experimental region stage requires opaque pixels or a binary transparent domain')
    rgb, domain = rgba[:, :, :3], rgba[:, :, 3] == 255
    if not domain.any():
        raise ValueError('The input contains no opaque domain')
    guidance = rolling_guidance(rgb, domain, guidance_scale, guidance_range, guidance_iterations)
    palette, families = learn_palette(guidance, domain, shades, hue_width, neutral_chroma, shade_error)
    palette_lab, lab = perceptual(palette), perceptual(guidance)
    labels = nearest_labels(lab, palette_lab, domain)
    initial = labels.copy()
    locks, protected_edges = source_locks(guidance, labels, palette, edge_contrast)
    progress({'palette_colors': len(palette), 'families': len(families), 'protected_source_edges': protected_edges})
    changes = label_sweep(lab, labels, palette_lab, locks, boundary, sweeps)
    spatial = labels.copy()
    before_energy = energy(lab, labels, palette_lab, boundary, region_cost)
    labels, merges = merge_regions(lab, labels, palette_lab, locks, boundary, region_cost, progress)
    after_energy = energy(lab, labels, palette_lab, boundary, region_cost)
    if after_energy['total'] > before_energy['total'] + max(1e-3, abs(before_energy['total']) * 1e-8):
        raise RuntimeError('Region transactions increased the complete energy')
    if not np.array_equal(labels[locks], initial[locks]) or not np.array_equal(labels >= 0, domain):
        raise RuntimeError('Source structure or domain changed outside the labeling contract')
    output.mkdir(parents=True, exist_ok=True)
    save_rgb(output / 'source.png', rgb, domain)
    save_rgb(output / 'structure.png', guidance, domain)
    reports = {}
    for name, current in [('palette_only', initial), ('spatial', spatial), ('posterized', labels)]:
        actual = palette[np.maximum(current, 0)]
        save_rgb(output / (name + '.png'), actual, domain)
        reports[name] = {'topology': topology(current), 'source_error': error(rgb, actual, domain)}
        progress({name: reports[name]})
    np.savez_compressed(output / 'labels.npz', labels=labels, palette=palette, locked=locks)
    save_rgb(output / 'difference.png', np.abs(rgb.astype(np.int16) - palette[np.maximum(labels, 0)]).astype(np.uint8), domain)
    result = {'method': 'Experimental shared tonal palette, discrete spatial labeling and energy-driven region merging',
              'paper_relationship': 'Inspired by rolling guidance, PosterChild and region-merging energies; not an author-code reproduction',
              'input': str(source.resolve()), 'input_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'implementation_sha256': {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                                        for name in ('posterize.py', 'palette_model.py', 'raster_regions.py', 'structure_filter.py')},
              'native_dimensions': [rgb.shape[1], rgb.shape[0]], 'downsampled': False,
              'palette': [{'index': index, 'rgb': list(map(int, color))} for index, color in enumerate(palette)],
              'families': families, 'maximum_shades_per_family': shades,
              'options': {'hue_width_degrees': hue_width, 'neutral_chroma': neutral_chroma,
                          'shade_error': shade_error, 'boundary_weight': boundary, 'component_weight': region_cost,
                          'edge_contrast': edge_contrast, 'label_sweeps': sweeps,
                          'guidance_scale_native_pixels': guidance_scale, 'guidance_range': guidance_range,
                          'guidance_iterations': guidance_iterations},
              'source_to_structure': error(rgb, guidance, domain),
              'stages': reports, 'source_structure': {'protected_edges': protected_edges,
              'locked_pixels': int(locks.sum()), 'changed_locked_labels': 0,
              'reference': 'Native-resolution structural guidance, not every original texture edge'},
              'domain_difference_pixels': 0, 'spatial_label_changes': int(changes),
              'region_merging': merges, 'energy_before_merging': before_energy, 'energy_after_merging': after_energy,
              'native_bucket_paths_measured': False, 'compact_fit_placements_measured': False}
    if stacks:
        result['stack_construction'] = make_stacks(labels, palette, output, progress)
    result['runtime_seconds'] = time.perf_counter() - started
    (output / 'report.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--shades-per-family', type=int, default=4)
    parser.add_argument('--hue-width', type=float, default=40.)
    parser.add_argument('--neutral-chroma', type=float, default=3.)
    parser.add_argument('--shade-error', type=float, default=1.5)
    parser.add_argument('--boundary-weight', type=float, default=6.)
    parser.add_argument('--component-weight', type=float, default=64.)
    parser.add_argument('--edge-contrast', type=float, default=24.)
    parser.add_argument('--label-sweeps', type=int, default=2)
    parser.add_argument('--guidance-scale', type=float, default=DEFAULT_GUIDANCE_SCALE)
    parser.add_argument('--guidance-range', type=float, default=.075)
    parser.add_argument('--guidance-iterations', type=int, default=3)
    parser.add_argument('--no-stacks', action='store_true')
    args = parser.parse_args()
    if not 1 <= args.shades_per_family <= 4 or not 0 < args.hue_width <= 180 \
            or args.label_sweeps < 0 or args.neutral_chroma < 0 or args.shade_error < 0 \
            or args.boundary_weight < 0 or args.component_weight < 0 or args.edge_contrast < 0 \
            or args.guidance_scale < 0 or args.guidance_range <= 0 or args.guidance_iterations < 0 \
            or not all(np.isfinite(value) for value in (args.hue_width, args.neutral_chroma, args.shade_error,
                        args.boundary_weight, args.component_weight, args.edge_contrast, args.guidance_scale, args.guidance_range)):
        parser.error('Invalid palette, spatial, or structure options')
    if args.source.resolve().parent == args.output.resolve():
        parser.error('Use a separate output directory to preserve the source')
    def progress(entry):
        print(json.dumps(entry), file=sys.stderr, flush=True)
    try:
        result = run(args.source, args.output, args.shades_per_family, args.hue_width, args.neutral_chroma,
                     args.shade_error, args.boundary_weight, args.component_weight, args.edge_contrast,
                     args.label_sweeps, not args.no_stacks, progress, args.guidance_scale, args.guidance_range,
                     args.guidance_iterations)
        print(json.dumps(result, indent=2))
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
