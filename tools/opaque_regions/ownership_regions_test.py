"""Check solid connected-owner reconstruction and dependency-based contour completion."""

from pathlib import Path
import json
import tempfile

import numpy as np
import shapely
from shapely.geometry import Point, Polygon, box

from ownership_regions import rebuild
from paint_stack import geometry_metrics, polygon_svg
from simplify import render


def main():
    size = (64, 64)
    domain = box(0, 0, *size)
    with tempfile.TemporaryDirectory(prefix='fls-ownership-regions-') as temporary:
        root = Path(temporary)
        green = Point(32, 32).buffer(12, quad_segs=16)
        stack = [('#ff0000', box(0, 0, 32, 64)), ('#0000ff', box(32, 0, 64, 64)), ('#00ff00', green)]
        source = render(polygon_svg(stack, size), size)
        result, report = rebuild(stack, source, source, domain, root / 'circle', source_growth=0., local_growth=0.,
                                 progress=lambda event: json.dumps(event))
        assert report['accepted'], report
        assert np.array_equal(render(polygon_svg(result, size), size), source)
        for color, shape in result:
            assert not any(part.interiors for part in ([shape] if shape.geom_type == 'Polygon' else shape.geoms))
            if color in ('#ff0000', '#0000ff'):
                assert geometry_metrics(shape)['points'] == 4
        red_outer = Polygon(box(4, 4, 60, 60).exterior, [box(16, 16, 48, 48).exterior.coords])
        nested = [('#ffffff', domain), ('#ff0000', red_outer),
                  ('#0000ff', box(16, 16, 48, 48)), ('#ff0000', box(24, 24, 40, 40))]
        image = render(polygon_svg(nested, size), size)
        result, report = rebuild(nested, image, image, domain, root / 'nested', source_growth=0., local_growth=0.)
        assert report['accepted'] and report['geometry']['holes'] == 0
        assert sum(color == '#ff0000' for color, _ in result) == 2
        assert np.array_equal(render(polygon_svg(result, size), size), image)
        again, check = rebuild(nested, image, image, domain, root / 'repeat', source_growth=0., local_growth=0.)
        assert [(color, shape.wkb) for color, shape in result] == [(color, shape.wkb) for color, shape in again]
        ring = Polygon(domain.exterior, [box(20, 20, 44, 44).exterior.coords])
        masked = [('#ff0000', ring)]
        image = render(polygon_svg(masked, size), size)
        result, report = rebuild(masked, image, image, ring, root / 'domain', source_growth=0., local_growth=0.)
        assert report['accepted'] and shapely.union_all([shape for _, shape in result]).equals(ring)
        tiny = [('#0000ff', domain), ('#000000', box(20, 20, 21, 21))]
        pixels = render(polygon_svg(tiny, size), size)
        retained, strict = rebuild(tiny, pixels, pixels, domain, root / 'tiny_strict',
            source_growth=0., local_growth=0.)
        assert strict['accepted'] and any(color == '#000000' for color, _ in retained)
        absorbed, relaxed = rebuild(tiny, pixels, pixels, domain, root / 'tiny_absorbed',
            source_growth=5., local_growth=20.)
        assert relaxed['accepted'] and not any(color == '#000000' for color, _ in absorbed)
        assert relaxed['island_transfers'][0]['color_distance'] > 48
    print('Ownership reconstruction passed: simple lower rectangles, solid nested regions, '
          'separate same-color positions, locked silhouette and deterministic output.')


if __name__ == '__main__':
    main()
