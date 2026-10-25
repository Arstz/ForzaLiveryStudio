"""Behavior checks for source colors, error budgets and unchanged SVG geometry."""

import json
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image

from palette_refit import fit_palette
from posterize import build_stack, save_rgb, write_manifest
from recolor import digest, geometry_signature, recolor_svg, run
import xml.etree.ElementTree as ET


def main():
    labels = np.zeros((24, 32), dtype=np.int32)
    labels[:, 16:] = 1
    labels[:3] = -1
    domain = labels >= 0
    palette = np.array([[180, 100, 50], [240, 240, 230]], dtype=np.uint8)
    target = np.array([[175, 95, 95], [255, 255, 255]], dtype=np.uint8)
    source = target[np.maximum(labels, 0)]
    source[~domain] = (0, 255, 0)
    families = [{'palette_indices': [0, 1]}]
    for method in ('source', 'mean'):
        fitted, report = fit_palette(source, labels, palette, families, method)
        assert np.array_equal(fitted, target), (method, fitted)
        assert report['accepted'] and report['source_rgb_rmse_after'] == 0
        assert report['source_mean_oklab_distance_after'] < 1e-4
        assert all(color['observed_in_its_source_region'] for color in report['per_color'])
    alternative = source.copy()
    alternative[~domain] = (255, 0, 255)
    assert np.array_equal(fit_palette(alternative, labels, palette, families)[0], target)
    repeated = fit_palette(source, labels, palette, families)
    assert np.array_equal(repeated[0], target)
    assert repeated[1] == fit_palette(source, labels, palette, families)[1]

    random = np.random.default_rng(49)
    mixture = np.full((100, 100, 3), 254, dtype=np.uint8)
    mixture[:12] = random.integers(160, 190, (12, 100, 3), dtype=np.uint8)
    one = np.zeros(mixture.shape[:2], dtype=np.int32)
    old = np.array([[245, 245, 245]], dtype=np.uint8)
    fitted, budget = fit_palette(mixture, one, old, [{'palette_indices': [0]}])
    assert budget['source_rgb_rmse_after'] <= budget['source_rgb_rmse_before'] + 1e-8
    assert budget['source_mean_oklab_distance_after'] <= budget['source_mean_oklab_distance_before'] + 1e-8
    assert budget['rgb_error_price'] > 0
    if not np.array_equal(fitted, old):
        assert np.any(np.all(mixture.reshape(-1, 3) == fitted[0], axis=1))
    relaxed, _ = fit_palette(mixture, one, old, [{'palette_indices': [0]}], source_rmse_growth=3)
    assert np.any(np.all(mixture.reshape(-1, 3) == relaxed[0], axis=1))

    identical = np.full(source.shape, 210, dtype=np.uint8)
    coincident, merged = fit_palette(identical, labels, palette, families)
    assert len(np.unique(coincident, axis=0)) == 1 and merged['color_count'] == 1
    assert merged['shared_appearance_colors'][0]['palette_indices'] == [0, 1]
    inverted_target = target[::-1].copy()
    inverted, _ = fit_palette(inverted_target[np.maximum(labels, 0)], labels, palette, families)
    assert np.array_equal(inverted, inverted_target)

    svg = b'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="24"><path fill="#b46432" d="M0 3H16V24H0Z"/><path fill="#f0f0e6" fill-rule="nonzero" d="M16 3H32V24H16Z"/></svg>'
    recolored, audit = recolor_svg(svg, palette, target)
    assert audit['path_count'] == 2 and audit['geometry_unchanged']
    assert geometry_signature(ET.fromstring(svg)) == geometry_signature(ET.fromstring(recolored))
    assert recolor_svg(svg, palette, palette)[0] == svg
    same, _ = recolor_svg(svg, palette, coincident)
    assert len(list(ET.fromstring(same))) == 2

    with tempfile.TemporaryDirectory(prefix='fls-palette-') as directory:
        root = Path(directory)
        project = root / 'project'
        project.mkdir()
        save_rgb(project / 'source.png', source, domain)
        save_rgb(project / 'posterized.png', palette[np.maximum(labels, 0)], domain)
        np.savez_compressed(project / 'labels.npz', labels=labels, palette=palette)
        masks, metric = build_stack(labels, [1, 0])
        write_manifest(project / 'stacked', masks, palette, (32, 24), project / 'posterized.png')
        (project / 'report.json').write_text(json.dumps({'method': 'fixture', 'palette': palette.tolist(),
            'families': families, 'stages': {'posterized': {'topology': metric}},
            'stack_construction': {'appearance_groups': 2}, 'runtime_seconds': 0.}))
        (project / 'input.svg').write_bytes(svg)
        before = digest(project / 'posterized.png')
        first = run(project, root / 'first', project / 'input.svg')
        second = run(project, root / 'second', project / 'input.svg')
        assert first['palette_refit']['geometry_unchanged'] and first['palette_refit']['labels_unchanged']
        assert first['palette'] == second['palette']
        assert (root / 'first/candidate.svg').read_bytes() == (root / 'second/candidate.svg').read_bytes()
        assert (root / 'first/posterized.png').read_bytes() == (root / 'second/posterized.png').read_bytes()
        assert digest(project / 'posterized.png') == before
        for mask in (project / 'stacked').glob('mask_*.png'):
            assert digest(mask) == digest(root / 'first/stacked' / mask.name)
        assert np.array_equal(np.load(root / 'first/labels.npz')['labels'], labels)
        assert np.array_equal(np.asarray(Image.open(root / 'first/posterized.png'))[:, :, 3], domain * 255)
        save_rgb(project / 'source.png', identical, domain)
        merged_project = run(project, root / 'merged', project / 'input.svg')
        assert merged_project['stack_construction']['appearance_groups'] == 1
        assert merged_project['stages']['posterized']['topology']['label_groups'] == 2
        assert merged_project['palette_refit']['path_count'] == 2
        try:
            run(project, project / 'nested', project / 'input.svg')
        except ValueError:
            pass
        else:
            raise AssertionError('Nested outputs must be rejected')
    print('Palette fitting tests passed: source hue recovery, mean control, original-error budget, '
          'shared colors with separate draw items, fixed masks and SVG geometry, alpha and determinism.')


if __name__ == '__main__':
    main()
