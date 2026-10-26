"""Complete hidden regions and fit compact contours in a validated opaque SVG."""

import argparse
from dataclasses import replace
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
import cv2
import numpy as np
from PIL import Image
from PySide6.QtCore import QByteArray, QPointF, QRectF, Qt, qVersion
from PySide6.QtGui import QGuiApplication, QImage, QPainter, QPainterPath
from PySide6.QtSvg import QSvgRenderer
import shapely
from shapely.geometry import LineString, Polygon, box
from shapely.ops import polygonize

from fair_curves import Encoding, RingBank, boundary_measurements, flatten, path_counts, polygons
from layer_completion import Completion, Paint, topological_order


APPLICATION = None


def application():
    global APPLICATION
    APPLICATION = QGuiApplication.instance() or APPLICATION or QGuiApplication([])
    return APPLICATION


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_path(data, rule, flatten_tolerance=.05):
    tokens = re.findall(r'[MLCZ]|[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?', data)
    if re.sub(r'[\s,]', '', ''.join(tokens)) != re.sub(r'[\s,]', '', data):
        raise ValueError('Use canonical closed M/L/C/Z paths from the native exporter')
    path, rings, ring, current, first, position = QPainterPath(), [], [], None, None, 0
    path.setFillRule(Qt.FillRule.OddEvenFill if rule == 'evenodd' else Qt.FillRule.WindingFill)
    while position < len(tokens):
        command = tokens[position]
        count = {'M': 2, 'L': 2, 'C': 6, 'Z': 0}.get(command)
        if count is None or position + count >= len(tokens) and count:
            raise ValueError('Malformed path command')
        values = np.array(list(map(float, tokens[position + 1:position + count + 1])))
        if not np.all(np.isfinite(values)):
            raise ValueError('Non-finite path coordinates')
        position += count + 1
        if command == 'M':
            if ring:
                raise ValueError('Open SVG subpath')
            current = first = values.copy()
            ring = [current]
            path.moveTo(*current)
        elif command in ('L', 'C'):
            if current is None:
                raise ValueError('A path must start with M')
            curve = np.vstack((current, values.reshape(-1, 2)))
            ring.extend(flatten(curve, flatten_tolerance)[1:])
            current = curve[-1]
            if command == 'L':
                path.lineTo(*values)
            else:
                path.cubicTo(*values)
        else:
            if current is None or len(ring) < 3:
                raise ValueError('Empty or malformed closed subpath')
            ring.append(first)
            rings.append(LineString(ring))
            ring, current = [], None
            path.closeSubpath()
    if ring or not rings:
        raise ValueError('Every SVG subpath must be closed')
    faces = list(polygonize(shapely.node(shapely.MultiLineString(rings))))
    selected = [face for face in faces if path.contains(QPointF(*face.representative_point().coords[0]))]
    geometry = shapely.union_all(selected)
    if geometry.is_empty or not geometry.is_valid:
        raise ValueError('The path has no valid complete filled geometry')
    return geometry


def read_svg(data, size, flatten_tolerance=.05):
    svg = ET.fromstring(data)
    if any(float(svg.attrib.get(key, 'nan')) != value for key, value in zip(('width', 'height'), size)):
        raise ValueError('SVG dimensions must match the original native pixel grid')
    viewbox = list(map(float, svg.attrib.get('viewBox', f'0 0 {size[0]} {size[1]}').replace(',', ' ').split()))
    if viewbox != [0, 0, *size]:
        raise ValueError('Use native SVG coordinates without a viewBox transform')
    result = []
    for element in svg.iter():
        tag = element.tag.rsplit('}', 1)[-1]
        if tag not in ('svg', 'path') or tag == 'svg' and element is not svg \
                or any(key in element.attrib for key in ('transform', 'style', 'clip-path', 'mask', 'filter')) \
                or element.attrib.get('stroke', 'none') != 'none' \
                or any(float(element.attrib.get(key, '1')) != 1 for key in ('opacity', 'fill-opacity')):
            raise ValueError('Use a flat validated SVG with fully opaque solid fills')
        if tag == 'path':
            color = element.attrib.get('fill', '').lower()
            if not re.fullmatch(r'#[0-9a-f]{6}', color):
                raise ValueError('Every path must have a solid RGB fill')
            rule = element.attrib.get('fill-rule', 'nonzero')
            if rule not in ('evenodd', 'nonzero'):
                raise ValueError('Unsupported SVG fill rule')
            result.append((color, parse_path(element.attrib.get('d', ''), rule, flatten_tolerance)))
    if not result:
        raise ValueError('The SVG has no paint items')
    return result


