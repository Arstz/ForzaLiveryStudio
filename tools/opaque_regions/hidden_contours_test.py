"""Checks for full-geometry occlusion-constrained contour changes."""

from shapely.geometry import Polygon, box
from fair_curves import polygons

from hidden_contours import complete_contour, contour_proposals, open_hidden_holes
from layer_completion import difference_area


def main():
    required = box(0, 0, 20, 20)
    hidden_lobe = Polygon([(10, 10), (50, 10), (45, 13), (50, 16), (45, 19), (50, 22), (10, 22)])
    support = required.union(hidden_lobe)
    allowed = support.union(box(10, 0, 60, 40))
    candidates = contour_proposals(support, required, allowed)
    assert any(shape.equals(required) for _, shape in candidates)
    assert any(support.difference(shape).area > 0 for _, shape in candidates)
    assert any(shape.difference(support).area > 0 for _, shape in candidates)
    for _, shape in candidates:
        assert shape.is_valid and difference_area(required, shape) < 1e-6
        assert difference_area(shape, allowed) < 1e-6
    tiny = box(30, 0, 30.001, .001)
    completed = complete_contour(box(0, 0, 20, 20), required.union(tiny), allowed.union(tiny), .01)
    assert completed.covers(tiny)
    ring = box(0, 0, 60, 60).difference(box(20, 20, 40, 40))
    locked = contour_proposals(ring, ring, ring)
    assert not locked
    hidden_hole = contour_proposals(ring, ring, box(0, 0, 60, 60))
    assert any(not shape.interiors for _, shape in hidden_hole if shape.geom_type == 'Polygon')
    ring = box(0, 0, 20, 20).difference(box(8, 8, 12, 12))
    cover = box(0, 8, 10, 12)
    channel_required = ring.difference(cover)
    score = lambda shape: sum(512 * len(part.interiors) + len(part.exterior.coords) for part in polygons(shape))
    opened = open_hidden_holes(ring, channel_required, cover, score)
    assert not opened.interiors and opened.covers(channel_required)
    assert ring.difference(opened).difference(cover).area < 1e-6
    assert opened.union(cover).equals(ring.union(cover))
    assert contour_proposals(required, Polygon(), allowed) == [('remove-hidden', Polygon())]
    repeated = contour_proposals(support, required, allowed)
    assert [(name, shape.wkb) for name, shape in candidates] == [(name, shape.wkb) for name, shape in repeated]
    print('Hidden contour tests passed: retraction, expansion, visible territory, tiny components, '
          'locked silhouette, holes and deterministic proposals.')


if __name__ == '__main__':
    main()
