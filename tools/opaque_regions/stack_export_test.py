"""Checks for joint-stack ownership, frozen curves and native opaque export."""

import gzip
import json
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image
from bottom_up import visible_masks
from fair_curves import RingBank
from layer_completion import Completion
from simplify import (freeze_sources, original_encodings, partition_stack, read_svg,
                      render, run, svg_bytes)


def main():
    size = (100, 80)
    white = '<path fill="#ffffff" d="M0 0L100 0L100 80L0 80Z"/>'
    ring = '<path fill="#ff0000" fill-rule="evenodd" d="M10 10L90 10L90 70L10 70ZM30 30L70 30L70 50L30 50Z"/>'
    curved = '<path fill="#0000ff" d="M0 0C20 0 40 0 60 0L60 4L0 4Z"/>'
    document = lambda paths: ('<svg xmlns="http://www.w3.org/2000/svg" width="100" height="80">'
                              + paths + '</svg>').encode()
    data = document(white + ring + curved)
    paints = read_svg(data, size)
    reference, _, _ = visible_masks(paints)
    required, colors, domain, items = partition_stack(paints, reference, 1e-6)
    completion = Completion(required, colors, domain, ring_bank=RingBank())
    original_encodings(data, paints, ring_bank=completion.ring_bank)
    freeze_sources(completion, items, paints, data, {2: 2})
    encoded = svg_bytes(items, completion, size)[0]
    nodes = list(ET.fromstring(encoded))
    assert nodes[-1].attrib['d'] == ET.fromstring(curved).attrib['d']
    assert nodes[-1].attrib['fill-rule'] == 'nonzero'
    changed = list(paints)
    changed[1] = (changed[1][0], changed[1][1].convex_hull)
    try:
        partition_stack(changed, reference, 1e-6)
    except ValueError:
        pass
    else:
        raise AssertionError('Changed visible color ownership must be rejected')
    from shapely.affinity import translate
    shifted = list(paints)
    shifted[1] = shifted[1][0], translate(shifted[1][1], xoff=.1)
    partition_stack(shifted, reference, 1e-6, .25)
    try:
        partition_stack(shifted, reference, 1e-6, .05)
    except ValueError:
        pass
    else:
        raise AssertionError('Ownership motion outside the configured band must be rejected')
    try:
        partition_stack(list(reversed(paints)), reference, 1e-6)
    except ValueError:
        pass
    else:
        raise AssertionError('Incorrect export order must be rejected')
    tool = Path(__file__).resolve().parents[2] / 'build/Release/fls_opaque_regions.exe'
    if not tool.is_file():
        raise RuntimeError('Build the native audit tool with tools/build.ps1')
    with tempfile.TemporaryDirectory(prefix='fls-stack-export-') as temporary:
        root = Path(temporary)
        project = root / 'input'
        project.mkdir()
        baseline = project / 'baseline.svg'
        baseline.write_bytes(data)
        image = render(data, size)
        Image.fromarray(image).save(project / 'source.png')
        np.savez_compressed(project / 'labels.npz', labels=np.zeros(size[::-1], np.int32),
            palette=np.array([[255, 255, 255], [255, 0, 0], [0, 0, 255]], np.uint8))
        options = dict(search='bottom-up', window_start=1, window_stop=2, hole_weight=512., search_states=64)
        result = run(project, baseline, root / 'output', tool, **options)
        assert result['accepted'], result['rejections']
        assert result['bucket_export']['passed'] and result['native_return_code'] == 0
        assert result['before_native_metrics']['holes'] == 1
        assert result['after_native_metrics']['holes'] == 0
        assert result['after_native_metrics']['paths'] == 4
        assert result['baseline_to_optimized']['rmse'] == 0
        assert result['optimized_silhouette_difference_area'] == 0
        plan = json.loads(gzip.decompress((root / 'output/support_plan.json.gz').read_bytes()))
        assert plan['ownership_mode'] == 'partitioned_colors'
        assert plan['frozen_sources'] == {'0': 0, '3': 2}
        assert result['ordering']['stack_search']['promoted_lower_visible_area']['#ffffff'] > 0
        candidate = list(ET.fromstring((root / 'output/candidate.svg').read_bytes()))
        assert candidate[-1].attrib['d'] == ET.fromstring(curved).attrib['d']
        resumed = run(project, baseline, root / 'resumed', tool,
            resume_plan=root / 'output/support_plan.json.gz', **options)
        assert resumed['accepted'] and resumed['ordering']['resumed_stage'] == 'complete'
        assert (root / 'output/optimized.svg').read_bytes() == (root / 'resumed/optimized.svg').read_bytes()
        baseline.write_bytes(document(white))
        Image.fromarray(render(baseline.read_bytes(), size)).save(project / 'source.png')
        noop = run(project, baseline, root / 'noop', tool, search='bottom-up', search_states=8)
        assert not noop['accepted'] and noop['status'] == 'unchanged'
        assert (root / 'noop/optimized.svg').read_bytes() == baseline.read_bytes()
        assert noop['baseline_to_optimized']['rmse'] == 0
        grouped = document(white
            + '<path fill="#0000ff" d="M20 30L30 30L30 40L20 40Z"/>'
            + '<path fill="#0000ff" d="M70 30L80 30L80 40L70 40Z"/>'
            + '<path fill="#ff0000" fill-rule="evenodd" d="M10 10L90 10L90 70L10 70Z'
              'M20 30L30 30L30 40L20 40ZM70 30L80 30L80 40L70 40Z"/>'
            + '<path fill="#00ff00" d="M30 30L70 30L70 40L30 40Z"/>')
        baseline.write_bytes(grouped)
        Image.fromarray(render(grouped, size)).save(project / 'source.png')
        np.savez_compressed(project / 'labels.npz', labels=np.zeros(size[::-1], np.int32),
            palette=np.array([[255, 255, 255], [255, 0, 0], [0, 0, 255], [0, 255, 0]], np.uint8))
        combined = run(project, baseline, root / 'combined', tool, search='bottom-up',
            window_start=3, window_stop=4, search_states=1, hole_weight=512.)
        assert combined['accepted'], combined['rejections']
        assert combined['before_native_metrics']['paths'] == 5
        assert combined['after_native_metrics']['paths'] == combined['after_native_metrics']['fills'] == 4
        assert combined['after_native_metrics']['holes'] == 0
        assert combined['bucket_export']['pen_anchors'] == 16
        assert combined['baseline_to_optimized']['rmse'] == 0
    print('Joint-stack export tests passed: color ownership, frozen curves and fill rules, '
          'opaque hole restoration, native Bucket export, saved partition and deterministic no-op.')


if __name__ == '__main__':
    main()
