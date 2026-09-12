"""Audit triangulated vinyl geometry for an affine region-cover dictionary."""

import argparse
from collections import Counter, defaultdict
import gzip
import hashlib
import json
from pathlib import Path
import re

import numpy as np
from scipy.spatial import ConvexHull


SUGGESTED = {
    2113, 2117, 2118, 2135, 2136, 2311, 1640, 1431, 1432, 3431, 3432,
    3631, 3632, 2105, 2124, 2127, 2131, 2134, 2214, 2215, 2230, 2239,
    2301, 2302, 2321, 2323, 2331, 607, 608, 939, 930, 137, 138, 126,
    124, 322, 323, 324, 325, 326, 339, 812, 120, 136,
}


def cross(left, right):
    return left[..., 0] * right[..., 1] - left[..., 1] * right[..., 0]


def area(points):
    return float(cross(points, np.roll(points, -1, axis=0)).sum() / 2)


def contains(points, point):
    inside = False
    for left, right in zip(points, np.roll(points, -1, axis=0)):
        if (left[1] > point[1]) != (right[1] > point[1]):
            intersection = left[0] + (point[1] - left[1]) * (
                right[0] - left[0]
            ) / (right[1] - left[1])
            if point[0] < intersection:
                inside = not inside
    return inside


def simplify(points):
    while len(points) > 3:
        left = points - np.roll(points, 1, axis=0)
        right = np.roll(points, -1, axis=0) - points
        tolerance = 1e-10 * np.maximum(
            1.0, np.linalg.norm(left, axis=1) * np.linalg.norm(right, axis=1)
        )
        keep = (np.abs(cross(left, right)) > tolerance) | (
            (left * right).sum(axis=1) < 0
        )
        if np.all(keep) or np.count_nonzero(keep) < 3:
            break
        points = points[keep]
    return points


