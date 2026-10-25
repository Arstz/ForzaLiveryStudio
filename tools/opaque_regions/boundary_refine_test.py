"""Behavior checks for boundary alignment, topology, domain and deterministic motion."""

from pathlib import Path
import tempfile

import numpy as np
from PIL import Image
from scipy import ndimage

from boundary_refine import AXIAL, BITS, NEIGHBORS, SIMPLE, refine_boundaries
from posterize import run as posterize
from refine import digest, run


def signature(mask):
    count = ndimage.label(mask)[1]
    holes = ndimage.label(~np.pad(mask, 1), np.ones((3, 3)))[1] - 1
    diagonal_count = ndimage.label(mask, np.ones((3, 3)))[1]
    diagonal_holes = ndimage.label(~np.pad(mask, 1))[1] - 1
    return count, holes, diagonal_count, diagonal_holes


def topology(labels, order):
    rank = np.full(int(labels.max()) + 2, -1, dtype=int)
    rank[np.array(order) + 1] = np.arange(len(order))
    return ([signature(labels == color) for color in order],
            [signature(rank[labels + 1] >= level) for level in range(len(order))])


def main():
    for pattern in np.flatnonzero(SIMPLE):
        for surrounding in (False, True):
            mask = np.full((7, 7), surrounding, dtype=bool)
            for bit, (y, x) in enumerate(NEIGHBORS):
                mask[y + 3, x + 3] = bool(pattern & (1 << bit))
            mask[3, 3] = False
            before = signature(mask)
            mask[3, 3] = True
            assert signature(mask) == before, (pattern, surrounding)

    palette = np.array([[20, 20, 20], [235, 235, 235]], dtype=np.uint8)
    labels = np.zeros((32, 80), dtype=np.int32)
    labels[:, 34:] = 1
    expected = np.zeros_like(labels)
    expected[:, 40:] = 1
    source = palette[expected]
    actual, report = refine_boundaries(source, labels, palette, [0, 1])
    assert np.array_equal(actual, expected), np.unique(np.sum(actual != expected, axis=0), return_counts=True)
    assert report['original_rgb_squared_error_after'] == 0 and report['changed_pixels'] == 32 * 6
    assert topology(actual, [0, 1]) == topology(labels, [0, 1])
    again, repeated = refine_boundaries(source, labels, palette, [0, 1])
    assert np.array_equal(actual, again) and report['moves_per_sweep'] == repeated['moves_per_sweep']
    no_op, unchanged = refine_boundaries(source, expected, palette, [0, 1])
    assert np.array_equal(no_op, expected) and unchanged['pixel_moves'] == 0
    disabled, _ = refine_boundaries(source, labels, palette, [0, 1], sweeps=0)
    assert np.array_equal(disabled, labels)

    colors = np.array([[10, 10, 10], [80, 80, 80], [160, 160, 160], [240, 240, 240]], dtype=np.uint8)
    conflict = None
    for encoded in range(4 ** 8):
        patch = (encoded // (4 ** np.arange(8))) % 4
        if not np.any(patch[AXIAL] == 3) or not all(color in patch for color in (1, 2)):
            continue
        old = int(np.sum((patch == 0) * BITS))
        new = int(np.sum((patch == 3) * BITS))
        cumulative = int(np.sum((patch >= 2) * BITS))
        if SIMPLE[old] and SIMPLE[new] and not SIMPLE[cumulative]:
            conflict = patch
            break
    assert conflict is not None
    labels = np.zeros((9, 9), dtype=np.int32)
    for color, (y, x) in zip(conflict, NEIGHBORS):
        labels[y + 4, x + 4] = color
    source = colors[labels]
    source[4, 4] = colors[3]
    actual, _ = refine_boundaries(source, labels, colors, [0, 1, 2, 3], boundary=0)
    assert actual[4, 4] == 0 and topology(actual, [0, 1, 2, 3]) == topology(labels, [0, 1, 2, 3])

    random = np.random.default_rng(1729)
    labels = np.zeros((64, 72), dtype=np.int32)
    labels[8:56, 8:64] = 1
    labels[20:44, 24:48] = 2
    labels[30:34, 8:25] = 2
    labels[:4] = -1
    source = np.clip(colors[np.maximum(labels, 0)].astype(int) + random.integers(-95, 96, (*labels.shape, 3)), 0, 255).astype(np.uint8)
    actual, report = refine_boundaries(source, labels, colors, [0, 1, 2])
    assert np.array_equal(actual >= 0, labels >= 0)
    assert topology(actual, [0, 1, 2]) == topology(labels, [0, 1, 2])
    assert report['original_rgb_squared_error_after'] <= report['original_rgb_squared_error_before']
    with tempfile.TemporaryDirectory(prefix='fls-boundary-') as directory:
        root = Path(directory)
        source = np.full((48, 64, 4), 255, dtype=np.uint8)
        source[:, :32, :3] = 15
        source[:3, :, 3] = 0
        Image.fromarray(source).save(root / 'source.png')
        baseline = posterize(root / 'source.png', root / 'baseline')
        baseline_hash = digest(root / 'baseline/posterized.png')
        first = run(root / 'baseline', root / 'first')
        second = run(root / 'baseline', root / 'second')
        assert first['palette'] == baseline['palette'] == second['palette']
        assert first['families'] == baseline['families']
        assert first['boundary_refinement']['paint_support_topology_preserved']
        assert first['boundary_refinement']['source_after']['rmse'] <= first['boundary_refinement']['source_before']['rmse']
        assert (root / 'first/posterized.png').read_bytes() == (root / 'second/posterized.png').read_bytes()
        assert np.array_equal(np.asarray(Image.open(root / 'first/posterized.png'))[:, :, 3], source[:, :, 3])
        assert digest(root / 'baseline/posterized.png') == baseline_hash
        assert first['refinement_input']['sha256']['posterized.png'] == baseline_hash
        try:
            run(root / 'baseline', root / 'baseline/nested')
        except ValueError as failure:
            assert 'separate' in str(failure)
        else:
            raise AssertionError('A nested output directory must be rejected')
    print('Boundary refinement tests passed: original-edge alignment, exhaustive simple-point safety, '
          'paint-mask topology, thin connections, holes, domain preservation, deterministic no-op and project export.')


if __name__ == '__main__':
    main()
