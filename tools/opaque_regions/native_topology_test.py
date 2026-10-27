"""Check actual native support holes and occlusion independently of target contours."""

import gzip
import json
from pathlib import Path
import tempfile

from native_topology import audit


def main():
    with tempfile.TemporaryDirectory(prefix='fls-native-topology-') as temporary:
        root = Path(temporary)
        geometry = root / 'geometry.json'
        geometry.write_text(json.dumps({'shapes': {'101': {'vertices': [[-1, -1, 1], [1, -1, 1],
            [1, 1, 1], [-1, 1, 1]], 'triangles': [[0, 1, 2], [0, 2, 3]]}}}))

        def rectangle(identifier, bounds):
            a, b, c, d = bounds
            return {'id': identifier, 'kind': 'shape', 'color': [0, 0, 0, 255],
                    'visual': {'kind': 'vector', 'shape_id': 101},
                    'transform': {'x': (a + c) / 2, 'y': (b + d) / 2,
                                  'scale_x': (c - a) / 2, 'scale_y': (d - b) / 2}}

        frame = {'id': 'frame', 'kind': 'group', 'children': [rectangle(str(index), bounds)
            for index, bounds in enumerate([(0, 0, 64, 10), (0, 54, 64, 64),
                                           (0, 10, 10, 54), (54, 10, 64, 54)])]}
        upper = {'id': 'upper', 'kind': 'group', 'children': [rectangle('cover', (10, 10, 54, 54))]}
        document = {'root': {'children': [frame, upper]}}
        project = root / 'filled.3so'
        project.write_bytes(gzip.compress(json.dumps(document).encode()))
        result = audit(project, geometry)
        assert result['summary']['target_holes'] == 0
        assert result['summary']['native_support_holes'] == result['summary']['native_holes_over_1px2'] == 1
        assert result['summary']['fully_occluded_native_holes'] == 1
        assert result['summary']['hole_area_exposed_to_below'] == 0
        document['root']['children'].pop()
        document['root']['transform'] = {'rotation': 90, 'x': 100}
        project.write_bytes(gzip.compress(json.dumps(document).encode()))
        result = audit(project, geometry)
        assert result['summary']['fully_occluded_native_holes'] == 0
        assert abs(result['summary']['hole_area_exposed_to_below'] - 44 ** 2) < 1e-5
    print('Native topology tests passed: real mesh union, target/support distinction, upper occlusion and world transforms.')


if __name__ == '__main__':
    main()
