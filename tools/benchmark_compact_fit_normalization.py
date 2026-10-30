"""Prepare saved contours and run sequential, quality-checked fitting trials."""

import argparse
import copy
import gzip
import hashlib
import json
import math
from pathlib import Path
import subprocess
import time


IDENTITY = (1, 0, 0, 1, 0, 0)
STRATEGIES = {
    "native8k": ("0", "0", 8000, 0),
    "uniform128": ("uniform128", "0", 6000, 1000),
    "uniform256": ("uniform256", "0", 6000, 1000),
    "uniform512": ("uniform512", "0", 6000, 1000),
    "square128": ("128", "128", 6000, 1000),
    "square256": ("256", "256", 6000, 1000),
    "uniform256_3k": ("uniform256", "0", 3000, 1000),
    "square256_3k": ("256", "256", 3000, 1000),
    "square512": ("512", "512", 6000, 1000),
    "uniform256_12k": ("uniform256", "0", 12000, 2000),
    "square256_12k": ("256", "256", 12000, 2000),
    "uniform256_finish4k": ("uniform256", "0", 6000, 4000),
    "square256_finish4k": ("256", "256", 6000, 4000),
    "anchor256": ("anchor256", "0", 6000, 1000),
    "affine256": ("affine256", "0", 6000, 1000),
    "hardaffine256": ("hardaffine256", "0", 6000, 1000),
    "native60k": ("0", "0", 60000, 0),
}
SCREEN = ("native8k", "uniform256", "square256", "anchor256")


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def compose(parent, local):
    a, b, c, d, x, y = parent
    e, f, g, h, u, v = local
    return (a*e+c*f, b*e+d*f, a*g+c*h, b*g+d*h,
            a*u+c*v+x, b*u+d*v+y)


def scene_transform(values):
    angle = math.radians(values.get("rotation", 0))
    cosine, sine = math.cos(angle), math.sin(angle)
    horizontal, vertical = values.get("scale_x", 1), values.get("scale_y", 1)
    shear = values.get("skew", 0)
    return (cosine*horizontal, sine*horizontal,
            (cosine*shear-sine)*vertical, (sine*shear+cosine)*vertical,
            values.get("x", 0), values.get("y", 0))


def map_point(point, transform, vector=False):
    a, b, c, d, x, y = transform
    return [a*point[0]+c*point[1]+(0 if vector else x),
            b*point[0]+d*point[1]+(0 if vector else y)]


def transform_request(request, transform):
    result = copy.deepcopy(request)
    for loop in result["loops"]:
        for point in loop["points"]:
            if point.get("kind") == "soft" and not point.get("explicitHandles"):
                raise ValueError("Materialize soft-point handles before transforming")
            point["position"] = map_point(point["position"], transform)
            for name in ("incoming", "outgoing"):
                if name in point:
                    point[name] = map_point(point[name], transform, vector=True)
    return result


def request_from_scene(node, transform):
    loops = []
    for kind, points in [("outer", node["points"])]+[("cutout", p) for p in node.get("cutouts", [])]:
        loops.append({"kind": kind, "points": [
            {"position": point["position"],
             "kind": "hard" if point.get("hard", True) else "soft",
             "explicitHandles": point.get("explicit_handles", False),
             "incoming": point.get("incoming", [0, 0]),
             "outgoing": point.get("outgoing", [0, 0])}
            for point in points]})
    return transform_request({"curveModel": "cubic-anchors-v1", "boundaryTolerance": .1, "loops": loops}, transform)


def polygon(points, kind="outer"):
    return {"kind": kind, "points": [{"position": list(p), "kind": "hard",
            "explicitHandles": True, "incoming": [0, 0], "outgoing": [0, 0]} for p in points]}


def ellipse(horizontal, vertical, kind="outer"):
    handle = 4*(math.sqrt(2)-1)/3
    points = []
    for index in range(4):
        angle = index*math.pi/2
        tangent = [-horizontal*math.sin(angle)*handle, vertical*math.cos(angle)*handle]
        points.append({"position": [horizontal*math.cos(angle), vertical*math.sin(angle)],
                       "kind": "soft", "explicitHandles": True,
                       "incoming": [-v for v in tangent], "outgoing": tangent})
    return {"kind": kind, "points": points}


