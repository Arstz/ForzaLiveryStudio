import argparse
from pathlib import Path

import cv2
import numpy as np
from PIL import Image


# ------------------------------------------------------------
# I/O
# ------------------------------------------------------------

def load_image(path):
    img = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if img is None:
        raise FileNotFoundError(f"Could not read image: {path}")
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB)


def save_rgb(path, arr):
    Image.fromarray(arr.astype(np.uint8), mode="RGB").save(path)


# ------------------------------------------------------------
# Basic preprocessing
# ------------------------------------------------------------

def resize_for_working(img, max_side):
    h, w = img.shape[:2]
    scale = min(max_side / max(h, w), 1.0)
    if scale == 1.0:
        return img
    nw, nh = int(round(w * scale)), int(round(h * scale))
    return cv2.resize(img, (nw, nh), interpolation=cv2.INTER_AREA)


def bilateral_stack(img, passes=2, d=9, sigma_color=35, sigma_space=35):
    out = img.copy()
    for _ in range(passes):
        out = cv2.bilateralFilter(out, d=d, sigmaColor=sigma_color, sigmaSpace=sigma_space)
    return out


def soft_flatten(img, strength=0.18):
    med = cv2.medianBlur(img, 5)
    out = cv2.addWeighted(img, 1.0 - strength, med, strength, 0.0)
    return out


def preserve_high_sat(img, sat_threshold=155, blend=0.30):
    hsv = cv2.cvtColor(img, cv2.COLOR_RGB2HSV)
    sat_mask = hsv[..., 1] >= sat_threshold
    boosted = img.astype(np.float32)
    out = img.astype(np.float32).copy()
    out[sat_mask] = out[sat_mask] * (1.0 - blend) + boosted[sat_mask] * blend
    return np.clip(out, 0, 255).astype(np.uint8), sat_mask


def protect_soft_details(img):
    hsv = cv2.cvtColor(img, cv2.COLOR_RGB2HSV)
    h = hsv[..., 0].astype(np.int32)
    s = hsv[..., 1].astype(np.int32)
    v = hsv[..., 2].astype(np.int32)

    pinkish = ((h >= 160) | (h <= 15)) & (s >= 18) & (v >= 120)
    bluish = ((h >= 85) & (h <= 140)) & (s >= 20) & (v >= 80)
    bright = v >= 175
    soft = pinkish | (bluish & bright)
    soft = cv2.GaussianBlur((soft.astype(np.uint8) * 255), (0, 0), 1.0) > 20
    return soft


def build_hard_edges(img, low=65, high=145, dilate_iter=1, min_component=16):
    gray = cv2.cvtColor(img, cv2.COLOR_RGB2GRAY)
    edges = cv2.Canny(gray, low, high)
    if dilate_iter > 0:
        kernel = np.ones((3, 3), np.uint8)
        edges = cv2.dilate(edges, kernel, iterations=dilate_iter)

    n, comps, stats, _ = cv2.connectedComponentsWithStats((edges > 0).astype(np.uint8), connectivity=8)
    keep = np.zeros_like(edges)
    for comp_id in range(1, n):
        area = stats[comp_id, cv2.CC_STAT_AREA]
        if area >= min_component:
            keep[comps == comp_id] = 255
    return keep > 0