def ownership(paints, size, draw_items=None):
    runs = []
    for color, geometry in paints:
        geometry = geometry.intersection(box(0, 0, *size))
        if draw_items is None and runs and runs[-1][0] == color:
            runs[-1][1].append(geometry)
        else:
            runs.append((color, [geometry]))
    covered, required, colors = Polygon(), [], []
    for key in reversed(range(len(runs))):
        color, geometries = runs[key]
        support = shapely.union_all(geometries)
        visible = support.difference(covered)
        first_owner = len(required)
        for part in sorted(polygons(visible), key=lambda shape: (*shape.bounds, shape.area)):
            if part.area > 1e-9:
                required.append(part)
                colors.append(color)
        if draw_items is not None:
            draw_items.append(Paint(key, color, frozenset(range(first_owner, len(required))), support))
        covered = covered.union(support)
    if draw_items is not None:
        draw_items.reverse()
    return required, colors, covered


def preserve_order(items, completion):
    edges = completion.dependencies(items)
    order = [item.key for item in items]
    positions = {key: index for index, key in enumerate(order)}
    if any(positions[first] >= positions[last] for first, last in edges):
        raise ValueError('The input draw order contradicts its visible ownership')
    return items, {'accepted': [], 'rejections': [], 'order': order, 'ordering_edges': sorted(edges),
        'preserved_input_order': True, 'fixed_original_ownership': True,
        'appearance_groups': len({item.color for item in items}), 'draw_items': len(items),
        'items': [{'key': item.key, 'fill': item.color, 'owners': sorted(item.owners)} for item in items]}


def svg_bytes(items, completion, size):
    output = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{size[0]}" height="{size[1]}" viewBox="0 0 {size[0]} {size[1]}">']
    geometries, totals = [], dict.fromkeys(('paths', 'components', 'holes', 'segments', 'lines', 'cubics', 'bezier_controls', 'cost'), 0)
    for index, item in enumerate(items):
        fixed = getattr(completion, 'fixed_items', {}).get(item.key)
        encoded, rule = fixed if fixed else (completion.encoding(item.support), 'evenodd')
        if not fixed and encoded.geometry.difference(completion.domain).area > 1e-9:
            from fair_curves import encode_geometry
            encoded = replace(encode_geometry(item.support, 0, completion.fairness, completion.flatten_tolerance,
                completion.bounds), weights=completion.weights)
        output.append(f'<path id="region_{index}" fill="{item.color}" fill-rule="{rule}" d="{encoded.d}"/>')
        geometries.append(encoded.geometry)
        for key, value in encoded.metrics().items():
            totals[key] += value
    output.append('</svg>')
    return ''.join(output).encode(), shapely.union_all(geometries), totals


def original_encodings(data, paints, flatten_tolerance=.05, area_tolerance=1e-6, ring_bank=None):
    originals = {}
    nodes = [element for element in ET.fromstring(data).iter() if element.tag.rsplit('}', 1)[-1] == 'path']
    for element, (_, geometry) in zip(nodes, paints):
        if geometry.geom_type != 'Polygon':
            continue
        path = element.attrib['d']
        if element.attrib.get('fill-rule', 'nonzero') != 'evenodd':
            try:
                evenodd = parse_path(path, 'evenodd', flatten_tolerance)
            except ValueError:
                continue
            if shapely.symmetric_difference(evenodd, geometry, grid_size=1e-8).area > area_tolerance:
                continue
        originals[shapely.normalize(geometry).wkb] = Encoding(path, geometry, 1, len(geometry.interiors),
            *path_counts(path))
        if ring_bank is not None:
            for contour in re.findall(r'M[^M]+', path):
                try:
                    ring = parse_path(contour, 'evenodd', flatten_tolerance)
                except ValueError:
                    continue
                if ring.geom_type == 'Polygon' and not ring.interiors:
                    ring_bank.add(contour, ring)
    return originals


