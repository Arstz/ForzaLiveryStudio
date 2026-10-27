"""Generate and compare Compact Fit projects from opaque region proposals."""

import argparse
import gzip
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image
import shapely

from bottom_up import StackSearch, visible_masks
from layer_completion import Completion
from simplify import (audit_native, digest, image_error, partition_stack, read_svg, render,
                      run as simplify, write_plan)


def compact_score(report, shape_weight=1., region_weight=256.):
    if not report.get('accepted') or not report.get('shape_budget_passed', True) \
            or not report.get('scene_verified', report.get('verified')) or report.get('shape_count') is None:
        return None
    return shape_weight * report['shape_count'] + region_weight * report['region_count']


def native_polygon_target(rings):
    coordinates = [np.asarray(ring, dtype=float) for ring in rings]
    signed = [float(np.sum(points[:, 0] * np.roll(points[:, 1], -1)
                          - points[:, 1] * np.roll(points[:, 0], -1))) for points in coordinates]
    outer = [(points, shapely.Polygon(points), []) for points, area in zip(coordinates, signed) if area > 0]
    for points, area in zip(coordinates, signed):
        if area >= 0:
            continue
        hole = shapely.Polygon(points)
        owners = [index for index, (_, polygon, _) in enumerate(outer) if polygon.covers(hole)]
        if not owners:
            raise ValueError('Native seed hole has no enclosing component')
        owner = min(owners, key=lambda index: outer[index][1].area)
        outer[owner][2].append(points)
    result = shapely.union_all([shapely.Polygon(points, holes) for points, _, holes in outer])
    if not result.is_valid:
        result = shapely.make_valid(result)
    if result.is_empty or not result.is_valid:
        raise ValueError('Native target polygons do not describe valid filled geometry')
    return result


