#!/usr/bin/env python3
"""Benchmark logged region paths and write CSV plus visual DSSIM comparisons.

Safe candidates simplify existing Pen junctions, semi-aggressive candidates use
closed RDP, and aggressive candidates use Visvalingam area removal.
"""

from __future__ import annotations

import argparse
import csv
import heapq
import json
import math
from dataclasses import dataclass
from pathlib import Path

import cv2
import matplotlib.pyplot as plt
import numpy as np
from PIL import Image, ImageDraw
from skimage.metrics import structural_similarity


@dataclass
class Candidate:
    aggression: str
    family: str
    setting: str
    points: list[tuple[float, float]]
    pen_points: int = 0
    controls: list[tuple[str, float, float]] | None = None
    dssim: float = math.inf
    iou: float = 0.0
    area_delta_percent: float = 0.0
    components: int = 0
    holes: int = 0


def parse_log(path: Path):
    metadata: dict[str, str] = {}
    sections: dict[str, list[list[str]]] = {}
    section = ""
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            sections[section] = []
            continue
        if section:
            if line.startswith("count=") or line.startswith("index,"):
                continue
            sections[section].append(next(csv.reader([line])))
        elif "=" in line:
            key, value = line.split("=", 1)
            metadata[key] = value

    source = [(row[1], float(row[2]), float(row[3]))
              for row in sections["source_qpainter_path"]]
    pen = [(row[1], float(row[2]), float(row[3]))
           for row in sections["optimized_pen_points"]]
    flattened = [(float(row[1]), float(row[2]))
                 for row in sections["flattened_optimized_contour"]]
    return metadata, source, pen, flattened


def signed_area(points):
    return sum(points[i][0] * points[(i + 1) % len(points)][1]
               - points[i][1] * points[(i + 1) % len(points)][0]
               for i in range(len(points))) * 0.5


def flatten_qpainter_path(elements, samples=32):
    subpaths = []
    current = []
    index = 0
    while index < len(elements):
        kind, x, y = elements[index]
        point = np.array((x, y), dtype=np.float64)
        if kind == "move":
            if len(current) >= 3:
                subpaths.append(current)
            current = [(x, y)]
            index += 1
            continue
        if not current:
            index += 1
            continue
        if kind == "line":
            current.append((x, y))
            index += 1
            continue
        if kind == "curve-control-1" and index + 2 < len(elements):
            second_kind, second_x, second_y = elements[index + 1]
            end_kind, end_x, end_y = elements[index + 2]
            if second_kind == "curve-data" and end_kind == "curve-data":
                start = np.array(current[-1], dtype=np.float64)
                control1 = point
                control2 = np.array((second_x, second_y), dtype=np.float64)
                end = np.array((end_x, end_y), dtype=np.float64)
                for step in range(1, samples + 1):
                    t = step / samples
                    u = 1.0 - t
                    value = (start * (u ** 3)
                             + control1 * (3.0 * u * u * t)
                             + control2 * (3.0 * u * t * t)
                             + end * (t ** 3))
                    current.append((float(value[0]), float(value[1])))
                index += 3
                continue
        index += 1
    if len(current) >= 3:
        subpaths.append(current)
    for subpath in subpaths:
        if np.linalg.norm(np.subtract(subpath[0], subpath[-1])) <= 1e-9:
            subpath.pop()
    valid = [subpath for subpath in subpaths if len(subpath) >= 3]
    return max(valid, key=lambda points: abs(signed_area(points))) if valid else []


def select_qpainter_subpath(elements):
    subpaths = []
    current = []
    for element in elements:
        if element[0] == "move" and current:
            subpaths.append(current)
            current = []
        current.append(element)
    if current:
        subpaths.append(current)
    valid = [(subpath, flatten_qpainter_path(subpath)) for subpath in subpaths]
    valid = [(subpath, polygon) for subpath, polygon in valid if len(polygon) >= 3]
    return max(valid, key=lambda item: abs(signed_area(item[1]))) if valid else ([], [])


def initial_pen_from_qpainter(elements, closure_tolerance=1e-6):
    if not elements or elements[0][0] != "move":
        return []
    start = np.array(elements[0][1:], dtype=np.float64)
    current = start.copy()
    points = [("hard", float(start[0]), float(start[1]))]
    index = 1
    while index < len(elements):
        kind, x, y = elements[index]
        end = np.array((x, y), dtype=np.float64)
        if kind == "line":
            closes = index == len(elements) - 1 and np.linalg.norm(end - start) <= closure_tolerance
            if not closes:
                points.append(("hard", x, y))
            current = end
            index += 1
            continue
        if kind == "curve-control-1" and index + 2 < len(elements):
            second_kind, second_x, second_y = elements[index + 1]
            end_kind, end_x, end_y = elements[index + 2]
            if second_kind == "curve-data" and end_kind == "curve-data":
                control1 = end
                control2 = np.array((second_x, second_y), dtype=np.float64)
                curve_end = np.array((end_x, end_y), dtype=np.float64)
                control = (control1 * 3.0 + control2 * 3.0
                           - current - curve_end) * 0.25
                points.append(("soft", float(control[0]), float(control[1])))
                closes = (index + 2 == len(elements) - 1
                          and np.linalg.norm(curve_end - start) <= closure_tolerance)
                if not closes:
                    points.append(("hard", end_x, end_y))
                current = curve_end
                index += 3
                continue
        index += 1
    return points


