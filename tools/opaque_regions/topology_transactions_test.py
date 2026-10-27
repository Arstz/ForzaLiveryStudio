"""Check hole promotion and source-supported neighboring-color reassignment."""

import numpy as np
import shapely
from shapely.geometry import Polygon, box

from rate_distortion import Item, RateDistortion, render_items
from paint_stack import canonical_geometry, polygon_svg
from simplify import image_error, render


def main():
    domain = box(0, 0, 64, 64)
    red = Polygon([(8, 8), (56, 8), (56, 56), (8, 56)],
                  [[(20, 20), (40, 20), (40, 40), (20, 40)]])
    stack = [('#ffffff', domain), ('#0000ff', box(20, 20, 40, 40)), ('#ff0000', red),
             ('#000000', box(0, 48, 64, 50))]
    source = render_items([Item(color, shape) for color, shape in stack], (64, 64))
    solver = RateDistortion(stack, source, domain, local_growth=0., topology_transactions=True)
    result, report = solver.run(10000, max_edits=24, source_rmse_limit=0., continue_below_budget=True)
    assert report['holes'] == 0 and report['regions'] == 4, report
    assert any(edit['kind'] == 'fill-hole-and-promote-current-owners' for edit in report['edits'])
    assert np.array_equal(render_items([Item(color, shape) for color, shape in result], (64, 64)), source)
    assert shapely.union_all([shape for _, shape in result]).equals(domain)
    repeated, check = RateDistortion(stack, source, domain, local_growth=0., topology_transactions=True).run(
        10000, max_edits=24, source_rmse_limit=0., continue_below_budget=True)
    assert [(color, shape.wkb) for color, shape in repeated] == [(color, shape.wkb) for color, shape in result]
    assert check['edits'] == report['edits']
    fragment = box(20, 20, 22, 22)
    noisy = [('#ffffff', domain), ('#808080', shapely.union_all([box(2, 2, 10, 10), fragment])),
             ('#818181', box(22, 18, 26, 24)), ('#000000', box(32, 0, 33, 64))]
    corrected = [('#ffffff', domain), ('#808080', box(2, 2, 10, 10)),
                 ('#818181', shapely.union_all([box(22, 18, 26, 24), fragment])), noisy[-1]]
    target = render_items([Item(color, shape) for color, shape in corrected], (64, 64))
    solver = RateDistortion(noisy, target, domain, local_growth=0., topology_transactions=True)
    cleaned, metrics = solver.run(10000, max_edits=24, source_rmse_limit=0., continue_below_budget=True)
    assert metrics['source_error']['rmse'] == 0 and metrics['regions'] == 4, metrics
    assert any(edit['kind'].startswith('absorb-neighbor-fragments-') for edit in metrics['edits'])
    assert np.array_equal(render_items([Item(color, shape) for color, shape in cleaned], (64, 64)), target)
    assert any(color == '#000000' and shape.equals(noisy[-1][1]) for color, shape in cleaned)
    distinct = [(color if color != '#818181' else '#b0b0b0', shape) for color, shape in noisy]
    distinct_source = render_items([Item(color, shape) for color, shape in distinct], (64, 64))
    solver = RateDistortion(distinct, distinct_source, domain, local_growth=8., topology_transactions=True)
    transferred, check = solver.run(10000, max_edits=24, source_rmse_limit=2., continue_below_budget=True)
    transferred_image = render_items([Item(color, shape) for color, shape in transferred], (64, 64))
    assert check['regions'] == 4 and check['source_error']['rmse'] <= 2.
    assert tuple(transferred_image[20, 20, :3]) == (176, 176, 176)
    assert any(color == '#000000' and shape.equals(noisy[-1][1]) for color, shape in transferred)
    redundant = Polygon([(0, 0), (16, 1e-10), (32, 0), (64, 0), (64, 64),
                         (48, 64 - 1e-10), (32, 64), (0, 64)])
    canonical = canonical_geometry(redundant)
    assert canonical_geometry(canonical).wkb == canonical.wkb
    encoded_stack = [('#ffffff', domain), ('#000000', redundant)]
    encoded = render(polygon_svg(encoded_stack, (64, 64)), (64, 64))
    predicted = render_items([Item(color, shape) for color, shape in encoded_stack], (64, 64))
    assert image_error(encoded, predicted)['max_channel_error'] <= 1
    print('Topology transactions passed: current-owner hole promotion, neighboring-color absorption, '
          'thin-detail preservation, exact silhouette and deterministic order.')


if __name__ == '__main__':
    main()
