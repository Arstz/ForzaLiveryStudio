"""Check split-position completion and complete-scene appearance constraints."""

from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

import numpy as np
import shapely
from shapely.geometry import LineString, Polygon, box

from bottom_up import visible_masks
from joint_regions import Paint, Search, order, run
from paint_stack import polygon_svg
from simplify import render


def main():
    tool = Path(sys.argv[1]).resolve()
    domain = box(0, 0, 64, 64)
    lower = box(8, 8, 56, 56)
    ring = Polygon(box(16, 16, 48, 48).exterior, [box(24, 24, 40, 40).exterior.coords])
    paints = [('#ffffff', domain), ('#0000ff', lower), ('#ff0000', ring),
              ('#00ff00', box(28, 24, 40, 40))]
    data = polygon_svg(paints, (64, 64))
    source = render(data, (64, 64))
    with tempfile.TemporaryDirectory(prefix='fls-joint-regions-') as temporary:
        root = Path(temporary)
        search = Search(tool, data, source, source, domain, root / 'transaction',
                        source_growth=0., local_growth=0.)
        required = search.items[1].required
        candidates = list(search.hole_proposals(search.items[2]))
        assert candidates
        selected = min(candidates, key=lambda candidate: -candidate['saving'])
        assert selected['details']['split_items'] == [1]
        assert selected['details']['promoted_items'] == 1
        assert np.array_equal(selected['image'], source)
        blue = [item for item in selected['items'] if item.color == '#0000ff']
        assert len(blue) == 2 and all(item.required.area > 0 for item in blue)
        assert shapely.union_all([item.required for item in blue]).symmetric_difference(required).area <= .0001
        red = next(item for item in selected['items'] if item.color == '#ff0000')
        assert not red.support.interiors and selected['saving'] > 0
        assert selected['items'].index(blue[0]) < selected['items'].index(red) < selected['items'].index(blue[1])
        assert blue[1].support.equals(box(24, 24, 28, 40)) or len(blue[1].support.exterior.coords) <= 5
        completed, stack, report = run(tool, data, source, source, domain, root / 'complete',
            maximum=3, absorb=False, source_growth=0., local_growth=0.)
        assert report['edits'] and all(not part.interiors for _, part in stack)
        assert np.array_equal(render(completed, (64, 64)), source)
        before, _, _ = visible_masks(paints)
        after, _, _ = visible_masks(stack)
        assert all(before[color].symmetric_difference(after[color]).area <= .0001 for color in before)
        repeated, _, repeated_report = run(tool, data, source, source, domain, root / 'repeat',
            maximum=3, absorb=False, source_growth=0., local_growth=0.)
        assert repeated == completed and repeated_report['edits'] == report['edits']
        impossible = [Paint(0, '#ff0000', domain, box(0, 0, 32, 64)),
                      Paint(1, '#0000ff', domain, box(32, 0, 64, 64))]
        assert order(impossible, domain)[1]['reason'] == 'dependency_cycle'
        impossible[1] = Paint(1, '#0000ff', box(32, 0, 64, 64), box(32, 0, 64, 64))
        assert order(impossible, domain)[0] is not None
        impossible[0] = Paint(0, '#ff0000', box(-1, 0, 64, 64), box(0, 0, 32, 64))
        assert order(impossible, domain)[1]['reason'] == 'geometry'
        hidden = paints[:-1] + [('#00ff00', box(36, 24, 56, 40))]
        hidden_data = polygon_svg(hidden, (64, 64))
        hidden_source = render(hidden_data, (64, 64))
        opened, opened_stack, opening = run(tool, hidden_data, hidden_source, hidden_source, domain,
            root / 'hidden_opening', maximum=1, absorb=False, source_growth=0., local_growth=0.)
        assert len(opened_stack) == len(hidden) and all(not shape.interiors for _, shape in opened_stack)
        assert np.array_equal(render(opened, (64, 64)), hidden_source)
        assert any(row['kind'] == 'open-hole-through-hidden-coverage' for row in opening['edits'])
        curved = b'<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">' \
            b'<path fill="#ffffff" d="M0 0L64 0L64 64L0 64Z"/>' \
            b'<path fill="#0000ff" d="M8 8L56 8L56 56L8 56Z"/>' \
            b'<path fill="#ff0000" fill-rule="evenodd" d="M48 32C48 40.83656 40.83656 48 32 48' \
            b'C23.16344 48 16 40.83656 16 32C16 23.16344 23.16344 16 32 16' \
            b'C40.83656 16 48 23.16344 48 32ZM24 24L40 24L40 40L24 40Z"/></svg>'
        curved_source = render(curved, (64, 64))
        preserved, _, curve_report = run(tool, curved, curved_source, curved_source, domain,
            root / 'curved_silhouette', maximum=1, absorb=False, source_growth=0., local_growth=0.)
        assert curve_report['edits'] and b'C48 40.83656 40.83656 48 32 48' in preserved
        assert np.array_equal(render(preserved, (64, 64)), curved_source)
        simple = polygon_svg([('#ffffff', domain)], (64, 64))
        white = render(simple, (64, 64))
        first, _, one = run(tool, simple, white, white, domain, root / 'noop1', maximum=2)
        second, _, two = run(tool, simple, white, white, domain, root / 'noop2', maximum=2)
        assert first == second and not one['edits'] and not two['edits']
        colored = [('#0000ff', domain), ('#ff0000',
            shapely.union_all([box(4, 4, 16, 16), box(40, 40, 42, 42)]))]
        noisy = polygon_svg(colored, (64, 64))
        pixels = render(noisy, (64, 64))
        absorbed, result, check = run(tool, noisy, pixels, pixels, domain, root / 'any_color',
            maximum=0, source_growth=8., local_growth=48.)
        assert check['edits'] and any('absorption' in row['kind'] for row in check['edits'])
        assert not any(color == '#ff0000' and shape.contains(box(40.1, 40.1, 41.9, 41.9)) for color, shape in result)
        guarded, _, guarded_report = run(tool, noisy, pixels, pixels, domain, root / 'guarded',
            maximum=0, source_growth=0., local_growth=0.)
        assert not guarded_report['edits'] and np.array_equal(render(guarded, (64, 64)), pixels)
        islands = polygon_svg([('#0000ff', domain), ('#ff0000', box(4, 4, 16, 16)),
                               ('#ff0000', box(40, 40, 42, 42)), ('#ff0000', box(50, 50, 52, 52))], (64, 64))
        pixels = render(islands, (64, 64))
        replay = Search(tool, islands, pixels, pixels, domain, root / 'independent_replay',
                        source_growth=16., local_growth=48.)
        assess = replay.assess
        def reject_batch(items, details, **settings):
            if len(details.get('edits', [])) > 1:
                replay.rejected.append(dict(details, reason='source_error'))
                return None
            return assess(items, details, **settings)
        with patch.object(replay, 'assess', side_effect=reject_batch):
            replay.absorb()
        assert len(replay.history) >= 2 and all(row['replayed_independently'] for row in replay.history)
        assert len(replay.items) == 2
        measured, measured_stack, measured_report = run(tool, data, source, source, domain,
            root / 'native_cost_guard', maximum=2, absorb=False, source_growth=0., local_growth=0.,
            native_score=lambda encoded, details: {'accepted': True, 'region_count': 4,
                'shape_count': 8 if details['kind'] == 'unchanged-joint-input' else 100})
        assert not measured_report['edits'] and len(measured_stack) == len(paints)
        assert np.array_equal(render(measured, (64, 64)), source)
        assert any(row['reason'] == 'native_encoded_cost' for row in measured_report['rejected'])
        simpler, _, simpler_report = run(tool, data, source, source, domain,
            root / 'native_topology_tradeoff', maximum=1, absorb=False, source_growth=0., local_growth=0.,
            native_score=lambda encoded, details: {'accepted': True, 'region_count': 4,
                'shape_count': 8 if details['kind'] == 'unchanged-joint-input' else 11,
                'encoding_cost': 500 if details['kind'] == 'unchanged-joint-input' else 100})
        assert simpler_report['edits'] and simpler_report['native_measurement']['shape_count'] == 11
        assert np.array_equal(render(simpler, (64, 64)), source)
        decoder = Search(tool, data, source, source, domain, root / 'invalid_native_candidate')
        valid = [[0., 0.], [10., 0.], [10., 10.], [0., 10.]]
        invalid = list(reversed(valid))
        response = {'regions': [{'key': 'input', 'candidates': [{'rings': [invalid]}, {'rings': [valid]}]}]}
        with patch('joint_regions.simplify_batch', return_value=response):
            decoded = decoder.simplify([{'key': 'input', 'required': [], 'allowed': []}])
        assert len(decoded['input']) == 1 and decoded['input'][0].area == 100.
        assert any(row['reason'] == 'invalid_native_geometry' for row in decoder.rejected)
        bridged = polygon_svg([('#ffffff', domain), ('#0000ff', box(8, 16, 24, 40)),
            ('#0000ff', box(40, 16, 56, 40)), ('#ff0000', box(24, 8, 40, 48))], (64, 64))
        pixels = render(bridged, (64, 64))
        grouping = Search(tool, bridged, pixels, pixels, domain, root / 'competing_group',
            source_growth=0., local_growth=0., shortlist=1, native_score=lambda encoded, details: {
                'accepted': details['kind'] != 'unhelpful-contour', 'region_count': 3,
                'shape_count': 4 if details['kind'] == 'unchanged-joint-input' else 3})
        groups = grouping.group()
        assert groups and any(len(proposal['items']) == 3 for proposal in groups)
        distraction = dict(groups[0], saving=groups[0]['saving'] + 1,
                           details={'kind': 'unhelpful-contour', 'item': 1})
        with patch.object(grouping, 'reassign_components', return_value=[]), \
                patch.object(grouping, 'contours', return_value=[distraction]), \
                patch.object(grouping, 'hole_proposals', return_value=[]):
            grouped, grouped_report = grouping.run(maximum=1, absorb=False)
        assert grouped_report['edits'][0]['kind'] == 'group-fixed-fragments'
        assert np.array_equal(render(grouped, (64, 64)), pixels)
        large = box(0, 0, 128, 64)
        branch = LineString([(20, 16), (60, 16), (60, 44), (104, 44)]).buffer(
            6, cap_style='flat', join_style='mitre')
        support = shapely.union_all([box(8, 8, 24, 24), branch])
        occluder = box(24, 4, 108, 58).difference(box(102, 43, 108, 45))
        visible = polygon_svg([('#ffffff', large), ('#c08080', support), ('#8080c0', occluder),
                               ('#80c080', box(104, 40, 120, 50))], (128, 64))
        pixels = render(visible, (128, 64))
        release = Search(tool, visible, pixels, pixels, large, root / 'released_branch',
                         source_growth=8., local_growth=32.)
        proposals = release.reassign_components()
        assert proposals and any(candidate['details']['reassigned_area'] >= 4 for candidate in proposals)
        assert any(len(next(item for item in candidate['items'] if item.key == 1).support.exterior.coords) == 5
                   for candidate in proposals)
        slit = LineString([(39, 32), *[(40 + index * .5, 30 if index % 2 else 34)
                           for index in range(26)], (53, 32)]).buffer(.1, join_style='mitre', cap_style='square')
        support = box(12, 12, 52, 52).difference(shapely.union_all([box(24, 24, 40, 40), slit]))
        assert not support.interiors
        exposed = polygon_svg([('#ffffff', domain), ('#0000ff', box(8, 8, 56, 56)),
                               ('#ff0000', support), ('#00ff00', box(40, 28, 56, 36))], (64, 64))
        pixels = render(exposed, (64, 64))
        broad = Search(tool, exposed, pixels, pixels, domain, root / 'simple_support_completion',
                       source_growth=0., local_growth=0.)
        completed = list(broad.hole_proposals(broad.items[2], broad=True))
        assert completed and any('complete-hull' in proposal['details']['kind'] for proposal in completed)
        assert all(np.array_equal(proposal['image'], pixels) for proposal in completed)
    print('Joint regions passed: cyclic hole completion, split ownership, simple promoted supports, '
          'any-color absorption, source budgets, silhouette, cycle diagnostics and determinism.')


if __name__ == '__main__':
    main()