def flatten_pen(points, samples=32):
    first_hard = next((i for i, point in enumerate(points) if point[0] == "hard"), -1)
    if first_hard < 0:
        return []
    ordered = points[first_hard:] + points[:first_hard]
    current = np.array(ordered[0][1:], dtype=np.float64)
    polygon = [tuple(current)]
    index = 1
    count = len(ordered)
    while index <= count:
        nxt = ordered[index % count]
        next_pos = np.array(nxt[1:], dtype=np.float64)
        if nxt[0] == "hard":
            if np.linalg.norm(current - next_pos) > 1e-9:
                polygon.append(tuple(next_pos))
            current = next_pos
            index += 1
            continue
        after = ordered[(index + 1) % count]
        after_pos = np.array(after[1:], dtype=np.float64)
        end = after_pos if after[0] == "hard" else (next_pos + after_pos) * 0.5
        for step in range(1, samples + 1):
            t = step / samples
            u = 1.0 - t
            polygon.append(tuple(current * (u * u) + next_pos * (2.0 * u * t)
                                 + end * (t * t)))
        current = end
        index += 2 if after[0] == "hard" else 1
    if len(polygon) > 1 and np.linalg.norm(np.subtract(polygon[0], polygon[-1])) <= 1e-9:
        polygon.pop()
    return polygon


def merge_pen_junctions(points, tolerance):
    remove = set()
    count = len(points)
    # Keep the first Hard point as the stable cyclic seam, matching the C++ path model.
    seam = next((i for i, point in enumerate(points) if point[0] == "hard"), -1)
    for i, point in enumerate(points):
        if i == seam or point[0] != "hard":
            continue
        previous = points[(i - 1) % count]
        following = points[(i + 1) % count]
        if previous[0] != "soft" or following[0] != "soft":
            continue
        implied_x = (previous[1] + following[1]) * 0.5
        implied_y = (previous[2] + following[2]) * 0.5
        if math.hypot(point[1] - implied_x, point[2] - implied_y) <= tolerance:
            remove.add(i)
    return [point for i, point in enumerate(points) if i not in remove]


def rdp_closed(points, epsilon):
    contour = np.asarray(points, dtype=np.float32).reshape((-1, 1, 2))
    simplified = cv2.approxPolyDP(contour, epsilon, True).reshape((-1, 2))
    return [(float(point[0]), float(point[1])) for point in simplified]


def point_segment_distance(point, start, end):
    point = np.asarray(point, dtype=np.float64)
    start = np.asarray(start, dtype=np.float64)
    end = np.asarray(end, dtype=np.float64)
    edge = end - start
    length_squared = float(np.dot(edge, edge))
    if length_squared <= 1e-18:
        return float(np.linalg.norm(point - start))
    at = float(np.clip(np.dot(point - start, edge) / length_squared, 0.0, 1.0))
    return float(np.linalg.norm(point - (start + edge * at)))


def turn_signal(points, radius=4):
    count = len(points)
    result = np.zeros(count, dtype=np.float64)
    for index in range(count):
        previous = np.asarray(points[(index - radius) % count], dtype=np.float64)
        current = np.asarray(points[index], dtype=np.float64)
        following = np.asarray(points[(index + radius) % count], dtype=np.float64)
        incoming = current - previous
        outgoing = following - current
        incoming_length = np.linalg.norm(incoming)
        outgoing_length = np.linalg.norm(outgoing)
        if incoming_length <= 1e-9 or outgoing_length <= 1e-9:
            continue
        cosine = np.clip(np.dot(incoming, outgoing)
                         / (incoming_length * outgoing_length), -1.0, 1.0)
        result[index] = math.acos(float(cosine))
    scale = float(np.percentile(result, 90))
    return np.clip(result / max(scale, 1e-9), 0.0, 3.0)


def vector_angle(left, right):
    left = np.asarray(left, dtype=np.float64)
    right = np.asarray(right, dtype=np.float64)
    denominator = np.linalg.norm(left) * np.linalg.norm(right)
    if denominator <= 1e-18:
        return 0.0
    cosine = np.clip(np.dot(left, right) / denominator, -1.0, 1.0)
    return math.acos(float(cosine))


def simplify_index_chain(points, chain, epsilon, curvature,
                         curvature_weight=0.0, tangent_limit=None):
    keep = {chain[0], chain[-1]}
    stack = [(0, len(chain) - 1)]
    while stack:
        first_position, last_position = stack.pop()
        if last_position - first_position <= 1:
            continue
        first = chain[first_position]
        last = chain[last_position]
        maximum_error = -1.0
        split_position = -1
        for position in range(first_position + 1, last_position):
            index = chain[position]
            distance = point_segment_distance(points[index], points[first], points[last])
            weighted = distance * (1.0 + curvature_weight * curvature[index])
            if weighted > maximum_error:
                maximum_error = weighted
                split_position = position
        tangent_failed = False
        if tangent_limit is not None and last_position - first_position > 1:
            chord = np.subtract(points[last], points[first])
            start_tangent = np.subtract(points[chain[first_position + 1]], points[first])
            end_tangent = np.subtract(points[last], points[chain[last_position - 1]])
            tangent_failed = (vector_angle(chord, start_tangent) > tangent_limit
                              or vector_angle(chord, end_tangent) > tangent_limit)
        if maximum_error > epsilon or tangent_failed:
            if split_position <= first_position or split_position >= last_position:
                split_position = (first_position + last_position) // 2
            split = chain[split_position]
            keep.add(split)
            stack.append((first_position, split_position))
            stack.append((split_position, last_position))
    return keep


def cyclic_rdp_indices(points, epsilon, curvature_weight=0.0,
                       tangent_limit_degrees=None):
    count = len(points)
    if count <= 3:
        return list(range(count))
    anchor = max(range(1, count),
                 key=lambda index: math.dist(points[0], points[index]))
    curvature = turn_signal(points)
    tangent_limit = (math.radians(tangent_limit_degrees)
                     if tangent_limit_degrees is not None else None)
    first_chain = list(range(0, anchor + 1))
    second_chain = list(range(anchor, count)) + [0]
    keep = simplify_index_chain(points, first_chain, epsilon, curvature,
                                curvature_weight, tangent_limit)
    keep.update(simplify_index_chain(points, second_chain, epsilon, curvature,
                                     curvature_weight, tangent_limit))
    return sorted(keep)


def points_at_indices(points, indices):
    return [points[index] for index in indices]


def cyclic_arc_indices(count, start, end):
    result = [start]
    index = start
    while index != end and len(result) <= count:
        index = (index + 1) % count
        result.append(index)
    return result


