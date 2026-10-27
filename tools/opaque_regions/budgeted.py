"""Select opaque painted regions under a complete-image Compact Fit budget."""

import gzip
import hashlib
import json
from pathlib import Path
import shutil
import time
from copy import deepcopy
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image
import shapely

from generate import fill
from fair_curves import path_counts, polygons
from paint_stack import DEFAULT_NATIVE_SHAPE_WEIGHT, geometry_metrics, order_stack, polygon_svg
from rate_distortion import RateDistortion, window_sums
from simplify import audit_native, digest, image_error, read_svg, render


DEFAULT_SOURCE_GROWTH = 8.
DEFAULT_LOCAL_GROWTH = 32.
DEFAULT_FIT_EVALUATIONS = 60000
DEFAULT_FIT_BOUNDARY = .5
DEFAULT_FIT_INWARD = .1
DEFAULT_FIT_AREA_RATIO = .002


def filled_quality(source, baseline, actual, source_growth, local_growth):
    error = image_error(source, actual)
    initial = image_error(source, baseline)
    window = min(16, source.shape[1], source.shape[0])
    scale = 3 * window ** 2
    def windows(image):
        return np.sqrt(window_sums(np.square(image[:, :, :3].astype(float)
            - source[:, :, :3]).sum(axis=2), window) / scale)
    growth = float(np.max(windows(actual) - windows(baseline)))
    return {'baseline_to_filled': image_error(baseline, actual), 'source_to_filled': error,
        'maximum_source_local_growth': growth,
        'quality_passed': error['rmse'] <= initial['rmse'] + source_growth
            and error['local_rmse'] <= initial['local_rmse'] + local_growth
            and growth <= local_growth + 1e-5 and error['alpha_max_error'] == 0}


def native_curve_hybrid(original, curved, before, after, shape_weight=DEFAULT_NATIVE_SHAPE_WEIGHT):
    document, candidate = ET.fromstring(original), ET.fromstring(curved)
    paths, replacements = list(document), list(candidate)
    old, new = before.get('per_path', []), after.get('per_path', [])
    if not len(paths) == len(replacements) == len(old) == len(new):
        return None
    selected, fallback = [], []
    for index, (path, replacement) in enumerate(zip(paths, replacements)):
        if path.attrib['fill'] != replacement.attrib['fill']:
            return None
        if path.attrib['d'] == replacement.attrib['d']:
            continue
        def score(report, outline):
            lines, cubics = path_counts(outline.attrib['d'])
            return (shape_weight * report['placements'] + lines + 2 * cubics,
                    report['placements'], lines + 2 * cubics) \
                if report.get('scene_eligible') else (float('inf'),) * 3
        if score(new[index], replacement) < score(old[index], path):
            document.remove(path)
            document.insert(index, deepcopy(replacement))
            selected.append(index)
        else:
            fallback.append(index)
    if not selected:
        return None
    return ET.tostring(document), {'kind': 'native-cost-guided-curve-selection',
        'curved_paths': selected, 'retained_original_paths': fallback}


def encoded_project_cost(path):
    document = json.loads(gzip.decompress(Path(path).read_bytes()))
    contours = []
    def visit(node):
        if node.get('kind') == 'contour':
            contours.append(node)
        for child in node.get('children', []):
            visit(child)
    visit(document['root'])
    points = [point for contour in contours for ring in [contour['points'], *contour['cutouts']] for point in ring]
    anchors, soft = len(points), sum(not point['hard'] for point in points)
    holes = sum(len(contour['cutouts']) for contour in contours)
    return {'anchors': anchors, 'soft_anchors': soft, 'holes': holes, 'draw_items': len(contours),
            'cost': anchors + soft + 32 * len(contours) + 256 * holes}


