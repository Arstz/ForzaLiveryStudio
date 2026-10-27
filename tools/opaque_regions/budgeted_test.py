"""Verify actual filled projects and total-image limits after topology search."""

import gzip
import json
from pathlib import Path
import tempfile
from unittest.mock import patch
from copy import deepcopy
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image
from shapely.geometry import box

from budgeted import encoded_project_cost, native_curve_hybrid, run
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
        document = ET.fromstring(svg.read_bytes())
        replacement = deepcopy(document)
        list(replacement)[1].set('d', 'M20 30C23 29 27 29 30 30L30 40L20 40Z')
        list(replacement)[2].set('d', 'M70 30C73 29 77 29 80 30L80 40L70 40Z')
        before = {'per_path': [{'scene_eligible': True, 'placements': 3} for _ in document]}
        after = {'per_path': [{'scene_eligible': True, 'placements': 2} for _ in document]}
        after['per_path'][1] = {'scene_eligible': False, 'placements': 0}
        hybrid, selected = native_curve_hybrid(ET.tostring(document), ET.tostring(replacement), before, after)
        assert selected['curved_paths'] == [2] and selected['retained_original_paths'] == [1]
        assert list(ET.fromstring(hybrid))[1].attrib['d'] == list(document)[1].attrib['d']
        assert list(ET.fromstring(hybrid))[2].attrib['d'] == list(replacement)[2].attrib['d']
        angles = np.linspace(0, 2 * np.pi, 32, endpoint=False)
        ring = 'M' + 'L'.join(f'{40 + 10 * np.cos(a):.6f} {40 + 10 * np.sin(a):.6f}' for a in angles) + 'Z'
        circular = b'<svg xmlns="http://www.w3.org/2000/svg"><path fill="#0000ff" d="' + ring.encode() + b'"/></svg>'
        smooth = b'<svg xmlns="http://www.w3.org/2000/svg"><path fill="#0000ff" d="' \
            b'M50 40C50 45.523 45.523 50 40 50C34.477 50 30 45.523 30 40' \
            b'C30 34.477 34.477 30 40 30C45.523 30 50 34.477 50 40Z"/></svg>'
        coarse = {'per_path': [{'scene_eligible': True, 'placements': 2}]}
        refined = {'per_path': [{'scene_eligible': True, 'placements': 3}]}
        assert native_curve_hybrid(circular, smooth, coarse, refined)[1]['curved_paths'] == [0]
        assert native_curve_hybrid(circular, smooth, coarse, refined, shape_weight=64.) is None
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
        encoding = encoded_project_cost(root / 'result/after.3so')
        assert encoding == {'anchors': 16, 'soft_anchors': 0, 'holes': 0, 'draw_items': 4, 'cost': 144}
        assert report['cost_weights']['native_shapes'] == 4.
        assert np.array_equal(np.asarray(Image.open(root / 'result/after.png')), original)
        svg.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
                       b'<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/></svg>')
        Image.fromarray(render(svg.read_bytes(), (100, 80))).save(project / 'source.png')
        unchanged = run(project, svg, root / 'unchanged', tool, total_shape_budget=1, evaluations=1000)
        assert unchanged['accepted'] and not unchanged['improved']
        assert unchanged['after_shape_count'] == unchanged['after_region_count'] == 1
        resumed = run(project, svg, root / 'resumed', tool, total_shape_budget=1, evaluations=1000,
            resume_stack=root / 'unchanged/stack_plan.json.gz', max_edits=0, outer_passes=0)
        saved = json.loads(gzip.decompress((root / 'unchanged/stack_plan.json.gz').read_bytes()))
        assert resumed['accepted'] and resumed['after_shape_count'] == 1
        assert (root / 'resumed/internal/contours_0.svg').read_text() == saved['encoded_svg']
        assert resumed['trials'][0]['geometry']['curve_recovery']['resumed_encoded_contours']
        provisional = [('#ffffff', box(0, 0, 100, 80)), ('#000000', box(20, 20, 22, 22))]
        with patch('outer_regions.run', return_value=(provisional, {'accepted': True,
                   'requires_final_source_validation': True})), patch('fit_repair.propose', return_value=None):
            rejected = run(project, svg, root / 'provisional', tool, total_shape_budget=5,
                evaluations=1000, max_edits=0, cleanup_edits=0, curve_tolerance=0.,
                source_growth=8., local_growth=0.)
        assert not rejected['accepted'] and not (root / 'provisional/after.3so').exists()
        trial = rejected['trials'][0]
        assert trial['source_to_filled']['rmse'] < 8. and trial['maximum_source_local_growth'] > 0.
        assert not trial['quality_passed']
        assert 'Source-relative image error budget exceeded' in trial['fit']['rejections']
        svg.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
            b'<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
            b'<path fill="#0000ff" d="M8 8L80 8L80 64L8 64Z"/>'
            b'<path fill="#ff0000" fill-rule="evenodd" d="M16 16L64 16L64 56L16 56Z'
            b'M24 24L48 24L48 48L24 48Z"/>'
            b'<path fill="#00ff00" d="M36 24L48 24L48 48L36 48Z"/></svg>')
        Image.fromarray(render(svg.read_bytes(), (100, 80))).save(project / 'source.png')
        previous = run(project, svg, root / 'joint_before', tool, evaluations=1000,
            max_edits=0, cleanup_edits=0, outer_passes=0, curve_tolerance=0.,
            source_growth=0., local_growth=0., total_shape_budget=32)
        assert previous['accepted']
        optimized = run(project, svg, root / 'joint_after', tool, evaluations=1000,
            resume_stack=root / 'joint_before/stack_plan.json.gz', max_edits=0, cleanup_edits=0,
            outer_passes=0, joint_edits=3, curve_tolerance=0., source_growth=0., local_growth=0.,
            total_shape_budget=32)
        assert optimized['accepted'] and optimized['after_shape_count'] < previous['after_shape_count']
        assert optimized['selected'] != 'candidate_0'
        saved = json.loads(gzip.decompress((root / 'joint_after/stack_plan.json.gz').read_bytes()))
        assert saved['optimization']['holes'] == 0
        assert optimized['trials'][0]['fit']['shape_count'] == previous['after_shape_count']
        assert (root / 'joint_after/ordering.json').read_bytes() == (
            root / 'joint_after' / next(t['directory'] for t in optimized['trials']
                                      if t['name'] == optimized['selected']) / 'ordering.json').read_bytes()
    print('Budgeted generation tests passed: measured native counts, joint topology/order reduction, '
          'opaque filled projects and unchanged native-resolution appearance.')


if __name__ == '__main__':
    main()