def quadratic_reconstruction(points, anchor_indices, samples=12,
                             minimum_curve_bow=None):
    if len(anchor_indices) < 3:
        return [], []
    reconstructed = []
    controls = []
    count = len(points)
    for anchor_position, start_index in enumerate(anchor_indices):
        end_index = anchor_indices[(anchor_position + 1) % len(anchor_indices)]
        arc_indices = cyclic_arc_indices(count, start_index, end_index)
        arc = np.asarray([points[index] for index in arc_indices], dtype=np.float64)
        distances = np.zeros(len(arc), dtype=np.float64)
        if len(arc) > 1:
            distances[1:] = np.cumsum(np.linalg.norm(np.diff(arc, axis=0), axis=1))
        total = distances[-1]
        parameters = (distances / total if total > 1e-9
                      else np.linspace(0.0, 1.0, len(arc)))
        start = arc[0]
        end = arc[-1]
        weights = 2.0 * (1.0 - parameters) * parameters
        fixed = ((1.0 - parameters)[:, None] ** 2 * start
                 + parameters[:, None] ** 2 * end)
        denominator = float(np.dot(weights, weights))
        control = ((weights[:, None] * (arc - fixed)).sum(axis=0) / denominator
                   if denominator > 1e-12 else (start + end) * 0.5)
        curve_bow = point_segment_distance(control, start, end) * 0.5
        curved = minimum_curve_bow is None or curve_bow >= minimum_curve_bow
        if not curved:
            control = (start + end) * 0.5
        if anchor_position == 0:
            controls.append(("hard", float(start[0]), float(start[1])))
        if curved:
            controls.append(("soft", float(control[0]), float(control[1])))
        if anchor_position + 1 < len(anchor_indices):
            controls.append(("hard", float(end[0]), float(end[1])))
        for step in range(samples):
            t = step / samples
            u = 1.0 - t
            value = start * (u * u) + control * (2.0 * u * t) + end * (t * t)
            reconstructed.append((float(value[0]), float(value[1])))
    return reconstructed, controls


def triangle_area(a, b, c):
    return abs((b[0] - a[0]) * (c[1] - a[1])
               - (b[1] - a[1]) * (c[0] - a[0])) * 0.5


def visvalingam(points, target):
    count = len(points)
    if target >= count:
        return list(points)
    previous = [(i - 1) % count for i in range(count)]
    following = [(i + 1) % count for i in range(count)]
    alive = [True] * count
    versions = [0] * count
    heap = []

    def queue(i):
        versions[i] += 1
        heapq.heappush(heap, (triangle_area(points[previous[i]], points[i],
                                            points[following[i]]), versions[i], i))

    for i in range(count):
        queue(i)
    remaining = count
    while remaining > max(3, target):
        _, version, i = heapq.heappop(heap)
        if not alive[i] or version != versions[i]:
            continue
        left, right = previous[i], following[i]
        alive[i] = False
        following[left] = right
        previous[right] = left
        remaining -= 1
        queue(left)
        queue(right)
    return [point for i, point in enumerate(points) if alive[i]]


def render(points, width, height, supersample):
    image = Image.new("L", (width * supersample, height * supersample), 0)
    scaled = [(x * supersample, y * supersample) for x, y in points]
    ImageDraw.Draw(image).polygon(scaled, fill=255)
    return np.asarray(image.resize((width, height), Image.Resampling.LANCZOS),
                      dtype=np.uint8)


def score(base, candidate):
    similarity = structural_similarity(base, candidate, data_range=255)
    base_fill = base >= 128
    candidate_fill = candidate >= 128
    union = np.count_nonzero(base_fill | candidate_fill)
    intersection = np.count_nonzero(base_fill & candidate_fill)
    return (1.0 - similarity) * 0.5, intersection / union if union else 1.0


def raster_topology(mask):
    binary = np.asarray(mask >= 128, dtype=np.uint8)
    components = cv2.connectedComponents(binary, connectivity=8)[0] - 1
    contours, hierarchy = cv2.findContours(binary, cv2.RETR_CCOMP,
                                            cv2.CHAIN_APPROX_SIMPLE)
    holes = 0 if hierarchy is None else sum(1 for item in hierarchy[0] if item[3] >= 0)
    return int(np.count_nonzero(binary)), int(components), int(holes)


def write_points(path, candidate, base_components, base_holes):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["aggression", candidate.aggression])
        writer.writerow(["family", candidate.family])
        writer.writerow(["setting", candidate.setting])
        writer.writerow(["points", len(candidate.points)])
        writer.writerow(["pen_points", candidate.pen_points])
        writer.writerow(["dssim", f"{candidate.dssim:.12g}"])
        writer.writerow(["iou", f"{candidate.iou:.12g}"])
        writer.writerow(["binary_area_delta_percent",
                         f"{candidate.area_delta_percent:.12g}"])
        writer.writerow(["reference_components", base_components])
        writer.writerow(["candidate_components", candidate.components])
        writer.writerow(["reference_holes", base_holes])
        writer.writerow(["candidate_holes", candidate.holes])
        writer.writerow(["topology_matches",
                         int(candidate.components == base_components
                             and candidate.holes == base_holes)])
        writer.writerow([])
        if candidate.controls is not None:
            writer.writerow(["index", "type", "x", "y"])
            for i, (kind, x, y) in enumerate(candidate.controls):
                writer.writerow([i, kind, f"{x:.17g}", f"{y:.17g}"])
        else:
            writer.writerow(["index", "x", "y"])
            for i, (x, y) in enumerate(candidate.points):
                writer.writerow([i, f"{x:.17g}", f"{y:.17g}"])


def effective_points(candidate):
    return candidate.pen_points or len(candidate.points)


def topology_matches(candidate, components, holes):
    return candidate.components == components and candidate.holes == holes


def best_candidates(candidates, limits, base_components, baseline_points):
    rows = []
    for limit in limits:
        for aggression in ("safe", "semi-aggressive", "aggressive"):
            eligible = [candidate for candidate in candidates
                        if candidate.aggression == aggression
                        and candidate.dssim <= limit
                        and candidate.components == base_components
                        and effective_points(candidate) < baseline_points]
            if eligible:
                best = min(eligible, key=lambda candidate: (
                    effective_points(candidate), candidate.dssim,
                    len(candidate.points)))
                rows.append((limit, aggression, best))
    return rows


