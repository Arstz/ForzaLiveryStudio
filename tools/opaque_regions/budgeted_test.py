"""Verify actual filled projects and total-image limits after topology search."""

import gzip
import json
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image

from budgeted import run
from simplify import render


def main():
    tool = Path(__file__).resolve().parents[2] / 'build/Release/fls_opaque_regions.exe'
    with tempfile.TemporaryDirectory(prefix='fls-budgeted-') as temporary:
        root = Path(temporary)
        project = root / 'source'
        project.mkdir()
        svg = project / 'input.svg'
        svg.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
            b'<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
            b'<path fill="#0000ff" d="M20 30L30 30L30 40L20 40Z"/>'
            b'<path fill="#0000ff" d="M70 30L80 30L80 40L70 40Z"/>'
            b'<path fill="#ff0000" fill-rule="evenodd" d="M10 10L90 10L90 70L10 70Z'
            b'M20 30L30 30L30 40L20 40ZM70 30L80 30L80 40L70 40Z"/>'
            b'<path fill="#00ff00" d="M30 30L70 30L70 40L30 40Z"/></svg>')
        original = render(svg.read_bytes(), (100, 80))
        Image.fromarray(original).save(project / 'source.png')
        report = run(project, svg, root / 'result', tool, total_shape_budget=5,
                     evaluations=1000, source_growth=0., local_growth=0., render_rmse=0., local_rmse=0.)
        assert report['accepted'], report
        assert report['after_shape_count'] == report['after_region_count'] == 4
        assert report['trials'][0]['geometry']['holes'] == 0
        assert report['total_shape_budget'] == 5 and not report['downsampled']
        assert not list((root / 'result').glob('*.svg'))
        data = json.loads(gzip.decompress((root / 'result/after.3so').read_bytes()))
        shapes = [child for group in data['root']['children'] for child in group['children']
                  if child['kind'] == 'shape']
        assert len(shapes) == 4 and all(shape['opacity'] == 1 and shape['color'][3] == 255 for shape in shapes)
        background = [child for child in data['root']['children'][0]['children'] if child['kind'] == 'contour'][0]
        assert len(background['points']) == 4 and all(point['hard'] for point in background['points'])
        assert np.array_equal(np.asarray(Image.open(root / 'result/after.png')), original)
        svg.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
                       b'<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/></svg>')
        Image.fromarray(render(svg.read_bytes(), (100, 80))).save(project / 'source.png')
        unchanged = run(project, svg, root / 'unchanged', tool, total_shape_budget=1, evaluations=1000)
        assert unchanged['accepted'] and not unchanged['improved']
        assert unchanged['after_shape_count'] == unchanged['after_region_count'] == 1
    print('Budgeted generation tests passed: measured native counts, joint topology/order reduction, '
          'opaque filled projects and unchanged native-resolution appearance.')


if __name__ == '__main__':
    main()
