"""Recover native Bucket curves while retaining source and domain checks."""

import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import numpy as np

from paint_stack import order_stack, polygon_svg
from rate_distortion import window_sums
from simplify import image_error, read_svg, render


def recover(tool, stack, size, domain, source, baseline, output, *, tolerance=1.,
            source_growth=8., local_growth=32., progress=None):
    output = Path(output)
    output.mkdir()
    original = polygon_svg(stack, size)
    input_path = output / 'input.svg'
    input_path.write_bytes(original)
    progress = progress or (lambda event: None)
    if tolerance <= 0:
        return original, stack, {'enabled': False}
    progress({'recovering_bucket_curves': len(stack), 'tolerance': tolerance})
    process = subprocess.run([str(tool), str(input_path), '--recover-fill-curves',
        '--curve-fit-tolerance', str(tolerance), '--output', str(output)], capture_output=True, text=True)
    if process.returncode:
        raise ValueError('Bucket curve recovery failed: ' + process.stderr[-2000:])
    proposed = ET.fromstring((output / 'recovered.svg').read_bytes())
    document = ET.fromstring(original)
    candidates = list(proposed)
    paths = list(document)
    geometries = read_svg((output / 'recovered.svg').read_bytes(), size)
    if len(paths) != len(candidates) or len(paths) != len(geometries):
        raise ValueError('Bucket curve recovery changed the draw item mapping')
    conversion = json.loads((output / 'recovery.json').read_text())
    window = min(16, *size)
    scale = 3 * window ** 2
    reference_error = np.square(baseline[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
    window_limit = np.sqrt(window_sums(reference_error, window) / scale) + local_growth
    strict_window_limit = window_limit.copy()
    initial = render(original, size)
    initial_error = np.square(initial[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
    initial_windows = np.sqrt(window_sums(initial_error, window) / scale)
    window_limit = np.maximum(window_limit, initial_windows)
    global_limit = image_error(source, baseline)['rmse'] + source_growth
    accepted, rejected = [], []
    priority = sorted(range(len(paths)), key=lambda index: (
        conversion['paths'][index]['after_anchors'] - conversion['paths'][index]['before_anchors'], index))
    for rank, index in enumerate(priority, 1):
        if not conversion['paths'][index]['curved_spans']:
            continue
        color, geometry = geometries[index]
        if not geometry.is_valid or geometry.difference(domain).area > 1e-6:
            rejected.append({'path': index, 'reason': 'domain'})
            continue
        document.remove(paths[index])
        document.insert(index, candidates[index])
        data = ET.tostring(document)
        actual = render(data, size)
        error = image_error(source, actual)
        squared = np.square(actual[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
        maximum_growth = float(np.max(np.sqrt(window_sums(squared, window) / scale) - window_limit))
        if error['rmse'] <= global_limit and maximum_growth <= 1e-5 and error['alpha_max_error'] == 0:
            paths[index] = candidates[index]
            accepted.append(index)
        else:
            document.remove(candidates[index])
            document.insert(index, paths[index])
            rejected.append({'path': index, 'reason': 'source error', 'source_error': error,
                             'local_budget_excess': maximum_growth})
        if rank % 16 == 0:
            progress({'bucket_curves_accepted': len(accepted), 'rejected': len(rejected)})
    data = ET.tostring(document)
    (output / 'selected_unordered.svg').write_bytes(data)
    recovered = read_svg(data, size)
    ordered, ordering = order_stack(recovered)
    for path in list(document):
        document.remove(path)
    for index in ordering['order']:
        document.append(paths[index])
    data = ET.tostring(document)
    (output / 'selected.svg').write_bytes(data)
    final = render(data, size)
    final_error = np.square(final[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2)
    final_windows = np.sqrt(window_sums(final_error, window) / scale)
    source_local_passed = bool(np.all(final_windows <= strict_window_limit + 1e-5))
    details = {'enabled': True, 'method': conversion['method'], 'tolerance': tolerance,
        'accepted_paths': accepted, 'rejected_paths': rejected, 'native_conversion': conversion,
        'source_error': image_error(source, final), 'ordering': dict(ordering, accepted=True),
        'initial_provisional_windows': int(np.count_nonzero(initial_windows > strict_window_limit + 1e-5)),
        'vector_source_local_passed': source_local_passed,
        'requires_final_source_validation': not source_local_passed,
        'vector_source_local_excess': float(np.max(final_windows - strict_window_limit))}
    (output / 'selection.json').write_text(json.dumps(details, indent=2))
    return data, ordered, details