def fill(tool, svg, reference, original, output, cache, evaluations, shape_budget,
         boundary, inward, area_ratio, render_rmse, local_rmse, max_error, progress,
         silhouette_tolerance=1., flatten_tolerance=.05, workers=4, scene_validated=False, polygon_seed=False,
         total_shape_budget=3000):
    output.mkdir()
    prepared = subprocess.run([str(tool), str(svg), '--prepare-fill-targets', '--output', str(output)],
        capture_output=True, text=True)
    if prepared.returncode:
        return {'accepted': False, 'verified': False, 'shape_count': None,
            'error': prepared.stderr, 'return_code': prepared.returncode}
    target_data = json.loads((output / 'fill_targets.json').read_text())
    if target_data['input_sha256'] != digest(svg):
        raise ValueError('Native target polygons changed their input provenance')
    seeds = []
    if polygon_seed:
        for target in target_data['regions']:
            mesh = shapely.constrained_delaunay_triangles(native_polygon_target(target['rings']))
            triangles = [list(np.asarray(part.exterior.coords)[:3].flatten()) for part in mesh.geoms]
            seeds.append(triangles)
    manifest = output / 'seed_triangles.json'
    manifest.write_text(json.dumps({'version': 1, 'input_sha256': digest(svg),
        'source': 'native Compact Fit target polygons', 'regions': seeds}))
    arguments = [str(tool), str(svg), '--fill-only', '--reference', str(reference),
        '--original', str(original), '--output', str(output), '--compact-fit-cache', str(cache),
        '--compact-fit-evaluations', str(evaluations), '--compact-fit-shape-budget', str(shape_budget),
        '--compact-fit-total-shape-budget', str(total_shape_budget),
        '--compact-fit-boundary', str(boundary), '--compact-fit-inward', str(inward),
        '--compact-fit-area-ratio', str(area_ratio), '--render-rmse', str(render_rmse),
        '--render-max-error', str(max_error), '--abstraction-local-rmse', str(local_rmse),
        '--silhouette-tolerance', str(silhouette_tolerance),
        '--flatten-tolerance', str(flatten_tolerance)]
    arguments += ['--compact-fit-workers', str(workers)]
    if polygon_seed:
        arguments += ['--compact-fit-polygon-seed', '--compact-fit-seeds', str(manifest)]
    if scene_validated:
        arguments.append('--scene-validated-fills')
    (output / 'invocation.json').write_text(json.dumps(arguments, indent=2))
    progress({'filling': str(output), 'evaluations_per_region': evaluations})
    with (output / 'stdout.json').open('w') as stdout, (output / 'stderr.log').open('w') as stderr:
        result = subprocess.run(arguments, stdout=stdout, stderr=stderr)
    report_path = output / 'report.json'
    report = json.loads(report_path.read_text()) if report_path.is_file() else {
        'accepted': False, 'verified': False, 'shape_count': None,
        'error': (output / 'stderr.log').read_text(errors='replace')[-4000:]}
    if polygon_seed and report.get('failed_regions', 0) > 0 and not report.get('accepted'):
        (output / 'coarse_seed_report.json').write_text(json.dumps(report, indent=2))
        failed = [row['path'] for row in report.get('per_path', [])
                  if not row.get('verified') and not row.get('complete') and row.get('polygon_seed_used')]
        refined = []
        for index in failed:
            precise = native_polygon_target(target_data['regions'][index]['rings'])
            bounds = precise.bounds
            step = max(.01, max(bounds[2] - bounds[0], bounds[3] - bounds[1]) / 16)
            precise = shapely.segmentize(precise, step)
            mesh = shapely.constrained_delaunay_triangles(precise)
            seeds[index] = [list(np.asarray(part.exterior.coords)[:3].flatten()) for part in mesh.geoms]
            refined.append(index)
        if refined:
            manifest.write_text(json.dumps({'version': 1, 'input_sha256': digest(svg),
                'refined_regions': refined, 'seed_conditioning': 'boundary subdivisions', 'regions': seeds}))
            progress({'refining_polygon_seeds': len(refined), 'quality_limits_unchanged': True})
            shutil.copyfile(output / 'stderr.log', output / 'coarse_seed_stderr.log')
            with (output / 'stdout.json').open('w') as stdout, (output / 'stderr.log').open('w') as stderr:
                result = subprocess.run(arguments, stdout=stdout, stderr=stderr)
            report = json.loads(report_path.read_text()) if report_path.is_file() else {
                'accepted': False, 'verified': False, 'shape_count': None,
                'error': (output / 'stderr.log').read_text(errors='replace')[-4000:]}
            report['refined_seed_regions'] = refined
    report['return_code'] = result.returncode
    report['opaque_to_compact_target'] = target_data['opaque_to_compact_target']
    progress({'filled': str(output), 'accepted': report['accepted'],
        'shape_count': report.get('shape_count'), 'failed_regions': report.get('failed_regions')})
    return report