def render(data, size):
    application()
    renderer = QSvgRenderer(QByteArray(data))
    if not renderer.isValid():
        raise ValueError('The final-output SVG renderer rejected the document')
    image = QImage(*size, QImage.Format.Format_RGBA8888)
    image.fill(Qt.GlobalColor.transparent)
    painter = QPainter(image)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    renderer.render(painter, QRectF(0, 0, *size))
    painter.end()
    return np.frombuffer(image.constBits(), np.uint8).reshape(size[1], image.bytesPerLine())[:, :size[0] * 4].reshape(size[1], size[0], 4).copy()


def image_error(before, after, window=16):
    delta = after.astype(np.float64) - before
    squared = np.mean(delta[:, :, :3] ** 2, axis=2)
    height, width = squared.shape
    window = min(window, height, width)
    summed = cv2.integral(squared)
    local = summed[window:, window:] - summed[:-window, window:] - summed[window:, :-window] + summed[:-window, :-window]
    return {'rmse': float(np.sqrt(squared.mean())), 'max_channel_error': float(np.max(np.abs(delta[:, :, :3]))),
            'local_rmse': float(np.sqrt(local.max(initial=0)) / window), 'local_window': window,
            'alpha_rmse': float(np.sqrt(np.mean(delta[:, :, 3] ** 2))),
            'alpha_max_error': float(np.max(np.abs(delta[:, :, 3])))}


def audit_native(tool, svg, reference, original, directory, flatten_tolerance=.05, area_tolerance=1e-6,
                 path_weight=256, hole_weight=96):
    directory.mkdir(parents=True, exist_ok=True)
    arguments = [str(tool), str(svg), '--reference', str(reference), '--original', str(original),
        '--output', str(directory), '--passes', '0', '--conversion-rmse', '.5', '--conversion-max-error', '16',
        '--weight-paths', str(path_weight), '--weight-holes', str(hole_weight), '--flatten-tolerance', str(flatten_tolerance),
        '--area-tolerance', str(area_tolerance)]
    with (directory / 'stdout.json').open('w') as stdout, (directory / 'stderr.log').open('w') as stderr:
        process = subprocess.run(arguments, stdout=stdout, stderr=stderr)
    report = json.loads((directory / 'report.json').read_text()) if (directory / 'report.json').is_file() else None
    return process.returncode, report, arguments


def ownership_signature(completion):
    digest = hashlib.sha256()
    for color, geometry in zip(completion.colors, completion.required):
        digest.update(color.encode())
        digest.update(geometry.wkb)
    return digest.hexdigest()


def write_plan(path, items, completion, baseline_sha, stage, accepted):
    data = {'version': 1, 'input_svg_sha256': baseline_sha,
        'ownership_sha256': ownership_signature(completion), 'stage': stage, 'accepted': accepted,
        'ownership_mode': getattr(completion, 'ownership_mode', 'grouped_runs'),
        'items': [{'key': item.key, 'color': item.color, 'owners': sorted(item.owners),
                   'geometry_wkb': item.support.wkb.hex()} for item in items]}
    if data['ownership_mode'] == 'partitioned_colors':
        data['ownership_tolerance'] = getattr(completion, 'ownership_tolerance', 0.)
        data['ownership_regions'] = [{'color': color, 'geometry_wkb': geometry.wkb.hex()}
                                    for color, geometry in zip(completion.colors, completion.required)]
        data['frozen_sources'] = getattr(completion, 'frozen_sources', {})
        data['stack_search'] = getattr(completion, 'stack_search', None)
        data['stack_details'] = getattr(completion, 'stack_details', None)
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_bytes(gzip.compress(json.dumps(data).encode(), mtime=0))
    temporary.replace(path)


