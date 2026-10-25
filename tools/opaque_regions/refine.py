"""Refine an existing shared-palette region map against its original source."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import sys
import time

import numpy as np
from PIL import Image

from boundary_refine import (DEFAULT_BAND, DEFAULT_BOUNDARY_WEIGHT, DEFAULT_EDGE_SCALE,
                             DEFAULT_SWEEPS, refine_boundaries, source_edge_agreement)
from posterize import build_stack, error, mask_geometry, save_rgb, topology, write_manifest


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(project, output, sweeps=DEFAULT_SWEEPS, band=DEFAULT_BAND, boundary=DEFAULT_BOUNDARY_WEIGHT,
        edge_scale=DEFAULT_EDGE_SCALE, progress=None):
    started = time.perf_counter()
    progress = progress or (lambda entry: None)
    project, output = project.resolve(), output.resolve()
    if output == project or project in output.parents or output in project.parents:
        raise ValueError('Use a separate, non-nested output directory to preserve the input project')
    report = json.loads((project / 'report.json').read_text())
    rgba = np.asarray(Image.open(project / 'source.png').convert('RGBA'))
    rgb, domain = rgba[:, :, :3], rgba[:, :, 3] == 255
    if np.any((rgba[:, :, 3] != 0) & ~domain):
        raise ValueError('Boundary refinement requires an opaque or binary-transparent source')
    with np.load(project / 'labels.npz') as data:
        before, palette = data['labels'], data['palette']
        locked = data['locked'] if 'locked' in data else np.zeros(before.shape, dtype=bool)
    if before.shape != domain.shape or not np.array_equal(before >= 0, domain) \
            or np.any(before[domain] >= len(palette)):
        raise ValueError('Source dimensions, domain and palette labels must agree')
    baseline = np.asarray(Image.open(project / 'posterized.png').convert('RGBA'))
    if not np.array_equal(baseline[:, :, 3], rgba[:, :, 3]) \
            or not np.array_equal(baseline[domain, :3], palette[before[domain]]):
        raise ValueError('The input label map does not reproduce its saved posterization')
    order = report['stack_construction']['selected']['palette_order']
    before_masks, before_geometry = build_stack(before, order)
    before_topology = topology(before)
    labels, refinement = refine_boundaries(rgb, before, palette, order, sweeps, band, boundary, edge_scale, progress)
    after_masks, after_geometry = build_stack(labels, order)
    after_topology = topology(labels)
    if before_topology['components'] != after_topology['components'] or before_topology['holes'] != after_topology['holes']:
        raise RuntimeError('Refinement changed visible-label topology')
    for old, new in zip(before_masks, after_masks):
        if any(old[2][key] != new[2][key] for key in ('components', 'holes')):
            raise RuntimeError('Refinement changed the connected paint supports or their holes')
    before_edges = source_edge_agreement(rgb, palette[np.maximum(before, 0)], domain)
    after_edges = source_edge_agreement(rgb, palette[np.maximum(labels, 0)], domain)
    accepted = after_edges['mean_aligned_gradient_ratio'] + 1e-7 >= before_edges['mean_aligned_gradient_ratio']
    if not accepted:
        labels, after_masks, after_geometry, after_topology, after_edges = before.copy(), before_masks, before_geometry, before_topology, before_edges
    output.mkdir(parents=True, exist_ok=True)
    for filename in ('source.png', 'structure.png', 'palette_only.png', 'spatial.png'):
        shutil.copyfile(project / filename, output / filename)
    shutil.copyfile(project / 'posterized.png', output / 'unrefined.png')
    save_rgb(output / 'posterized.png', palette[np.maximum(labels, 0)], domain)
    save_rgb(output / 'difference.png', np.abs(rgb.astype(np.int16) - palette[np.maximum(labels, 0)]).astype(np.uint8), domain)
    save_rgb(output / 'refinement_difference.png',
             np.abs(palette[np.maximum(before, 0)].astype(np.int16) - palette[np.maximum(labels, 0)]).astype(np.uint8), domain)
    np.savez_compressed(output / 'labels.npz', labels=labels, palette=palette, locked=locked, unrefined_labels=before)
    write_manifest(output / 'stacked', after_masks, palette, (labels.shape[1], labels.shape[0]), output / 'posterized.png')
    flat = [(int(color), labels == color, mask_geometry(labels == color)) for color in order]
    write_manifest(output / 'flat', flat, palette, (labels.shape[1], labels.shape[0]), output / 'posterized.png')
    result = copy.deepcopy(report)
    result['method'] += '; original-source boundary refinement'
    result['refinement_input'] = {'project': str(project),
        'sha256': {name: digest(project / name) for name in ('report.json', 'source.png', 'posterized.png', 'labels.npz', 'stacked/manifest.json')}}
    result['boundary_implementation_sha256'] = {name: digest(Path(__file__).with_name(name))
        for name in ('refine.py', 'boundary_refine.py')}
    result['stages']['unrefined'] = report['stages']['posterized']
    result['stages']['posterized'] = {'topology': after_topology,
                                     'source_error': error(rgb, palette[np.maximum(labels, 0)], domain)}
    result['source_structure']['changed_locked_labels'] = int(np.count_nonzero(labels[locked] != before[locked]))
    result['source_structure']['reference'] = 'Inherited guidance locks apply to pre-refinement labeling; source-guided boundary motion has separate error and topology checks'
    result['stack_construction'] = {'selected': {'name': 'refined_' + report['stack_construction']['selected']['name'],
        'palette_order': order, **after_geometry, 'pixel_mismatch': 0},
        'score': 'Fixed input paint order; boundary refinement improves source agreement while retaining paint-mask topology',
        'trials': [], 'appearance_groups': len(order), 'compound_svg_objects_are_not_bucket_paths': True}
    result['boundary_refinement'] = {**refinement, 'accepted': accepted,
        'rejection': '' if accepted else 'Source-aligned gradient response decreased; the input map was retained',
        'source_before': error(rgb, palette[np.maximum(before, 0)], domain),
        'source_after': result['stages']['posterized']['source_error'],
        'edge_agreement_before': before_edges, 'edge_agreement_after': after_edges,
        'visible_topology_preserved': True, 'paint_support_topology_preserved': True,
        'paint_geometry_before': before_geometry, 'paint_geometry_after': after_geometry}
    result['input_pipeline_runtime_seconds'] = report['runtime_seconds']
    result['runtime_seconds'] = time.perf_counter() - started
    (output / 'report.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--sweeps', type=int, default=DEFAULT_SWEEPS)
    parser.add_argument('--band', type=float, default=DEFAULT_BAND)
    parser.add_argument('--boundary-weight', type=float, default=DEFAULT_BOUNDARY_WEIGHT)
    parser.add_argument('--edge-scale', type=float, default=DEFAULT_EDGE_SCALE)
    args = parser.parse_args()
    try:
        report = run(args.project, args.output, args.sweeps, args.band, args.boundary_weight, args.edge_scale,
                     lambda entry: print(json.dumps(entry), file=sys.stderr, flush=True))
        print(json.dumps(report, indent=2))
    except (OSError, ValueError, KeyError, RuntimeError) as failure:
        print(str(failure), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
