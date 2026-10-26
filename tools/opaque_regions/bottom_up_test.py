"""Checks for joint opaque geometry, grouping and stack construction."""

import math

from shapely import affinity
from shapely.geometry import Polygon, box

from bottom_up import StackSearch, visible_masks
from fair_curves import polygons


def check(scene, **options):
    search = StackSearch(scene, beam_width=options.pop('beam_width', 4), max_states=options.pop('max_states', 96), **options)
    proposals, report = search.run()
    expected, _, domain = visible_masks(scene)
    for paints, details in proposals:
        actual, _, silhouette = visible_masks(paints)
        assert silhouette.symmetric_difference(domain).area < 1e-6
        for color, shape in expected.items():
            assert shape.symmetric_difference(actual.get(color, Polygon())).area < 1e-6
        assert details['search_cost'] < report['baseline_search_cost']
    return search, proposals, report


def main():
    white, red, blue = '#ffffff', '#ff0000', '#0000ff'
    outer, inner = box(10, 10, 90, 90), box(30, 30, 70, 70)
    scene = [(white, box(0, 0, 100, 100)), (red, outer.difference(inner))]
    search, proposals, _ = check(scene)
    assert proposals
    paints, details = proposals[0]
    assert len(paints) == 3 and sum(layer['holes'] for layer in details['layers']) == 0
    assert paints[0][1].equals(box(0, 0, 100, 100))
    assert paints[-1][0] == white and paints[-1][1].covers(inner)
    assert details['layers'][0]['committed_visible_area'] < paints[0][1].area
    repeated = check(scene)[1][0][0]
    assert [(c, s.wkb) for c, s in paints] == [(c, s.wkb) for c, s in repeated]
    _, bounded, bounded_report = check(scene, beam_width=1, max_states=1)
    assert bounded and bounded_report['expanded_states'] == 1
    _, bridged, _ = check([(red, box(0, 0, 20, 40)), (red, box(40, 0, 60, 40)),
                           (blue, box(20, 0, 40, 40))])
    assert bridged and len(bridged[0][0]) == 2
    assert any(c == red and s.covers(box(0, 0, 60, 40)) for c, s in bridged[0][0])
    first_hole, second_hole = box(20, 30, 30, 40), box(70, 30, 80, 40)
    body = box(10, 10, 90, 70).difference(first_hole.union(second_hole))
    scene = [(white, box(0, 0, 100, 80)), (blue, first_hole), (blue, second_hole),
             (red, body), ('#00ff00', box(30, 30, 70, 40))]
    _, grouped_cutouts, _ = check(scene, start=3, stop=4, max_states=1)
    assert grouped_cutouts
    stack, details = grouped_cutouts[0]
    assert len(stack) == len({color for color, _ in stack}) == 4
    assert sum(len(part.interiors) for _, part in stack) == 0
    assert details['filled_holes'] == 2 and details['removed_hidden_source_positions'] == [1, 2]
    assert next(shape for color, shape in stack if color == blue).equals(box(20, 30, 80, 40))
    green = '#00ff00'
    scene = [(white, box(0, 0, 100, 80)), (blue, first_hole), (green, second_hole), (red, body)]
    _, mixed, _ = check(scene, start=3, stop=4, max_states=2)
    assert mixed and len(mixed[0][0]) == 4
    assert sum(len(part.interiors) for _, shape in mixed[0][0] for part in polygons(shape)) == 0
    assert mixed[0][1]['promoted_colors'] in ([blue, green], [green, blue])
    notched = Polygon([(0, 0), (60, 0), (40, 20), (30, 12), (20, 20)])
    cover = Polygon([(20, 20), (30, 12), (40, 20), (30, 30)])
    _, triangular, _ = check([(red, notched), (blue, cover)])
    assert triangular and any(c == red and len(s.simplify(0).exterior.coords) == 4 for c, s in triangular[0][0])
    interleaved = [(red, box(0, 0, 32, 64)), (blue, box(20, 0, 52, 64)),
                   (red, box(28, 12, 36, 28))]
    _, positions, _ = check(interleaved)
    assert not positions or all(sum(layer['holes'] for layer in r['layers']) == 0
                                and sum(len(polygons(shape)) for _, shape in p) >= 3 for p, r in positions)
    _, transparent, _ = check([(red, outer.difference(inner))])
    assert not transparent
    _, noop, report = check([(red, outer)])
    assert not noop and report['validated_arrangements'] == 0
    separate = affinity.rotate(box(0, 0, 20, 10), 17)
    _, wrappers, _ = check([(red, separate), (red, affinity.translate(separate, xoff=100))])
    assert not wrappers
    _, hidden, _ = check([(red, outer), (blue, outer)], stop=1)
    assert hidden and len(hidden[0][0]) == 1
    check([(white, box(0, 0, 100, 100)), (red, notched), (blue, cover)], start=1)
    rectangle = box(0, 0, 80, 10)
    assert math.isclose(search.metrics(rectangle)['cost'],
                        search.metrics(affinity.rotate(rectangle, 17))['cost'], abs_tol=1e-7)
    assert math.isclose(search.metrics(box(0, 0, 80, .01))['cost'],
                        search.metrics(rectangle)['cost'], abs_tol=1e-7)
    print('Bottom-up tests passed: deferred ownership, hole restoration, connected grouping, '
          'nonrectangular shortcuts, separate color positions, silhouette, fixed windows, '
          'angle regularity and deterministic no-op.')


if __name__ == '__main__':
    main()
