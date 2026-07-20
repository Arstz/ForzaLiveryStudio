#!/usr/bin/env python3
"""Benchmark largest-region path simplifiers using supersampled-mask DSSIM."""

from __future__ import annotations

import argparse
import csv
import heapq
import math
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw
from skimage.metrics import structural_similarity


@dataclass
class Candidate:
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

    pen = [(row[1], float(row[2]), float(row[3]))
           for row in sections["optimized_pen_points"]]
    flattened = [(float(row[1]), float(row[2]))
                 for row in sections["flattened_optimized_contour"]]
    return metadata, pen, flattened


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--width", type=int, default=1056)
    parser.add_argument("--height", type=int, default=1434)
    parser.add_argument("--supersample", type=int, default=4)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = args.output or args.log.with_name("region_path_optimization.csv")

    metadata, pen, flattened = parse_log(args.log)
    base = render(flattened, args.width, args.height, args.supersample)
    base_area, base_components, base_holes = raster_topology(base)
    candidates: list[Candidate] = []

    pen_tolerances = sorted(set(
        (0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5,
         2.0, 3.0, 4.0, 6.0, 8.0, 10.0, 12.0, 16.0,
         24.0, 32.0, 48.0, 64.0, 96.0, 128.0, 256.0, 1024.0)
        + tuple(round(value, 3) for value in np.arange(1.0, 1.501, 0.01))
        + tuple(round(value, 2) for value in np.arange(8.0, 12.001, 0.1))))
    for tolerance in pen_tolerances:
        reduced_pen = merge_pen_junctions(pen, tolerance)
        candidates.append(Candidate("pen-junction", f"tolerance={tolerance:g}",
                                    flatten_pen(reduced_pen), len(reduced_pen),
                                    reduced_pen))

    rdp_epsilons = sorted(set(
        (0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0, 1.5,
         2.0, 3.0, 4.0, 6.0, 8.0, 12.0)
        + tuple(round(value, 3) for value in np.arange(0.36, 0.551, 0.01))
        + tuple(round(value, 4) for value in np.arange(0.48, 0.501, 0.001))
        + tuple(round(value, 3) for value in np.arange(0.75, 1.001, 0.005))
        + tuple(round(value, 3) for value in np.arange(1.55, 2.101, 0.05))
        + tuple(round(value, 3) for value in np.arange(4.1, 6.101, 0.1))))
    for epsilon in rdp_epsilons:
        candidates.append(Candidate("closed-rdp", f"epsilon={epsilon:g}",
                                    rdp_closed(flattened, epsilon)))

    vis_targets = sorted(set(
        (1200, 900, 700, 500, 350, 250, 175, 125, 90, 64, 48, 32)
        + tuple(range(350, 501, 10)) + tuple(range(350, 411))
        + tuple(range(125, 176, 5))
        + tuple(range(64, 91, 2))), reverse=True)
    for target in vis_targets:
        candidates.append(Candidate("visvalingam", f"target={target}",
                                    visvalingam(flattened, target)))

    for index, candidate in enumerate(candidates, 1):
        mask = render(candidate.points, args.width, args.height, args.supersample)
        candidate.dssim, candidate.iou = score(base, mask)
        area, candidate.components, candidate.holes = raster_topology(mask)
        candidate.area_delta_percent = ((area - base_area) * 100.0 / base_area
                                        if base_area else 0.0)
        print(f"[{index:02d}/{len(candidates)}] {candidate.family:13s} "
              f"{candidate.setting:18s} points={len(candidate.points):5d} "
              f"pen={candidate.pen_points:4d} DSSIM={candidate.dssim:.8f} "
              f"IoU={candidate.iou:.8f}", flush=True)

    candidates.sort(key=lambda item: (item.family, len(item.points), item.dssim))
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["family", "setting", "points", "pen_points", "dssim", "iou",
                         "binary_area_delta_percent", "components", "holes",
                         "topology_matches"])
        for candidate in candidates:
            writer.writerow([candidate.family, candidate.setting, len(candidate.points),
                             candidate.pen_points, f"{candidate.dssim:.12g}",
                             f"{candidate.iou:.12g}",
                             f"{candidate.area_delta_percent:.12g}",
                             candidate.components, candidate.holes,
                             int(candidate.components == base_components
                                 and candidate.holes == base_holes)])

    print(f"\nInput: source #{metadata.get('source_index', '?')}, "
          f"Pen={len(pen)}, flattened={len(flattened)}")
    for limit in (0.001, 0.005, 0.01):
        print(f"\nBest at DSSIM <= {limit:g}:")
        for family in ("pen-junction", "closed-rdp", "visvalingam"):
            eligible = [item for item in candidates
                        if item.family == family and item.dssim <= limit
                        and item.components == base_components
                        and item.holes == base_holes]
            if not eligible:
                print(f"  {family:13s}: none")
                continue
            best = min(eligible, key=lambda item: (item.pen_points or len(item.points),
                                                   len(item.points), item.dssim))
            effective = best.pen_points or len(best.points)
            print(f"  {family:13s}: {effective} control/path points, "
                  f"{len(best.points)} raster points, DSSIM={best.dssim:.8f}, "
                  f"IoU={best.iou:.8f}, area={best.area_delta_percent:+.5f}%, "
                  f"topology={best.components} components/{best.holes} holes "
                  f"({best.setting})")
            safe_limit = str(limit).replace(".", "p")
            write_points(output.with_name(
                f"region_path_best_{family}_{safe_limit}.csv"), best,
                base_components, base_holes)
    print(f"\nFull results: {output}")


if __name__ == "__main__":
    main()