def write_best_csv(path, rows, baseline_points):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["dssim_limit", "aggression", "family", "setting",
                         "effective_points", "raster_points", "point_reduction_percent",
                         "actual_dssim", "iou", "binary_area_delta_percent",
                         "components", "holes"])
        for limit, aggression, candidate in rows:
            points = effective_points(candidate)
            reduction = (baseline_points - points) * 100.0 / baseline_points
            writer.writerow([f"{limit:.12g}", aggression, candidate.family,
                             candidate.setting, points, len(candidate.points),
                             f"{reduction:.12g}", f"{candidate.dssim:.12g}",
                             f"{candidate.iou:.12g}",
                             f"{candidate.area_delta_percent:.12g}",
                             candidate.components, candidate.holes])


def write_tradeoff_chart(path, candidates, best_rows):
    colors = {"safe": "#2ca02c", "semi-aggressive": "#ff9800",
              "aggressive": "#d62728"}
    fig, axes = plt.subplots(1, 2, figsize=(15, 6), constrained_layout=True)
    for aggression, color in colors.items():
        values = [candidate for candidate in candidates
                  if candidate.aggression == aggression and math.isfinite(candidate.dssim)]
        axes[0].scatter([max(candidate.dssim, 1e-9) for candidate in values],
                        [effective_points(candidate) for candidate in values],
                        s=16, alpha=0.55, label=aggression, color=color)
        tier_rows = [(limit, candidate) for limit, tier, candidate in best_rows
                     if tier == aggression]
        axes[1].plot([limit for limit, _ in tier_rows],
                     [effective_points(candidate) for _, candidate in tier_rows],
                     marker="o", linewidth=2, label=aggression, color=color)
    axes[0].set_xscale("log")
    axes[0].set_xlabel("Actual DSSIM (log scale)")
    axes[0].set_ylabel("Effective contour / Pen points")
    axes[0].set_title("All optimization candidates")
    axes[0].grid(True, which="both", alpha=0.25)
    axes[0].legend()
    axes[1].set_xscale("log")
    axes[1].set_xlabel("Allowed DSSIM (log scale)")
    axes[1].set_ylabel("Lowest eligible point count")
    axes[1].set_title("Best result at each DSSIM limit")
    axes[1].grid(True, which="both", alpha=0.25)
    axes[1].legend()
    fig.suptitle("Region path optimization: quality / complexity trade-off")
    fig.savefig(path, dpi=180)
    plt.close(fig)


def best_candidates_by_family(candidates, limits, base_components,
                              base_holes, baseline_points):
    del base_holes
    rows = []
    families = sorted({candidate.family for candidate in candidates})
    for limit in limits:
        for family in families:
            eligible = [candidate for candidate in candidates
                        if candidate.family == family
                        and candidate.dssim <= limit
                        and candidate.components == base_components
                        and effective_points(candidate) < baseline_points]
            if eligible:
                best = min(eligible, key=lambda candidate: (
                    effective_points(candidate), candidate.dssim,
                    len(candidate.points)))
                rows.append((limit, family, best))
    return rows


def write_family_best_csv(path, rows, baseline_points):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["dssim_limit", "family", "setting", "effective_points",
                         "raster_points", "point_reduction_percent",
                         "actual_dssim", "iou", "binary_area_delta_percent",
                         "components", "holes"])
        for limit, family, candidate in rows:
            points = effective_points(candidate)
            reduction = (baseline_points - points) * 100.0 / baseline_points
            writer.writerow([f"{limit:.12g}", family, candidate.setting,
                             points, len(candidate.points), f"{reduction:.12g}",
                             f"{candidate.dssim:.12g}", f"{candidate.iou:.12g}",
                             f"{candidate.area_delta_percent:.12g}",
                             candidate.components, candidate.holes])


def write_family_tradeoff_chart(path, candidates):
    families = sorted({candidate.family for candidate in candidates})
    colors = plt.cm.tab10(np.linspace(0.0, 1.0, max(1, len(families))))
    fig, axis = plt.subplots(figsize=(13, 8), constrained_layout=True)
    for family, color in zip(families, colors):
        values = [candidate for candidate in candidates
                  if candidate.family == family and math.isfinite(candidate.dssim)]
        axis.scatter([max(candidate.dssim, 1e-9) for candidate in values],
                     [effective_points(candidate) for candidate in values],
                     s=28, alpha=0.72, label=family, color=color)
    axis.set_xscale("log")
    axis.set_xlabel("Actual DSSIM (log scale)")
    axis.set_ylabel("Effective hard/soft contour points")
    axis.set_title("Curve-preserving cyclic RDP variants")
    axis.grid(True, which="both", alpha=0.25)
    axis.legend()
    fig.savefig(path, dpi=180)
    plt.close(fig)


def difference_preview(base, candidate):
    base_fill = base >= 128
    candidate_fill = candidate >= 128
    image = np.full((*base.shape, 3), 245, dtype=np.uint8)
    image[base_fill & candidate_fill] = (45, 45, 45)
    image[base_fill & ~candidate_fill] = (230, 65, 65)
    image[~base_fill & candidate_fill] = (40, 170, 220)
    return image


def write_preview_chart(path, base, best_rows, preview_limits,
                        width, height, supersample):
    tiers = ("safe", "semi-aggressive", "aggressive")
    fig, axes = plt.subplots(len(tiers), len(preview_limits),
                             figsize=(5 * len(preview_limits), 5 * len(tiers)),
                             constrained_layout=True)
    row_lookup = {(limit, tier): candidate
                  for limit, tier, candidate in best_rows}
    for row, tier in enumerate(tiers):
        for column, limit in enumerate(preview_limits):
            axis = axes[row, column]
            candidate = row_lookup.get((limit, tier))
            if candidate is None:
                axis.text(0.5, 0.5, "No connected point-reducing candidate",
                          ha="center", va="center")
                axis.set_axis_off()
                continue
            mask = render(candidate.points, width, height, supersample)
            axis.imshow(difference_preview(base, mask))
            axis.set_title(f"{tier}, limit {limit:g}\n"
                           f"{effective_points(candidate)} points, "
                           f"DSSIM {candidate.dssim:.6g}")
            axis.set_axis_off()
    fig.suptitle("Black: matching fill, red: removed, cyan: added")
    fig.savefig(path, dpi=160)
    plt.close(fig)


