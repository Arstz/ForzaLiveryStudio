"""Check Bucket curve recovery and the editor's GPU Compact Fit entry point."""

import gzip
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
import xml.etree.ElementTree as ET

from shapely.geometry import box

from curve_targets import recover
from fair_curves import path_curves
from simplify import read_svg, render


def main():
    tool = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='fls-curve-pipeline-') as temporary:
        root = Path(temporary)
        source = root / 'polygon.svg'
        points = [(128 + 80 * math.cos(index * 2 * math.pi / 96),
                   128 + 80 * math.sin(index * 2 * math.pi / 96)) for index in range(96)]
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256">'
            '<path fill="#ffffff" d="M0 0L256 0L256 256L0 256Z"/>'
            '<path fill="#0000ff" d="M' + 'L'.join(f'{x:.10f} {y:.10f}' for x, y in points) + 'Z"/></svg>')

        def invoke(arguments):
            result = subprocess.run([str(tool), *map(str, arguments)], capture_output=True, text=True)
            assert result.returncode == 0, (result.returncode, result.stderr[-2000:], result.stdout[-3000:])

        curves = root / 'curves'
        invoke([source, '--recover-fill-curves', '--curve-fit-tolerance', '.75', '--output', curves])
        recovered = curves / 'recovered.svg'
        conversion = json.loads((curves / 'recovery.json').read_text())
        assert conversion['paths'][0]['before_anchors'] == conversion['paths'][0]['after_anchors'] == 4
        assert conversion['paths'][1]['after_anchors'] < 32 and conversion['paths'][1]['curved_spans'] > 0
        targets = root / 'targets'
        invoke([recovered, '--prepare-fill-targets', '--output', targets])
        output = root / 'filled'
        invoke([recovered, '--fill-only', '--reference', targets / 'fill_targets.png', '--output', output,
                '--compact-fit-evaluations', '60000', '--compact-fit-workers', '1',
                '--compact-fit-boundary', '.5', '--compact-fit-inward', '.1', '--compact-fit-area-ratio', '.005',
                '--render-rmse', '4', '--render-max-error', '255', '--abstraction-local-rmse', '24'])
        report = json.loads((output / 'report.json').read_text())
        assert report['accepted'] and report['shape_count'] <= 4
        assert report['renderer'] == 'NativeShapeRenderer' and not report['leeway_used']
        assert report['bucket_input']['soft_anchors'] > 0
        assert all(row['gpu_requested'] and not row['polygon_seed_used']
                   and row['pipeline'] == 'editor curve-first Compact Fit' for row in report['per_path'])
        assert all('profileSeed' in row['diagnostics'] for row in report['per_path'])
        assert any('CUDA' in row['diagnostics'].get('optimizerBackend', '')
                   or row['diagnostics']['profileSeed'].get('profileCompute', {}).get('gpuBatches', 0) > 0
                   for row in report['per_path'])
        document = json.loads(gzip.decompress((output / 'generated_compact.3so').read_bytes()))
        shapes = [child for group in document['root']['children'] for child in group['children']
                  if child['kind'] == 'shape']
        assert any(shape['visual']['shape_id'] == 102 for shape in shapes)
        assert all(shape['opacity'] == 1 and shape['color'][3] == 255 and not shape['mask'] for shape in shapes)
        assert report['silhouette_difference_area'] <= 1
        baseline = render(source.read_bytes(), (256, 256))
        stack = read_svg(source.read_bytes(), (256, 256)) + [('#000000', box(4, 4, 16, 16))]
        _, _, selection = recover(tool, stack, (256, 256), box(0, 0, 256, 256), baseline, baseline,
            root / 'provisional_curves', tolerance=.75, source_growth=20., local_growth=24.)
        assert selection['initial_provisional_windows'] > 0
        assert 1 in selection['accepted_paths']
        assert selection['requires_final_source_validation'] and not selection['vector_source_local_passed']
        polygon = source.read_bytes()
        points = [(128 + 80 * math.cos(index * 2 * math.pi / 48),
                   128 + 80 * math.sin(index * 2 * math.pi / 48)) for index in range(48)]
        polygon = polygon[:polygon.index(b'<path fill="#0000ff"')] + (
            '<path fill="#0000ff" d="M' + 'L'.join(f'{x:.10f} {y:.10f}' for x, y in points)
            + 'Z"/></svg>').encode()
        smooth = polygon[:polygon.index(b'<path fill="#0000ff"')] + (
            b'<path fill="#0000ff" d="M208 128C208 172.18278 172.18278 208 128 208'
            b'C83.81722 208 48 172.18278 48 128C48 83.81722 83.81722 48 128 48'
            b'C172.18278 48 208 83.81722 208 128Z"/></svg>')
        selective_baseline = render(polygon, (256, 256))
        selective_source = selective_baseline.copy()
        selective_source[:, :128] = render(smooth, (256, 256))[:, :128]
        partial, _, details = recover(tool, read_svg(polygon, (256, 256)), (256, 256),
            box(0, 0, 256, 256), selective_source, selective_baseline, root / 'chain_recovery',
            tolerance=.75, source_growth=0., local_growth=0., encoded=polygon)
        assert details['partial_paths'] and 1 in details['accepted_paths']
        assert b'C' in partial
        assert np.array_equal(render(partial, (256, 256))[:, 160:], selective_baseline[:, 160:])
        (root / 'partial.svg').write_bytes(partial)
        invoke([root / 'partial.svg', '--prepare-fill-targets', '--output', root / 'partial_targets'])
        preserved, _, _ = recover(tool, read_svg(smooth, (256, 256)), (256, 256),
            box(0, 0, 256, 256), render(smooth, (256, 256)), render(smooth, (256, 256)),
            root / 'preserved_curves', encoded=smooth)
        assert b'C208' in preserved
        points = [(128 + 80 * math.cos(-math.pi / 2 + index * math.pi / 24),
                   128 + 80 * math.sin(-math.pi / 2 + index * math.pi / 24)) for index in range(25)]
        mixed = polygon[:polygon.index(b'<path fill="#0000ff"')] + (
            '<path fill="#0000ff" d="M' + 'L'.join(f'{x:.10f} {y:.10f}' for x, y in points)
            + 'C83.81722 208 48 172.18278 48 128C48 83.81722 83.81722 48 128 48Z"/></svg>').encode()
        mixed_image = render(mixed, (256, 256))
        improved, _, _ = recover(tool, read_svg(mixed, (256, 256)), (256, 256),
            box(0, 0, 256, 256), mixed_image, mixed_image, root / 'mixed_chains',
            encoded=mixed, tolerance=.75, source_growth=4., local_growth=16.)
        assert improved.count(b'C') > mixed.count(b'C')
        old_curves = [curve for curve in path_curves(list(ET.fromstring(mixed))[1].attrib['d']) if len(curve) == 4]
        new_curves = list(path_curves(list(ET.fromstring(improved))[1].attrib['d']))
        assert all(any(np.array_equal(curve, original) for curve in new_curves) for original in old_curves)
        (root / 'mixed.svg').write_bytes(improved)
        invoke([root / 'mixed.svg', '--prepare-fill-targets', '--output', root / 'mixed_targets'])
    print('Curve pipeline passed: Bucket cubics, preserved straight corners, editor profile initialization, '
          'GPU execution, native curved shapes and opaque project rendering.')


if __name__ == '__main__':
    main()
