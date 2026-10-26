"""Geometry, ownership, ordering and export checks for opaque simplification."""

import gzip
import json
from pathlib import Path
import tempfile

import numpy as np
from PIL import Image
import shapely
from shapely.geometry import Polygon, box

from fair_curves import Encoding, RingBank, encode_geometry, path_counts
from layer_completion import Completion, Paint, topological_order
from simplify import image_error, original_encodings, ownership, parse_path, read_svg, render, run, svg_bytes


def check_stack(completion, items):
    edges = completion.dependencies(items)
    order = topological_order([item.key for item in items], edges)
    assert order is not None
    by_key = {item.key: item for item in items}
    covered = Polygon()
    visible = {}
    for key in reversed(order):
        item = by_key[key]
        visible[item.color] = visible.get(item.color, Polygon()).union(item.support.difference(covered))
        covered = covered.union(item.support)
    for color in set(completion.colors):
        expected = shapely.union_all([shape for shape, fill in zip(completion.required, completion.colors) if fill == color])
        assert visible[color].symmetric_difference(expected).area < 1e-6
    assert completion.domain.symmetric_difference(covered).area < 1e-6


def main():
    rectangle = box(0, 0, 80, 40)
    encoded = encode_geometry(rectangle)
    assert encoded.lines == 4 and encoded.cubics == encoded.holes == 0
    assert encoded.geometry.equals(rectangle)
    assert path_counts('M0 0L80 0L80 40L0 40Z') == (4, 0)
    original_path = 'M0 10C0 4 4 0 10 0C16 0 20 4 20 10C20 16 16 20 10 20C4 20 0 16 0 10Z'
    original_geometry = parse_path(original_path, 'evenodd')
    original_encoding = Encoding(original_path, original_geometry, 1, 0, 0, 4)
    retained = encode_geometry(original_geometry, 0,
        originals={shapely.normalize(original_geometry).wkb: original_encoding})
    assert retained.d == original_path and retained.cubics == 4 and retained.lines == 0
    ring_bank = RingBank()
    ring_bank.add(original_path, original_geometry)
    holed = box(-10, -10, 30, 30).difference(original_geometry)
    retained_hole = encode_geometry(holed, .01, ring_bank=ring_bank)
    assert original_path in retained_hole.d and retained_hole.holes == 1 and retained_hole.cubics == 4
    clipped_curve = original_geometry.intersection(box(-1, -1, 18, 21))
    retained_spans = encode_geometry(clipped_curve, 0, ring_bank=ring_bank)
    assert retained_spans.cubics >= 2 and retained_spans.geometry.is_valid
    assert retained_spans.segments < len(clipped_curve.exterior.coords) - 1
    assert retained_spans.geometry.symmetric_difference(clipped_curve).area < 1e-6
    tiny = box(1500, 800, 1500.0001, 800.0001)
    assert encode_geometry(tiny).geometry.equals(tiny)
    tiny_domain = box(0, 0, 10, 10)
    tiny_owner = box(4, 4, 4.0001, 4.0001)
    tiny_completion = Completion([tiny_domain.difference(tiny_owner), tiny_owner],
                                 ['#ffffff', '#000000'], tiny_domain)
    tiny_items = tiny_completion.split_items([
        Paint(0, '#ffffff', frozenset([0]), tiny_domain),
        Paint(1, '#000000', frozenset([1]), tiny_owner)])
    assert {owner for item in tiny_items for owner in item.owners} == {0, 1}
    x = np.linspace(0, 80, 321)
    wavy = Polygon(np.vstack((np.column_stack((x, .25 * np.sin(x * 2))), [[80, 40], [0, 40]])))
    fair = encode_geometry(wavy, 1.)
    assert fair.segments <= 8 and fair.geometry.is_valid
    assert fair.geometry.boundary.hausdorff_distance(wavy.boundary) <= 1.
    circle = shapely.Point(40, 40).buffer(25, quad_segs=64)
    encoded_circle = encode_geometry(circle, .5)
    assert 0 < encoded_circle.cubics <= 12 and encoded_circle.geometry.is_valid
    assert encoded_circle.geometry.boundary.hausdorff_distance(circle.boundary) <= .5
    narrow = box(0, 0, 100, 100).difference(shapely.Point(50, 50).buffer(49.99, quad_segs=64))
    fitted_narrow = encode_geometry(narrow, 1.)
    assert fitted_narrow.geometry.is_valid and fitted_narrow.holes == 1
    assert fitted_narrow.geometry.difference(narrow.buffer(1.)).area < 1e-6
    two_holes = box(0, 0, 200, 100).difference(shapely.Point(50, 50).buffer(49.9999, quad_segs=64)) \
        .difference(shapely.Point(150, 50).buffer(20, quad_segs=64))
    fitted_holes = encode_geometry(two_holes, 1., bounds=(0., 0., 200., 100.))
    assert fitted_holes.holes == 2 and fitted_holes.cubics > 0 and fitted_holes.segments < 520
    assert box(0, 0, 200, 100).covers(fitted_holes.geometry)
    red, blue, green = '#ff0000', '#0000ff', '#00ff00'
    domain = box(0, 0, 100, 80)
    disk = shapely.Point(50, 40).buffer(15, quad_segs=16)
    left, right = box(0, 0, 50, 80), box(50, 0, 100, 80)
    required = [left.difference(disk), right.difference(disk), disk]
    completion = Completion(required, [red, blue, green], domain)
    items, report = completion.run(2)
    check_stack(completion, items)
    assert sum(len(shape.support.interiors) for shape in items) == 0
    assert len(items) == 3 and report['accepted']
    assert all(item.support.equals(item.support.envelope) for item in items if item.color != green)
    hole = box(20, 20, 40, 40)
    completion = Completion([domain.difference(hole), hole], [red, blue], domain)
    items, report = completion.run(2)
    check_stack(completion, items)
    assert all(not part.interiors for item in items for part in [item.support])
    assert any(item.color == red and item.support.equals(domain) for item in items)
    body, highlight = box(20, 20, 80, 60), box(40, 30, 60, 50)
    highlights = Completion([domain.difference(body), highlight, body.difference(highlight)],
                           ['#ffffff', '#ffffff', red], domain, background='#ffffff', path_weight=64)
    highlighted, _ = highlights.run(2)
    check_stack(highlights, highlighted)
    assert len([item for item in highlighted if item.color == '#ffffff']) == 2
    assert all(not part.interiors for item in highlighted for part in [item.support])
    source_highlights = Completion([domain.difference(body), highlight, body.difference(highlight)],
        ['#ffffff', '#ffffff', red], domain, background='#ffffff', source_supports={red: body.difference(highlight)})
    kept, _ = source_highlights.run(0)
    check_stack(source_highlights, kept)
    assert len(kept) == 2 and sum(len(item.support.interiors) for item in kept) == 1
    charged, _ = source_highlights.run(2)
    check_stack(source_highlights, charged)
    assert len(charged) == 2 and sum(len(item.support.interiors) for item in charged) == 1
    islands = [box(10, 20, 30, 60), box(50, 20, 70, 60)]
    bridge_domain = box(10, 20, 70, 60)
    required = [bridge_domain.difference(shapely.union_all(islands)), *islands]
    completion = Completion(required, [blue, red, red], bridge_domain)
    items, report = completion.run(2)
    check_stack(completion, items)
    assert len([item for item in items if item.color == red]) == 1
    first, second = Paint(0, red, frozenset([0]), domain), Paint(1, blue, frozenset([1]), domain)
    conflict = Completion([left, right], [red, blue], domain)
    assert topological_order([0, 1], conflict.dependencies([first, second])) is None
    assert topological_order([0, 1, 2], {(0, 2), (1, 2)}) == [0, 1, 2]
    assert topological_order([0, 1], {(0, 1), (1, 0)}) is None
    positions = Completion([box(0, 0, 40, 80), box(40, 0, 70, 80), box(70, 0, 100, 80)],
                           [red, blue, red], domain)
    separated = [Paint(0, red, frozenset([0]), box(0, 0, 60, 80)),
                 Paint(1, blue, frozenset([1]), box(40, 0, 80, 80)),
                 Paint(2, red, frozenset([2]), box(70, 0, 100, 80))]
    assert topological_order([0, 1, 2], positions.dependencies(separated)) == [0, 1, 2]
    merged = Paint(0, red, frozenset([0, 2]), separated[0].support.union(separated[2].support))
    assert topological_order([0, 1], positions.dependencies([merged, separated[1]])) is None
    lifted, lifted_report = positions.improve_components(separated, 1, 3)
    check_stack(positions, lifted)
    assert len(lifted) == 2 and any(step['relocated'] for step in lifted_report)
    same_fill = Completion([left, right], [red, red], domain)
    assert same_fill.dependencies([Paint(0, red, frozenset([0]), domain),
                                   Paint(1, red, frozenset([1]), domain)]) == set()
    clipped_domain = box(0, 0, 10, 4)
    clipped_required = box(2, 0, 4, 4)
    mixed_clip = Completion([clipped_required, box(0, 0, 2, 4), box(4, 0, 10, 4)],
                            [red, blue, green], clipped_domain)
    clipped_proposals = mixed_clip.candidates(Paint(0, red, frozenset([0]), box(2, 0, 6, 4)),
                                               box(0, 0, 4, 4).union(box(6, 0, 10, 4)))
    assert clipped_proposals and all(shape.is_valid and clipped_required.difference(shape).area < 1e-6
                                    for _, _, shape in clipped_proposals)
    assert parse_path('M0 0L40 0L40 40L0 40ZM10 10L30 10L30 30L10 30Z', 'evenodd').area == 1200
    assert parse_path('M0 0L40 0L40 40L0 40ZM10 10L30 10L30 30L10 30Z', 'nonzero').area == 1600
    winding_data = b'<svg width="40" height="40"><path fill="#ff0000" fill-rule="nonzero" d="M0 0L40 0L40 40L0 40ZM10 10L30 10L30 30L10 30Z"/></svg>'
    assert not original_encodings(winding_data, read_svg(winding_data, (40, 40)))
    compatible = winding_data.replace(b'M10 10L30 10L30 30L10 30Z', b'M10 10L10 30L30 30L30 10Z')
    assert len(original_encodings(compatible, read_svg(compatible, (40, 40)))) == 1
    for path in ('M0 0L10 0L0 10', 'M0 0L10 0Z M20 20L30 20L20 30', 'Mnan 0L10 0L0 10Z'):
        try:
            parse_path(path, 'evenodd')
        except ValueError:
            pass
        else:
            raise AssertionError('Open or invalid paths must be rejected')
    before = np.zeros((32, 32, 4), dtype=np.uint8)
    before[:, :, 3] = 255
    after = before.copy()
    after[8:24, 8:24, :3] = 255
    assert image_error(before, after)['local_rmse'] == 255
    completion = Completion([domain], [red], domain)
    first, first_report = completion.run(2)
    second, second_report = completion.run(2)
    assert first_report['accepted'] == second_report['accepted'] == []
    assert svg_bytes(first, completion, (100, 80))[0] == svg_bytes(second, completion, (100, 80))[0]
    tool = Path(__file__).resolve().parents[2] / 'build/Release/fls_opaque_regions.exe'
    if tool.is_file():
        with tempfile.TemporaryDirectory(prefix='fls-region-fit-') as temporary:
            root = Path(temporary)
            project = root / 'input'
            project.mkdir()
            labels = np.zeros((80, 100), dtype=np.int32)
            np.savez_compressed(project / 'labels.npz', labels=labels, palette=np.array([[255, 0, 0]], np.uint8))
            rgba = np.zeros((80, 100, 4), dtype=np.uint8)
            rgba[:] = [255, 0, 0, 255]
            Image.fromarray(rgba).save(project / 'source.png')
            baseline = project / 'baseline.svg'
            baseline.write_bytes(svg_bytes(first, completion, (100, 80))[0])
            report = run(project, baseline, root / 'output', tool)
            assert not report['accepted'] and report['status'] == 'unchanged'
            assert (root / 'output/optimized.svg').read_bytes() == baseline.read_bytes()
            assert report['baseline_to_optimized']['rmse'] == 0
            assert report['native_return_code'] == 0 and report['bucket_export']['passed']
            assert report['source_to_optimized']['rmse'] == 0
            assert np.array_equal(render((root / 'output/optimized.svg').read_bytes(), (100, 80)), rgba)
            resumed = run(project, baseline, root / 'resumed', tool,
                          resume_plan=root / 'output/support_plan.json.gz')
            assert resumed['ordering']['resumed_stage'] == 'complete'
            assert (root / 'resumed/optimized.svg').read_bytes() == baseline.read_bytes()
            positioned = ('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
                '<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
                '<path fill="#ff0000" d="M0 0L60 0L60 80L0 80Z"/>'
                '<path fill="#0000ff" d="M40 0L80 0L80 80L40 80Z"/>'
                '<path fill="#ff0000" d="M70 0L100 0L100 80L70 80Z"/></svg>').encode()
            np.savez_compressed(project / 'labels.npz', labels=labels,
                palette=np.array([[255, 255, 255], [255, 0, 0], [0, 0, 255]], np.uint8))
            baseline.write_bytes(positioned)
            Image.fromarray(render(positioned, (100, 80))).save(project / 'source.png')
            retained_order = run(project, baseline, root / 'positioned', tool, passes=0)
            assert retained_order['ownership_mode'] == 'draw_items' and retained_order['ordering']['preserved_input_order']
            assert [item['fill'] for item in retained_order['ordering']['items']] == ['#ffffff', '#ff0000', '#0000ff', '#ff0000']
            assert retained_order['ordering']['items'][0]['owners'] == []
            assert (root / 'positioned/optimized.svg').read_bytes() == positioned
            restored_order = run(project, baseline, root / 'positioned_resumed', tool, passes=0,
                                 resume_plan=root / 'positioned/support_plan.json.gz')
            assert restored_order['ownership_mode'] == 'draw_items'
            assert (root / 'positioned_resumed/optimized.svg').read_bytes() == positioned
            try:
                run(project, baseline, project / 'nested', tool)
            except ValueError:
                pass
            else:
                raise AssertionError('Nested output must not overwrite inputs')
            palette = np.array([[255, 255, 255], [255, 0, 0]], np.uint8)
            np.savez_compressed(project / 'labels.npz', labels=labels, palette=palette)
            top = np.column_stack((np.linspace(20, 80, 121), 20 + .25 * np.sin(np.linspace(20, 80, 121) * 2)))
            outline = 'M20 60' + ''.join(f'L{x:.9g} {y:.9g}' for x, y in top) + 'L80 60Z'
            artwork = ('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
                '<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
                '<path fill="#ff0000" d="' + outline + '"/></svg>').encode()
            baseline.write_bytes(artwork)
            Image.fromarray(render(artwork, (100, 80))).save(project / 'source.png')
            improved = run(project, baseline, root / 'improved', tool, source_growth=4.)
            assert improved['accepted'] and improved['bucket_export']['passed']
            assert Image.open(root / 'improved/difference.png').mode == 'RGB'
            assert improved['after_native_metrics']['svg_segments'] < improved['before_native_metrics']['svg_segments'] / 4
            rejected = run(project, baseline, root / 'rejected', tool, render_rmse=0., local_rmse=0., source_growth=0.)
            assert not rejected['accepted']
            assert (root / 'rejected/optimized.svg').read_bytes() == artwork
            waves = np.column_stack((np.linspace(20, 80, 121),
                20 + 1.5 * np.sin(np.linspace(20, 80, 121) * .5)))
            outline = 'M20 60' + ''.join(f'L{x:.9g} {y:.9g}' for x, y in waves) + 'L80 60Z'
            detailed = ('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
                '<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
                '<path fill="#ff0000" d="' + outline + '"/></svg>').encode()
            baseline.write_bytes(detailed)
            Image.fromarray(render(detailed, (100, 80))).save(project / 'source.png')
            refined = run(project, baseline, root / 'refined', tool, tolerance=2., passes=0,
                          render_rmse=2., source_growth=4.)
            assert refined['baseline_to_candidate']['rmse'] <= 2.
            assert any(trial['boundary_tolerance'] not in (2., 1.5, 1., .5, .25)
                       for trial in refined['fitting_trials'])
            selected = next(trial for trial in refined['fitting_trials']
                            if trial['boundary_tolerance'] == refined['effective_boundary_tolerance'])
            assert not selected['rejections'] and selected['render_error'] == refined['baseline_to_candidate']
    print('Opaque simplification tests passed: straight and wavy contours, bounded curves, hidden completion, '
          'hole filling, fixed ownership, cycles, fill rules, local error, deterministic output and native export.')


if __name__ == '__main__':
    main()