def write_interactive_viewer(path, source_points, best_rows,
                             comparison_candidates=None):
    """Write an offline canvas viewer for the unique recommended contours."""
    palette = {
        "source": "#ffffff",
        "safe": "#4ade80",
        "semi-aggressive": "#facc15",
        "aggressive": "#fb7185",
    }
    layers = [{
        "name": "Source contour",
        "details": f"Reference · {len(source_points)} raster points",
        "tier": "source",
        "color": palette["source"],
        "visible": True,
        "points": [[round(x, 4), round(y, 4)] for x, y in source_points],
    }]
    if comparison_candidates is None:
        unique = {}
        for limit, tier, candidate in best_rows:
            key = (tier, candidate.family, candidate.setting)
            if key not in unique:
                unique[key] = {"candidate": candidate, "limits": []}
            unique[key]["limits"].append(limit)

        first_in_tier = set()
        for (tier, family, setting), entry in unique.items():
            candidate = entry["candidate"]
            limits = ", ".join(f"{value:g}" for value in entry["limits"])
            visible = tier not in first_in_tier
            first_in_tier.add(tier)
            layers.append({
                "name": f"{tier} · {effective_points(candidate)} points",
                "details": (f"DSSIM {candidate.dssim:.8g} · limits {limits} · "
                            f"{family} {setting}"),
                "tier": tier,
                "color": palette[tier],
                "visible": visible,
                "points": [[round(x, 4), round(y, 4)]
                           for x, y in candidate.points],
            })
    else:
        colors = ("#60a5fa", "#4ade80", "#facc15", "#fb923c",
                  "#fb7185", "#c084fc", "#2dd4bf")
        families = sorted({candidate.family for candidate in comparison_candidates})
        family_colors = {family: colors[index % len(colors)]
                         for index, family in enumerate(families)}
        first_in_family = set()
        ordered = sorted(comparison_candidates,
                         key=lambda candidate: (candidate.family,
                                                effective_points(candidate),
                                                candidate.dssim,
                                                candidate.setting))
        for candidate in ordered:
            visible = candidate.family not in first_in_family
            first_in_family.add(candidate.family)
            layers.append({
                "name": (f"{candidate.family} · "
                         f"{effective_points(candidate)} points"),
                "details": (f"DSSIM {candidate.dssim:.8g} · "
                            f"IoU {candidate.iou:.8g} · {candidate.setting}"),
                "tier": candidate.family,
                "color": family_colors[candidate.family],
                "visible": visible,
                "points": [[round(x, 4), round(y, 4)]
                           for x, y in candidate.points],
            })

    layer_json = json.dumps(layers, separators=(",", ":"))
    template = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Region path optimization viewer</title>
