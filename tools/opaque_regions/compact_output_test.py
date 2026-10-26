"""Headless checks for measured fills, saved projects and native rendering."""

import gzip
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from cli_test import png


def main():
    tool = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='fls-compact-output-') as temporary:
        root = Path(temporary)
        source = root / 'input.svg'
        reference = root / 'reference.png'
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="#ff0000" d="M0 0H64V64H0Z"/>'
            '<path fill="#0000ff" d="M16 16H48V48H16Z"/>'
            '<path fill="#ff0000" d="M24 24H40V40H24Z"/></svg>')
        png(reference, pixel=lambda x, y: (255, 0, 0) if not (16 <= x < 48 and 16 <= y < 48)
            or 24 <= x < 40 and 24 <= y < 40 else (0, 0, 255))
        output = root / 'result'
        cache = root / 'cache'

        def invoke(extra=(), expected=0):
            process = subprocess.run([str(tool), str(source), '--fill-only', '--reference', str(reference),
                '--output', str(output), '--compact-fit-cache', str(cache), '--render-rmse', '0',
                '--render-max-error', '0', '--abstraction-local-rmse', '0', *extra], capture_output=True, text=True)
            assert process.returncode == expected, (process.returncode, process.stdout[-2000:], process.stderr)
            return json.loads((output / 'report.json').read_text())

        first = invoke()
        assert first['accepted'] and first['verified'] and first['shape_count'] == 3
        assert first['region_count'] == 3 and first['color_count'] == 2
        assert first['renderer'] == 'NativeShapeRenderer' and first['opaque_to_filled']['rmse'] == 0
        assert first['silhouette_difference_area'] == 0 and first['downsampled'] is False
        assert not first['leeway_used'] and not list(output.glob('*.svg'))
        packed = output / 'generated_compact.3so'
        saved = json.loads(gzip.decompress(packed.read_bytes()))
        groups = saved['root']['children']
        assert len(groups) == 3
        assert [group['children'][0]['color'] for group in groups] == [
            [0, 0, 255, 255], [255, 0, 0, 255], [0, 0, 255, 255]]
        shapes = [shape for group in groups for shape in group['children'] if shape['kind'] == 'shape']
        contours = [node for group in groups for node in group['children'] if node['kind'] == 'contour']
        assert len(contours) == 3 and all(not node['visible'] for node in contours)
        assert len({shape['id'] for shape in shapes}) == 3
        assert all(shape['opacity'] == 1 and not shape['mask'] for shape in shapes)
        repeated = invoke()
        assert all(row['cache_hit'] for row in repeated['per_path'])
        assert json.loads(gzip.decompress(packed.read_bytes())) == saved
        cached = next(cache.glob('*.json'))
        cached.write_text('{}')
        repaired = invoke()
        assert repaired['accepted'] and any(not row['cache_hit'] for row in repaired['per_path'])
        capped = invoke(['--compact-fit-total-shape-budget', '2', '--scene-validated-fills'], expected=3)
        assert not capped['accepted'] and capped['shape_count'] == 3
        assert not capped['shape_budget_passed'] and capped['over_budget_by'] == 1
        assert capped['failed_regions'] == 0 and not packed.exists()
        assert not (output / 'generated_compact.png').exists()
        invoke()
        original = packed.read_bytes()
        collision = subprocess.run([str(tool), str(source), '--fill-only', '--reference', str(packed),
            '--output', str(output)], capture_output=True)
        assert collision.returncode == 2 and packed.read_bytes() == original
        source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="#ff0000" fill-rule="evenodd" d="M0 0H64V64H0Z M16 16H48V48H16Z"/></svg>')
        from struct import pack
        import zlib
        def chunk(kind, payload):
            data = kind + payload
            return pack('>I', len(payload)) + data + pack('>I', zlib.crc32(data))
        raw = b''.join(b'\0' + bytes(channel for x in range(64) for channel in
            ((0, 0, 0, 0) if 16 <= x < 48 and 16 <= y < 48 else (255, 0, 0, 255))) for y in range(64))
        reference.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', pack('>IIBBBBB', 64, 64, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
        failed = invoke(['--compact-fit-shape-budget', '1'], expected=3)
        assert not failed['accepted'] and not failed['verified']
        assert failed['shape_count'] is None and failed['failed_regions'] == 1
        assert not packed.exists() and not (output / 'generated_compact.png').exists()
        limited = invoke(['--compact-fit-shape-budget', '1', '--scene-validated-fills'], expected=3)
        assert not limited['scene_verified'] and limited['shape_count'] is None
    print('Compact output tests passed: actual counts, separate color positions, opaque native '
          'project rendering, transforms, caching, corrupt-cache recovery and failed-fill rejection.')


if __name__ == '__main__':
    main()