def read_plan(path, completion, baseline_sha, passes=0):
    data = json.loads(gzip.decompress(path.read_bytes()))
    if data.get('version') != 1 or data.get('input_svg_sha256') != baseline_sha \
            or data.get('ownership_sha256') != ownership_signature(completion):
        raise ValueError('The saved support plan does not match the current baseline ownership')
    items = [Paint(row['key'], row['color'], frozenset(row['owners']),
                   shapely.from_wkb(bytes.fromhex(row['geometry_wkb']))) for row in data['items']]
    if len({item.key for item in items}) != len(items) or any(item.owners and
            {completion.colors[owner] for owner in item.owners} != {item.color} for item in items):
        raise ValueError('The saved support plan has invalid draw positions or colors')
    edges = completion.dependencies(items)
    order = topological_order([item.key for item in items], edges)
    if order is None:
        raise ValueError('The saved support plan contains an ordering cycle')
    by_key = {item.key: item for item in items}
    if data['stage'] == 'global':
        result, report = completion.finish([by_key[key] for key in order], passes, data['accepted'])
        report['resumed_stage'] = data['stage']
        return result, report
    return [by_key[key] for key in order], {'resumed_stage': data['stage'], 'accepted': data['accepted'],
        'rejections': [], 'order': order, 'ordering_edges': sorted(edges), 'fixed_original_ownership': True,
        'appearance_groups': len({item.color for item in items}), 'draw_items': len(items),
        'items': [{'key': key, 'fill': by_key[key].color, 'owners': sorted(by_key[key].owners)} for key in order]}


def partition_stack(paints, reference, area_tolerance, ownership_tolerance=0.):
    from bottom_up import visible_masks
    visible, owned, domain = visible_masks(paints)
    for color in set(reference) | set(visible):
        expected = reference.get(color, Polygon())
        difference = shapely.symmetric_difference(expected, visible.get(color, Polygon()), grid_size=1e-8)
        if ownership_tolerance:
            difference = difference.difference(expected.boundary.buffer(ownership_tolerance))
        difference = difference.area
        if difference > area_tolerance:
            raise ValueError('The new stack changed the fixed visible color ownership')
    colors = [color for color, _ in paints]
    items = [Paint(index, color, frozenset([index]), support)
             for index, (color, support) in enumerate(paints)]
    return owned, colors, domain, items


def freeze_sources(completion, items, source_paints, data, mapping):
    nodes = [element for element in ET.fromstring(data).iter() if element.tag.rsplit('}', 1)[-1] == 'path']
    by_key = {item.key: item for item in items}
    completion.frozen_sources, completion.fixed_items = {}, {}
    for key, source in mapping.items():
        key, source = int(key), int(source)
        if key not in by_key or not 0 <= source < len(source_paints):
            raise ValueError('Invalid preserved source path')
        item, (color, geometry), node = by_key[key], source_paints[source], nodes[source]
        if item.color != color or not item.support.equals(geometry):
            raise ValueError('A preserved source path changed its fill or geometry')
        parts = polygons(geometry)
        encoded = Encoding(node.attrib['d'], geometry, len(parts), sum(len(part.interiors) for part in parts),
                           *path_counts(node.attrib['d']), weights=completion.weights)
        completion.frozen_sources[key] = source
        completion.fixed_items[key] = (encoded, node.attrib.get('fill-rule', 'nonzero'))


def paint_topology(metrics):
    return {'colors': metrics['fills'], 'regions': metrics['components'],
            'paths': metrics['paths'], 'holes': metrics['holes']}