<style>
  :root { color-scheme: dark; font-family: Inter, Segoe UI, sans-serif; }
  * { box-sizing: border-box; }
  body { margin: 0; background: #111318; color: #e8eaf0; overflow: hidden; }
  #app { display: grid; grid-template-columns: minmax(270px, 360px) 1fr;
         height: 100vh; }
  aside { padding: 16px; background: #191c23; border-right: 1px solid #343946;
          overflow: auto; }
  h1 { margin: 0 0 5px; font-size: 18px; }
  .hint { margin: 0 0 14px; color: #aab0bd; font-size: 12px; line-height: 1.45; }
  .buttons { display: flex; flex-wrap: wrap; gap: 7px; margin-bottom: 13px; }
  button { border: 1px solid #424958; border-radius: 6px; padding: 6px 10px;
           background: #292e39; color: #f3f4f7; cursor: pointer; }
  button:hover { background: #363d4b; }
  .control { display: grid; grid-template-columns: 72px 1fr 52px; gap: 8px;
             align-items: center; margin: 9px 0; font-size: 12px; }
  input[type="range"] { min-width: 0; accent-color: #70a5ff; }
  output { color: #c7cbd5; text-align: right; font-variant-numeric: tabular-nums; }
  .layers-title { margin: 16px 0 7px; font-size: 13px; color: #cfd3dc; }
  #layers { display: grid; gap: 6px; }
  .layer { display: grid; grid-template-columns: 20px 12px 1fr; gap: 7px;
           align-items: start; padding: 8px; border: 1px solid #303541;
           border-radius: 6px; background: #20242c; cursor: pointer; }
  .swatch { width: 10px; height: 10px; border-radius: 50%; margin-top: 3px; }
  .name { font-size: 12px; line-height: 1.3; text-transform: capitalize; }
  .details { color: #969dab; font-size: 10px; line-height: 1.35; margin-top: 2px; }
  main { position: relative; min-width: 0; background: #0b0d11; }
  canvas { display: block; width: 100%; height: 100%; cursor: grab; }
  canvas.dragging { cursor: grabbing; }
  #status { position: absolute; right: 12px; bottom: 10px; padding: 5px 8px;
            border-radius: 5px; background: #151922cc; color: #bcc2ce;
            font-size: 11px; pointer-events: none; }
  @media (max-width: 720px) {
    #app { grid-template-columns: 250px 1fr; }
    aside { padding: 10px; }
  }
</style>
</head>
<body>
<div id="app">
  <aside>
    <h1>Contour comparison</h1>
    <p class="hint">Wheel to zoom · drag to pan · rotate applies to all visible
      contours. Each recommendation is embedded, so this file works offline.</p>
    <div class="buttons">
      <button id="fit">Fit</button><button id="reset">Reset view</button>
      <button id="showAll">Show all</button><button id="hideAll">Hide all</button>
    </div>
    <label class="control"><span>Rotation</span><input id="rotation" type="range"
      min="-180" max="180" step="1" value="0"><output id="rotationValue">0°</output></label>
    <label class="control"><span>Opacity</span><input id="opacity" type="range"
      min="0.05" max="1" step="0.05" value="0.85"><output id="opacityValue">85%</output></label>
    <label class="control"><span>Line width</span><input id="lineWidth" type="range"
      min="0.5" max="6" step="0.5" value="2"><output id="lineWidthValue">2 px</output></label>
    <label class="control"><span>Fill</span><input id="fill" type="checkbox"><output></output></label>
    <div class="layers-title">Results</div>
    <div id="layers"></div>
  </aside>
  <main><canvas id="canvas"></canvas><div id="status"></div></main>
</div>
<script>
const layers = __LAYER_DATA__;
const canvas = document.getElementById('canvas');
const ctx = canvas.getContext('2d');
const layerPanel = document.getElementById('layers');
const rotation = document.getElementById('rotation');
const opacity = document.getElementById('opacity');
const lineWidth = document.getElementById('lineWidth');
const fill = document.getElementById('fill');
const status = document.getElementById('status');
const view = { scale: 1, panX: 0, panY: 0, rotation: 0 };
let width = 1, height = 1, dragging = false, lastX = 0, lastY = 0;

for (const layer of layers) {
  const path = new Path2D();
  if (layer.points.length) {
    path.moveTo(layer.points[0][0], layer.points[0][1]);
    for (let index = 1; index < layer.points.length; ++index)
      path.lineTo(layer.points[index][0], layer.points[index][1]);
    path.closePath();
  }
  layer.path = path;
}

const bounds = { minX: Infinity, maxX: -Infinity,
                 minY: Infinity, maxY: -Infinity };
for (const layer of layers) for (const point of layer.points) {
  bounds.minX = Math.min(bounds.minX, point[0]);
  bounds.maxX = Math.max(bounds.maxX, point[0]);
  bounds.minY = Math.min(bounds.minY, point[1]);
  bounds.maxY = Math.max(bounds.maxY, point[1]);
}
const centerX = (bounds.minX + bounds.maxX) / 2;
const centerY = (bounds.minY + bounds.maxY) / 2;

function buildLayerPanel() {
  layerPanel.replaceChildren();
  layers.forEach((layer, index) => {
    const label = document.createElement('label');
    label.className = 'layer';
    const checkbox = document.createElement('input');
    checkbox.type = 'checkbox';
    checkbox.checked = layer.visible;
    checkbox.addEventListener('change', () => { layer.visible = checkbox.checked; draw(); });
    const swatch = document.createElement('span');
    swatch.className = 'swatch';
    swatch.style.background = layer.color;
    const text = document.createElement('span');
    text.innerHTML = `<div class="name">${layer.name}</div><div class="details">${layer.details}</div>`;
    label.append(checkbox, swatch, text);
    layer.checkbox = checkbox;
    layerPanel.append(label);
  });
}

function fitView() {
  const angle = Math.abs(view.rotation * Math.PI / 180);
  const cosine = Math.abs(Math.cos(angle));
  const sine = Math.abs(Math.sin(angle));
  const shapeWidth = Math.max(1, bounds.maxX - bounds.minX);
  const shapeHeight = Math.max(1, bounds.maxY - bounds.minY);
  const rotatedWidth = shapeWidth * cosine + shapeHeight * sine;
  const rotatedHeight = shapeWidth * sine + shapeHeight * cosine;
  view.scale = 0.9 * Math.min(width / rotatedWidth, height / rotatedHeight);
  view.panX = 0;
  view.panY = 0;
  draw();
}

function resize() {
  const rect = canvas.getBoundingClientRect();
  width = Math.max(1, rect.width);
  height = Math.max(1, rect.height);
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * ratio);
  canvas.height = Math.round(height * ratio);
  draw();
}

function draw() {
  const ratio = window.devicePixelRatio || 1;
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.save();
  ctx.translate(width / 2 + view.panX, height / 2 + view.panY);
  ctx.scale(view.scale, view.scale);
  ctx.rotate(view.rotation * Math.PI / 180);
  ctx.translate(-centerX, -centerY);
  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';
  for (const layer of layers) {
    if (!layer.visible) continue;
    ctx.strokeStyle = layer.color;
    ctx.fillStyle = layer.color;
    ctx.globalAlpha = Number(opacity.value);
    ctx.lineWidth = Number(lineWidth.value) / view.scale;
    if (fill.checked) {
      ctx.globalAlpha = Number(opacity.value) * 0.12;
      ctx.fill(layer.path);
      ctx.globalAlpha = Number(opacity.value);
    }
    ctx.stroke(layer.path);
  }
  ctx.restore();
  const visible = layers.filter(layer => layer.visible).length;
  status.textContent = `${visible}/${layers.length} visible · ${view.scale.toFixed(3)}× · ${view.rotation}°`;
}

canvas.addEventListener('wheel', event => {
  event.preventDefault();
  const rect = canvas.getBoundingClientRect();
  const mouseX = event.clientX - rect.left;
  const mouseY = event.clientY - rect.top;
  const factor = Math.exp(-event.deltaY * 0.0015);
  const nextScale = Math.min(1000, Math.max(0.001, view.scale * factor));
  const applied = nextScale / view.scale;
  view.panX = mouseX - width / 2 - (mouseX - width / 2 - view.panX) * applied;
  view.panY = mouseY - height / 2 - (mouseY - height / 2 - view.panY) * applied;
  view.scale = nextScale;
  draw();
}, { passive: false });
canvas.addEventListener('pointerdown', event => {
  dragging = true; lastX = event.clientX; lastY = event.clientY;
  canvas.setPointerCapture(event.pointerId); canvas.classList.add('dragging');
});
canvas.addEventListener('pointermove', event => {
  if (!dragging) return;
  view.panX += event.clientX - lastX; view.panY += event.clientY - lastY;
  lastX = event.clientX; lastY = event.clientY; draw();
});
canvas.addEventListener('pointerup', event => {
  dragging = false; canvas.releasePointerCapture(event.pointerId);
  canvas.classList.remove('dragging');
});

rotation.addEventListener('input', () => {
  view.rotation = Number(rotation.value);
  document.getElementById('rotationValue').value = `${view.rotation}°`;
  draw();
});
opacity.addEventListener('input', () => {
  document.getElementById('opacityValue').value = `${Math.round(opacity.value * 100)}%`;
  draw();
});
lineWidth.addEventListener('input', () => {
  document.getElementById('lineWidthValue').value = `${lineWidth.value} px`;
  draw();
});
fill.addEventListener('change', draw);
document.getElementById('fit').addEventListener('click', fitView);
document.getElementById('reset').addEventListener('click', () => {
  view.rotation = 0; rotation.value = 0;
  document.getElementById('rotationValue').value = '0°'; fitView();
});
document.getElementById('showAll').addEventListener('click', () => {
  layers.forEach(layer => { layer.visible = true; layer.checkbox.checked = true; }); draw();
});
document.getElementById('hideAll').addEventListener('click', () => {
  layers.forEach(layer => { layer.visible = false; layer.checkbox.checked = false; }); draw();
});
window.addEventListener('resize', resize);
buildLayerPanel();
resize();
fitView();
</script>
</body>
</html>
"""
    path.write_text(template.replace("__LAYER_DATA__", layer_json),
                    encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--width", type=int, default=1056)
    parser.add_argument("--height", type=int, default=1434)
    parser.add_argument("--supersample", type=int, default=4)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--curve-focus", action="store_true",
                        help="test curve-preserving cyclic RDP variants near epsilon 1.9")
    args = parser.parse_args()
    output = args.output or Path(__file__).resolve().parent / "region_path_optimization_results.csv"
    output.parent.mkdir(parents=True, exist_ok=True)

    metadata, source, pen, flattened = parse_log(args.log)
    source_subpath, raw_source_outer = select_qpainter_subpath(source)
    source_pen = initial_pen_from_qpainter(source_subpath)
    source_outer = flatten_pen(source_pen)
    if len(source_outer) < 3 or len(raw_source_outer) < 3:
        raise ValueError("The log does not contain a valid source outer contour")
    base = render(source_outer, args.width, args.height, args.supersample)
    base_area, base_components, base_holes = raster_topology(base)
    candidates: list[Candidate] = [
        Candidate("safe", "logged-optimized", "current-fill-result",
                  flatten_pen(pen), len(pen), pen)
    ]
    dssim_limits = (0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005,
                    0.01, 0.02, 0.05)
    preview_limits = (0.001, 0.005, 0.01)

    if args.curve_focus:
        focus_epsilons = tuple(round(value, 2)
                               for value in np.arange(1.6, 2.201, 0.05))
        for epsilon in focus_epsilons:
            candidates.append(Candidate("semi-aggressive", "closed-rdp-opencv",
                                        f"epsilon={epsilon:g}",
                                        rdp_closed(source_outer, epsilon)))
            indices = cyclic_rdp_indices(source_outer, epsilon)
            candidates.append(Candidate("semi-aggressive", "cyclic-rdp",
                                        f"epsilon={epsilon:g}",
                                        points_at_indices(source_outer, indices)))
            reconstructed, controls = quadratic_reconstruction(source_outer, indices)
            candidates.append(Candidate("semi-aggressive", "rdp-quadratic",
                                        f"epsilon={epsilon:g}", reconstructed,
                                        len(controls), controls))
        for epsilon in (1.8, 1.9, 2.0):
            for weight in (0.5, 1.0, 2.0, 4.0):
                indices = cyclic_rdp_indices(source_outer, epsilon,
                                             curvature_weight=weight)
                candidates.append(Candidate(
                    "semi-aggressive", "curvature-weighted-rdp",
                    f"epsilon={epsilon:g},weight={weight:g}",
                    points_at_indices(source_outer, indices)))
        for epsilon in (1.8, 1.9, 2.0):
            indices = cyclic_rdp_indices(source_outer, epsilon)
            for minimum_bow in (0.25, 0.5, 0.75, 1.0):
                reconstructed, controls = quadratic_reconstruction(
                    source_outer, indices, minimum_curve_bow=minimum_bow)
                candidates.append(Candidate(
                    "semi-aggressive", "rdp-hybrid-quadratic",
                    f"epsilon={epsilon:g},minimum_bow={minimum_bow:g}",
                    reconstructed, len(controls), controls))
        for epsilon in (1.8, 1.9, 2.0):
            for angle in (8.0, 12.0, 16.0, 20.0, 30.0):
                indices = cyclic_rdp_indices(source_outer, epsilon,
                                             tangent_limit_degrees=angle)
                candidates.append(Candidate(
                    "semi-aggressive", "tangent-guarded-rdp",
                    f"epsilon={epsilon:g},angle={angle:g}",
                    points_at_indices(source_outer, indices)))
                if epsilon == 1.9 and angle in (12.0, 20.0, 30.0):
                    reconstructed, controls = quadratic_reconstruction(
                        source_outer, indices)
                    candidates.append(Candidate(
                        "semi-aggressive", "tangent-rdp-quadratic",
                        f"epsilon={epsilon:g},angle={angle:g}", reconstructed,
                        len(controls), controls))
    else:
        pen_tolerances = sorted(set(
            (0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5,
             2.0, 3.0, 4.0, 6.0, 8.0, 10.0, 12.0, 16.0,
             24.0, 32.0, 48.0, 64.0, 96.0, 128.0, 256.0, 1024.0)
            + tuple(round(value, 3) for value in np.arange(1.0, 1.501, 0.01))
            + tuple(round(value, 2) for value in np.arange(8.0, 12.001, 0.1))))
        for tolerance in pen_tolerances:
            reduced_pen = merge_pen_junctions(pen, tolerance)
            candidates.append(Candidate(
                "safe", "pen-junction", f"tolerance={tolerance:g}",
                flatten_pen(reduced_pen), len(reduced_pen), reduced_pen))

        rdp_epsilons = sorted(set(
            (0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5,
             2.0, 3.0, 4.0, 6.0, 8.0, 12.0)
            + tuple(round(value, 3) for value in np.arange(0.36, 0.551, 0.01))
            + tuple(round(value, 4) for value in np.arange(0.48, 0.501, 0.001))
            + tuple(round(value, 3) for value in np.arange(0.75, 1.001, 0.005))
            + tuple(round(value, 3) for value in np.arange(1.55, 2.101, 0.05))
            + tuple(round(value, 3) for value in np.arange(4.1, 6.101, 0.1))))
        for epsilon in rdp_epsilons:
            candidates.append(Candidate("semi-aggressive", "closed-rdp",
                                        f"epsilon={epsilon:g}",
                                        rdp_closed(source_outer, epsilon)))

        vis_targets = sorted(set(
            (1200, 900, 700, 500, 350, 250, 175, 125, 90, 64, 48, 32)
            + tuple(range(350, 501, 10)) + tuple(range(350, 411))
            + tuple(range(125, 176, 5))
            + tuple(range(64, 91, 2))), reverse=True)
        for target in vis_targets:
            candidates.append(Candidate("aggressive", "visvalingam",
                                        f"target={target}",
                                        visvalingam(source_outer, target)))

    for index, candidate in enumerate(candidates, 1):
        mask = render(candidate.points, args.width, args.height, args.supersample)
        candidate.dssim, candidate.iou = score(base, mask)
        area, candidate.components, candidate.holes = raster_topology(mask)
        candidate.area_delta_percent = ((area - base_area) * 100.0 / base_area
                                        if base_area else 0.0)
        print(f"[{index:03d}/{len(candidates)}] {candidate.aggression:15s} "
              f"{candidate.family:16s} "
              f"{candidate.setting:18s} points={len(candidate.points):5d} "
              f"pen={candidate.pen_points:4d} DSSIM={candidate.dssim:.8f} "
              f"IoU={candidate.iou:.8f}", flush=True)

    baseline_points = int(metadata.get("original_pen_point_count", len(source_outer)))
    candidates.sort(key=lambda item: (item.aggression, item.family,
                                      effective_points(item), item.dssim))
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["aggression", "family", "setting", "effective_points",
                         "raster_points", "pen_points", "point_reduction_percent",
                         "dssim", "iou", "binary_area_delta_percent", "components",
                         "holes", "topology_matches"])
        for candidate in candidates:
            points = effective_points(candidate)
            reduction = (baseline_points - points) * 100.0 / baseline_points
            writer.writerow([candidate.aggression, candidate.family, candidate.setting,
                             points, len(candidate.points), candidate.pen_points,
                             f"{reduction:.12g}", f"{candidate.dssim:.12g}",
                             f"{candidate.iou:.12g}",
                             f"{candidate.area_delta_percent:.12g}",
                             candidate.components, candidate.holes,
                             int(candidate.components == base_components
                                 and candidate.holes == base_holes)])

    best_output = output.with_name(f"{output.stem}_best_by_dssim.csv")
    tradeoff_chart = output.with_name(f"{output.stem}_tradeoff.png")
    preview_chart = output.with_name(f"{output.stem}_previews.png")
    viewer = output.with_name(f"{output.stem}_viewer.html")
    print(f"\nInput: region #{metadata.get('region_index', '?')}, "
          f"source outer={len(source_outer)}, Pen={len(pen)}, flattened={len(flattened)}")
    if args.curve_focus:
        family_rows = best_candidates_by_family(
            candidates, dssim_limits, base_components, base_holes, baseline_points)
        write_family_best_csv(best_output, family_rows, baseline_points)
        write_family_tradeoff_chart(tradeoff_chart, candidates)
        write_interactive_viewer(viewer, source_outer, [], candidates)
        for limit in preview_limits:
            print(f"\nBest curve-preserving variants at DSSIM <= {limit:g}:")
            matching = [(family, candidate)
                        for row_limit, family, candidate in family_rows
                        if row_limit == limit]
            for family, best in matching:
                print(f"  {family:24s}: {effective_points(best):4d} effective, "
                      f"DSSIM={best.dssim:.8f}, IoU={best.iou:.8f}, "
                      f"area={best.area_delta_percent:+.5f}% ({best.setting})")
    else:
        best_rows = best_candidates(candidates, dssim_limits,
                                    base_components, baseline_points)
        write_best_csv(best_output, best_rows, baseline_points)
        write_tradeoff_chart(tradeoff_chart, candidates, best_rows)
        write_preview_chart(preview_chart, base, best_rows, preview_limits,
                            args.width, args.height, args.supersample)
        write_interactive_viewer(viewer, source_outer, best_rows)
        for limit in preview_limits:
            print(f"\nBest at DSSIM <= {limit:g}:")
            for aggression in ("safe", "semi-aggressive", "aggressive"):
                matching = [candidate for row_limit, tier, candidate in best_rows
                            if row_limit == limit and tier == aggression]
                if not matching:
                    print(f"  {aggression:15s}: none")
                    continue
                best = matching[0]
                effective = effective_points(best)
                print(f"  {aggression:15s}: {effective} control/path points, "
                      f"{len(best.points)} raster points, DSSIM={best.dssim:.8f}, "
                      f"IoU={best.iou:.8f}, area={best.area_delta_percent:+.5f}%, "
                      f"topology={best.components} components/{best.holes} holes "
                      f"({best.setting})")
                safe_limit = str(limit).replace(".", "p")
                write_points(output.with_name(
                    f"region_path_best_{aggression}_{safe_limit}.csv"), best,
                    base_components, base_holes)
    print(f"\nFull results: {output}")
    print(f"Best by DSSIM: {best_output}")
    print(f"Trade-off chart: {tradeoff_chart}")
    if not args.curve_focus:
        print(f"Visual previews: {preview_chart}")
    print(f"Interactive viewer: {viewer}")


if __name__ == "__main__":
    main()
