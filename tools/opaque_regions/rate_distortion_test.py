"""Geometry, visibility and ordering checks for complete-image budget search."""

import numpy as np
import shapely
from shapely.geometry import Polygon, box

from paint_stack import geometry_metrics, order_stack
from rate_distortion import Item, RateDistortion, render_items, window_sums


def main():
    domain = box(0, 0, 100, 80)
    red = Polygon([(10, 10), (90, 10), (90, 70), (10, 70)],
                  [[(20, 30), (30, 30), (30, 40), (20, 40)],
                   [(70, 30), (80, 30), (80, 40), (70, 40)]])
    stack = [('#ffffff', domain), ('#0000ff', box(20, 30, 30, 40)),
        ('#0000ff', box(70, 30, 80, 40)), ('#ff0000', red), ('#00ff00', box(30, 30, 70, 40))]
    source = render_items([Item(color, shape) for color, shape in stack], (100, 80))
    solver = RateDistortion(stack, source, domain, local_growth=0.)
    result, report = solver.run(4, max_edits=100, source_rmse_limit=0.)
    assert report['within_geometry_budget'], report
    assert report['regions'] == 4 and report['holes'] == 0, report
    assert report['source_error']['rmse'] == 0
    assert any(edit['kind'] == 'reorder-and-complete' for edit in report['edits'])
    assert shapely.union_all([shape for _, shape in result]).equals(domain)
    repeat, repeated = RateDistortion(stack, source, domain, local_growth=0.).run(4, source_rmse_limit=0.)
    assert [(color, shape.wkb) for color, shape in result] == [(color, shape.wkb) for color, shape in repeat]
    assert repeated['edits'] == report['edits']
    layered = [('#ffffff', domain), ('#ff0000', box(10, 10, 90, 70)),
               ('#0000ff', box(20, 20, 80, 60)), ('#ff0000', box(30, 30, 70, 50))]
    reference = render_items([Item(color, shape) for color, shape in layered], (100, 80))
    split, metrics = RateDistortion(layered, reference, domain, local_growth=0.).run(2, source_rmse_limit=0.)
    assert not metrics['within_geometry_budget']
    assert sum(color == '#ff0000' for color, _ in split) == 2
    assert metrics['source_error']['rmse'] == 0
    ordered, diagnostics = order_stack(layered)
    assert diagnostics['fixed_ownership'] and diagnostics['edges']
    assert np.array_equal(render_items([Item(color, shape) for color, shape in ordered], (100, 80)), reference)
    positions = {source: index for index, source in enumerate(diagnostics['order'])}
    assert all(positions[first] < positions[last] for first, last in diagnostics['edges'])
    assert sum(color == '#ff0000' for color, _ in ordered) == 2
    disjoint = [('#ffffff', domain), ('#0000ff', Polygon([(10, 10), (35, 10), (45, 30), (30, 45), (10, 30)])),
                ('#ff0000', box(60, 10, 80, 30))]
    simpler_first, changed = order_stack(disjoint)
    assert changed['order'] == [0, 2, 1]
    assert np.array_equal(render_items([Item(color, shape) for color, shape in disjoint], (100, 80)),
                          render_items([Item(color, shape) for color, shape in simpler_first], (100, 80)))
    fractional = [('#ffffff', box(0, 0, 1000, 800)),
                  ('#ff0000', box(10.123456783, 10.123456783, 990.987654325, 790.987654325))]
    stable, numeric = order_stack(fractional)
    assert numeric['order'] == [0, 1] and numeric['edges'] == [(0, 1)]
    assert stable[1][1].equals(fractional[1][1])
    detailed = [('#ffffff', domain), ('#ff0000', box(20, 20, 80, 60))]
    approximate = [('#ffffff', domain), ('#ff0000', Polygon([(20, 20), (80, 20), (20, 60)]))]
    target = render_items([Item(color, shape) for color, shape in detailed], (100, 80))
    refiner = RateDistortion(approximate, target, domain, reference_stack=detailed)
    recovered, improvement = refiner.refine_from_reference(2, max_edits=4)
    assert improvement['source_error']['rmse'] == 0 and improvement['triangle_bound'] == 2
    assert len(improvement['refinements']) == 1 and recovered[1][1].equals(detailed[1][1])
    redundant = Polygon([(0, 0), (50, 0), (100, 0), (100, 80), (50, 80), (0, 80)])
    assert geometry_metrics(redundant)['points'] == 4
    shifted = Polygon([(50, 0), (100, 0), (100, 80), (0, 80), (0, 0)])
    assert geometry_metrics(shifted)['points'] == 4
    from adaptive_vertices import refine
    adaptive = RateDistortion(approximate, target, domain, reference_stack=detailed)
    recovered, vertices = refine(adaptive, 2, max_edits=4)
    assert vertices['source_error']['rmse'] == 0 and vertices['triangle_bound'] == 2
    assert len(vertices['edits']) == 1 and recovered[1][1].equals(detailed[1][1])
    values = np.arange(36).reshape(6, 6)
    sums = window_sums(values, 3)
    assert sums.shape == (4, 4) and sums[1, 2] == values[1:4, 2:5].sum()
    for _ in range(2):
        plain = [('#ffffff', domain)]
        white = np.full((80, 100, 4), 255, np.uint8)
        unchanged, check = RateDistortion(plain, white, domain).run(1)
        assert unchanged[0][1].equals(domain) and not check['edits']
        assert geometry_metrics(unchanged[0][1])['triangle_bound'] == 1
    print('Budget search tests passed: hidden completion, real geometry reduction, hole filling, '
          'joint ordering, separate same-fill positions, silhouette, local errors and deterministic no-op.')


if __name__ == '__main__':
    main()