def run(project, svg, output, tool, *, start=0, stop=None, search_states=32, shortlist=2,
        tolerance=1., area_tolerance=.01, path_weight=256., hole_weight=512.,
        shape_weight=1., region_weight=256., render_rmse=4., local_rmse=24., max_error=255.,
        source_growth=1., evaluations=6000, shape_budget=3000, boundary=2., inward=.5,
        area_ratio=.02, cache=None, progress=None, silhouette_tolerance=1., flatten_tolerance=.05,
        workers=4, resume_search=None, ownership_tolerance=.25, scene_validated=False, polygon_seed=False,
        total_shape_budget=3000):
    started = time.monotonic()
    progress = progress or (lambda entry: None)
    project, svg, output, tool = [Path(path).resolve() for path in (project, svg, output, tool)]
    implementation = {name: digest(Path(__file__).with_name(name)) for name in
        ('generate.py', 'simplify.py', 'bottom_up.py', 'hidden_contours.py', 'layer_completion.py', 'fair_curves.py')}
    if output.exists() or output == project or output.is_relative_to(project) or project.is_relative_to(output):
        raise ValueError('Use a new output directory outside the input project')
    if shortlist < 1 or not all(np.isfinite(v) and v >= 0 for v in
            (shape_weight, region_weight, render_rmse, local_rmse, max_error, source_growth)):
        raise ValueError('Invalid shortlist, objective weights or quality tolerances')
    if shape_weight + region_weight <= 0:
        raise ValueError('The measured shape and region objective must have positive weight')
    original = np.asarray(Image.open(project / 'source.png').convert('RGBA'))
    size = original.shape[1], original.shape[0]
    data = svg.read_bytes()
    paints = read_svg(data, size, flatten_tolerance)
    with np.load(project / 'labels.npz') as labels:
        if labels['labels'].shape != original.shape[:2]:
            raise ValueError('Region labels must retain the source dimensions')
        palette = {'#' + ''.join(f'{int(value):02x}' for value in color) for color in labels['palette']}
    if not {color for color, _ in paints} <= palette:
        raise ValueError('The opaque contour palette must match the source project')
    output.mkdir(parents=True)
    internal = output / 'internal'
    internal.mkdir()
    baseline = render(data, size)
    reference = internal / 'opaque_baseline.png'
    Image.fromarray(baseline).save(reference)
    shutil.copyfile(project / 'source.png', output / 'source.png')
    shutil.copyfile(reference, output / 'opaque_baseline.png')
    code, audit, _ = audit_native(tool, svg, reference, project / 'source.png', internal / 'audit',
        flatten_tolerance, area_tolerance, path_weight, hole_weight)
    if code or not audit or not audit.get('bucket_export', {}).get('passed'):
        raise ValueError('The opaque baseline failed native export validation')
    if resume_search is not None:
        saved = json.loads(gzip.decompress(Path(resume_search).read_bytes()))
        if saved.get('version') != 1 or saved.get('input_svg_sha256') != digest(svg):
            raise ValueError('The saved search does not match the opaque baseline')
        proposals = [([(row['fill'], shapely.from_wkb(bytes.fromhex(row['geometry_wkb'])))
                       for row in trial['stack']], trial['details']) for trial in saved['proposals']]
        search = saved['search']
    else:
        solver = StackSearch(paints, start, stop, shortlist, search_states, tolerance, area_tolerance,
            path_weight, hole_weight, progress=progress, measured_selection=True,
            ownership_tolerance=ownership_tolerance)
        proposals, search = solver.run()
    (output / 'ordering_search.json').write_text(json.dumps(search, indent=2))
    saved = {'version': 1, 'input_svg_sha256': digest(svg), 'search': search,
        'proposals': [{'details': details, 'stack': [{'fill': color, 'geometry_wkb': shape.wkb.hex()}
            for color, shape in stack]} for stack, details in proposals]}
    (output / 'search_plan.json.gz').write_bytes(gzip.compress(json.dumps(saved).encode(), mtime=0))
    visible, _, _ = visible_masks(paints)
    plans = internal / 'plans'
    plans.mkdir()
    for index, (stack, details) in enumerate(proposals[:shortlist]):
        slack = details.get('ownership_tolerance', 0.)
        required, colors, domain, items = partition_stack(stack, visible, area_tolerance, slack)
        completion = Completion(required, colors, domain, area_tolerance=area_tolerance)
        completion.ownership_mode = 'partitioned_colors'
        completion.ownership_tolerance = slack
        completion.frozen_sources = details.get('frozen_sources', {})
        completion.stack_search, completion.stack_details = search, details
        write_plan(plans / f'{index}.json.gz', items, completion, digest(svg), 'complete', [])
    cache = Path(cache).resolve() if cache else internal / 'fit_cache'
    fits = []
    baseline_fit = fill(tool, svg, reference, project / 'source.png', internal / 'baseline_fill', cache,
        evaluations, shape_budget, boundary, inward, area_ratio, render_rmse, local_rmse, max_error, progress,
        silhouette_tolerance, flatten_tolerance, workers, scene_validated, polygon_seed, total_shape_budget)
    fits.append({'name': 'baseline', 'directory': internal / 'baseline_fill', 'fit': baseline_fit,
        'geometry': audit['after'], 'score': compact_score(baseline_fit, shape_weight, region_weight),
        'baseline_to_filled': None})
    for index, (stack, details) in enumerate(proposals[:shortlist]):
        plan = plans / f'{index}.json.gz'
        geometry_directory = internal / f'geometry_{index}'
        try:
            geometry = simplify(project, svg, geometry_directory, tool, tolerance=tolerance,
                area_tolerance=area_tolerance, path_weight=path_weight, hole_weight=hole_weight,
                render_rmse=render_rmse, local_rmse=local_rmse, max_error=max_error,
                source_growth=source_growth, search='bottom-up', resume_plan=plan, progress=progress,
                flatten_tolerance=flatten_tolerance)
        except (ValueError, RuntimeError) as failure:
            progress({'candidate': index, 'geometry_rejected': str(failure)})
            fits.append({'name': f'candidate_{index}', 'directory': geometry_directory,
                'fit': {'accepted': False, 'verified': False, 'shape_count': None, 'error': str(failure)},
                'score': None, 'geometry': None, 'baseline_to_filled': None})
            continue
        metric_only = not geometry['accepted'] and geometry['rejections'] \
            and all(row['reason'] == 'no_encoded_complexity_reduction' for row in geometry['rejections'])
        if not geometry['accepted'] and not metric_only:
            progress({'candidate': index, 'geometry_rejected': geometry['rejections']})
            continue
        candidate_svg = geometry_directory / ('native/optimized.svg' if metric_only else 'optimized.svg')
        candidate_reference = geometry_directory / 'fitting_reference.png'
        Image.fromarray(render(candidate_svg.read_bytes(), size)).save(candidate_reference)
        fit_directory = internal / f'candidate_fill_{index}'
        measured = fill(tool, candidate_svg, candidate_reference,
            project / 'source.png', fit_directory, cache, evaluations, shape_budget, boundary,
            inward, area_ratio, render_rmse, local_rmse, max_error, progress, silhouette_tolerance, flatten_tolerance,
            workers, scene_validated, polygon_seed, total_shape_budget)
        cumulative = None
        if measured.get('scene_candidate') and (fit_directory / 'generated_compact.png').exists():
            actual = np.asarray(Image.open(fit_directory / 'generated_compact.png').convert('RGBA'))
            cumulative = image_error(baseline, actual)
            source_error = image_error(original, actual)
            source_reference = (baseline_fit['original_to_filled'] if baseline_fit.get('accepted')
                else image_error(original, baseline))
            if cumulative['rmse'] > render_rmse or cumulative['local_rmse'] > local_rmse \
                    or cumulative['max_channel_error'] > max_error \
                    or source_error['rmse'] > source_reference['rmse'] + source_growth:
                measured['accepted'] = False
                measured.setdefault('rejections', []).append('Cumulative filled-image error budget exceeded')
        candidate_metrics = json.loads((geometry_directory / 'native/report.json').read_text())['after']
        fits.append({'name': f'candidate_{index}', 'directory': fit_directory, 'fit': measured,
            'geometry': candidate_metrics, 'geometry_proxy_increased': metric_only,
            'score': compact_score(measured, shape_weight, region_weight), 'baseline_to_filled': cumulative})
    valid = [trial for trial in fits if trial['score'] is not None]
    valid.sort(key=lambda trial: (trial['score'], trial['name'] != 'baseline', trial['fit']['shape_count'],
        trial['fit']['region_count'], trial['name']))
    selected = valid[0] if valid else None
    if baseline_fit.get('accepted'):
        shutil.copyfile(internal / 'baseline_fill/generated_compact.3so', output / 'before.3so')
        shutil.copyfile(internal / 'baseline_fill/generated_compact.png', output / 'before.png')
    if selected:
        shutil.copyfile(selected['directory'] / 'generated_compact.3so', output / 'after.3so')
        shutil.copyfile(selected['directory'] / 'generated_compact.png', output / 'after.png')
        after = np.asarray(Image.open(output / 'after.png').convert('RGBA'))
        Image.fromarray(np.abs(original[:, :, :3].astype(np.int16) - after[:, :, :3]).astype(np.uint8)).save(output / 'source_difference.png')
        if (output / 'before.png').exists():
            before = np.asarray(Image.open(output / 'before.png').convert('RGBA'))
            Image.fromarray(np.abs(before[:, :, :3].astype(np.int16) - after[:, :, :3]).astype(np.uint8)).save(output / 'difference.png')
    report = {'method': 'Occlusion-constrained contour replacement and measured Compact Fit selection',
        'accepted': selected is not None,
        'improved': selected is not None and baseline_fit.get('accepted', False)
            and selected['score'] < compact_score(baseline_fit, shape_weight, region_weight),
        'recovered_valid_output': selected is not None and not baseline_fit.get('accepted', False),
        'status': 'filled' if selected else 'no_valid_filled_output',
        'native_dimensions': size, 'downsampled': False, 'palette_unchanged': True, 'leeway_used': False,
        'input': {'project': str(project), 'svg_sha256': digest(svg), 'source_sha256': digest(project / 'source.png')},
        'objective': {'shape_weight': shape_weight, 'region_weight': region_weight,
            'shape_count_is_measured': True, 'region_count_is_connected_bucket_targets': True},
        'quality_limits': {'render_rmse': render_rmse, 'local_rmse': local_rmse, 'maximum_channel_error': max_error,
            'source_rmse_growth': source_growth, 'compact_boundary_allowance': boundary,
            'compact_inward_allowance': inward, 'compact_area_error_ratio': area_ratio,
            'silhouette_area_tolerance': silhouette_tolerance, 'flatten_tolerance': flatten_tolerance,
            'ownership_boundary_tolerance': ownership_tolerance},
        'implementation_sha256': implementation,
        'source_to_opaque': image_error(original, baseline), 'selected': selected['name'] if selected else None,
        'before_shape_count': baseline_fit.get('shape_count') if baseline_fit.get('accepted') else None,
        'after_shape_count': selected['fit']['shape_count'] if selected else None,
        'before_region_count': baseline_fit.get('region_count'),
        'after_region_count': selected['fit']['region_count'] if selected else None,
        'trials': [{**trial, 'directory': str(trial['directory'].relative_to(output))} for trial in fits],
        'runtime_seconds': time.monotonic() - started}
    (output / 'report.json').write_text(json.dumps(report, indent=2))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('project', type=Path)
    parser.add_argument('--svg', type=Path, required=True, help='Internal validated opaque contour input')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--strategy', choices=('budgeted', 'bidirectional'), default='budgeted')
    parser.add_argument('--total-shape-budget', type=int, default=3000)
    parser.add_argument('--max-edits', type=int, default=2400)
    parser.add_argument('--cleanup-edits', type=int, default=160)
    parser.add_argument('--curve-tolerance', type=float, default=1.)
    parser.add_argument('--source-local-growth', type=float)
    parser.add_argument('--resume-stack', type=Path)
    parser.add_argument('--native-tool', type=Path, default=Path(__file__).resolve().parents[2] / 'build/Release/fls_opaque_regions.exe')
    parser.add_argument('--window-start', type=int, default=0)
    parser.add_argument('--window-stop', type=int)
    parser.add_argument('--search-states', type=int, default=32)
    parser.add_argument('--shortlist', type=int, default=2)
    parser.add_argument('--boundary-tolerance', type=float, default=1.)
    parser.add_argument('--weight-paths', type=float, default=256.)
    parser.add_argument('--weight-holes', type=float, default=512.)
    parser.add_argument('--weight-shapes', type=float, default=1.)
    parser.add_argument('--weight-regions', type=float, default=256.)
    parser.add_argument('--render-rmse', type=float, default=4.)
    parser.add_argument('--local-rmse', type=float, default=24.)
    parser.add_argument('--max-error', type=float, default=255.)
    parser.add_argument('--source-rmse-growth', type=float)
    parser.add_argument('--silhouette-tolerance', type=float, default=1.)
    parser.add_argument('--flatten-tolerance', type=float, default=.05)
    parser.add_argument('--ownership-tolerance', type=float, default=.25)
    parser.add_argument('--compact-fit-evaluations', type=int, default=60000)
    parser.add_argument('--compact-fit-workers', type=int, default=4)
    parser.add_argument('--compact-fit-shape-budget', type=int, default=3000)
    parser.add_argument('--compact-fit-boundary', type=float)
    parser.add_argument('--compact-fit-inward', type=float)
    parser.add_argument('--compact-fit-area-ratio', type=float)
    parser.add_argument('--compact-fit-cache', type=Path)
    parser.add_argument('--resume-search', type=Path)
    parser.add_argument('--scene-validated-fills', action='store_true')
    parser.add_argument('--polygon-seeds', action='store_true')
    args = parser.parse_args()
    try:
        if args.strategy == 'budgeted':
            from budgeted import DEFAULT_LOCAL_GROWTH, DEFAULT_SOURCE_GROWTH, run as budgeted_run
            report = budgeted_run(args.project, args.svg, args.output, args.native_tool,
                total_shape_budget=args.total_shape_budget, evaluations=args.compact_fit_evaluations,
                workers=args.compact_fit_workers,
                source_growth=DEFAULT_SOURCE_GROWTH if args.source_rmse_growth is None else args.source_rmse_growth,
                local_growth=DEFAULT_LOCAL_GROWTH if args.source_local_growth is None else args.source_local_growth,
                render_rmse=args.render_rmse,
                local_rmse=args.local_rmse, silhouette_tolerance=args.silhouette_tolerance,
                cache=args.compact_fit_cache, resume_stack=args.resume_stack, max_edits=args.max_edits,
                cleanup_edits=args.cleanup_edits, curve_tolerance=args.curve_tolerance,
                flatten_tolerance=args.flatten_tolerance, max_error=args.max_error,
                boundary=2. if args.compact_fit_boundary is None else args.compact_fit_boundary,
                inward=.5 if args.compact_fit_inward is None else args.compact_fit_inward,
                area_ratio=.02 if args.compact_fit_area_ratio is None else args.compact_fit_area_ratio,
                shape_budget=args.compact_fit_shape_budget,
                progress=lambda entry: print(json.dumps(entry), file=sys.stderr, flush=True))
        else:
            report = run(args.project, args.svg, args.output, args.native_tool,
            start=args.window_start, stop=args.window_stop, search_states=args.search_states,
            shortlist=args.shortlist, tolerance=args.boundary_tolerance, path_weight=args.weight_paths,
            hole_weight=args.weight_holes, shape_weight=args.weight_shapes, region_weight=args.weight_regions,
            render_rmse=args.render_rmse, local_rmse=args.local_rmse, max_error=args.max_error,
            source_growth=1. if args.source_rmse_growth is None else args.source_rmse_growth,
            evaluations=args.compact_fit_evaluations,
            shape_budget=args.compact_fit_shape_budget,
            boundary=2. if args.compact_fit_boundary is None else args.compact_fit_boundary,
            inward=.5 if args.compact_fit_inward is None else args.compact_fit_inward,
            area_ratio=.02 if args.compact_fit_area_ratio is None else args.compact_fit_area_ratio,
            cache=args.compact_fit_cache, silhouette_tolerance=args.silhouette_tolerance,
            flatten_tolerance=args.flatten_tolerance, workers=args.compact_fit_workers,
            resume_search=args.resume_search,
            ownership_tolerance=args.ownership_tolerance,
            scene_validated=args.scene_validated_fills,
            polygon_seed=args.polygon_seeds,
            total_shape_budget=args.total_shape_budget,
            progress=lambda entry: print(json.dumps(entry), file=sys.stderr, flush=True))
        print(json.dumps({key: value for key, value in report.items() if key != 'trials'}, indent=2))
        return 0 if report['accepted'] else 3
    except (ValueError, OSError, RuntimeError, KeyError) as failure:
        print(str(failure), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
