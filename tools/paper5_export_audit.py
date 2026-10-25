"""Re-render an existing author checkpoint and audit closed-curve opaque export.

Uses the existing research environment; it does not install or train a model.
"""

import argparse
import hashlib
import importlib
import json
from pathlib import Path
import subprocess
import sys
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="Native checkpoint directory with model.pt and metrics.json")
    parser.add_argument("--experiment-root", type=Path,
                        default=Path(__file__).resolve().parents[1] / "docs/sensetive/vectorization_experiments")
    parser.add_argument("--tool", type=Path,
                        default=Path(__file__).resolve().parents[1] / "build/Release/fls_opaque_regions.exe")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.experiment_root.resolve()))
    import run_paper5 as experiment
    import numpy as np
    import torch
    from PIL import Image

    torch.set_num_threads(2)
    started = time.perf_counter()
    report = json.loads((args.directory / "metrics.json").read_text(encoding="utf-8"))
    if report["working_size"] != report["source_size"]:
        raise ValueError("Checkpoint was downsampled; use a native-resolution checkpoint")
    width, height = report["working_size"]
    model = experiment.GaussianImage_Cholesky(
        loss_type="L2", opt_type="adan", num_curves=report["final_curves"], num_samples=report["samples"],
        H=height, W=width, BLOCK_H=16, BLOCK_W=16, device=torch.device("cuda:0"), lr=0.01,
        mode="closed", bezier_degree=report["degree_argument"], quantize=False).cuda()
    checkpoint = args.directory / "model.pt"
    model.load_state_dict(torch.load(checkpoint, weights_only=True))
    args.output.mkdir(parents=True, exist_ok=True)
    model_module = importlib.import_module(model.__class__.__module__)
    author_rasterizer = model_module.rasterize_gaussians
    gaussian_coverage = None

    def capture_coverage(*arguments, **keywords):
        nonlocal gaussian_coverage
        keywords['return_alpha'] = True
        color, alpha = author_rasterizer(*arguments, **keywords)
        gaussian_coverage = alpha.detach().cpu().numpy()
        return color

    model_module.rasterize_gaussians = capture_coverage
    with torch.no_grad():
        try:
            splat = model()["render"][0].permute(1, 2, 0).cpu().numpy()
        finally:
            model_module.rasterize_gaussians = author_rasterizer
        depths = model.get_depth.cpu().numpy()
        opacity = model.get_opacity.cpu().numpy()
        points = model._control_points.cpu().numpy().astype(np.float64)
        colors = torch.sigmoid(model._features_dc).cpu().numpy()
        raw = model._depth.flatten().cpu().numpy()
    image = Image.fromarray(np.rint(splat * 255).clip(0, 255).astype(np.uint8))
    image.save(args.output / "splat.png")
    interior = depths[:, 2, 0]
    boundary = depths[:, 0, 0]
    count = len(points)
    # Stable front-to-back Gaussian ties put the lower source index in front.
    corrected = np.lexsort((np.arange(count), interior))[::-1]
    legacy = np.argsort(-raw, kind="stable")
    curve_interleavings = 0
    for index in range(count):
        other = np.arange(index + 1, count)
        # Float32 ties use curve-major Gaussian indices in the author renderer.
        later_boundary_before_earlier_interior = boundary[other] < interior[index]
        earlier_boundary_before_later_interior = boundary[index] <= interior[other]
        curve_interleavings += int((later_boundary_before_earlier_interior
                                    & earlier_boundary_before_later_interior).sum())

    def export(order, path, pixel_center_offset=0.0):
        lines = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
                 f'viewBox="0 0 {width} {height}">',
                 f'<path fill="#ffffff" d="M0 0H{width}V{height}H0Z"/>']
        segments = 0
        for index in order:
            controls = (points[index] + 1) * [width / 2, height / 2] + pixel_center_offset
            if not np.isfinite(controls).all():
                raise ValueError(f"Nonfinite controls in curve {index}")
            half = (len(controls) - 2) // 2
            parts = [controls[:half + 2], np.concatenate([controls[half + 1:], controls[:1]])]
            data = f'M{controls[0, 0]:.9f} {controls[0, 1]:.9f}'
            for part in parts:
                for cubic in experiment.cubic_approximation(part, tolerance=0.05):
                    data += 'C' + ' '.join(f'{coordinate:.9f}' for coordinate in cubic[1:].ravel())
                    segments += 1
            color = np.rint(colors[index] * 255).clip(0, 255).astype(int)
            fill = '#%02x%02x%02x' % tuple(color)
            lines.append(f'<path id="curve-{index}" fill="{fill}" fill-rule="nonzero" d="{data}Z"/>')
        path.write_text('\n'.join(lines + ['</svg>']), encoding="utf-8")
        return segments

    export(legacy, args.output / "legacy.svg")
    export(corrected, args.output / "sorted.svg")
    segments = export(corrected, args.output / "opaque.svg", pixel_center_offset=0.5)
    audits = {}
    for name in ("legacy", "sorted", "opaque"):
        output = args.output / name
        completed = subprocess.run([str(args.tool.resolve()), str((args.output / (name + '.svg')).resolve()),
            "--reference", str((args.output / 'splat.png').resolve()), "--output", str(output.resolve()),
            "--passes", "0"], capture_output=True, text=True, encoding="utf-8", errors="replace")
        if completed.returncode not in (0, 3):
            raise RuntimeError(completed.stderr or completed.stdout)
        audits[name] = json.loads((output / 'report.json').read_text(encoding="utf-8"))
    import xml.etree.ElementTree as ET
    root = ET.parse(args.output / 'opaque.svg').getroot()
    root.remove(root[0])
    for element in root:
        element.set('fill', '#000000')
    ET.register_namespace('', 'http://www.w3.org/2000/svg')
    ET.ElementTree(root).write(args.output / 'coverage.svg', encoding='utf-8', xml_declaration=True)
    alpha_image = np.zeros((height, width, 4), dtype=np.uint8)
    gaussian_coverage = np.asarray(gaussian_coverage).reshape(height, width)
    alpha_image[:, :, 3] = np.rint(gaussian_coverage * 255).clip(0, 255).astype(np.uint8)
    Image.fromarray(alpha_image).save(args.output / 'gaussian_coverage.png')
    completed = subprocess.run([str(args.tool.resolve()), str((args.output / 'coverage.svg').resolve()),
        '--reference', str((args.output / 'gaussian_coverage.png').resolve()),
        '--output', str((args.output / 'coverage').resolve()), '--passes', '0'],
        capture_output=True, text=True, encoding='utf-8', errors='replace')
    if completed.returncode not in (0, 3):
        raise RuntimeError(completed.stderr or completed.stdout)
    path_coverage = np.asarray(Image.open(args.output / 'coverage/before.png').convert('RGBA'))[:, :, 3] / 255
    coverage_audit = {
        'background_path_excluded': True,
        'mean_absolute_coverage_error': float(np.abs(gaussian_coverage - path_coverage).mean()),
        'missing_coverage_area_pixels': float(np.maximum(gaussian_coverage - path_coverage, 0).sum()),
        'extra_coverage_area_pixels': float(np.maximum(path_coverage - gaussian_coverage, 0).sum()),
        'splat_above_0_99_path_below_0_01_pixels': int(((gaussian_coverage > .99) & (path_coverage < .01)).sum()),
        'note': 'Actual Gaussian foreground alpha versus opaque filled-path coverage, including boundary antialiasing. White curve fills remain part of foreground support.',
    }
    saved = args.directory / 'splat.png'
    replay_error = None
    if saved.exists():
        a, b = np.asarray(Image.open(saved).convert('RGB'), dtype=float), np.asarray(image, dtype=float)
        replay_error = {"rmse": float(np.sqrt(np.mean((a - b) ** 2))),
                        "max_channel_error": float(np.max(np.abs(a - b)))}
    audit = {
        "checkpoint_sha256": digest(checkpoint), "native_size": [width, height], "curves": count,
        "cubic_segments": segments, "exported_curve_paths": count, "missing_curve_paths": 0,
        "qt_captured_curve_paths": audits['opaque']['captured_paths'] - 1,
        "qt_missing_curve_paths": count + 1 - audits['opaque']['captured_paths'],
        "svg_topology": audits['opaque']['svg_before'],
        "all_paths_explicitly_closed": True, "fill_rule": "nonzero", "transforms_baked": True,
        "normalized_to_pixel_transform": "(point + 1) * [width, height] / 2 + 0.5",
        "pixel_center_convention": "The CUDA 2D shader samples at integer coordinates; Qt SVG pixel centers are half-integer. Only curve geometry receives the +0.5 translation.",
        "cubic_approximation_bound_native_pixels": 0.05,
        "depth_sort": "reverse stable front-to-back float32 sigmoid depth; Gaussian index tie-break",
        "changed_order_positions": int((legacy != corrected).sum()),
        "interior_depth_ties": int(count - len(np.unique(interior))),
        "interleaved_boundary_interior_pairs": curve_interleavings,
        "one_path_per_curve_cannot_represent_interleaving": bool(curve_interleavings),
        "splat_opacity": {"minimum": float(opacity.min()), "mean": float(opacity.mean()),
                           "maximum": float(opacity.max()), "fraction_below_0_999": float((opacity < .999).mean())},
        "opacity_note": "Gaussian alpha and spatial coverage are discarded by curve-interior opaque conversion; output is not assumed equivalent.",
        "splat_versus_path_coverage": coverage_audit,
        "saved_splat_replay": replay_error,
        "legacy_source_to_opaque": audits['legacy']['source_to_opaque'],
        "sorting_only_source_to_opaque": audits['sorted']['source_to_opaque'],
        "corrected_source_to_opaque": audits['opaque']['source_to_opaque'],
        "opaque_baseline_validated": audits['opaque']['status'] != 'baseline_rejected',
        "seconds": time.perf_counter() - started,
    }
    (args.output / 'checkpoint_audit.json').write_text(json.dumps(audit, indent=2), encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == '__main__':
    main()