def build_external_silhouette(img, threshold=245):
    gray = cv2.cvtColor(img, cv2.COLOR_RGB2GRAY)
    fg = gray < threshold
    fg = cv2.morphologyEx(fg.astype(np.uint8), cv2.MORPH_CLOSE, np.ones((5, 5), np.uint8))
    fg = cv2.morphologyEx(fg, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    contours, _ = cv2.findContours(fg, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    mask = np.zeros_like(gray, dtype=np.uint8)
    if contours:
        largest = max(contours, key=cv2.contourArea)
        cv2.drawContours(mask, [largest], -1, 255, thickness=2)
    return mask > 0


# ------------------------------------------------------------
# Quantization in LAB, then subtle reconstruction
# ------------------------------------------------------------

def kmeans_quantize_lab(img, k=16, attempts=5):
    lab = cv2.cvtColor(img, cv2.COLOR_RGB2LAB)
    data = lab.reshape((-1, 3)).astype(np.float32)
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 60, 0.5)
    _, labels, centers = cv2.kmeans(
        data,
        k,
        None,
        criteria,
        attempts,
        cv2.KMEANS_PP_CENTERS,
    )
    centers = np.clip(np.round(centers), 0, 255).astype(np.uint8)
    quant_lab = centers[labels.flatten()].reshape(lab.shape)
    quant_rgb = cv2.cvtColor(quant_lab, cv2.COLOR_LAB2RGB)
    label_map = labels.reshape(img.shape[:2]).astype(np.int32)
    return quant_rgb, label_map, centers


def edge_guided_detail_restore(base_img, quant_img, edge_mask, sat_mask=None, detail_alpha=0.18, sat_alpha=0.12):
    out = quant_img.astype(np.float32).copy()
    base = base_img.astype(np.float32)

    if edge_mask is not None:
        edge = edge_mask.astype(np.float32)
        edge = cv2.GaussianBlur(edge, (5, 5), 0.8)
        edge = np.clip(edge, 0.0, 1.0)[..., None]
        out = out * (1.0 - detail_alpha * edge) + base * (detail_alpha * edge)

    if sat_mask is not None:
        sat = sat_mask.astype(np.float32)
        sat = cv2.GaussianBlur(sat, (5, 5), 0.8)
        sat = np.clip(sat, 0.0, 1.0)[..., None]
        out = out * (1.0 - sat_alpha * sat) + base * (sat_alpha * sat)

    return np.clip(out, 0, 255).astype(np.uint8)


# ------------------------------------------------------------
# Conservative cleanup
# ------------------------------------------------------------

def cleanup_quantized_regions_conservative(
    quant_img,
    label_map,
    protect_mask,
    edge_mask,
    min_region=10,
    max_passes=4,
    contrast_threshold=20.0,
):
    out_labels = label_map.copy()
    kernel = np.ones((3, 3), np.uint8)

    def label_mean(lbl_mask):
        px = quant_img[lbl_mask]
        if len(px) == 0:
            return np.array([0, 0, 0], dtype=np.float32)
        return px.astype(np.float32).mean(axis=0)

    for _ in range(max_passes):
        changed = False
        unique_labels = np.unique(out_labels)
        means = {int(lbl): label_mean(out_labels == lbl) for lbl in unique_labels}

        for lbl in unique_labels:
            lbl = int(lbl)
            mask = (out_labels == lbl).astype(np.uint8)
            n, comps, stats, _ = cv2.connectedComponentsWithStats(mask, connectivity=8)
            for comp_id in range(1, n):
                area = stats[comp_id, cv2.CC_STAT_AREA]
                if area >= min_region:
                    continue

                comp = comps == comp_id
                if np.any(protect_mask & comp):
                    continue
                if np.any(edge_mask & comp):
                    continue

                dilated = cv2.dilate(comp.astype(np.uint8), kernel, iterations=1).astype(bool)
                border = dilated & (~comp)
                neigh = out_labels[border]
                neigh = neigh[neigh != lbl]
                if neigh.size == 0:
                    continue

                vals, counts = np.unique(neigh, return_counts=True)
                comp_mean = quant_img[comp].astype(np.float32).mean(axis=0)
                best_lbl = None
                best_score = None
                for cand_lbl, count in zip(vals, counts):
                    cand_lbl = int(cand_lbl)
                    color_dist = float(np.linalg.norm(comp_mean - means[cand_lbl]))
                    if color_dist > contrast_threshold:
                        continue
                    score = color_dist - 0.15 * float(count)
                    if best_score is None or score < best_score:
                        best_score = score
                        best_lbl = cand_lbl
                if best_lbl is None:
                    continue
                out_labels[comp] = best_lbl
                changed = True

        if not changed:
            break

    used = sorted(np.unique(out_labels))
    remap = {old: new for new, old in enumerate(used)}
    final_labels = np.vectorize(remap.get)(out_labels)

    centers = []
    for old_lbl in used:
        px = quant_img[out_labels == old_lbl]
        if len(px) == 0:
            centers.append(np.array([0, 0, 0], dtype=np.uint8))
        else:
            centers.append(np.round(px.mean(axis=0)).astype(np.uint8))
    final_centers = np.array(centers, dtype=np.uint8)
    final_img = final_centers[final_labels]
    return final_img, final_labels, final_centers


# ------------------------------------------------------------
# Preview compositing
# ------------------------------------------------------------

def blend_soft_edges(img, edge_mask, color=(22, 22, 22), alpha=0.05, blur_sigma=0.7):
    mask = edge_mask.astype(np.float32)
    if blur_sigma > 0:
        k = int(max(3, round(blur_sigma * 6) | 1))
        mask = cv2.GaussianBlur(mask, (k, k), blur_sigma)
    mask = np.clip(mask, 0.0, 1.0)[..., None]
    color_arr = np.array(color, dtype=np.float32).reshape(1, 1, 3)
    img_f = img.astype(np.float32)
    out = img_f * (1.0 - alpha * mask) + color_arr * (alpha * mask)
    return np.clip(out, 0, 255).astype(np.uint8)


def make_palette_image(colors, width_hint=512, height=64):
    colors = np.array(colors, dtype=np.uint8)
    n = max(1, len(colors))
    sw = max(32, width_hint // n)
    palette = np.zeros((height, sw * n, 3), dtype=np.uint8)
    for i, c in enumerate(colors):
        palette[:, i * sw:(i + 1) * sw] = c
    return palette


# ------------------------------------------------------------
# Main
# ------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="Manual preprocessing experiment v7: LAB quantization + edge-guided restore")
    ap.add_argument("input", help="Input image path")
    ap.add_argument("--outdir", default="manual_preprocess_v7_out", help="Output directory")
    ap.add_argument("--max-side", type=int, default=768)
    ap.add_argument("--colors", type=int, default=16)
    ap.add_argument("--bilateral-passes", type=int, default=2)
    ap.add_argument("--bilateral-d", type=int, default=9)
    ap.add_argument("--sigma-color", type=int, default=35)
    ap.add_argument("--sigma-space", type=int, default=35)
    ap.add_argument("--flatten-strength", type=float, default=0.18)
    ap.add_argument("--edge-low", type=int, default=65)
    ap.add_argument("--edge-high", type=int, default=145)
    ap.add_argument("--edge-dilate", type=int, default=1)
    ap.add_argument("--edge-min-component", type=int, default=16)
    ap.add_argument("--detail-alpha", type=float, default=0.18)
    ap.add_argument("--sat-alpha", type=float, default=0.12)
    ap.add_argument("--sat-threshold", type=int, default=155)
    ap.add_argument("--cleanup", action="store_true", help="Generate conservative cleaned variant")
    ap.add_argument("--min-region", type=int, default=10)
    ap.add_argument("--contrast-threshold", type=float, default=20.0)
    ap.add_argument("--preview-edges", action="store_true", help="Apply subtle edge preview")
    ap.add_argument("--preview-alpha", type=float, default=0.05)
    args = ap.parse_args()

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    img = load_image(args.input)
    work = resize_for_working(img, args.max_side)
    save_rgb(outdir / "01_input_resized.png", work)

    smooth = bilateral_stack(
        work,
        passes=args.bilateral_passes,
        d=args.bilateral_d,
        sigma_color=args.sigma_color,
        sigma_space=args.sigma_space,
    )
    save_rgb(outdir / "02_smoothed.png", smooth)

    flattened = soft_flatten(smooth, strength=args.flatten_strength)
    save_rgb(outdir / "03_soft_flattened.png", flattened)

    sat_preserved, sat_mask = preserve_high_sat(flattened, sat_threshold=args.sat_threshold, blend=0.30)
    save_rgb(outdir / "04_sat_preserved_base.png", sat_preserved)
    sat_vis = np.zeros_like(work)
    sat_vis[sat_mask] = [255, 255, 255]
    save_rgb(outdir / "05_sat_mask.png", sat_vis)

    protect_mask = protect_soft_details(smooth)
    protect_vis = np.zeros_like(work)
    protect_vis[protect_mask] = [255, 180, 180]
    save_rgb(outdir / "06_protect_mask.png", protect_vis)

    hard_edges = build_hard_edges(
        smooth,
        low=args.edge_low,
        high=args.edge_high,
        dilate_iter=args.edge_dilate,
        min_component=args.edge_min_component,
    )
    edge_vis = np.zeros_like(work)
    edge_vis[hard_edges] = [255, 255, 255]
    save_rgb(outdir / "07_hard_edges.png", edge_vis)

    silhouette = build_external_silhouette(smooth)
    sil_vis = np.zeros_like(work)
    sil_vis[silhouette] = [255, 255, 255]
    save_rgb(outdir / "08_external_silhouette.png", sil_vis)

    quant, label_map, centers = kmeans_quantize_lab(sat_preserved, k=args.colors)
    save_rgb(outdir / "09_quantized_raw.png", quant)

    restored = edge_guided_detail_restore(
        base_img=sat_preserved,
        quant_img=quant,
        edge_mask=(hard_edges | silhouette),
        sat_mask=sat_mask,
        detail_alpha=args.detail_alpha,
        sat_alpha=args.sat_alpha,
    )
    save_rgb(outdir / "10_restored_raw.png", restored)

    if args.cleanup:
        cleaned, cleaned_labels, cleaned_centers = cleanup_quantized_regions_conservative(
            quant,
            label_map,
            protect_mask=protect_mask,
            edge_mask=hard_edges | silhouette,
            min_region=args.min_region,
            contrast_threshold=args.contrast_threshold,
        )
        cleaned = edge_guided_detail_restore(
            base_img=sat_preserved,
            quant_img=cleaned,
            edge_mask=(hard_edges | silhouette),
            sat_mask=sat_mask,
            detail_alpha=max(0.08, args.detail_alpha * 0.8),
            sat_alpha=max(0.06, args.sat_alpha * 0.8),
        )
        save_rgb(outdir / "11_quantized_cleaned.png", cleaned)
        final_img = cleaned.copy()
        palette_colors = cleaned_centers
    else:
        save_rgb(outdir / "11_quantized_cleaned.png", restored)
        final_img = restored.copy()
        palette_colors = centers

    if args.preview_edges:
        final_img = blend_soft_edges(final_img, hard_edges | silhouette, alpha=args.preview_alpha)
    save_rgb(outdir / "12_final.png", final_img)

    palette = make_palette_image(palette_colors, width_hint=work.shape[1], height=64)
    save_rgb(outdir / "13_palette.png", palette)

    print(f"Saved outputs to: {outdir.resolve()}")
    print(f"Raw palette size: {len(centers)}")
    print("Suggested starting settings:")
    print("- Best balance: --colors 16 --flatten-strength 0.18")
    print("- More saturated originals: raise --sat-alpha to 0.16")
    print("- More fine edge recovery: raise --detail-alpha to 0.22")
    print("- Cleanup only if needed: --cleanup --min-region 8")
    print("- Light edge preview: --preview-edges --preview-alpha 0.04")


if __name__ == "__main__":
    main()
