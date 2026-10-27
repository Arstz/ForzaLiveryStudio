"""End-to-end checks for selection using actual filled project counts."""

import gzip
import json
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image

from generate import compact_score, native_polygon_target, run
from simplify import render


def main():
    assert compact_score({'accepted': False, 'verified': True, 'shape_count': 0}) is None
    assert compact_score({'accepted': True, 'verified': False, 'shape_count': 0}) is None
    assert compact_score({'accepted': True, 'verified': True, 'shape_count': None}) is None
    region = native_polygon_target([[(0, 0), (20, 0), (20, 20), (0, 20)],
        [(5, 5), (5, 15), (15, 15), (15, 5)]])
    assert region.area == 300 and len(region.interiors) == 1
    tool = Path(__file__).resolve().parents[2] / 'build/Release/fls_opaque_regions.exe'
    with tempfile.TemporaryDirectory(prefix='fls-generate-') as temporary:
        root = Path(temporary)
        project = root / 'source'
        project.mkdir()
        baseline = project / 'baseline.svg'
        data = ('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
            '<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
            '<path fill="#0000ff" d="M20 30L30 30L30 40L20 40Z"/>'
            '<path fill="#0000ff" d="M70 30L80 30L80 40L70 40Z"/>'
            '<path fill="#ff0000" fill-rule="evenodd" d="M10 10L90 10L90 70L10 70Z'
            'M20 30L30 30L30 40L20 40ZM70 30L80 30L80 40L70 40Z"/>'
            '<path fill="#00ff00" d="M30 30L70 30L70 40L30 40Z"/></svg>').encode()
        baseline.write_bytes(data)
        Image.fromarray(render(data, (100, 80))).save(project / 'source.png')
        np.savez_compressed(project / 'labels.npz', labels=np.zeros((80, 100), np.int32),
            palette=np.array([[255, 255, 255], [0, 0, 255], [255, 0, 0], [0, 255, 0]], np.uint8))
        output = root / 'output'
        report = run(project, baseline, output, tool, start=3, stop=4, search_states=1, shortlist=2,
            evaluations=6000, render_rmse=0, local_rmse=0, source_growth=0)
        assert report['accepted'] and (report['improved'] or report['recovered_valid_output']), report
        assert report['after_shape_count'] == report['after_region_count'] == 4
        assert report['before_region_count'] == 5
        if report['before_shape_count'] is not None:
            assert report['before_shape_count'] > 4
        else:
            assert not (output / 'before.3so').exists()
        assert not list(output.glob('*.svg'))
        saved = json.loads(gzip.decompress((output / 'after.3so').read_bytes()))
        assert len(saved['root']['children']) == 4
        shapes = [shape for group in saved['root']['children'] for shape in group['children'] if shape['kind'] == 'shape']
        assert len(shapes) == 4 and all(shape['color'][3] == 255 for shape in shapes)
        assert all(trial['fit']['renderer'] == 'NativeShapeRenderer' for trial in report['trials']
                   if trial['fit']['accepted'])
        assert np.array_equal(np.asarray(Image.open(output / 'after.png')),
                              np.asarray(Image.open(project / 'source.png')))
        baseline.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
            b'<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/></svg>')
        Image.fromarray(render(baseline.read_bytes(), (100, 80))).save(project / 'source.png')
        noop = run(project, baseline, root / 'noop', tool, search_states=1, shortlist=1, evaluations=6000)
        assert noop['accepted'] and not noop['improved'] and noop['selected'] == 'baseline'
        assert noop['before_shape_count'] == noop['after_shape_count'] == 1
        assert (root / 'noop/before.3so').read_bytes() == (root / 'noop/after.3so').read_bytes()
    print('Generation tests passed: measured shape selection, region grouping, opaque saved projects, '
          'native rendered quality, rejected invalid counts and baseline retention.')


if __name__ == '__main__':
    main()
