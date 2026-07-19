from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path
import shutil
import time
from typing import Any

import cv2
import numpy as np
from PIL import Image

from tools.manual_preprocess_test_v7 import (
    bilateral_stack,
    blend_soft_edges,
    build_external_silhouette,
    build_hard_edges,
    cleanup_quantized_regions_conservative,
    edge_guided_detail_restore,
    kmeans_quantize_lab,
    load_image,
    make_palette_image,
    preserve_high_sat,
    protect_soft_details,
    save_rgb,
    soft_flatten,
)


PRESETS: dict[str, dict[str, Any]] = {
    "anime_default": {"colors": 16, "flatten_strength": 0.18, "sat_alpha": 0.16, "detail_alpha": 0.22},
    "anime_soft": {"colors": 16, "flatten_strength": 0.18, "sat_alpha": 0.12, "detail_alpha": 0.14},
    "anime_detail": {"colors": 16, "flatten_strength": 0.18, "sat_alpha": 0.18, "detail_alpha": 0.28},
    "debug_cleanup": {
        "colors": 16,
        "flatten_strength": 0.18,
        "sat_alpha": 0.16,
        "detail_alpha": 0.22,
        "cleanup": True,
        "min_region": 8,
    },
}

DEFAULTS: dict[str, Any] = {
    "bilateral_passes": 2,
    "bilateral_d": 9,
    "sigma_color": 35,
    "sigma_space": 35,
    "edge_low": 65,
    "edge_high": 145,
    "edge_dilate": 1,
    "edge_min_component": 16,
    "sat_threshold": 155,
    "cleanup": False,
    "min_region": 10,
    "contrast_threshold": 20.0,
    "preview_edges": False,
    "preview_alpha": 0.05,
}


@dataclass(frozen=True)
class PreprocessResult:
    output_dir: Path
    selected_target: Path
    artifacts: dict[str, Path]
    settings: dict[str, Any]
    palette_count: int
    runtime_seconds: float
    equivalent_command: list[str]

    def metadata(self) -> dict[str, Any]:
        data = asdict(self)
        data["output_dir"] = str(self.output_dir)
        data["selected_target"] = str(self.selected_target)
        data["artifacts"] = {key: str(value) for key, value in self.artifacts.items()}
        return data


def resolve_preprocess_settings(preset: str = "anime_default", overrides: dict[str, Any] | None = None) -> dict[str, Any]:
    if preset not in PRESETS:
        raise ValueError(f"Unknown preprocessing preset: {preset}")
    settings = {**DEFAULTS, **PRESETS[preset]}
    unknown = sorted(set(overrides or {}) - set(settings))
    if unknown:
        raise ValueError(f"Unknown preprocessing settings: {', '.join(unknown)}")
    settings.update({key: value for key, value in (overrides or {}).items() if value is not None})
    if not 2 <= int(settings["colors"]) <= 256:
        raise ValueError("colors must be between 2 and 256")
    for key in ("flatten_strength", "sat_alpha", "detail_alpha", "preview_alpha"):
        if not 0.0 <= float(settings[key]) <= 1.0:
            raise ValueError(f"{key} must be between 0 and 1")
    if int(settings["edge_low"]) >= int(settings["edge_high"]):
        raise ValueError("edge_low must be less than edge_high")
    return settings


