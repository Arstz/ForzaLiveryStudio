"""Check outer-contour construction, visible-island salience and fixed-owner regrouping."""

from pathlib import Path
import json
import subprocess
import sys
import tempfile

import numpy as np
import shapely
from shapely.geometry import Polygon, box

from outer_regions import constraint, from_rings, insignificant_components, regroup, run, simplify_batch, transfer_weak_islands
from paint_stack import geometry_metrics, polygon_svg
from rate_distortion import RateDistortion
from simplify import render


def main():
    tool = Path(sys.argv[1]).resolve()
    domain = box(0, 0, 64, 64)
    support = Polygon([(8, 8), (16, 8), (20, 6), (24, 8), (28, 6), (32, 8),
                       (36, 6), (40, 8), (48, 8), (48, 48), (8, 48)])
    core = box(8, 16, 48, 48)
    with tempfile.TemporaryDirectory(prefix='fls-outer-regions-') as temporary:
        root = Path(temporary)
        manifest = [constraint('hidden', support, core, box(8, 4, 48, 48)),
                    constraint('locked', support, support, support)]
        candidates = simplify_batch(tool, manifest, root / 'native')
        hidden = from_rings(candidates['regions'][0]['candidates'][0]['rings'])
        locked = from_rings(candidates['regions'][1]['candidates'][0]['rings'])
        assert geometry_metrics(hidden)['points'] == 4 and hidden.covers(core)
        assert locked.symmetric_difference(support).area <= .0001
        locked_output = root / 'locked_progress'
        locked_output.mkdir()
        progress_path = locked_output / 'outer_progress.json'
        progress_path.write_text('{}')
        with progress_path.open('r'):
            process = subprocess.run([str(tool), str(root / 'native/constraints.json'),
                '--simplify-fill-contours', '--area-tolerance', '.0001', '--output', str(locked_output)],
                capture_output=True, text=True)
        assert process.returncode == 0, process.stderr
        assert len(json.loads((locked_output / 'outer_contours.json').read_text())['regions']) == 2
        hole = Polygon([(8, 8), (56, 8), (56, 56), (8, 56)],
                       [[(20, 20), (40, 20), (40, 40), (20, 40)]])
        stack = [('#ffffff', domain), ('#0000ff', box(20, 20, 40, 40)), ('#ff0000', hole)]
        source = render(polygon_svg(stack, (64, 64)), (64, 64))
        grouped, diagnostics = regroup(stack, source, source, domain, source_growth=0., local_growth=0.)
        assert diagnostics['edits'] and all(not part.interiors for _, shape in grouped
                                           for part in ([shape] if shape.geom_type == 'Polygon' else shape.geoms))
        assert np.array_equal(render(polygon_svg(grouped, (64, 64)), (64, 64)), source)
        split = [('#ffffff', domain), ('#ff0000', box(4, 4, 16, 16)),
                 ('#0000ff', box(12, 12, 52, 52)), ('#ff0000', box(40, 40, 56, 56))]
        split_source = render(polygon_svg(split, (64, 64)), (64, 64))
        merged, report = regroup(split, split_source, split_source, domain, source_growth=0., local_growth=0.)
        assert np.array_equal(render(polygon_svg(merged, (64, 64)), (64, 64)), split_source)
        assert report['rejected'].get('dependency_cycle', 0) or len(merged) < len(split)
        noisy = [('#808080', domain), ('#818181', shapely.union_all([box(4, 4, 20, 20), box(40, 40, 42, 42)])),
                 ('#000000', box(30, 4, 31, 16))]
        image = render(polygon_svg(noisy, (64, 64)), (64, 64))
        solver = RateDistortion(noisy, image, domain, local_growth=8.)
        _, owned = solver.visibility()
        assert any(part.area == 4 for part in insignificant_components(solver, 1, owned[1]))
        assert not insignificant_components(solver, 2, owned[2])
        complete, metrics = run(tool, noisy, image, image, domain, root / 'complete',
            passes=1, source_growth=2., local_growth=8.)
        assert metrics['accepted']
        assert any(color == '#000000' and shape.equals(noisy[-1][1]) for color, shape in complete)
        assert shapely.union_all([shape for _, shape in complete]).equals(domain)
        repeated, repeated_metrics = run(tool, noisy, image, image, domain, root / 'repeat',
            passes=1, source_growth=2., local_growth=8.)
        assert [(color, shape.wkb) for color, shape in complete] == [(color, shape.wkb) for color, shape in repeated]
        rough = [('#ffffff', domain), ('#808080', support)]
        rough_image = render(polygon_svg(rough, (64, 64)), (64, 64))
        unchanged, guarded = run(tool, rough, rough_image, rough_image, domain, root / 'rough_guarded',
            passes=1, boundary_tolerance=2., source_growth=12., local_growth=24.)
        assert guarded['accepted'] and guarded['after']['points'] == guarded['before']['points']
        smoothed, check = run(tool, rough, rough_image, rough_image, domain, root / 'rough',
            passes=1, boundary_tolerance=2., source_growth=12., local_growth=48.)
        assert check['accepted'] and check['after']['points'] < check['before']['points']
        assert check['source_error']['rmse'] <= 12.
        small = box(40, 40, 42, 42)
        surround = Polygon(box(36, 36, 48, 48).exterior, [small.exterior.coords])
        neighbor = [('#000000', domain), ('#808080', shapely.union_all([box(4, 4, 20, 20), small])),
                    ('#818181', surround)]
        neighbor_source = render(polygon_svg(neighbor, (64, 64)), (64, 64))
        solver = RateDistortion(neighbor, neighbor_source, domain, local_growth=2.)
        solver.budget = float('inf')
        transfers = transfer_weak_islands(solver, 2. ** 2 * 64 * 64 * 3, tool, root / 'neighbor_transfers')
        assert any(entry['from'] == 1 and entry['to'] == [2] and entry['area'] == 4 for entry in transfers)
        assert solver.items[2].geometry.covers(small)
        tiny = Polygon([(10, 10), (10 + 1e-10, 10), (10, 10 + 1e-10)])
        precision = simplify_batch(tool, [constraint('precision', tiny, tiny, domain)], root / 'precision')
        assert from_rings(precision['regions'][0]['candidates'][0]['rings']).is_empty
        opening = box(20, 30, 26, 34)
        interior = box(26, 26, 38, 38)
        receiver = box(20, 20, 44, 44).difference(shapely.union_all([interior, opening]))
        bridge = [('#000000', domain), ('#808080', shapely.union_all([box(4, 4, 10, 10), opening])),
                  ('#818181', receiver)]
        for occluded in (False, True):
            example = bridge + ([('#0000ff', interior)] if occluded else [])
            pixels = render(polygon_svg(example, (64, 64)), (64, 64))
            solver = RateDistortion(example, pixels, domain, local_growth=2.)
            solver.budget = float('inf')
            transactions = transfer_weak_islands(solver, 2. ** 2 * 64 * 64 * 3,
                tool, root / ('occluded_bridge' if occluded else 'visible_bridge'))
            assert bool(transactions) == occluded
            assert solver.items[2].metrics['holes'] == 0
    print('Outer-region tests passed: Bucket corridor simplification, complete geometry constraints, '
          'cutout repainting, fixed-owner grouping, salience, silhouette and deterministic output.')


if __name__ == '__main__':
    main()
