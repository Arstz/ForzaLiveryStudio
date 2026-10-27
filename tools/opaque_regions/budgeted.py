"""Select opaque painted regions under a complete-image Compact Fit budget."""

import gzip
import json
from pathlib import Path
import shutil
import time

import numpy as np
from PIL import Image
import shapely

from generate import fill
from fair_curves import polygons
from paint_stack import geometry_metrics, order_stack, polygon_svg
from rate_distortion import RateDistortion, window_sums
from simplify import audit_native, digest, image_error, read_svg, render


DEFAULT_SOURCE_GROWTH = 8.
DEFAULT_LOCAL_GROWTH = 32.
DEFAULT_FIT_EVALUATIONS = 60000


def run(project, svg, output, tool, *, total_shape_budget=3000, evaluations=DEFAULT_FIT_EVALUATIONS,
        workers=4, source_growth=DEFAULT_SOURCE_GROWTH, local_growth=DEFAULT_LOCAL_GROWTH,
        render_rmse=4., local_rmse=24.,
        silhouette_tolerance=1., cache=None, progress=None, resume_stack=None, max_edits=2400,
        flatten_tolerance=.05, max_error=255., boundary=2., inward=.5, area_ratio=.02,
        shape_budget=3000, cleanup_edits=160, curve_tolerance=1.):
    started = time.monotonic()
    progress = progress or (lambda event: None)
    project, svg, output, tool = [Path(value).resolve() for value in (project, svg, output, tool)]
    implementation = {name: digest(Path(__file__).with_name(name)) for name in
        ('budgeted.py', 'rate_distortion.py', 'adaptive_vertices.py', 'paint_stack.py', 'generate.py', 'curve_targets.py', 'fit_repair.py')}
    if total_shape_budget < 1 or evaluations < 1 or workers < 1 or max_edits < 0 or cleanup_edits < 0 \
            or any(not np.isfinite(value) or value < 0 for value in
                   (source_growth, local_growth, render_rmse, local_rmse, silhouette_tolerance, curve_tolerance)):
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
    cache = Path(cache).resolve() if cache else internal / 'fit_cache'
    trials = []
    pending_repair = None
    for attempt in range(3):
        contours = internal / f'contours_{attempt}.svg'
        from curve_targets import recover
        if pending_repair is None:
            data, stack, curves = recover(tool, stack, size, domain, source, baseline,
                internal / f'curves_{attempt}', tolerance=curve_tolerance,
                source_growth=source_growth, local_growth=local_growth, progress=progress)
        else:
            data, details = pending_repair
            pending_repair = None
            stack = read_svg(data, size, flatten_tolerance)
            _, diagnostic = order_stack(stack)
            diagnostic['order'] = list(range(len(stack)))
            diagnostic['accepted'] = all(first < last for first, last in diagnostic['edges'])
            diagnostic['retained_input_order'] = True
            diagnostic['layers'].sort(key=lambda layer: layer['source_index'])
            curves = {'enabled': True, 'native_error_repair': details, 'ordering': diagnostic}
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
        structural['encoded_contours'] = fitted.get('bucket_input', {})
        saved = {'input_sha256': digest(svg), 'source_sha256': digest(project / 'source.png'),
            'optimization': structural, 'encoded_svg': data.decode(),
            'stack': [{'fill': color, 'wkb': shape.wkb.hex()} for color, shape in stack]}
        (output / 'stack_plan.json.gz').write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
        (output / 'optimization.json').write_text(json.dumps(structural, indent=2))
        trial = {'name': f'candidate_{attempt}', 'directory': str(directory.relative_to(output)),
            'fit': fitted, 'geometry': json.loads(json.dumps(structural)),
            'baseline_to_guide': image_error(baseline, guide)}
        if (directory / 'generated_compact.png').exists():
            actual = np.asarray(Image.open(directory / 'generated_compact.png').convert('RGBA'))
            trial['baseline_to_filled'] = image_error(baseline, actual)
            trial['source_to_filled'] = image_error(source, actual)
            fitted['original_to_filled'] = trial['source_to_filled']
            error = trial['source_to_filled']
            window = min(16, *size)
            scale = 3 * window ** 2
            initial_windows = np.sqrt(window_sums(np.square(
                baseline[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2), window) / scale)
            final_windows = np.sqrt(window_sums(np.square(
                actual[:, :, :3].astype(float) - source[:, :, :3]).sum(axis=2), window) / scale)
            trial['maximum_source_local_growth'] = float(np.max(final_windows - initial_windows))
            trial['quality_passed'] = error['rmse'] <= source_before['rmse'] + source_growth \
                and error['local_rmse'] <= source_before['local_rmse'] + local_growth \
                and trial['maximum_source_local_growth'] <= local_growth + 1e-5 \
                and error['alpha_max_error'] == 0
            if not trial['quality_passed']:
                fitted['accepted'] = False
                fitted.setdefault('rejections', []).append('Source-relative image error budget exceeded')
        trials.append(trial)
        count = fitted.get('shape_count') or fitted.get('attempted_placements', 0)
        diagnostic_image = directory / 'generated_compact.png'
        if not diagnostic_image.exists():
            diagnostic_image = directory / fitted.get('rejected_render', 'generated_unculled.png')
        if not fitted.get('accepted') and attempt < 2 and diagnostic_image.exists():
            from fit_repair import propose
            pending_repair = propose(data, paints, source, baseline,
                np.asarray(Image.open(diagnostic_image).convert('RGBA')), internal / f'repair_{attempt}',
                local_rmse=local_rmse, local_growth=local_growth, source_growth=source_growth)
            if pending_repair is not None:
                progress({'native_error_repair': pending_repair[1]})
                continue
        if curve_tolerance and not fitted.get('accepted') and max_edits and attempt < 2:
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
                and structural['triangle_bound'] <= total_shape_budget and attempt < 2:
            geometry_budget = max(1, int(structural['triangle_bound'] * total_shape_budget / count * .98))
            optimizer = RateDistortion(stack, source, domain, progress, local_growth=local_growth,
                reference_image=baseline, checkpoint=checkpoint, reference_stack=paints)
            stack, structural = optimizer.run(geometry_budget, max_edits=max_edits)
            continue
        break
    eligible = [trial for trial in trials if trial['fit'].get('shape_count') is not None
                and trial['fit']['shape_count'] <= total_shape_budget
                and trial['fit'].get('shape_budget_passed') and trial.get('source_to_filled')]
    eligible.sort(key=lambda trial: (not trial['fit'].get('accepted', False),
        trial['source_to_filled']['rmse'], trial['fit']['region_count'], trial['fit']['shape_count']))
    selected = eligible[0] if eligible else None
    if selected:
        prefix = 'after' if selected['fit'].get('accepted') else 'budget_candidate'
        directory = output / selected['directory']
        shutil.copyfile(directory / 'generated_compact.3so', output / f'{prefix}.3so')
        shutil.copyfile(directory / 'generated_compact.png', output / f'{prefix}.png')
        actual = np.asarray(Image.open(output / f'{prefix}.png').convert('RGBA'))
        Image.fromarray(np.abs(source[:, :, :3].astype(np.int16) - actual[:, :, :3]).astype(np.uint8)).save(output / 'source_difference.png')
        Image.fromarray(np.abs(baseline[:, :, :3].astype(np.int16) - actual[:, :, :3]).astype(np.uint8)).save(output / 'difference.png')
    accepted = bool(selected and selected['fit'].get('accepted'))
    improved = accepted and (selected['fit']['region_count'] < audit['before']['paths']
        or selected['geometry']['holes'] < audit['before']['holes'])
    report = {'method': 'Current-owner topology transactions, Bucket curves and editor GPU Compact Fit',
        'accepted': accepted, 'improved': improved, 'selected': selected['name'] if selected else None,
        'status': 'filled' if accepted else 'budget_candidate' if selected else 'no_budget_feasible_output',
        'native_dimensions': size, 'downsampled': False, 'leeway_used': False,
        'total_shape_budget': total_shape_budget, 'source_to_opaque': source_before,
        'compact_fit_pipeline': 'editor curve-first Compact Fit', 'gpu_requested': True,
        'polygon_seeds_used': False,
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
