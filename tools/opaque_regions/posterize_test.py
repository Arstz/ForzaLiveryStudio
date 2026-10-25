"""Behavior checks for shared shades, structural detail, merging and paint order."""

from pathlib import Path
import tempfile

import numpy as np
from PIL import Image

from palette_model import learn_palette, perceptual
from posterize import build_stack, energy, run
from raster_regions import components, merge_regions, source_locks
from structure_filter import rolling_guidance


def main():
    palette = np.array([[100, 100, 100], [105, 105, 105], [190, 20, 20]], dtype=np.uint8)
    labels = np.zeros((40, 64), dtype=np.int32)
    labels[10:15, 8:13] = 1
    labels[:, 32] = 2
    source = palette[labels]
    lab, colors = perceptual(source), perceptual(palette)
    locks, edge_count = source_locks(source, labels, palette, 24)
    assert edge_count > 0 and locks[:, 32].all()
    before = energy(lab, labels, colors, 6, 64)
    result, report = merge_regions(lab, labels, colors, locks)
    after = energy(lab, result, colors, 6, 64)
    assert result[10:15, 8:13].max() == 0
    assert np.all(result[:, 32] == 2) and np.array_equal(result[locks], labels[locks])
    assert after['total'] < before['total'] and report['accepted_merges'] > 0
    assert abs(before['total'] - after['total'] - report['energy_reduction']) < 1e-3
    assert report['after_components'] == components(result)[1]
    repeated, repeated_report = merge_regions(lab, labels, colors, locks)
    assert np.array_equal(repeated, result) and repeated_report == report

    wrong = np.zeros((16, 30), dtype=np.int32)
    wrong[:, 10:20] = 1
    flat = np.broadcast_to(colors[0], (16, 30, 3)).copy()
    merged, summary = merge_regions(flat, wrong, colors, np.zeros(wrong.shape, dtype=bool))
    assert not merged.any() and summary['before_components'] == 3 and summary['after_components'] == 1

    nested = np.zeros((40, 60), dtype=np.int32)
    nested[4:36, 4:56] = 1
    nested[12:28, 20:40] = 2
    masks, metric = build_stack(nested, [0, 1, 2])
    assert masks[0][1].all() and masks[1][1][12:28, 20:40].all()
    assert metric['components'] == 3 and metric['holes'] == 0
    assert np.all(nested[masks[2][1]] == 2)

    x = np.linspace(.1, 1, 256)
    ramp = np.stack((220 * x, 90 * x, 55 * x), axis=-1).clip(0, 255).astype(np.uint8)[None]
    learned, families = learn_palette(ramp, np.ones(ramp.shape[:2], dtype=bool))
    assert all(len(family['palette_indices']) <= 4 for family in families)
    assigned = [color for family in families for color in family['palette_indices']]
    assert sorted(assigned) == list(range(len(learned)))
    assert learn_palette(ramp, np.ones(ramp.shape[:2], dtype=bool))[1] == families
    accent = np.empty((128, 128, 3), dtype=np.uint8)
    accent[:] = (20, 140, 210)
    accent[60:62, 60:64] = (230, 70, 20)
    accent_palette, accent_families = learn_palette(accent, np.ones(accent.shape[:2], dtype=bool))
    accent_error = np.linalg.norm(perceptual(accent_palette) - perceptual(np.array([230, 70, 20])), axis=1).min()
    assert accent_error < 2 and len(accent_families) >= 2

    texture = np.full((80, 120, 3), 90, dtype=np.uint8)
    texture[:, 60:] = 190
    pattern = ((np.indices(texture.shape[:2]).sum(axis=0) % 2) * 30 - 15)[:, :, None]
    texture = np.clip(texture.astype(np.int16) + pattern, 0, 255).astype(np.uint8)
    domain = np.ones(texture.shape[:2], dtype=bool)
    structure = rolling_guidance(texture, domain)
    assert np.std(structure[10:70, 10:45, 0]) < 3
    assert abs(float(structure[20:60, 15:40].mean()) - 90) < 3
    assert abs(float(structure[20:60, 80:105].mean()) - 190) < 3
    assert np.array_equal(rolling_guidance(texture, domain, scale=0), texture)

    with tempfile.TemporaryDirectory(prefix='fls-posterize-') as directory:
        root = Path(directory)
        image = np.full((48, 80, 4), 255, dtype=np.uint8)
        image[8:40, 8:72, :3] = (170, 35, 25)
        image[8:40, 40, :3] = (10, 10, 10)
        image[:3, :, 3] = 0
        path = root / 'input.png'
        Image.fromarray(image).save(path)
        first = run(path, root / 'first')
        second = run(path, root / 'second')
        assert first['native_dimensions'] == [80, 48] and not first['downsampled']
        assert first['domain_difference_pixels'] == first['source_structure']['changed_locked_labels'] == 0
        assert first['stack_construction']['selected']['pixel_mismatch'] == 0
        assert first['palette'] == second['palette'] and first['stages'] == second['stages']
        assert (root / 'first/posterized.png').read_bytes() == (root / 'second/posterized.png').read_bytes()
        assert (root / 'first/labels.npz').is_file() and (root / 'first/stacked/manifest.json').is_file()
        assert np.array_equal(np.asarray(Image.open(root / 'first/posterized.png'))[:, :, 3], image[:, :, 3])
        assert all(len(family['palette_indices']) <= 4 for family in first['families'])
    print('Posterization tests passed: shared shade cap, faint-region absorption, thin source structure, '
          'neighbor coalescence, exact merge energy, opaque paint order, domain preservation and determinism.')


if __name__ == '__main__':
    main()