def run_preprocessing(
    input_path: str | Path,
    output_dir: str | Path,
    preset: str = "anime_default",
    overrides: dict[str, Any] | None = None,
) -> PreprocessResult:
    started = time.perf_counter()
    source = Path(input_path)
    if not source.is_file():
        raise FileNotFoundError(f"Input image does not exist: {source}")
    outdir = Path(output_dir)
    outdir.mkdir(parents=True, exist_ok=True)
    settings = resolve_preprocess_settings(preset, overrides)
    cv2.setRNGSeed(0)

    img = load_image(source)
    # The active pipeline deliberately operates in the source coordinate space.
    # Downstream path masks, placements, previews, and exports all retain these
    # exact dimensions, so preprocessing must never introduce a working resize.
    work = img.copy()
    smooth = bilateral_stack(work, int(settings["bilateral_passes"]), int(settings["bilateral_d"]), int(settings["sigma_color"]), int(settings["sigma_space"]))
    flattened = soft_flatten(smooth, float(settings["flatten_strength"]))
    sat_preserved, sat_mask = preserve_high_sat(flattened, int(settings["sat_threshold"]), blend=0.30)
    protect_mask = protect_soft_details(smooth)
    hard_edges = build_hard_edges(smooth, int(settings["edge_low"]), int(settings["edge_high"]), int(settings["edge_dilate"]), int(settings["edge_min_component"]))
    silhouette = build_external_silhouette(smooth)
    quant, label_map, centers = kmeans_quantize_lab(sat_preserved, int(settings["colors"]), attempts=1)
    restored = edge_guided_detail_restore(sat_preserved, quant, hard_edges | silhouette, sat_mask, float(settings["detail_alpha"]), float(settings["sat_alpha"]))

    sat_vis = np.zeros_like(work)
    sat_vis[sat_mask] = 255
    protect_vis = np.zeros_like(work)
    protect_vis[protect_mask] = [255, 180, 180]
    edge_vis = np.zeros_like(work)
    edge_vis[hard_edges] = 255
    silhouette_vis = np.zeros_like(work)
    silhouette_vis[silhouette] = 255
    images = {
        "01_input_original.png": work,
        "02_smoothed.png": smooth,
        "03_soft_flattened.png": flattened,
        "04_sat_preserved_base.png": sat_preserved,
        "05_sat_mask.png": sat_vis,
        "06_protect_mask.png": protect_vis,
        "07_hard_edges.png": edge_vis,
        "08_external_silhouette.png": silhouette_vis,
        "09_quantized_raw.png": quant,
        "10_restored_raw.png": restored,
    }

    palette_colors = centers
    final_img = restored
    if settings["cleanup"]:
        cleaned, _, palette_colors = cleanup_quantized_regions_conservative(
            quant, label_map, protect_mask, hard_edges | silhouette,
            min_region=int(settings["min_region"]), contrast_threshold=float(settings["contrast_threshold"]),
        )
        final_img = edge_guided_detail_restore(sat_preserved, cleaned, hard_edges | silhouette, sat_mask, max(0.08, float(settings["detail_alpha"]) * 0.8), max(0.06, float(settings["sat_alpha"]) * 0.8))
    images["11_quantized_cleaned.png"] = final_img
    if settings["preview_edges"]:
        final_img = blend_soft_edges(final_img, hard_edges | silhouette, alpha=float(settings["preview_alpha"]))
    images["12_final.png"] = final_img
    images["13_palette.png"] = make_palette_image(palette_colors, width_hint=work.shape[1], height=64)

    artifacts: dict[str, Path] = {}
    for name, image in images.items():
        path = outdir / name
        save_rgb(path, image)
        artifacts[name] = path
    label_path = outdir / "09_quantized_labels.png"
    Image.fromarray(label_map.astype(np.uint16)).save(label_path)
    artifacts[label_path.name] = label_path
    canonical = artifacts["12_final.png"] if settings["cleanup"] or settings["preview_edges"] else artifacts["10_restored_raw.png"]
    selected = outdir / "selected_target.png"
    shutil.copyfile(canonical, selected)
    artifacts["selected_target.png"] = selected
    palette_alias = outdir / "palette.png"
    shutil.copyfile(artifacts["13_palette.png"], palette_alias)
    artifacts["palette.png"] = palette_alias
    command = ["python", "tools/manual_preprocess_test_v7.py", str(source), "--outdir", str(outdir)]
    flag_names = {"colors": "colors", "flatten_strength": "flatten-strength", "sat_alpha": "sat-alpha", "detail_alpha": "detail-alpha", "min_region": "min-region"}
    for key, flag in flag_names.items():
        command.extend([f"--{flag}", str(settings[key])])
    if settings["cleanup"]:
        command.append("--cleanup")
    if settings["preview_edges"]:
        command.append("--preview-edges")
    return PreprocessResult(outdir, selected, artifacts, settings, len(palette_colors), time.perf_counter() - started, command)