def affine_signature(points):
    count = len(points)
    if count < 3 or count > 256:
        return None
    signatures = []
    for ordered in (points, points[::-1]):
        matrices = np.stack(
            (np.roll(ordered, -(count // 3), axis=0) - ordered,
             np.roll(ordered, -(2 * count // 3), axis=0) - ordered), axis=2
        )
        determinants = np.abs(np.linalg.det(matrices))
        maximum = float(determinants.max())
        if maximum <= 1e-12:
            continue
        for index in np.flatnonzero(determinants >= maximum * (1 - 1e-8)):
            normalized = (np.roll(ordered, -int(index), axis=0) - ordered[index])
            normalized = normalized @ np.linalg.inv(matrices[index]).T
            signatures.append(tuple(np.round(normalized, 5).flatten()))
    if not signatures:
        return None
    canonical = np.asarray(min(signatures), dtype='<f8')
    canonical[canonical == 0] = 0
    return hashlib.sha256(canonical.tobytes()).hexdigest()[:20]


def geometry_metrics(raw):
    raw_vertices = np.asarray(raw['vertices'], dtype=float)
    triangles = np.asarray(raw['triangles'], dtype=int)
    raw_alpha = raw_vertices[:, 2] if raw_vertices.shape[1] > 2 else np.ones(len(raw_vertices))
    visible = np.any(raw_alpha[triangles] > 0, axis=1)
    invisible_count = int(np.count_nonzero(~visible))
    triangles = triangles[visible]
    used_vertices = np.unique(triangles)
    vertex_remap = np.full(len(raw_vertices), -1, dtype=int)
    vertex_remap[used_vertices] = np.arange(len(used_vertices))
    triangles = vertex_remap[triangles]
    raw_vertices = raw_vertices[used_vertices]
    vertices, indices = np.unique(raw_vertices[:, :2], axis=0, return_inverse=True)
    triangles = indices[triangles]
    alpha = raw_vertices[:, 2] if raw_vertices.shape[1] > 2 else np.ones(len(raw_vertices))
    result = {
        'vertices': len(vertices), 'triangles': len(triangles),
        'invisible_triangles': invisible_count,
        'opaque': bool(np.all(np.abs(alpha - 1) <= 1e-10)),
        'alpha_min': float(alpha.min()), 'alpha_max': float(alpha.max()),
    }
    triangle_points = vertices[triangles]
    triangle_areas = np.abs(cross(
        triangle_points[:, 1] - triangle_points[:, 0],
        triangle_points[:, 2] - triangle_points[:, 0],
    )) / 2
    result['mesh_area'] = float(triangle_areas.sum())
    edges = Counter()
    for triangle in triangles[triangle_areas > 1e-12]:
        for left, right in zip(triangle, np.roll(triangle, -1)):
            edges[tuple(sorted((int(left), int(right))))] += 1
    adjacency = defaultdict(list)
    for (left, right), count in edges.items():
        if count == 1:
            adjacency[left].append(right)
            adjacency[right].append(left)
    if not adjacency or any(count > 2 for count in edges.values()) or any(
        len(neighbors) != 2 for neighbors in adjacency.values()
    ):
        result['geometry_status'] = 'nonmanifold_or_overlapping_mesh'
        return result
    loops = []
    remaining = set(adjacency)
    while remaining:
        first = min(remaining)
        current, previous = first, -1
        loop = []
        while current in remaining:
            remaining.remove(current)
            loop.append(current)
            neighbors = sorted(adjacency[current])
            following = neighbors[1] if neighbors[0] == previous else neighbors[0]
            previous, current = current, following
        if current != first:
            result['geometry_status'] = 'invalid_boundary'
            return result
        loops.append(simplify(vertices[loop]))
    depths = [sum(contains(other, loop[0]) for other in loops if other is not loop)
              for loop in loops]
    silhouette_area = sum(abs(area(loop)) * (-1 if depth % 2 else 1)
                          for loop, depth in zip(loops, depths))
    hull_area = float(ConvexHull(vertices).volume)
    result.update({
        'geometry_status': 'valid_boundary' if abs(silhouette_area - result['mesh_area'])
        <= max(1, result['mesh_area']) * 1e-7 else 'mesh_union_needs_boolean_check',
        'loops': len(loops), 'components': sum(depth % 2 == 0 for depth in depths),
        'holes': sum(depth % 2 == 1 for depth in depths),
        'boundary_vertices': sum(len(loop) for loop in loops),
        'solidity': silhouette_area / hull_area,
        'area': silhouette_area,
    })
    if len(loops) == 1:
        points = loops[0]
        if area(points) < 0:
            points = points[::-1]
        left = points - np.roll(points, 1, axis=0)
        right = np.roll(points, -1, axis=0) - points
        turns = cross(left, right)
        result['reflex_vertices'] = int(np.count_nonzero(turns < -1e-8))
        negative = turns < -1e-8
        result['concave_runs'] = int(np.count_nonzero(negative & ~np.roll(negative, 1)))
        result['affine_signature'] = affine_signature(points)
    return result


def union_metrics(record):
    loops = [simplify(np.asarray(points, dtype=float)) for points in record['contours']]
    vertices = np.concatenate(loops)
    hull_area = float(ConvexHull(vertices).volume)
    result = {
        'geometry_status': 'verified_grid_union', 'opaque': record['opaque'],
        'area': record['area'], 'loops': len(loops),
        'components': sum(area(loop) > 0 for loop in loops),
        'holes': sum(area(loop) < 0 for loop in loops),
        'boundary_vertices': sum(len(loop) for loop in loops),
        'solidity': record['area'] / hull_area,
        'concave_runs': 0,
        'reflex_vertices': 0,
        'affine_signature': None,
    }
    for points in loops:
        left = points - np.roll(points, 1, axis=0)
        right = np.roll(points, -1, axis=0) - points
        negative = cross(left, right) < -1e-8
        result['concave_runs'] += int(np.count_nonzero(negative & ~np.roll(negative, 1)))
        result['reflex_vertices'] += int(np.count_nonzero(negative))
    if len(loops) == 1:
        result['affine_signature'] = affine_signature(loops[0])
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--report', type=Path)
    parser.add_argument('--union-report', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    asset = root / 'assets/vector/shape_geometry.json.gz'
    with gzip.open(asset, 'rt', encoding='utf-8') as handle:
        shapes = json.load(handle)['shapes']
    names = json.loads((root / 'assets/vector/shape_names.json').read_text(encoding='utf-8'))
    existing = set(json.loads((root / 'assets/differential_shapes.json').read_text())['shape_ids'])
    native = {}
    if args.union_report:
        native = {record['id']: record for record in json.loads(args.union_report.read_text())['records']}
    registry = (root / 'src/core/shape_registry.cpp').read_text(encoding='utf-8')
    ranges = [(int(start, 16), int(end, 16), family) for start, end, family in
              re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+), "([^"]+)"\}', registry)]
    records = []
    for key in sorted(shapes, key=int):
        shape_id = int(key)
        family = next((family for start, end, family in ranges if start <= shape_id <= end), 'unknown')
        record = {
            'id': shape_id, 'name': names.get(key, {}).get('name', key),
            'family': family, 'suggested': shape_id in SUGGESTED,
            'existing': shape_id in existing,
        }
        try:
            record.update(geometry_metrics(shapes[key]))
        except (ValueError, IndexError, np.linalg.LinAlgError) as error:
            record['geometry_status'] = type(error).__name__
        if shape_id in native:
            record['raw_mesh_status'] = record['geometry_status']
            record.update(union_metrics(native[shape_id]))
        records.append(record)
    groups = defaultdict(list)
    for record in records:
        signature = record.get('affine_signature')
        if signature and record.get('geometry_status') in ('valid_boundary', 'verified_grid_union') and record.get('opaque'):
            groups[signature].append(record['id'])
    priority = lambda shape_id: (shape_id not in existing, shape_id not in SUGGESTED, shape_id)
    for record in records:
        equivalent = groups.get(record.get('affine_signature'), [])
        if equivalent and record.get('opaque'):
            record['affine_representative'] = min(equivalent, key=priority)
        reasons = []
        if not record.get('opaque'):
            reasons.append('nonopaque')
        if record.get('geometry_status') not in ('valid_boundary', 'verified_grid_union'):
            reasons.append(record.get('geometry_status', 'invalid'))
        if 'Letters' in record['family'] and not record['suggested']:
            reasons.append('font')
        if record.get('loops', 1) > 3:
            reasons.append('many_loops')
        if record.get('boundary_vertices', 100000) > 192:
            reasons.append('perimeter_over_192')
        if record.get('concave_runs', 0) > 6:
            reasons.append('many_concave_runs')
        if record.get('affine_representative', record['id']) != record['id']:
            reasons.append('affine_duplicate')
        record['screening'] = reasons or ['eligible']
    report = {
        'geometry_sha256': hashlib.sha256(asset.read_bytes()).hexdigest(),
        'shape_count': len(records), 'suggested_count': len(SUGGESTED),
        'native_union_records': len(native),
        'screening_counts': dict(Counter(reason for record in records for reason in record['screening'])),
        'affine_duplicate_groups': [sorted(group) for group in groups.values() if len(group) > 1],
        'records': records,
        'limitations': [
            'Native reports use the visible triangle union on a 1e-6 coordinate grid; raw mesh incidence is retained separately.'
            if native else 'Raw mesh incidence and area agreement do not prove the polygon union.',
            'Affine signatures round normalized coordinates to five decimals and identify near-duplicates for review.',
            'Screening predicts geometry cost and novelty, not observed fill quality.',
        ],
    }
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key != 'records'}, indent=2))
    for record in records:
        if record['suggested'] or record['existing'] or record['screening'] == ['eligible']:
            print(f"{record['id']:4} {record['name'][:30]:30} "
                  f"v={record.get('boundary_vertices', 0):3} "
                  f"r={record.get('reflex_vertices', 0):2} "
                  f"solid={record.get('solidity', 0):.3f} "
                  f"rep={record.get('affine_representative', 0):4} "
                  f"{','.join(record['screening'])}")


if __name__ == '__main__':
    main()