def run(project, svg, output, tool, *, total_shape_budget=3000, evaluations=DEFAULT_FIT_EVALUATIONS,
        workers=4, source_growth=DEFAULT_SOURCE_GROWTH, local_growth=DEFAULT_LOCAL_GROWTH,
        render_rmse=4., local_rmse=24.,
        silhouette_tolerance=1., cache=None, progress=None, resume_stack=None, max_edits=2400,
        flatten_tolerance=.05, max_error=255., boundary=DEFAULT_FIT_BOUNDARY,
        inward=DEFAULT_FIT_INWARD, area_ratio=DEFAULT_FIT_AREA_RATIO,
        shape_budget=3000, cleanup_edits=160, curve_tolerance=1., outer_passes=2, outer_tolerance=2.,
        rebuild_ownership=False, joint_edits=None, joint_shortlist=3,
        joint_shape_weight=DEFAULT_NATIVE_SHAPE_WEIGHT):
    started = time.monotonic()
    progress = progress or (lambda event: None)
    project, svg, output, tool = [Path(value).resolve() for value in (project, svg, output, tool)]
    implementation = {name: digest(Path(__file__).with_name(name)) for name in
        ('budgeted.py', 'rate_distortion.py', 'adaptive_vertices.py', 'paint_stack.py', 'generate.py',
         'curve_targets.py', 'curve_chains.py', 'fit_repair.py', 'outer_regions.py', 'ownership_regions.py',
         'native_topology.py', 'joint_regions.py', 'fair_curves.py', 'hidden_contours.py',
         'bottom_up.py', 'layer_completion.py')}
    joint_edits = min(16, max_edits) if joint_edits is None else joint_edits
    if total_shape_budget < 1 or evaluations < 1 or workers < 1 or max_edits < 0 or cleanup_edits < 0 or outer_passes < 0 or joint_edits < 0 or joint_shortlist < 1 \
            or any(not np.isfinite(value) or value < 0 for value in
                   (source_growth, local_growth, render_rmse, local_rmse, silhouette_tolerance, curve_tolerance,
                    outer_tolerance, joint_shape_weight)):
        raise ValueError('Invalid image budget, search effort or quality allowance')
    if not all(np.isfinite(value) and value > 0 for value in
               (flatten_tolerance, boundary, inward, area_ratio)) or area_ratio >= 1 or shape_budget < 1:
        raise ValueError('Invalid contour or Compact Fit allowance')
    if output.exists() or project.is_relative_to(output) or output.is_relative_to(project):
        raise ValueError('Use a new output directory outside the input project')
    source = np.asarray(Image.open(project / 'source.png').convert('RGBA'))
    size = source.shape[1], source.shape[0]
    paints = read_svg(svg.read_bytes(), size, flatten_tolerance)
    domain = shapely.union_all([shape for _, shape in paints])
    baseline = render(svg.read_bytes(), size)
    source_before = image_error(source, baseline)
    output.mkdir(parents=True)
    internal = output / 'internal'
    internal.mkdir()
    snapshot = output / 'implementation'
    snapshot.mkdir()
    for name in implementation:
        shutil.copyfile(Path(__file__).with_name(name), snapshot / name)
    shutil.copyfile(project / 'source.png', output / 'source.png')
    Image.fromarray(baseline).save(output / 'opaque_baseline.png')
    code, audit, _ = audit_native(tool, svg, output / 'opaque_baseline.png', project / 'source.png',
        internal / 'audit', flatten_tolerance, .01, 256., 512.)
    if code or not audit or not audit.get('bucket_export', {}).get('passed'):
        raise ValueError('The opaque baseline failed native export validation')
    geometry_budget = max(1, int(total_shape_budget * .8))
    resume_contours = None
    def checkpoint(stack, optimization):
        optimization = dict(optimization, geometry_budget=geometry_budget,
                            within_geometry_budget=optimization['triangle_bound'] <= geometry_budget)
        saved = {'input_sha256': digest(svg), 'source_sha256': digest(project / 'source.png'),
            'optimization': optimization,
            'stack': [{'fill': color, 'wkb': shape.wkb.hex()} for color, shape in stack]}
        temporary = output / 'stack_checkpoint.tmp'
        temporary.write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
        temporary.replace(output / 'stack_checkpoint.json.gz')
    if resume_stack is not None:
        saved = json.loads(gzip.decompress(Path(resume_stack).read_bytes()))
        if saved.get('input_sha256') != digest(svg) or saved.get('source_sha256') != digest(project / 'source.png'):
            raise ValueError('The saved budget plan has different input provenance')
        stack = [(row['fill'], shapely.from_wkb(bytes.fromhex(row['wkb']))) for row in saved['stack']]
        structural = saved['optimization']
        if not max_edits and not outer_passes and saved.get('encoded_svg'):
            resume_contours = saved['encoded_svg'].encode()
        if not curve_tolerance and structural.get('triangle_bound', float('inf')) > total_shape_budget:
            optimizer = RateDistortion(stack, source, domain, progress, local_growth=local_growth,
                reference_image=baseline, checkpoint=checkpoint, reference_stack=paints)
            stack, structural = optimizer.run(geometry_budget, max_edits=max_edits)
    else:
        optimizer = RateDistortion(paints, source, domain, progress, local_growth=local_growth,
                                   reference_image=baseline, checkpoint=checkpoint)
        stack, structural = optimizer.run(geometry_budget, max_edits=max_edits)
    if not curve_tolerance and structural.get('within_geometry_budget') and 'optimizer' in locals():
        stack, structural = optimizer.refine(max(geometry_budget, int(total_shape_budget * .94)), max_edits=24)
    quality_budget = max(1, int(total_shape_budget * .98))
    if not curve_tolerance and structural['triangle_bound'] < quality_budget and max_edits:
        from adaptive_vertices import refine as refine_vertices
        split = [(color, part.simplify(0, preserve_topology=True)) for color, shape in stack for part in polygons(shape)]
        refiner = RateDistortion(split, source, domain, progress, local_growth=local_growth,
            reference_image=baseline, reference_stack=paints)
        stack, contours = refiner.refine_from_reference(quality_budget, max_edits=min(12, max_edits))
        stack, vertices = refine_vertices(refiner, quality_budget, max_edits=min(256, max_edits))
        structural = dict(structural, **{key: vertices[key] for key in
            ('geometry_budget', 'within_geometry_budget', 'regions', 'holes', 'points',
             'triangle_bound', 'cost', 'source_error')}, quality_refinement={'contours': contours, 'vertices': vertices})
    if cleanup_edits and max_edits:
        progress({'topology_cleanup': 'current-owner hole promotion and neighboring-color absorption'})
        cleanup = RateDistortion(stack, source, domain, progress, local_growth=local_growth,
            reference_image=baseline, reference_stack=stack, topology_transactions=True)
        current_error = image_error(source, cleanup.image)['rmse']
        stack, cleaned = cleanup.run(max(1, structural['triangle_bound'] * 2),
            max_edits=min(cleanup_edits, max_edits),
            source_rmse_limit=min(source_before['rmse'] + source_growth, current_error + .25),
            continue_below_budget=True)
        structural = dict(structural, topology_cleanup=cleaned)
    if outer_passes:
        from outer_regions import run as rebuild_outer
        stack, outer = rebuild_outer(tool, stack, source, baseline, domain, internal / 'outer_regions',
            passes=outer_passes, boundary_tolerance=outer_tolerance,
            source_growth=source_growth, local_growth=local_growth, progress=progress, ownership=rebuild_ownership)
        structural = dict(structural, outer_regions=outer)
    cache = Path(cache).resolve() if cache else internal / 'fit_cache'
    measurements, measured = [], {}
    def measure_joint(data, details):
        key = hashlib.sha256(data).hexdigest()
        if key in measured:
            return measured[key]
        name = f'native_probe_{len(measurements)}'
        contours, reference = internal / f'{name}.svg', internal / f'{name}.png'
        contours.write_bytes(data)
        guide = render(data, size)
        Image.fromarray(guide).save(reference)
        directory = internal / name
        fitted = fill(tool, contours, reference, project / 'source.png', directory, cache, evaluations,
            min(shape_budget, total_shape_budget), boundary, inward, area_ratio,
            render_rmse, local_rmse, max_error, progress, silhouette_tolerance=silhouette_tolerance,
            flatten_tolerance=flatten_tolerance, workers=workers, scene_validated=True,
            polygon_seed=False, total_shape_budget=total_shape_budget)
        if (directory / 'generated_compact.3so').exists():
            fitted['encoded_target_cost'] = encoded_project_cost(directory / 'generated_compact.3so')
        paints = read_svg(data, size, flatten_tolerance)
        _, ordering = order_stack(paints, retain_input_order=True)
        geometry = dict(structural, **{name: sum(geometry_metrics(shape)[name] for _, shape in paints)
            for name in ('regions', 'holes', 'points', 'triangle_bound', 'cost')},
            encoded_contours=fitted.get('bucket_input', {}), joint_native_measurement=details)
        trial = {'name': name, 'directory': str(directory.relative_to(output)),
                 'fit': fitted, 'geometry': geometry, 'baseline_to_guide': image_error(baseline, guide)}
        if (directory / 'generated_compact.png').exists():
            actual = np.asarray(Image.open(directory / 'generated_compact.png').convert('RGBA'))
            trial.update(filled_quality(source, baseline, actual, source_growth, local_growth))
            fitted['original_to_filled'] = trial['source_to_filled']
            if not trial['quality_passed']:
                fitted['accepted'] = False
                fitted.setdefault('rejections', []).append('Source-relative image error budget exceeded')
        saved = {'input_sha256': digest(svg), 'source_sha256': digest(project / 'source.png'),
            'optimization': geometry, 'encoded_svg': data.decode(),
            'stack': [{'fill': color, 'wkb': shape.wkb.hex()} for color, shape in paints]}
        (directory / 'stack_plan.json.gz').write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
        (directory / 'ordering.json').write_text(json.dumps(ordering, indent=2))
        measurements.append(trial)
        measured[key] = {'trial': name, 'accepted': bool(fitted.get('accepted') and trial.get('quality_passed')),
            'shape_count': fitted.get('shape_count'), 'region_count': fitted.get('region_count'),
            'encoding_cost': fitted.get('encoded_target_cost', {}).get('cost'),
            'maximum_source_local_growth': trial.get('maximum_source_local_growth')}
        return measured[key]
    comparison_contours = None
    if joint_edits:
        from joint_regions import run as joint_regions
        original_contours = resume_contours or polygon_svg(stack, size)
        proposed, stack, joint = joint_regions(tool, original_contours, source, baseline, domain,
            internal / 'joint_regions', source_growth=source_growth, local_growth=local_growth,
            maximum=joint_edits, progress=progress, native_score=measure_joint, shortlist=joint_shortlist,
            shape_weight=joint_shape_weight, flatten_tolerance=flatten_tolerance)
        structural = dict(structural, joint_regions=joint)
        if joint['edits']:
            comparison_contours = original_contours
        resume_contours = proposed
    try:
        stack, ordering = order_stack(stack)
        ordering['accepted'] = True
    except ValueError as failure:
        ordering = {'accepted': False, 'error': str(failure), 'appearance_groups': len({color for color, _ in stack}),
                    'edges': [], 'retained_order': True}
        progress({'retained_valid_order': str(failure)})
    (output / 'ordering.json').write_text(json.dumps(ordering, indent=2))
    structural.update({name: sum(geometry_metrics(shape)[name] for _, shape in stack)
                       for name in ('regions', 'holes', 'points', 'triangle_bound', 'cost')})
    saved = {'input_sha256': digest(svg), 'source_sha256': digest(project / 'source.png'),
        'optimization': structural, 'stack': [{'fill': color, 'wkb': shape.wkb.hex()} for color, shape in stack]}
    (output / 'stack_plan.json.gz').write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
    (output / 'optimization.json').write_text(json.dumps(structural, indent=2))
    trials = []
    joint_contours = resume_contours if joint_edits else None
    curve_comparison = None
    pending_repair = (resume_contours, {'resumed_encoded_contours': True}) if resume_contours else None
    attempts = 3 + (comparison_contours is not None)
    for attempt in range(attempts):
        contours = internal / f'contours_{attempt}.svg'
        from curve_targets import recover
        comparing_baseline = comparison_contours is not None and attempt == 0
        if comparing_baseline:
            data = comparison_contours
            stack = read_svg(data, size, flatten_tolerance)
            _, diagnostic = order_stack(stack)
            diagnostic['order'] = list(range(len(stack)))
            diagnostic['accepted'] = all(first < last for first, last in diagnostic['edges'])
            diagnostic['retained_input_order'] = True
            diagnostic['layers'].sort(key=lambda layer: layer['source_index'])
            curves = {'enabled': True, 'ordering': diagnostic, 'baseline_comparison': True}
        elif pending_repair is None:
            data, stack, curves = recover(tool, stack, size, domain, source, baseline,
                internal / f'curves_{attempt}', tolerance=curve_tolerance,
                source_growth=source_growth, local_growth=local_growth, progress=progress,
                flatten_tolerance=flatten_tolerance)
        else:
            data, details = pending_repair
            pending_repair = None
            stack = read_svg(data, size, flatten_tolerance)
            if joint_edits:
                data, stack, curves = recover(tool, stack, size, domain, source, baseline,
                    internal / f'curves_{attempt}', tolerance=curve_tolerance, encoded=data,
                    source_growth=source_growth, local_growth=local_growth, progress=progress,
                    flatten_tolerance=flatten_tolerance)
                curves['native_error_repair' if not details.get('resumed_encoded_contours') else 'joint_input'] = details
            else:
                _, diagnostic = order_stack(stack)
                diagnostic['order'] = list(range(len(stack)))
                diagnostic['accepted'] = all(first < last for first, last in diagnostic['edges'])
                diagnostic['retained_input_order'] = True
                diagnostic['layers'].sort(key=lambda layer: layer['source_index'])
                curves = {'enabled': True, 'ordering': diagnostic,
                    **(details if details.get('resumed_encoded_contours') else {'native_error_repair': details})}
        contours.write_bytes(data)
        structural['curve_recovery'] = {key: value for key, value in curves.items() if key != 'ordering'}
        if curves.get('ordering'):
            ordering = curves['ordering']
            (output / 'ordering.json').write_text(json.dumps(ordering, indent=2))
        reference = internal / f'contours_{attempt}.png'
        guide = render(contours.read_bytes(), size)
        Image.fromarray(guide).save(reference)
        directory = internal / f'fill_{attempt}'
        fitted = fill(tool, contours, reference, project / 'source.png', directory, cache, evaluations,
            min(shape_budget, total_shape_budget), boundary, inward, area_ratio,
            render_rmse, local_rmse, max_error, progress,
            silhouette_tolerance=silhouette_tolerance, flatten_tolerance=flatten_tolerance,
            workers=workers, scene_validated=True,
            polygon_seed=False, total_shape_budget=total_shape_budget)
        if (directory / 'generated_compact.3so').exists():
            fitted['encoded_target_cost'] = encoded_project_cost(directory / 'generated_compact.3so')
        structural['encoded_contours'] = fitted.get('bucket_input', {})
        structural.update({name: sum(geometry_metrics(shape)[name] for _, shape in stack)
                           for name in ('regions', 'holes', 'points', 'triangle_bound', 'cost')})
        saved = {'input_sha256': digest(svg), 'source_sha256': digest(project / 'source.png'),
            'optimization': structural, 'encoded_svg': data.decode(),
            'stack': [{'fill': color, 'wkb': shape.wkb.hex()} for color, shape in stack]}
        (output / 'stack_plan.json.gz').write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
        (output / 'optimization.json').write_text(json.dumps(structural, indent=2))
        (directory / 'stack_plan.json.gz').write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
        (directory / 'ordering.json').write_text(json.dumps(ordering, indent=2))
        trial = {'name': f'candidate_{attempt}', 'directory': str(directory.relative_to(output)),
            'fit': fitted, 'geometry': json.loads(json.dumps(structural)),
            'baseline_to_guide': image_error(baseline, guide)}
        if (directory / 'generated_compact.png').exists():
            actual = np.asarray(Image.open(directory / 'generated_compact.png').convert('RGBA'))
            trial.update(filled_quality(source, baseline, actual, source_growth, local_growth))
            fitted['original_to_filled'] = trial['source_to_filled']
            if not trial['quality_passed']:
                fitted['accepted'] = False
                fitted.setdefault('rejections', []).append('Source-relative image error budget exceeded')
        trials.append(trial)
        if not comparing_baseline and curve_comparison is None:
            curve_comparison = data, fitted
        if comparing_baseline:
            continue
        count = fitted.get('shape_count') or fitted.get('attempted_placements', 0)
        diagnostic_image = directory / 'generated_compact.png'
        if not diagnostic_image.exists():
            diagnostic_image = directory / fitted.get('rejected_render', 'generated_unculled.png')
        if not fitted.get('accepted') and attempt < attempts - 1 and diagnostic_image.exists():
            from fit_repair import propose
            pending_repair = propose(data, paints, source, baseline,
                np.asarray(Image.open(diagnostic_image).convert('RGBA')), internal / f'repair_{attempt}',
                local_rmse=local_rmse, local_growth=local_growth, source_growth=source_growth)
            if pending_repair is not None:
                progress({'native_error_repair': pending_repair[1]})
                if joint_edits:
                    repaired, details = pending_repair
                    repaired, _, joint = joint_regions(tool, repaired, source, baseline, domain,
                        internal / f'joint_repair_{attempt}', source_growth=source_growth,
                        local_growth=local_growth, maximum=min(4, joint_edits), progress=progress,
                        native_score=measure_joint, shortlist=joint_shortlist,
                        shape_weight=joint_shape_weight, flatten_tolerance=flatten_tolerance)
                    pending_repair = repaired, dict(details, joint_optimization=joint)
                continue
        if curve_tolerance and not fitted.get('accepted') and max_edits and attempt < attempts - 1:
            progress({'native_fit_feedback': attempt, 'shapes': count,
                      'failed_regions': fitted.get('failed_regions'), 'topology_cleanup': True})
            optimizer = RateDistortion(stack, source, domain, progress, local_growth=local_growth,
                reference_image=baseline, reference_stack=stack, topology_transactions=True)
            stack, cleaned = optimizer.run(max(1, sum(geometry_metrics(shape)['triangle_bound']
                for _, shape in stack) * 2), max_edits=min(cleanup_edits, max_edits),
                source_rmse_limit=source_before['rmse'] + source_growth, continue_below_budget=True)
            structural = dict(structural, native_feedback=cleaned)
            stack, ordering = order_stack(stack)
            curve_tolerance *= 1.5
            continue
        if not curve_tolerance and count and count > total_shape_budget \
                and structural['triangle_bound'] <= total_shape_budget and attempt < attempts - 1:
            geometry_budget = max(1, int(structural['triangle_bound'] * total_shape_budget / count * .98))
            optimizer = RateDistortion(stack, source, domain, progress, local_growth=local_growth,
                reference_image=baseline, checkpoint=checkpoint, reference_stack=paints)
            stack, structural = optimizer.run(geometry_budget, max_edits=max_edits)
            continue
        break
    if joint_contours is not None and curve_comparison is not None:
        reference = measure_joint(joint_contours, {'kind': 'joint-output-before-curve-fitting'})
        before = next(trial['fit'] for trial in measurements if trial['name'] == reference['trial'])
        hybrid = native_curve_hybrid(joint_contours, curve_comparison[0], before, curve_comparison[1],
                                    joint_shape_weight)
        if hybrid:
            measure_joint(*hybrid)
    trials.extend(measurements)
    eligible = [trial for trial in trials if trial['fit'].get('shape_count') is not None
                and trial['fit']['shape_count'] <= total_shape_budget
                and trial['fit'].get('shape_budget_passed') and trial.get('source_to_filled')]
    eligible.sort(key=lambda trial: (not trial['fit'].get('accepted', False),
        joint_shape_weight * trial['fit']['shape_count'] + trial['fit']['encoded_target_cost']['cost'],
        trial['fit']['shape_count'],
        trial['fit']['region_count'], trial['source_to_filled']['rmse']))
    selected = eligible[0] if eligible else None
    if selected:
        prefix = 'after' if selected['fit'].get('accepted') else 'budget_candidate'
        directory = output / selected['directory']
        shutil.copyfile(directory / 'stack_plan.json.gz', output / 'stack_plan.json.gz')
        shutil.copyfile(directory / 'ordering.json', output / 'ordering.json')
        ordering = json.loads((output / 'ordering.json').read_text())
        (output / 'optimization.json').write_text(json.dumps(selected['geometry'], indent=2))
        shutil.copyfile(directory / 'generated_compact.3so', output / f'{prefix}.3so')
        shutil.copyfile(directory / 'generated_compact.png', output / f'{prefix}.png')
        actual = np.asarray(Image.open(output / f'{prefix}.png').convert('RGBA'))
        Image.fromarray(np.abs(source[:, :, :3].astype(np.int16) - actual[:, :, :3]).astype(np.uint8)).save(output / 'source_difference.png')
        Image.fromarray(np.abs(baseline[:, :, :3].astype(np.int16) - actual[:, :, :3]).astype(np.uint8)).save(output / 'difference.png')
        from native_topology import audit as audit_topology
        selected['fit']['native_topology'] = audit_topology(output / f'{prefix}.3so',
            tool.parent / 'assets/vector/shape_geometry.json.gz', output / 'native_topology.json')['summary']
    accepted = bool(selected and selected['fit'].get('accepted'))
    improved = accepted and (selected['fit']['region_count'] < audit['before']['paths']
        or selected['geometry']['holes'] < audit['before']['holes'])
    report = {'method': 'Current-owner topology transactions, Bucket curves and editor GPU Compact Fit',
        'accepted': accepted, 'improved': improved, 'selected': selected['name'] if selected else None,
        'status': 'filled' if accepted else 'budget_candidate' if selected else 'no_budget_feasible_output',
        'native_dimensions': size, 'downsampled': False, 'leeway_used': False,
        'total_shape_budget': total_shape_budget, 'source_to_opaque': source_before,
        'cost_weights': {'native_shapes': joint_shape_weight, 'draw_items': 32, 'anchors': 1,
                         'soft_anchor_extra': 1, 'holes': 256},
        'compact_fit_pipeline': 'editor curve-first Compact Fit', 'gpu_requested': True,
        'polygon_seeds_used': False,
        'experimental_ownership_rebuild': rebuild_ownership,
        'appearance_groups': ordering['appearance_groups'],
        'ordering_edges': len(ordering['edges']),
        'before_region_count': audit['before']['paths'],
        'after_shape_count': selected['fit']['shape_count'] if selected else None,
        'after_region_count': selected['fit']['region_count'] if selected else None,
        'quality_limits': {'source_rmse': source_before['rmse'] + source_growth,
            'source_local_rmse': source_before['local_rmse'] + local_growth,
            'optimized_guide_to_fill_rmse': render_rmse, 'optimized_guide_to_fill_local_rmse': local_rmse,
            'silhouette_difference_area': silhouette_tolerance},
        'input': {'svg_sha256': digest(svg), 'source_sha256': digest(project / 'source.png')},
        'implementation_sha256': implementation, 'implementation_snapshot': 'implementation',
        'native_tool_sha256': digest(tool),
        'trials': trials, 'runtime_seconds': time.monotonic() - started}
    (output / 'report.json').write_text(json.dumps(report, indent=2))
    return report
