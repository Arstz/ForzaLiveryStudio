"""Headless CLI contract tests using only the Python standard library."""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def png(path, size=64, pixel=None):
    def chunk(kind, payload):
        data = kind + payload
        return struct.pack('>I', len(payload)) + data + struct.pack('>I', zlib.crc32(data))
    header = struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)
    rows = (b'\0' + bytes((255, 0, 0)) * size) * size if pixel is None else b''.join(
        b'\0' + bytes(channel for x in range(size) for channel in pixel(x, y)) for y in range(size))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header)
                     + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    tool = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='fls-opaque-') as directory:
        root = Path(directory)
        reference = root / 'splat.png'
        png(reference)
        baseline = root / 'baseline.svg'
        baseline.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
                            '<path fill="red" d="M0 0H32V64H0Z"/>'
                            '<path fill="red" d="M32 0H64V64H32Z"/></svg>')

        def run(name, source=baseline, reference=reference, extra=(), code=0):
            output = root / name
            result = subprocess.run([str(tool), str(source), '--reference', str(reference),
                                     '--output', str(output),
                                     *([] if '--strategy' in extra else ['--strategy', 'legacy']),
                                     *extra], capture_output=True, text=True)
            assert result.returncode == code, (result.returncode, result.stdout, result.stderr)
            report = json.loads((output / 'report.json').read_text())
            assert (output / 'before.png').read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
            return report, output

        report, output = run('merge')
        assert report['status'] == 'optimized', report
        assert report['before']['draw_items'] == 2 and report['after']['draw_items'] == 1, report
        assert report['svg_before']['total_path_count'] == 2 and report['svg_after']['total_path_count'] == 1
        assert report['before']['svg_segments'] == 8 and report['after']['svg_segments'] == 4
        assert report['before']['paths_above_one_per_fill'] == 1
        assert report['after']['paths_above_one_per_fill'] == 0
        assert report['options']['weight_native_items'] == 0 and report['compact_fit']['measured'] is False
        assert report['baseline_to_optimized']['rmse'] == 0, report
        assert report['source_to_opaque']['rmse'] == 0, report
        assert report['native_silhouette_difference_area'] <= 1e-6, report
        assert '<path' in (output / 'optimized_fls.svg').read_text()
        again, repeated = run('repeat')
        assert report['after'] == again['after']
        assert (output / 'optimized.svg').read_bytes() == (repeated / 'optimized.svg').read_bytes()

        unchanged, _ = run('unchanged', output / 'optimized.svg')
        assert unchanged['status'] == 'unchanged' and unchanged['accepted'] == 0

        wrong = root / 'wrong.svg'
        wrong.write_text(baseline.read_text().replace('fill="red"', 'fill="blue"'))
        rejected, rejected_output = run('rejected', wrong, code=3)
        assert rejected['status'] == 'baseline_rejected' and rejected['accepted'] == 0
        assert rejected['before'] is None and rejected['baseline_to_optimized'] is None
        assert rejected['svg_before']['total_path_count'] == rejected['svg_after']['total_path_count'] == 2
        assert (rejected_output / 'baseline.svg').read_bytes() == (rejected_output / 'optimized.svg').read_bytes()

        transparent = root / 'translucent.svg'
        transparent.write_text(baseline.read_text().replace('fill="red"', 'fill="red" opacity="0.5"'))
        nonopaque, nonopaque_output = run('nonopaque', transparent, code=3)
        assert nonopaque['opacity']['nonopaque_fills'] == 2
        assert nonopaque['translucent_to_forced_opaque']['rmse'] > 0
        assert not (nonopaque_output / 'optimized.svg').exists()

        invalid = subprocess.run([str(tool), str(baseline), '--reference', str(reference),
                                 '--output', str(root / 'invalid'), '--passes', '0.5'], capture_output=True)
        assert invalid.returncode == 2
        original_bytes = baseline.read_bytes()
        collision = subprocess.run([str(tool), str(baseline), '--reference', str(reference),
                                    '--output', str(root)], capture_output=True)
        assert collision.returncode == 2 and baseline.read_bytes() == original_bytes
        structural, structural_output = run('structural', wrong,
            extra=['--structural', '--strategy', 'savings', '--compare-strategies'])
        assert structural['conversion_passed'] is False and structural['structural_mode'] is True
        assert structural['baseline_to_optimized']['rmse'] == 0
        assert structural['svg_after']['total_path_count'] == 1
        assert len(structural['strategies']) == 3
        assert structural['validation_renderer'] == 'svg_paths'
        assert structural['before']['draw_items'] is None
        assert not (structural_output / 'optimized_fls.svg').exists()
        fitted, _ = run('compact-fit', extra=['--strategy', 'savings', '--compact-fit-limit', '4'])
        benchmark = fitted['compact_fit']
        assert benchmark['measured'] and benchmark['leeway_used'] is False
        assert benchmark['before']['complete'] and benchmark['before']['placements'] == 2
        assert benchmark['after']['complete'] and benchmark['after']['placements'] == 1
        for scene in ('before', 'after'):
            assert benchmark[scene]['image_error']['rmse'] == 0
            assert all(p['diagnostics']['leewayArea'] == 0 for p in benchmark[scene]['per_path'])
        incremental, incremental_output = run('incremental', extra=['--strategy', 'incremental', '--compact-fit-sample', '1'])
        assert incremental['status'] == 'optimized' and incremental['selected_strategy'] == 'incremental'
        assert incremental['after']['paths'] == 1 and incremental['after']['svg_coordinate_pairs'] == 5
        assert incremental['baseline_to_optimized']['rmse'] == 0
        assert incremental['options']['weight_controls'] == 1
        assert incremental['diagnostics'][0]['full_audits'] > 0
        assert incremental['bucket_input']['passed'] and incremental['bucket_export']['passed']
        raster_reference, all_mask, inset_mask = root / 'raster_target.png', root / 'all_mask.png', root / 'inset_mask.png'
        png(raster_reference, pixel=lambda x, y: (0, 0, 0) if 16 <= x < 48 and 16 <= y < 48 else (255, 255, 255))
        png(all_mask, pixel=lambda x, y: (255, 255, 255))
        png(inset_mask, pixel=lambda x, y: (255, 255, 255) if 16 <= x < 48 and 16 <= y < 48 else (0, 0, 0))
        manifest = root / 'raster_manifest.json'
        manifest.write_text(json.dumps({'version': 1, 'width': 64, 'height': 64, 'fill_opacity': 1,
            'items': [{'mask': all_mask.name, 'fill': '#ffffff'}, {'mask': inset_mask.name, 'fill': '#000000'}]}))
        traced, traced_output = run('raster_trace', manifest, raster_reference,
            extra=['--raster-regions', '--strategy', 'incremental', '--passes', '0'])
        assert traced['raster_trace']['mask_count'] == 2 and not traced['raster_trace']['resized']
        assert traced['bucket_export']['passed'] and traced['after']['paths'] == traced['after']['fills'] == 2
        assert traced['source_to_opaque']['rmse'] == 0 and traced['opacity']['nonopaque_fills'] == 0
        soft_manifest = root / 'soft_manifest.json'
        soft_mask = root / 'soft_mask.png'
        png(soft_mask, pixel=lambda x, y: (128, 128, 128))
        soft_manifest.write_text(manifest.read_text().replace(inset_mask.name, soft_mask.name))
        soft = subprocess.run([str(tool), str(soft_manifest), '--raster-regions', '--reference', str(raster_reference),
                               '--output', str(root / 'soft_trace')], capture_output=True)
        assert soft.returncode == 2 and b'binary' in soft.stderr
        clipped_curves = root / 'clipped_curves.svg'
        clipped_curves.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="red" d="M-0.0000000001 16C-0.0000000001 9.372583 5.372583 4 12 4'
            'C18.627417 4 24 9.372583 24 16C24 22.627417 18.627417 28 12 28'
            'C5.372583 28 -0.0000000001 22.627417 -0.0000000001 16Z'
            'M56 46C56 51.522847 51.522847 56 46 56C40.477153 56 36 51.522847 36 46'
            'C36 40.477153 40.477153 36 46 36C51.522847 36 56 40.477153 56 46Z"/></svg>')
        clipped, _ = run('clipped_curves', clipped_curves,
            extra=['--structural', '--strategy', 'incremental', '--passes', '0', '--render-rmse', '2'])
        assert clipped['viewport']['changed'] and clipped['bucket_export']['passed']
        assert clipped['after']['bezier_segments'] >= 4, clipped['after']
        sample = incremental['compact_fit_samples'][0]
        assert sample['before']['verified_placements'] == 2 and sample['after']['verified_placements'] == 1
        assert (incremental_output / 'compact_sample_00/report.json').exists()
        protected = incremental_output / 'compact_sample_00/before.svg'
        protected_bytes = protected.read_bytes()
        collision = subprocess.run([str(tool), str(protected), '--reference', str(reference),
            '--output', str(incremental_output), '--strategy', 'incremental', '--compact-fit-sample', '1'], capture_output=True)
        assert collision.returncode == 2 and protected.read_bytes() == protected_bytes

        compound = root / 'compound.svg'
        compound.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
                            '<path fill="red" d="M0 0H16V64H0Z M48 0H64V64H48Z"/></svg>')
        normalized, normalized_output = run('bucket-baseline', compound,
            extra=['--strategy', 'incremental', '--structural', '--passes', '0'])
        assert normalized['status'] == 'normalized' and normalized['accepted'] == 0
        assert normalized['bucket_input']['invalid_targets'] == 1
        assert normalized['bucket_baseline']['passed'] and normalized['bucket_export']['valid_targets'] == 2
        assert normalized['svg_before']['paths'] == 1 and normalized['svg_after']['paths'] == 2
        assert normalized['bucket_baseline']['image_error']['rmse'] == 0
        assert normalized['baseline_to_optimized']['rmse'] == 0
        assert (normalized_output / 'bucket_baseline.svg').exists()
        compound.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
                            '<path fill="red" fill-rule="nonzero" d="M0 0H64V64H0Z M16 16H48V48H16Z"/></svg>')
        winding, _ = run('bucket-fill-rule', compound,
            extra=['--strategy', 'incremental', '--passes', '0'])
        assert winding['bucket_input']['invalid_targets'] == 1
        assert winding['bucket_export']['valid_targets'] == 1 and winding['svg_after']['holes'] == 0
        assert winding['baseline_to_optimized']['rmse'] == 0
        compound.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
                            '<path fill="red" d="M0 0H0.4V64H0Z M0.6 0H1V64H0.6Z"/></svg>')
        unsafe, unsafe_output = run('bucket-coverage-rejected', compound,
            extra=['--strategy', 'incremental', '--structural', '--passes', '0',
                   '--render-rmse', '0', '--render-max-error', '0'], code=3)
        assert not unsafe['bucket_baseline']['passed'] and unsafe['bucket_baseline']['after']['passed']
        assert unsafe['bucket_baseline']['image_error']['rmse'] > 0
        assert unsafe['bucket_baseline']['silhouette_difference_area'] <= 1e-6
        assert not (unsafe_output / 'optimized.svg').exists()
        assert not (unsafe_output / 'bucket_baseline.svg').exists()
        assert (unsafe_output / 'bucket_candidate.svg').exists()
        normalized_only, normalized_only_output = run('bucket-coverage-only', compound,
            extra=['--strategy', 'incremental', '--structural', '--passes', '0',
                   '--render-rmse', '8', '--render-max-error', '255'])
        assert normalized_only['status'] == 'normalized' and normalized_only['accepted'] == 0
        assert normalized_only['input_to_topology_baseline']['rmse'] > 0
        assert normalized_only['topology_baseline_to_optimized']['rmse'] == 0
        assert normalized_only['baseline_to_optimized'] == normalized_only['input_to_topology_baseline']
        assert (normalized_only_output / 'optimization_difference.png').exists()
        degenerate_hole = root / 'degenerate-hole.svg'
        degenerate_hole.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="red" fill-rule="evenodd" d="M0 0H64V64H0Z '
            'M12 12L12.02 12.01L12.00000002 12Z"/></svg>')
        hole_normalized, _ = run('degenerate-hole', degenerate_hole,
            extra=['--strategy', 'supports', '--passes', '0'])
        assert hole_normalized['bucket_baseline']['filled_unconvertible_holes'] > 0
        assert hole_normalized['bucket_baseline']['fallback_triangles'] == 0
        assert hole_normalized['after']['paths'] == 1 and hole_normalized['after']['holes'] == 0
        assert hole_normalized['bucket_export']['passed']
        support_ring = root / 'support-ring.svg'
        support_ring.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<rect width="64" height="64" fill="white"/>'
            '<path fill="red" fill-rule="evenodd" d="M4 4H60V60H4Z M24 24H40V40H24Z"/></svg>')
        ring_result, _ = run('support-ring-fill', support_ring,
            extra=['--strategy', 'supports', '--structural', '--passes', '1',
                   '--render-rmse', '0', '--render-max-error', '0',
                   '--compact-fit-limit', '4', '--compact-fit-evaluations', '6000'])
        assert ring_result['after']['paths'] == 3 and ring_result['after']['holes'] == 0
        assert ring_result['compact_fit']['after']['complete']
        assert ring_result['compact_fit']['after']['placements'] == 3
        assert ring_result['compact_fit']['leeway_used'] is False
        assert ring_result['compact_fit']['after']['image_error']['rmse'] == 0
        color_supports, support_output = run('color-supports',
            extra=['--strategy', 'supports', '--passes', '1', '--visible-area-tolerance', '0.25'])
        assert color_supports['selected_strategy'] == 'supports' and color_supports['after']['paths'] == 1
        assert color_supports['after']['single_path_fills'] == 1
        assert color_supports['approximate_color_merging'] is False
        assert color_supports['options']['visible_color_area'] == 0.25
        assert color_supports['topology_baseline_to_optimized']['rmse'] == 0
        assert color_supports['bucket_export']['valid_targets'] == 1
        assert (support_output / 'ordering.json').exists()
        repeat_supports, repeat_support_output = run('color-supports-repeat',
            extra=['--strategy', 'supports', '--passes', '1', '--visible-area-tolerance', '0.25'])
        assert repeat_supports['after'] == color_supports['after']
        assert (repeat_support_output / 'optimized.svg').read_bytes() == (support_output / 'optimized.svg').read_bytes()
        viewport_source = root / 'viewport.svg'
        viewport_source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
                                   '<rect x="-4" y="-4" width="72" height="72" fill="red"/>'
                                   '<rect x="100" y="100" width="4" height="4" fill="blue"/></svg>')
        windowed, windowed_output = run('viewport-correctness', viewport_source,
            extra=['--strategy', 'incremental', '--passes', '0'])
        assert windowed['viewport']['changed'] and windowed['viewport']['passed']
        assert windowed['viewport']['image_error']['rmse'] == 0
        assert windowed['baseline_to_optimized']['rmse'] == 0
        assert windowed['status'] == 'normalized' and windowed['bucket_export']['valid_targets'] == 1
        assert '-4' not in (windowed_output / 'optimized.svg').read_text()
        junction_source = root / 'junction.svg'
        junction_source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
                                   '<path fill="white" fill-rule="evenodd" d="M0 0H64V64H0Z '
                                   'M16 24H18V26H16Z M22 24H24V26H22Z '
                                   'M16 36H18V38H16Z M22 36H24V38H22Z"/>'
                                   '<path fill="red" d="M0 32L20 12L40 32L20 52Z"/></svg>')
        candidate, candidate_output = run('rejected-ordering-candidate', junction_source,
            extra=['--strategy', 'difficulty', '--structural', '--render-rmse', '0', '--render-max-error', '0'])
        assert candidate['accepted'] == 0 and candidate['ordering_candidate']['selected'] is False
        assert candidate['ordering_candidate']['bucket']['passed']
        assert candidate['ordering_candidate']['image_error']['rmse'] > 0
        assert (candidate_output / 'ordering_candidate.svg').exists()
        assert (candidate_output / 'ordering_candidate.png').exists()
        assert (candidate_output / 'ordering_candidate_difference.png').exists()
        assert (candidate_output / 'optimized.svg').read_bytes() == (candidate_output / 'baseline.svg').read_bytes()

        shades = root / 'shades.svg'
        shades.write_text(baseline.read_text().replace('fill="red"', 'fill="#fc0000"', 1))
        absorption_flags = ['--strategy', 'supports', '--absorb-colors', '--passes', '0',
                            '--render-rmse', '2', '--render-max-error', '8', '--original', str(reference)]
        absorbed, absorbed_output = run('local-absorption', shades, extra=absorption_flags)
        stage = absorbed['color_absorption']
        assert absorbed['status'] == 'optimized' and stage['status'] == 'accepted'
        assert stage['accepted_merges'] == 1
        assert stage['before']['paths'] == 2 and stage['after']['paths'] == 1
        assert absorbed['before']['paths'] == 2 and absorbed['topology_before']['paths'] == 1
        assert stage['quality_target'] == 'original_raster' and stage['source_after']['rmse'] == 0
        assert stage['source_before']['rmse'] > 0 and absorbed['original_to_final']['rmse'] == 0
        assert stage['source_detail']['passed'] and absorbed['bucket_export']['valid_targets'] == 1
        assert absorbed['approximate_color_merging'] and absorbed['options']['color_absorption_enabled']
        assert not (absorbed_output / 'palette_only.svg').exists()
        for name in ('absorbed.svg', 'absorbed.png', 'absorption_difference.png', 'absorption.json'):
            assert (absorbed_output / name).exists()
        repeated_absorption, repeated_output = run('local-absorption-repeat', shades, extra=absorption_flags)
        assert repeated_absorption['after'] == absorbed['after']
        assert (repeated_output / 'optimized.svg').read_bytes() == (absorbed_output / 'optimized.svg').read_bytes()
        disabled_absorption, _ = run('local-absorption-disabled', shades,
            extra=absorption_flags + ['--absorption-passes', '0'])
        assert disabled_absorption['color_absorption']['accepted_merges'] == 0
        assert disabled_absorption['after']['paths'] == 2
        incorrect_shade = root / 'incorrect-shade.svg'
        incorrect_shade.write_text(baseline.read_text().replace('fill="red"', 'fill="#800000"', 1))
        corrected, _ = run('source-correction', incorrect_shade,
            extra=absorption_flags + ['--structural', '--render-rmse', '64', '--render-max-error', '255',
                                      '--abstraction-local-rmse', '0'])
        assert corrected['color_absorption']['after']['paths'] == 1
        assert corrected['original_to_final']['rmse'] == 0
        assert corrected['final_absorption_source_detail']['maximum_added_local_rmse'] == 0
        repeated_shade = root / 'repeated-shade.svg'
        repeated_shade.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="#fc0000" d="M0 0H16V64H0Z"/>'
            '<path fill="red" d="M16 0H48V64H16Z"/>'
            '<path fill="#fc0000" d="M48 0H64V64H48Z"/></svg>')
        class_merged, _ = run('joint-color-class', repeated_shade, extra=absorption_flags)
        assert class_merged['before']['paths'] == 3 and class_merged['after']['paths'] == 1
        assert class_merged['before']['fills'] == 2 and class_merged['after']['fills'] == 1
        assert class_merged['original_to_final']['rmse'] == 0
        assert any(entry.get('kind') == 'color-class-absorption-group'
                   and entry['removed_fills'].count('#fc0000') == 2
                   for entry in class_merged['color_absorption']['diagnostics'])
        completion_source = root / 'completion.svg'
        completion_source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="white" d="M0 0H64V64H0Z"/>'
            '<path fill="#780000" d="M4 4H60V32H4Z"/>'
            '<path fill="#7c0000" d="M4 32H32V60H4Z"/>'
            '<path fill="black" d="M32 32H60V60H32Z"/></svg>')
        completion_reference = root / 'completion.png'
        png(completion_reference, pixel=lambda x, y: (255, 255, 255)
            if x < 4 or x >= 60 or y < 4 or y >= 60 else (0, 0, 0)
            if x >= 32 and y >= 32 else (120, 0, 0))
        completion, _ = run('hidden-completion', completion_source, reference=completion_reference,
            extra=['--strategy', 'supports', '--absorb-colors', '--passes', '0',
                   '--render-rmse', '4', '--render-max-error', '8', '--original', str(completion_reference)])
        assert completion['after']['paths'] == 3 and completion['after']['svg_segments'] == 12
        assert completion['original_to_final']['rmse'] == 0
        component_source = root / 'component.svg'
        component_source.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">'
            '<path fill="white" d="M0 0H64V64H0Z"/>'
            '<path fill="#780000" fill-rule="evenodd" d="M8 8H56V56H8Z '
            'M20 20H28V28H20Z M36 36H44V44H36Z"/></svg>')
        component_reference = root / 'component.png'
        png(component_reference, pixel=lambda x, y: (255, 255, 255)
            if x < 8 or x >= 56 or y < 8 or y >= 56 or (36 <= x < 44 and 36 <= y < 44) else (120, 0, 0))
        component, _ = run('component-ownership', component_source, reference=component_reference,
            extra=['--strategy', 'supports', '--absorb-colors', '--passes', '0', '--structural',
                   '--render-rmse', '32', '--render-max-error', '255', '--original', str(component_reference)])
        assert component['before']['paths'] == 2 and component['after']['paths'] == 3
        assert component['after']['holes'] == 0 and component['after']['svg_segments'] == 12
        assert component['original_to_final']['rmse'] == 0 and component['bucket_export']['valid_targets'] == 3
        assert any(entry.get('kind') == 'component-absorption-group'
                   for entry in component['color_absorption']['diagnostics'])
        seam = root / 'seam.svg'
        seam.write_text(baseline.read_text().replace('H32', 'H31.5').replace('M32', 'M31.5'))
        _, seam_input = run('seam-input', seam,
            extra=['--strategy', 'incremental', '--passes', '0', '--structural',
                   '--render-rmse', '64', '--render-max-error', '255'])
        guarded, _ = run('source-guard-without-color-edits', seam,
            extra=['--strategy', 'incremental', '--absorb-colors', '--absorption-passes', '0',
                   '--passes', '1', '--structural', '--render-rmse', '64', '--render-max-error', '255',
                   '--original', str(seam_input / 'before.png')])
        assert guarded['color_absorption']['accepted_merges'] == 0
        assert guarded['accepted'] == 0 and guarded['after']['paths'] == 2
        assert guarded['original_to_final']['rmse'] == 0
        assert any('cumulative input limits' in entry.get('rejection', '') for entry in guarded['diagnostics'])
        abstracted, abstraction_output = run('abstraction', shades,
            extra=['--strategy', 'incremental', '--abstract-colors'])
        assert abstracted['abstraction']['status'] == 'accepted', abstracted['abstraction']
        assert abstracted['before']['paths'] == 2 and abstracted['after']['paths'] == 1
        assert abstracted['opaque_to_abstracted']['rmse'] > 0
        assert abstracted['abstracted_to_optimized']['rmse'] == 0
        assert abstracted['baseline_to_optimized']['rmse'] == abstracted['opaque_to_abstracted']['rmse']
        assert abstracted['abstraction']['component_deletion_enabled'] is False
        assert abstracted['abstraction']['discarded_low_impact_components'] == 0
        assert abstracted['opaque_to_final_appearance']['max_local_rmse'] <= 12
        assert abstracted['abstraction']['color_guards']['max_original_color_drift'] <= 5
        assert abstracted['abstraction']['color_guards']['minimum_significant_boundary_contrast_retention'] >= 0.75
        assert abstracted['opaque_to_palette']['rmse'] > 0
        for name in ('abstracted.svg', 'abstracted.png', 'abstraction_difference.png', 'topology_difference.png',
                     'abstraction.json', 'palette_only.svg', 'palette_only.png', 'palette_difference.png'):
            assert (abstraction_output / name).exists()
        for name in ('regional_candidate.svg', 'regional_candidate.png', 'regional_difference.png'):
            assert (abstraction_output / name).exists()
        assert abstracted['abstraction']['reconstruction']['accepted'] is True
        assert abstracted['abstraction']['reconstruction']['after']['holes'] == 0
        support_only, _ = run('source-support', shades,
            extra=['--strategy', 'incremental', '--abstract-colors', '--skip-region-reconstruction'])
        assert support_only['after']['paths'] == 1 and support_only['opaque_to_final_appearance']['max_local_rmse'] <= 12
        for option, value in (('--abstraction-local-window', '0'), ('--contrast-retention', '1.1')):
            invalid = subprocess.run([str(tool), str(shades), '--reference', str(reference),
                '--output', str(root / 'invalid-color-option'), '--abstract-colors', option, value], capture_output=True)
            assert invalid.returncode == 2
        repeated, repeated_output = run('abstraction-repeat', shades,
            extra=['--strategy', 'incremental', '--abstract-colors'])
        assert (abstraction_output / 'optimized.svg').read_bytes() == (repeated_output / 'optimized.svg').read_bytes()
        cached, _ = run('abstraction', shades, extra=['--strategy', 'incremental', '--abstract-colors'])
        assert cached['abstraction']['partition_cache_hit'] is True
        assert abstracted['after'] == cached['after']
        shades.write_text(shades.read_text().replace('#fc0000', '#fb0000'))
        changed, _ = run('abstraction', shades, extra=['--strategy', 'incremental', '--abstract-colors'])
        assert changed['abstraction']['partition_cache_hit'] is False
        (abstraction_output / 'abstraction_partition.dat').write_bytes(b'corrupt')
        repaired, _ = run('abstraction', shades, extra=['--strategy', 'incremental', '--abstract-colors'])
        assert repaired['abstraction']['partition_cache_hit'] is False
        protected = abstraction_output / 'abstracted.svg'
        protected_bytes = protected.read_bytes()
        collision = subprocess.run([str(tool), str(protected), '--reference', str(reference),
            '--output', str(abstraction_output), '--abstract-colors'], capture_output=True)
        assert collision.returncode == 2 and protected.read_bytes() == protected_bytes
        protected = abstraction_output / 'palette_only.svg'
        protected_bytes = protected.read_bytes()
        collision = subprocess.run([str(tool), str(protected), '--reference', str(reference),
            '--output', str(abstraction_output), '--abstract-colors'], capture_output=True)
        assert collision.returncode == 2 and protected.read_bytes() == protected_bytes
    print('Opaque CLI tests passed: native encoding reduction, zero-error merge, deterministic no-op, '
          'conversion rejection, explicit opacity error, PNG artifacts, invalid options.')


if __name__ == '__main__':
    main()