def run(project, svg, output, native_tool, tolerance=1., fairness=1., passes=2, render_rmse=4.,
        local_rmse=24., max_error=255., source_growth=.25, silhouette_tolerance=.001, progress=None,
        flatten_tolerance=.05, area_tolerance=1e-6, resume_plan=None, path_weight=256, hole_weight=96,
        search='selective', window_start=0, window_stop=None, beam_width=4, search_states=96, angle_weight=4.):
    started = time.perf_counter()
    progress = progress or (lambda entry: None)
    project, svg, output, native_tool = map(lambda path: Path(path).resolve(), (project, svg, output, native_tool))
    if output == project or project in output.parents or output in project.parents or svg.is_relative_to(output):
        raise ValueError('Use a separate, non-nested output directory to preserve the project and baseline')
    if resume_plan is not None and Path(resume_plan).resolve().is_relative_to(output):
        raise ValueError('Use a separate output directory to preserve the saved support plan')
    values = (tolerance, fairness, render_rmse, local_rmse, max_error, source_growth, silhouette_tolerance,
              flatten_tolerance, area_tolerance, path_weight, hole_weight, angle_weight)
    if not all(np.isfinite(value) and value >= 0 for value in values) or tolerance <= 0 or passes < 0 \
            or flatten_tolerance <= 0:
        raise ValueError('Invalid fitting or validation tolerances')
    if not native_tool.is_file():
        raise ValueError('Build the native audit tool with tools/build.ps1 before running this stage')
    rgba = np.asarray(Image.open(project / 'source.png').convert('RGBA'))
    if np.any((rgba[:, :, 3] != 0) & (rgba[:, :, 3] != 255)):
        raise ValueError('Use an opaque source or a binary transparent domain')
    size = (rgba.shape[1], rgba.shape[0])
    data = svg.read_bytes()
    paints = read_svg(data, size, flatten_tolerance)
    saved_plan = json.loads(gzip.decompress(Path(resume_plan).read_bytes())) if resume_plan is not None else None
    ownership_mode = saved_plan.get('ownership_mode', 'grouped_runs') if saved_plan else (
        'partitioned_colors' if search == 'bottom-up' else 'draw_items' if passes == 0 else 'grouped_runs')
    if search not in ('selective', 'bottom-up') or ownership_mode not in ('grouped_runs', 'draw_items', 'partitioned_colors'):
        raise ValueError('Unsupported saved ownership mode')
    input_items = [] if ownership_mode == 'draw_items' else None
    required, colors, domain = ownership(paints, size, input_items)
    with np.load(project / 'labels.npz') as archive:
        palette = archive['palette']
        if archive['labels'].shape != rgba.shape[:2]:
            raise ValueError('Project labels must retain the native dimensions')
    palette_colors = {'#' + ''.join(f'{int(value):02x}' for value in color) for color in palette}
    if not {color for color, _ in paints} <= palette_colors:
        raise ValueError('The baseline SVG must use the project palette')
    output.mkdir(parents=True, exist_ok=True)
    baseline = render(data, size)
    (output / 'baseline.svg').write_bytes(data)
    Image.fromarray(baseline).save(output / 'before.png')
    progress({'baseline_native_audit': 'started'})
    baseline_exit, baseline_audit, baseline_arguments = audit_native(native_tool, output / 'baseline.svg',
        output / 'before.png', project / 'source.png', output / 'baseline_native', flatten_tolerance, area_tolerance,
        path_weight, hole_weight)
    if baseline_exit or not baseline_audit or not baseline_audit.get('bucket_input', {}).get('passed'):
        raise ValueError('The input must pass the native opaque baseline and Bucket audit')
    progress({'loaded_paths': len(paints), 'fixed_ownership_regions': len(required), 'colors': len(set(colors))})
    stack_search, stack_details, frozen = None, None, {}
    if ownership_mode == 'partitioned_colors':
        from bottom_up import StackSearch, visible_masks
        reference, _, _ = visible_masks(paints)
        if saved_plan:
            planned_paints = [(row['color'], shapely.from_wkb(bytes.fromhex(row['geometry_wkb'])))
                              for row in saved_plan['items']]
            required, colors, _, input_items = partition_stack(planned_paints, reference, area_tolerance,
                saved_plan.get('ownership_tolerance', 0.))
            serialized = saved_plan.get('ownership_regions', [])
            if len(serialized) != len(required):
                raise ValueError('The saved stack has an invalid visible ownership partition')
            for row, shape, color in zip(serialized, required, colors):
                stored = shapely.from_wkb(bytes.fromhex(row['geometry_wkb']))
                if row['color'] != color or shapely.symmetric_difference(stored, shape, grid_size=1e-8).area > area_tolerance:
                    raise ValueError('The saved stack changed its visible ownership partition')
            required = [shapely.from_wkb(bytes.fromhex(row['geometry_wkb'])) for row in serialized]
            frozen = saved_plan.get('frozen_sources', {})
            stack_search = saved_plan.get('stack_search')
            stack_details = saved_plan.get('stack_details')
        else:
            solver = StackSearch(paints, window_start, window_stop, beam_width, search_states,
                tolerance, area_tolerance, path_weight, hole_weight, angle_weight, progress)
            proposals, stack_search = solver.run()
            planned_paints = paints
            frozen = dict(enumerate(range(len(paints))))
            if proposals:
                planned_paints, stack_details = proposals[0]
                new_count = len(planned_paints) - window_start - len(paints) + solver.stop
                frozen = {index: index for index in range(window_start)}
                frozen.update({window_start + new_count + index - solver.stop: index
                               for index in range(solver.stop, len(paints))})
                frozen = stack_details.get('frozen_sources', frozen)
            required, colors, _, input_items = partition_stack(planned_paints, reference, area_tolerance)
    background = paints[0][0] if paints[0][1].symmetric_difference(domain).area <= silhouette_tolerance else None
    source_supports = {color: shapely.union_all([geometry for fill, geometry in paints if fill == color])
                       for color in set(colors) if color != background}
    ring_bank = RingBank(flatten_tolerance)
    originals = original_encodings(data, paints, flatten_tolerance, area_tolerance, ring_bank)
    completion = Completion(required, colors, domain, tolerance, fairness, area_tolerance,
                            progress=progress, background=background, flatten_tolerance=flatten_tolerance,
                            source_supports=source_supports, original_encodings=originals,
                            path_weight=path_weight, hole_weight=hole_weight, ring_bank=ring_bank)
    completion.ownership_mode = ownership_mode
    completion.stack_search = stack_search
    completion.stack_details = stack_details
    completion.ownership_tolerance = saved_plan.get('ownership_tolerance', 0.) if saved_plan else 0.
    if ownership_mode == 'partitioned_colors':
        freeze_sources(completion, input_items, paints, data, frozen)
    completion.on_checkpoint = lambda items, stage, accepted: write_plan(
        output / ('support_plan_' + stage + '.json.gz'), items, completion, digest(svg), stage, accepted)
    if resume_plan is not None:
        items, ordering = read_plan(Path(resume_plan), completion, digest(svg), passes)
    elif input_items is not None:
        items, ordering = preserve_order(input_items, completion)
    else:
        items, ordering = completion.run(passes)
    if stack_search is not None:
        ordering['stack_search'] = stack_search
        ordering['selected_stack'] = stack_details
        ordering['fixed_original_ownership'] = False
        ordering['fixed_visible_color_ownership'] = True
        ordering['preserved_input_order'] = not bool(stack_details)
    if not {item.color for item in items} <= palette_colors:
        raise ValueError('The saved support plan changed the project palette')
    write_plan(output / 'support_plan.json.gz', items, completion, digest(svg), 'complete', ordering['accepted'])
    source_before = image_error(rgba, baseline)
    fitting_trials = []

    def fit(boundary_tolerance):
        completion.tolerance = boundary_tolerance
        completion.encodings.clear()
        candidate, candidate_domain, metrics = svg_bytes(items, completion, size)
        attempted = render(candidate, size)
        source_candidate = image_error(rgba, attempted)
        before_after = image_error(baseline, attempted)
        silhouette = domain.symmetric_difference(candidate_domain).area
        rejections = []
        for label, value, limit in (('render_rmse', before_after['rmse'], render_rmse),
                                   ('local_rmse', before_after['local_rmse'], local_rmse),
                                   ('peak_error', before_after['max_channel_error'], max_error),
                                   ('source_growth', source_candidate['rmse'] - source_before['rmse'], source_growth),
                                   ('silhouette', silhouette, silhouette_tolerance)):
            if value > limit + 1e-9:
                rejections.append({'reason': label, 'measured': value, 'limit': limit})
        if before_after['alpha_max_error'] > 0:
            rejections.append({'reason': 'alpha_coverage_changed', 'measured': before_after['alpha_max_error']})
        fitting_trials.append({'boundary_tolerance': completion.tolerance, 'metrics': metrics,
                               'render_error': before_after, 'source_error': source_candidate, 'rejections': rejections})
        progress({'fitting_trial': len(fitting_trials), **fitting_trials[-1]})
        return {'candidate': candidate, 'metrics': metrics, 'attempted': attempted,
                'source_error': source_candidate, 'render_error': before_after, 'silhouette': silhouette,
                'rejections': rejections, 'tolerance': boundary_tolerance}

    best, upper = None, None
    for scale in (1., .75, .5, .25, .125):
        fitted = fit(tolerance * scale)
        if not fitted['rejections']:
            best = fitted
            break
        upper = fitted['tolerance']
        if completion.tolerance <= 2 * flatten_tolerance:
            break
    if best is not None and upper is not None:
        lower = best['tolerance']
        for _ in range(3):
            if upper - lower <= flatten_tolerance:
                break
            fitted = fit((lower + upper) / 2)
            if fitted['rejections']:
                upper = fitted['tolerance']
            else:
                lower = fitted['tolerance']
                if fitted['metrics']['cost'] < best['metrics']['cost']:
                    best = fitted
    selected = best or fitted
    completion.tolerance = selected['tolerance']
    candidate, metrics, attempted = selected['candidate'], selected['metrics'], selected['attempted']
    source_candidate, before_after = selected['source_error'], selected['render_error']
    silhouette, rejections = selected['silhouette'], list(selected['rejections'])
    output.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(project / 'source.png', output / 'source.png')
    (output / 'baseline.svg').write_bytes(data)
    (output / 'candidate.svg').write_bytes(candidate)
    Image.fromarray(baseline).save(output / 'before.png')
    Image.fromarray(attempted).save(output / 'candidate.png')
    Image.fromarray(np.abs(baseline[:, :, :3].astype(np.int16) - attempted[:, :, :3]).astype(np.uint8)).save(output / 'candidate_difference.png')
    native_directory = output / 'native'
    progress({'native_audit': 'started', 'candidate_metrics': metrics, 'appearance_rejections': rejections})
    native_exit, native, arguments = audit_native(native_tool, output / 'candidate.svg', output / 'candidate.png',
        project / 'source.png', native_directory, flatten_tolerance, area_tolerance, path_weight, hole_weight)
    if native_exit != 0 or native is None or not native.get('bucket_export', {}).get('passed'):
        rejections.append({'reason': 'native_export_rejected', 'return_code': native_exit})
    final_data = (native_directory / 'optimized.svg').read_bytes() if native and (native_directory / 'optimized.svg').is_file() else data
    final = render(final_data, size)
    final_error, final_source = image_error(baseline, final), image_error(rgba, final)
    final_geometries = read_svg(final_data, size, flatten_tolerance)
    final_silhouette = domain.symmetric_difference(shapely.union_all([geometry for _, geometry in final_geometries])).area
    if final_silhouette > silhouette_tolerance:
        rejections.append({'reason': 'final_output_silhouette', 'measured': final_silhouette,
                           'limit': silhouette_tolerance})
    if final_error['rmse'] > render_rmse or final_error['local_rmse'] > local_rmse \
            or final_error['max_channel_error'] > max_error or final_error['alpha_max_error'] > 0 \
            or final_source['rmse'] > source_before['rmse'] + source_growth:
        rejections.append({'reason': 'final_output_appearance'})
    before_metrics = baseline_audit['after']
    if native and native['after']['cost'] >= before_metrics['cost']:
        rejections.append({'reason': 'no_encoded_complexity_reduction'})
    accepted = not rejections
    (output / 'optimized.svg').write_bytes(final_data if accepted else data)
    final = final if accepted else baseline
    Image.fromarray(final).save(output / 'after.png')
    Image.fromarray(np.abs(baseline[:, :, :3].astype(np.int16) - final[:, :, :3]).astype(np.uint8)).save(output / 'difference.png')
    Image.fromarray(np.abs(rgba[:, :, :3].astype(np.int16) - final[:, :, :3]).astype(np.uint8)).save(output / 'source_difference.png')
    (output / 'ordering.json').write_text(json.dumps(ordering, indent=2))
    final_geometries = final_geometries if accepted else paints
    report = {'method': ('Bottom-up joint opaque grouping, geometry and draw order' if ownership_mode == 'partitioned_colors'
                         else 'Selective opaque support completion with bounded fair contour fitting'),
        'accepted': accepted, 'status': 'optimized' if accepted else 'unchanged', 'rejections': rejections,
        'input': {'project': str(project), 'svg': str(svg), 'svg_sha256': digest(svg),
                  'labels_sha256': digest(project / 'labels.npz'), 'source_sha256': digest(project / 'source.png')},
        'implementation_sha256': {name: digest(Path(__file__).with_name(name)) for name in
            ('simplify.py', 'layer_completion.py', 'fair_curves.py', 'bottom_up.py', 'hidden_contours.py')},
        'native_dimensions': list(size), 'downsampled': False, 'qt_version': qVersion(),
        'ownership_mode': ownership_mode,
        'palette_unchanged': True, 'fully_opaque_solid_fills': True,
        'options': {'boundary_tolerance_native_pixels': tolerance, 'fairness': fairness, 'passes': passes,
                    'render_rmse': render_rmse, 'local_rmse': local_rmse, 'max_error': max_error,
                    'source_rmse_growth': source_growth, 'silhouette_area_tolerance': silhouette_tolerance,
                    'flatten_tolerance_native_pixels': flatten_tolerance, 'intersection_area_tolerance': area_tolerance,
                    'weight_paths': path_weight, 'weight_holes': hole_weight, 'weight_angles': angle_weight,
                    'search': search, 'window_start': window_start, 'window_stop': window_stop,
                    'beam_width': beam_width, 'search_states': search_states},
        'ordering': ordering, 'candidate_metrics': metrics, 'before_native_metrics': before_metrics,
        'paint_topology': {'before': paint_topology(before_metrics),
                           'after': paint_topology(native['after'] if accepted and native else before_metrics)},
        'effective_boundary_tolerance': completion.tolerance, 'fitting_trials': fitting_trials,
        'support_plan_sha256': digest(output / 'support_plan.json.gz'),
        'resumed_plan': {'path': str(Path(resume_plan).resolve()), 'sha256': digest(Path(resume_plan))}
            if resume_plan is not None else None,
        'after_native_metrics': native['after'] if accepted and native else before_metrics,
        'bucket_export': native.get('bucket_export') if accepted and native else baseline_audit.get('bucket_export'),
        'baseline_to_candidate': before_after, 'source_to_baseline': source_before,
        'source_to_candidate': source_candidate, 'baseline_to_optimized': image_error(baseline, final),
        'source_to_optimized': image_error(rgba, final), 'candidate_silhouette_difference_area': silhouette,
        'optimized_silhouette_difference_area': final_silhouette if accepted else 0.,
        'boundary_before': boundary_measurements([geometry for _, geometry in paints]),
        'boundary_after': boundary_measurements([geometry for _, geometry in final_geometries]),
        'baseline_native_audit_arguments': baseline_arguments, 'baseline_native_return_code': baseline_exit,
        'native_audit_arguments': arguments, 'native_return_code': native_exit,
        'compact_fit_placements_measured': False, 'runtime_seconds': time.perf_counter() - started}
    (output / 'report.json').write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--svg', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--native-tool', type=Path, default=Path(__file__).resolve().parents[2] / 'build/Release/fls_opaque_regions.exe')
    parser.add_argument('--boundary-tolerance', type=float, default=1.)
    parser.add_argument('--fairness', type=float, default=1.)
    parser.add_argument('--passes', type=int, default=2)
    parser.add_argument('--render-rmse', type=float, default=4.)
    parser.add_argument('--local-rmse', type=float, default=24.)
    parser.add_argument('--max-error', type=float, default=255.)
    parser.add_argument('--source-rmse-growth', type=float, default=.25)
    parser.add_argument('--silhouette-tolerance', type=float, default=.001)
    parser.add_argument('--flatten-tolerance', type=float, default=.05)
    parser.add_argument('--area-tolerance', type=float, default=1e-6)
    parser.add_argument('--resume-plan', type=Path)
    parser.add_argument('--weight-paths', type=float, default=256)
    parser.add_argument('--weight-holes', type=float, default=96)
    parser.add_argument('--search', choices=('selective', 'bottom-up'), default='selective')
    parser.add_argument('--window-start', type=int, default=0)
    parser.add_argument('--window-stop', type=int)
    parser.add_argument('--beam-width', type=int, default=4)
    parser.add_argument('--search-states', type=int, default=96)
    parser.add_argument('--weight-angles', type=float, default=4.)
    args = parser.parse_args()
    try:
        report = run(args.project, args.svg, args.output, args.native_tool, args.boundary_tolerance,
                     args.fairness, args.passes, args.render_rmse, args.local_rmse, args.max_error,
                     args.source_rmse_growth, args.silhouette_tolerance,
                     lambda entry: print(json.dumps(entry), file=sys.stderr, flush=True),
                     args.flatten_tolerance, args.area_tolerance, args.resume_plan, args.weight_paths, args.weight_holes,
                     args.search, args.window_start, args.window_stop, args.beam_width, args.search_states, args.weight_angles)
        print(json.dumps(report, indent=2))
    except (OSError, ValueError, KeyError, RuntimeError, ET.ParseError) as failure:
        print(str(failure), file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
