"""Check Bucket curve recovery and the editor's GPU Compact Fit entry point."""

import gzip
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

from shapely.geometry import box

from curve_targets import recover
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
    print('Curve pipeline passed: Bucket cubics, preserved straight corners, editor profile initialization, '
          'GPU execution, native curved shapes and opaque project rendering.')


if __name__ == '__main__':
    main()
