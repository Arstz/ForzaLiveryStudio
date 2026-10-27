"""Measure target contours and actual saved native-region support separately."""

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import shapely
from shapely.affinity import affine_transform
from shapely.geometry import Polygon

from fair_curves import polygons


def matrix(transform):
    angle = math.radians(transform.get('rotation', 0))
    cosine, sine = math.cos(angle), math.sin(angle)
    x, y = transform.get('scale_x', 1), transform.get('scale_y', 1)
    skew = transform.get('skew', 0)
    return np.array([[cosine * x, (cosine * skew - sine) * y, transform.get('x', 0)],
                     [sine * x, (sine * skew + cosine) * y, transform.get('y', 0)], [0., 0., 1.]])


def audit(project, geometry, output=None):
    project, geometry = Path(project), Path(geometry)
    document = json.loads(gzip.decompress(project.read_bytes()))
    data = geometry.read_bytes()
    registry = json.loads(gzip.decompress(data) if data[:2] == b'\x1f\x8b' else data)['shapes']
    cache = {}

    def primitive(identifier):
        if identifier not in cache:
            shape = registry[str(identifier)]
            vertices, triangles = np.array(shape['vertices'], dtype=float), np.array(shape['triangles'])
            if identifier == 3801:
                vertices[:, 0] *= 2
            active = np.max(vertices[triangles, 2], axis=1) > 0
            cache[identifier] = shapely.union_all([Polygon(points[:, :2]) for points in vertices[triangles[active]]],
                                                 grid_size=1e-7)
        return cache[identifier]

    def support(layer, parent):
        world = parent @ matrix(layer.get('transform', {}))
        if not layer.get('visible', True):
            return Polygon()
        if layer.get('kind') == 'shape':
            if layer.get('mask', False) or layer.get('opacity', 1) != 1 or layer['color'][3] != 255 \
                    or layer['visual']['kind'] != 'vector':
                raise ValueError('Native topology auditing requires opaque unmasked vector placements')
            return affine_transform(primitive(layer['visual']['shape_id']),
                [world[0, 0], world[0, 1], world[1, 0], world[1, 1], world[0, 2], world[1, 2]])
        return shapely.union_all([support(child, world) for child in layer.get('children', [])], grid_size=1e-5)

    covered = Polygon()
    rows = []
    parent = matrix(document['root'].get('transform', {}))
    for group in reversed(document['root']['children']):
        shape = support(group, parent)
        parts = polygons(shape)
        holes = [Polygon(ring) for part in parts for ring in part.interiors]
        contours = [child for child in group.get('children', []) if child.get('kind') == 'contour']
        rows.append({'id': group['id'], 'target_holes': sum(len(contour['cutouts']) for contour in contours),
            'target_anchors': sum(len(ring) for contour in contours for ring in [contour['points'], *contour['cutouts']]),
            'native_support_components': len(parts), 'native_support_holes': len(holes),
            'native_holes_over_1px2': sum(hole.area > 1 for hole in holes),
            'fully_occluded_native_holes': sum(hole.difference(covered).area <= .0001 for hole in holes),
            'hole_area_exposed_to_below': sum(hole.difference(covered).area for hole in holes)})
        covered = shapely.union_all([covered, shape], grid_size=1e-5)
    rows.reverse()
    keys = ['target_holes', 'target_anchors', 'native_support_components', 'native_support_holes',
            'native_holes_over_1px2', 'fully_occluded_native_holes', 'hole_area_exposed_to_below']
    report = {'method': 'Union of deployed native triangle support at saved world transforms',
        'union_grid_pixels': 1e-5, 'positive_alpha_support': True,
        'project_sha256': hashlib.sha256(project.read_bytes()).hexdigest(),
        'geometry_sha256': hashlib.sha256(geometry.read_bytes()).hexdigest(),
        'summary': {key: sum(row[key] for row in rows) for key in keys}, 'regions': rows}
    if output is not None:
        Path(output).write_text(json.dumps(report, indent=2))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--geometry', type=Path, default=Path('build/Release/assets/vector/shape_geometry.json.gz'))
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    print(json.dumps(audit(arguments.project, arguments.geometry, arguments.output)['summary'], indent=2))