def prepare(output, source):
    (output/"requests").mkdir(parents=True, exist_ok=True)
    cases, fingerprints, inventory = [], {}, []

    def add(name, request, **metadata):
        positions = [p["position"] for loop in request["loops"] for p in loop["points"]]
        width = max(p[0] for p in positions)-min(p[0] for p in positions)
        height = max(p[1] for p in positions)-min(p[1] for p in positions)
        if width <= 0 or height <= 0:
            raise ValueError("Degenerate region: "+name)
        path = (output/"requests"/(name+".json")).resolve()
        write_json(path, {"request": request})
        cases.append(dict(name=name, request=str(path), width=width, height=height,
                          points=len(positions), holes=sum(l["kind"] == "cutout" for l in request["loops"]),
                          hard=sum(p["kind"] == "hard" for l in request["loops"] for p in l["points"]), **metadata))

    for project in sorted(source.rglob("*.3so")):
        raw = project.read_bytes()
        document = json.loads(gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw)

        def walk(node, parent):
            world = compose(parent, scene_transform(node.get("transform", {})))
            if node.get("kind") == "contour" and node.get("closed") and node.get("path_kind", "pen") == "pen":
                request = request_from_scene(node, world)
                origin = request["loops"][0]["points"][0]["position"]
                centered = transform_request(request, (1, 0, 0, 1, -origin[0], -origin[1]))
                rounded = json.loads(json.dumps(centered), parse_float=lambda n: round(float(n), 7))
                digest = hashlib.sha256(json.dumps(rounded, sort_keys=True).encode()).hexdigest()
                entry = dict(project=str(project), id=node["id"], fingerprint=digest)
                if digest in fingerprints:
                    entry["duplicate_of"] = fingerprints[digest]
                else:
                    name = project.parent.name+"_"+project.stem+"_"+node["id"][:20]
                    name += "_"+digest[:8]
                    fingerprints[digest] = name
                    add(name, request, family="saved", project=str(project), source_id=node["id"])
                inventory.append(entry)
            for child in node.get("children", []):
                walk(child, world)

        walk(document["root"], IDENTITY)

    variant_sources = {
        "contour_763cc063-e341-48d0-ab60-f44c5359f84a",
        "contour_23c46909-0629-4275-a64a-395e4a7f8fc9",
        "contour_6d6e43c7-e8ee-42fe-a26d-eec5a11edd3d",
    }
    for source_case in list(cases):
        if source_case.get("source_id") not in variant_sources:
            continue
        request = json.loads(Path(source_case["request"]).read_text())["request"]
        unit = 128/max(source_case["width"], source_case["height"])
        origin = request["loops"][0]["points"][0]["position"]
        canonical = transform_request(request, (unit, 0, 0, unit, -origin[0]*unit, -origin[1]*unit))
        for label, transform in {
            "tiny": (.03125, 0, 0, .03125, 0, 0),
            "large": (32, 0, 0, 32, 0, 0),
            "stretched": (16, 0, 0, .125, 0, 0),
            "rotated_sheared": (math.cos(.63), math.sin(.63), .7, 1.5, 12345, -9876),
        }.items():
            add("realvariant_"+source_case["source_id"][:20]+"_"+label,
                transform_request(canonical, transform), family="saved_variant",
                source_id=source_case["source_id"], project=source_case["project"], variant=label)

    controls = {
        "rectangle": [polygon([(-64, -48), (64, -48), (64, 48), (-64, 48)])],
        "triangle": [polygon([(-64, 48), (64, 48), (0, -64)])],
        "circle": [ellipse(64, 64)],
        "ellipse": [ellipse(96, 32)],
        "concave_L": [polygon([(-64, -64), (0, -64), (0, 0), (64, 0), (64, 64), (-64, 64)])],
        "narrow_U": [polygon([(-64, -64), (-48, -64), (-48, 48), (48, 48), (48, -64), (64, -64), (64, 64), (-64, 64)])],
        "square_hole": [polygon([(-64, -64), (64, -64), (64, 64), (-64, 64)]),
                        polygon([(-24, -24), (-24, 24), (24, 24), (24, -24)], "cutout")],
        "ring": [ellipse(64, 64), ellipse(36, 36, "cutout")],
    }
    for name, loops in controls.items():
        request = dict(curveModel="cubic-anchors-v1", boundaryTolerance=.1, loops=loops)
        add("control_"+name, request, family="control", source_id=name,
            expected_minimum=1 if name in ("rectangle", "triangle") else None)
        if name in ("rectangle", "circle", "concave_L", "ring"):
            for label, transform in {
                "tiny": (.03125, 0, 0, .03125, 0, 0),
                "large": (32, 0, 0, 32, 0, 0),
                "stretched": (16, 0, 0, .125, 0, 0),
                "rotated_sheared": (math.cos(.63), math.sin(.63), .7, 1.5, 12345, -9876),
            }.items():
                add("control_"+name+"_"+label, transform_request(request, transform),
                    family="variant", source_id=name, variant=label,
                    expected_minimum=1 if name == "rectangle" else None)

    geometry = json.loads(gzip.decompress(Path("assets/vector/shape_geometry.json.gz").read_bytes()))["shapes"]["102"]
    if any(vertex[2] != 1 for vertex in geometry["vertices"]):
        raise ValueError("Native outline extraction requires opaque vertices")
    edges = {}
    for triangle in geometry["triangles"]:
        for index in range(3):
            edge = tuple(sorted((triangle[index], triangle[(index+1) % 3])))
            edges[edge] = edges.get(edge, 0)+1
    adjacency = {}
    for (first, second), count in edges.items():
        if count == 1:
            adjacency.setdefault(first, []).append(second)
            adjacency.setdefault(second, []).append(first)
    if any(len(neighbors) != 2 for neighbors in adjacency.values()):
        raise ValueError("Native outline is not a simple loop")
    indices = [min(adjacency)]
    previous, current = indices[0], adjacency[indices[0]][0]
    while current != indices[0]:
        indices.append(current)
        following = next(index for index in adjacency[current] if index != previous)
        previous, current = current, following
    if len(indices) != len(adjacency):
        raise ValueError("Native outline has multiple components")
    request = dict(curveModel="cubic-anchors-v1", boundaryTolerance=.1,
                   loops=[polygon([geometry["vertices"][i][:2] for i in indices])])
    for label, transform in {
        "base": IDENTITY,
        "tiny": (.03125, 0, 0, .03125, 0, 0),
        "large": (32, 0, 0, 32, 0, 0),
        "stretched": (16, 0, 0, .125, 0, 0),
        "rotated_sheared": (math.cos(.63), math.sin(.63), .7, 1.5, 12345, -9876),
    }.items():
        add("native_circle_"+label, transform_request(request, transform), family="native_control",
            source_id="102", variant=label, expected_minimum=1, known_transform=list(transform))

    for fixture in sorted(Path("tools/fixtures").glob("compact_fit_*.json")):
        data = json.loads(fixture.read_text(encoding="utf-8-sig"))
        if "request" in data:
            add("fixture_"+fixture.stem, data["request"], family="fixture", source_id=str(fixture))

    selected = []
    saved = [c for c in cases if c["family"] == "saved"]
    buckets = [sorted([c for c in saved if c["holes"]], key=lambda c: c["name"]),
               sorted([c for c in saved if not c["holes"] and c["points"] <= 4], key=lambda c: c["name"]),
               sorted([c for c in saved if not c["holes"] and 5 <= c["points"] <= 10], key=lambda c: c["name"]),
               sorted([c for c in saved if not c["holes"] and 11 <= c["points"] <= 25], key=lambda c: c["name"]),
               sorted([c for c in saved if not c["holes"] and c["points"] > 25], key=lambda c: c["name"])]
    for bucket in buckets:
        for index in sorted(set([0, len(bucket)//2, len(bucket)-1])) if bucket else []:
            selected.append(bucket[index]["name"])
    selected += [c["name"] for c in cases if c["name"] in ("control_rectangle_large", "control_circle_stretched", "control_concave_L", "control_ring")]
    selected += [c["name"] for c in saved if "example2" in c["name"] and c["source_id"] in ("layer_copy_6", "layer_copy_18")]
    write_json(output/"cases.json", cases)
    write_json(output/"inventory.json", inventory)
    write_json(output/"grid_selection.json", sorted(set(selected)))
    print(json.dumps(dict(cases=len(cases), inventory=len(inventory), grid=len(set(selected)))), flush=True)


def run(output, executable, stage, timeout):
    cases = json.loads((output/"cases.json").read_text())
    selected = set(json.loads((output/"grid_selection.json").read_text()))
    results_path = output/(stage+".jsonl")
    existing = [json.loads(line) for line in results_path.read_text().splitlines()] if results_path.exists() else []
    done = {(r["case"], r["strategy"]) for r in existing}
    raw = output/"raw"
    raw.mkdir(exist_ok=True)
    for case in cases:
        if stage == "grid" and case["name"] not in selected:
            continue
        strategies = SCREEN if stage == "screen" else tuple(s for s in STRATEGIES if s not in SCREEN)
        for strategy in strategies:
            if (case["name"], strategy) in done:
                continue
            mode, height, budget, finish = STRATEGIES[strategy]
            if mode.startswith("anchor"):
                extent = float(mode[6:])
                largest = max(case["width"], case["height"])
                mode = str(extent*case["width"]/largest)
                height = str(extent*case["height"]/largest)
            command = [str(executable), "--normalization-benchmark", case["request"], mode, height, str(budget), str(finish)]
            start = time.perf_counter()
            row = dict(case=case["name"], strategy=strategy, command=command, timeout=False)
            label = case["name"]+"_"+strategy
            try:
                result = subprocess.run(command, capture_output=True, timeout=timeout)
                (raw/(label+".stdout.txt")).write_bytes(result.stdout)
                (raw/(label+".stderr.txt")).write_bytes(result.stderr)
                candidates = [line for line in result.stdout.decode("utf-8", errors="replace").splitlines() if line.startswith("{")]
                if candidates:
                    row.update(json.loads(candidates[-1]))
                row["exit"] = result.returncode
            except subprocess.TimeoutExpired as error:
                row.update(timeout=True, verified=False)
                (raw/(label+".stdout.txt")).write_bytes(error.stdout or b"")
                (raw/(label+".stderr.txt")).write_bytes(error.stderr or b"")
            row["wall_seconds"] = time.perf_counter()-start
            with results_path.open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(row)+"\n")
            print(json.dumps({k: row.get(k) for k in ("case", "strategy", "verified", "count", "total_ms", "wall_seconds", "timeout")}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("prepare", "screen", "grid"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=Path("build/Release/logs/HumanRef"))
    parser.add_argument("--executable", type=Path, default=Path("build/Release/fls_compact_fit_tests.exe"))
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args()
    if args.stage == "prepare":
        prepare(args.output, args.source)
    else:
        run(args.output, args.executable.resolve(), args.stage, args.timeout)
