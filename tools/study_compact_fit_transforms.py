"""Measure affine search frames without changing the production solver."""

import argparse
import copy
import hashlib
import itertools
import json
import math
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

sys.dont_write_bytecode = True

from benchmark_compact_fit_normalization import transform_request


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "build/transformation_study_20261030"
IDENTITY = np.eye(3)


def coefficients(matrix):
    return [float(matrix[0, 0]), float(matrix[1, 0]), float(matrix[0, 1]),
            float(matrix[1, 1]), float(matrix[0, 2]), float(matrix[1, 2])]


def matrix(values):
    a, b, c, d, x, y = values
    return np.array([[a, c, x], [b, d, y], [0., 0., 1.]])


def rotation(degrees):
    angle = math.radians(degrees)
    return matrix([math.cos(angle), math.sin(angle), -math.sin(angle), math.cos(angle), 0, 0])


def scaling(x, y=None):
    return np.diag([x, x if y is None else y, 1.])


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def transformed(document, transform):
    result = copy.deepcopy(document)
    result["request"] = transform_request(result["request"], coefficients(transform))
    for placement in result.get("oraclePlacements", []):
        placement["transform"] = coefficients(transform @ matrix(placement["transform"]))
    return result


def sampled_loops(request):
    loops = []
    for loop in request["loops"]:
        samples = []
        points = loop["points"]
        for index, start in enumerate(points):
            end = points[(index + 1) % len(points)]
            p0, p3 = np.array(start["position"]), np.array(end["position"])
            outgoing = np.array(start.get("outgoing", [0, 0]))
            incoming = np.array(end.get("incoming", [0, 0]))
            if (start.get("kind") == "hard" and end.get("kind") == "hard") or (not np.any(outgoing) and not np.any(incoming)):
                samples.append(p0)
                continue
            p1, p2 = p0 + outgoing, p3 + incoming
            for t in np.linspace(0, 1, 64, endpoint=False):
                samples.append((1-t)**3*p0 + 3*(1-t)**2*t*p1 + 3*(1-t)*t*t*p2 + t**3*p3)
        polygon = np.asarray(samples)
        other = np.roll(polygon, -1, axis=0)
        cross = polygon[:, 0]*other[:, 1] - polygon[:, 1]*other[:, 0]
        if (cross.sum() > 0) != (loop["kind"] == "outer"):
            polygon = polygon[::-1]
        loops.append(polygon)
    return loops


def geometry(request):
    loops = sampled_loops(request)
    points = np.concatenate(loops)
    lower, upper = points.min(axis=0), points.max(axis=0)
    center = (lower + upper) / 2
    area_twice = 0.
    centroid_sum = np.zeros(2)
    moment = np.zeros((2, 2))
    perimeter = 0.
    for polygon in loops:
        p, q = polygon - center, np.roll(polygon, -1, axis=0) - center
        cross = p[:, 0]*q[:, 1] - p[:, 1]*q[:, 0]
        area_twice += cross.sum()
        centroid_sum += ((p + q) * cross[:, None]).sum(axis=0)
        moment[0, 0] += ((p[:, 0]**2 + p[:, 0]*q[:, 0] + q[:, 0]**2) * cross).sum() / 12
        moment[1, 1] += ((p[:, 1]**2 + p[:, 1]*q[:, 1] + q[:, 1]**2) * cross).sum() / 12
        moment[0, 1] += ((2*p[:, 0]*p[:, 1] + p[:, 0]*q[:, 1]
                         + q[:, 0]*p[:, 1] + 2*q[:, 0]*q[:, 1]) * cross).sum() / 24
        perimeter += np.linalg.norm(q-p, axis=1).sum()
    area = area_twice / 2
    centroid = centroid_sum / (6*area)
    moment[1, 0] = moment[0, 1]
    covariance = moment / area - np.outer(centroid, centroid)
    eigenvalues, eigenvectors = np.linalg.eigh(covariance)
    condition = math.sqrt(max(eigenvalues) / max(min(eigenvalues), 1e-15))
    return dict(lower=lower, upper=upper, center=center, width=upper[0]-lower[0], height=upper[1]-lower[1],
                area=area, perimeter=perimeter, covariance=covariance,
                eigenvalues=eigenvalues, eigenvectors=eigenvectors, principal_aspect=condition)


def centered_frame(request, nonuniform=False):
    g = geometry(request)
    horizontal = 256/g["width"] if nonuniform else 256/max(g["width"], g["height"])
    vertical = 256/g["height"] if nonuniform else horizontal
    result = scaling(horizontal, vertical)
    result[:2, 2] = -result[:2, :2] @ g["center"]
    return result


def principal_frame(request):
    g = geometry(request)
    axes = g["eigenvectors"][:, ::-1].T.copy()
    if axes[0, 0] < 0:
        axes[0] *= -1
    if np.linalg.det(axes) < 0:
        axes[1] *= -1
    result = IDENTITY.copy()
    result[:2, :2] = np.diag(1/np.sqrt(g["eigenvalues"][::-1])) @ axes
    result[:2, 2] = -result[:2, :2] @ g["center"]
    whitened = transformed({"request": request}, result)
    return centered_frame(whitened["request"]) @ result


def affine_signature(request):
    positions = np.asarray([p["position"] for loop in request["loops"] for p in loop["points"]])
    if len(positions) > 40:
        return hashlib.sha256(json.dumps(request, sort_keys=True).encode()).hexdigest()
    anchors, largest = None, 0
    for indices in itertools.combinations(range(len(positions)), 3):
        a, b, c = positions[list(indices)]
        determinant = abs(float((b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])))
        if determinant > largest*(1+1e-7):
            largest, anchors = determinant, (a, b, c)
    a, b, c = anchors
    basis = np.column_stack((b-a, c-a))
    normalize = IDENTITY.copy()
    normalize[:2, :2] = np.linalg.inv(basis)
    normalize[:2, 2] = -normalize[:2, :2] @ a
    canonical = transformed({"request": request}, normalize)["request"]
    canonical.pop("boundaryTolerance", None)
    rounded = json.loads(json.dumps(canonical), parse_float=lambda value: round(float(value), 5))
    return hashlib.sha256(json.dumps(rounded, sort_keys=True).encode()).hexdigest()


VARIANTS = {
    "identity": IDENTITY,
    "small": scaling(.125),
    "large": scaling(8),
    "rotate15": rotation(15),
    "rotate45": rotation(45),
    "rotate90": rotation(90),
    "stretch4": scaling(4, .25),
    "stretch16": scaling(16, .0625),
    "shear1": matrix([1, 0, 1, 1, 0, 0]),
    "rotated_stretch4": rotation(45) @ scaling(4, .25),
}


def prepare(output):
    controls = output / "controls"
    controls.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(ROOT/"build/Release/fls_compact_fit_tests.exe"),
                    "--export-transformation-controls", str(controls)], check=True, capture_output=True)
    inventory = json.loads((ROOT/"build/normalization_expanded_20261029/cases.json").read_text())
    example = next(c for c in inventory if "example2_contour_cb2" in c["name"])
    authored = json.loads(Path(example["request"]).read_text())
    historical = []
    for file in ["screen.jsonl", "grid.jsonl", "repeat.jsonl"]:
        path = ROOT/"build/normalization_expanded_20261029"/file
        if path.exists():
            historical += [json.loads(line) for line in path.read_text().splitlines()]
    candidates = [row for row in historical if row["case"] == example["name"]
                  and row.get("verified") and row.get("count") == 2 and row.get("placements")]
    if candidates:
        chosen = min(candidates, key=lambda row: row.get("area_error", float("inf")))
        authored["oraclePlacements"] = chosen["placements"]
        authored["oracleSource"] = dict(case=example["name"], strategy=chosen["strategy"])
    documents = {path.stem: json.loads(path.read_text()) for path in sorted(controls.glob("*.json"))}
    documents["authored_circle_square"] = authored
    cases, jobs = [], []

    def add_job(case, frame_name, frame, budget=3000, extra=None, group="main"):
        key = f"{case['name']}__{frame_name}__{budget}__{group}"
        frame_path = output/"frames"/(key+".json")
        write(frame_path, coefficients(frame))
        request_path = case["request"]
        if extra:
            document = json.loads(Path(request_path).read_text())
            document["benchmarkOptions"] = extra.get("working", {})
            document["originalOptions"] = extra.get("original", {})
            if "requestTolerance" in extra:
                document["request"]["boundaryTolerance"] = extra["requestTolerance"]
            request_path = str(output/"ablation_requests"/(key+".json"))
            write(Path(request_path), document)
        jobs.append(dict(key=key, case=case["name"], family=case["family"], variant=case["variant"],
                         request=str(request_path), frame=str(frame_path), frame_name=frame_name,
                         budget=budget, group=group, extra=extra or {}))

    def add_case(name, family, variant, document, transform=None, frames=None):
        path = output/"requests"/(name+".json")
        write(path, document)
        g = geometry(document["request"])
        case = dict(name=name, family=family, variant=variant, request=str(path),
                    width=float(g["width"]), height=float(g["height"]), principal_aspect=g["principal_aspect"],
                    area=float(g["area"]), perimeter=float(g["perimeter"]),
                    transform=coefficients(transform if transform is not None else IDENTITY),
                    anchors=sum(len(loop["points"]) for loop in document["request"]["loops"]),
                    oracle_count=len(document.get("oraclePlacements", [])), affine_signature=affine_signature(document["request"]))
        cases.append(case)
        for frame_name in frames or ["native", "uniform", "square", "undo"]:
            frame = IDENTITY if frame_name == "native" else centered_frame(document["request"], frame_name == "square")
            if frame_name == "undo":
                frame = np.linalg.inv(transform) if transform is not None else centered_frame(document["request"])
            elif frame_name == "principal":
                frame = principal_frame(document["request"])
            add_job(case, frame_name, frame)
        return case

    for family, document in documents.items():
        canonical = transformed(document, centered_frame(document["request"]))
        for variant, transform in VARIANTS.items():
            add_case(family+"_"+variant, family, variant, transformed(canonical, transform), transform)
        if family in ["native_circle_square", "native_rectangle", "authored_circle_square"]:
            for variant, transform in {"translated": matrix([1,0,0,1,12345,-9876]), "mirrored": scaling(-1,1)}.items():
                add_case(family+"_"+variant, family, variant, transformed(canonical, transform), transform)

    unique = {}
    for entry in inventory:
        if entry["family"] != "saved" or "example2" in entry["name"] or entry["points"] > 30:
            continue
        document = json.loads(Path(entry["request"]).read_text())
        unique.setdefault(affine_signature(document["request"]), (entry, document))
    groups = [[], [], [], []]
    for entry, document in unique.values():
        groups[0 if entry["holes"] else 1 if entry["points"] <= 4 else 2 if entry["points"] <= 10 else 3].append((entry, document))
    for group in groups:
        group.sort(key=lambda pair: pair[0]["name"])
        for index in sorted(set([0, len(group)//2])) if group else []:
            entry, document = group[index]
            add_case("saved_"+entry["name"], "saved_"+affine_signature(document["request"])[:10], "original", document,
                     frames=["native", "uniform", "square", "principal"])

    # Include every stored example2 contour, grouping the five affine copies together.
    from benchmark_compact_fit_normalization import compose, request_from_scene, scene_transform
    import gzip
    raw = (ROOT/"build/Release/logs/HumanRef/simplestLeeway/example2.3so").read_bytes()
    scene = json.loads(gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw)
    def walk(node, parent):
        world = compose(parent, scene_transform(node.get("transform", {})))
        if node.get("kind") == "contour" and node.get("closed"):
            document = {"request": request_from_scene(node, world)}
            add_case("example2_actual_"+node["id"], "example2_actual", node["id"], document,
                     frames=["native", "uniform", "square", "principal"])
        for child in node.get("children", []):
            walk(child, world)
    walk(scene["root"], (1,0,0,1,0,0))

    for case in list(cases):
        if case["family"] in ["native_circle_square", "native_ellipse_square", "authored_circle_square"] \
                and case["variant"] in ["stretch4", "stretch16", "rotated_stretch4", "shear1"]:
            doc = json.loads(Path(case["request"]).read_text())
            add_job(case, "principal", principal_frame(doc["request"]))
        if case["family"] in ["native_circle_square", "authored_circle_square"] and case["variant"] in ["small", "large"]:
            factor = .125 if case["variant"] == "small" else 8
            for frame_name in ["native", "uniform"]:
                frame = IDENTITY if frame_name == "native" else np.linalg.inv(matrix(case["transform"]))
                add_job(case, frame_name, frame, extra={"working": {"boundaryTolerance": .1*factor if frame_name == "native" else .1,
                        "boundaryAllowance": 2*factor if frame_name == "native" else 2,
                        "inwardAllowance": .5*factor if frame_name == "native" else .5,
                        "observationScale": factor if frame_name == "native" else 1},
                        "original": {"boundaryAllowance": 2*factor, "inwardAllowance": .5*factor, "observationScale": factor},
                        "requestTolerance": .1*factor}, group="unit_equivalence")
    write(output/"cases.json", cases)
    write(output/"jobs.json", jobs)
    print(json.dumps(dict(cases=len(cases), jobs=len(jobs), independent_saved_geometries=len(unique))), flush=True)


def run(output, group=None):
    jobs = json.loads((output/"jobs.json").read_text())
    path = output/"results.jsonl"
    done = {row["key"] for row in map(json.loads, path.read_text().splitlines())} if path.exists() else set()
    raw = output/"raw"
    raw.mkdir(exist_ok=True)
    for index, job in enumerate(jobs):
        if job["key"] in done or (group and not job["group"].startswith(group)):
            continue
        arguments = [str(ROOT/"build/Release/fls_compact_fit_tests.exe"), "--normalization-benchmark", job["request"],
                     "matrix:"+job["frame"], "0", str(job["budget"]), "0", "fixed", "legacy"]
        if job.get("extra", {}).get("adaptive"):
            arguments.remove("legacy")
        if job.get("extra", {}).get("cpu"):
            arguments.append("cpu")
        started = time.perf_counter()
        try:
            process = subprocess.run(arguments, capture_output=True, timeout=60)
            (raw/(job["key"]+".out")).write_bytes(process.stdout)
            (raw/(job["key"]+".err")).write_bytes(process.stderr)
            parsed = [json.loads(line) for line in process.stdout.decode(errors="replace").splitlines() if line.startswith("{")]
            result = parsed[-1] if parsed else dict(verified=False, error="No JSON result", exit=process.returncode)
        except subprocess.TimeoutExpired:
            result = dict(verified=False, error="60-second test timeout", timeout=True)
        result.update(job)
        result["wall_seconds"] = time.perf_counter()-started
        with path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(result)+"\n")
        seed = result.get("working_diagnostics", {}).get("profileSeed", {})
        print(json.dumps(dict(index=index+1, total=len(jobs), case=job["case"], frame=job["frame_name"],
                              verified=result.get("verified"), count=result.get("count"), seed=seed.get("count"),
                              oracle=result.get("oracle_verified"), milliseconds=result.get("total_ms"))), flush=True)


def ablations(output):
    cases = json.loads((output/"cases.json").read_text())
    jobs = json.loads((output/"jobs.json").read_text())
    keys = {job["key"] for job in jobs}
    by_name = {case["name"]: case for case in cases}

    reference = json.loads(Path(by_name["authored_circle_square_identity"]["request"]).read_text())
    reference_points = reference["request"]["loops"][0]["points"]
    source_positions = np.asarray([point["position"] for point in reference_points])
    design = np.column_stack((source_positions, np.ones(len(source_positions))))
    for case in cases:
        if case["family"] != "example2_actual":
            continue
        document = json.loads(Path(case["request"]).read_text())
        target_points = document["request"]["loops"][0]["points"]
        if len(target_points) != len(reference_points):
            case["encoding"] = "alternative boundary anchors"
            continue
        target_positions = np.asarray([point["position"] for point in target_points])
        mapping = IDENTITY.copy()
        mapping[:2, :] = np.linalg.lstsq(design, target_positions, rcond=None)[0].T
        mapped = transformed(reference, mapping)
        residuals = []
        for a, b in zip(mapped["request"]["loops"][0]["points"], target_points):
            residuals += [np.linalg.norm(np.asarray(a[key])-np.asarray(b[key]))
                          for key in ["position", "incoming", "outgoing"]]
        case["affine_registration_residual"] = float(max(residuals))
        case["encoding"] = "identical affine copy"
        case["oracle_count"] = len(mapped.get("oraclePlacements", []))
        document["oraclePlacements"] = mapped.get("oraclePlacements", [])
        write(Path(case["request"]), document)
        for job in jobs:
            if job["case"] == case["name"] and job["request"] != case["request"]:
                other = json.loads(Path(job["request"]).read_text())
                other["oraclePlacements"] = document["oraclePlacements"]
                write(Path(job["request"]), other)

    def add(case, frame_name, frame, group, budget=3000, working=None, cpu=False, adaptive=False):
        key = f"{case['name']}__{frame_name}__{budget}__{group}"
        if key in keys:
            return
        keys.add(key)
        frame_path = output/"frames"/(key+".json")
        write(frame_path, coefficients(frame))
        document = json.loads(Path(case["request"]).read_text())
        document["benchmarkOptions"] = working or {}
        path = output/"ablation_requests"/(key+".json")
        write(path, document)
        jobs.append(dict(key=key, case=case["name"], family=case["family"], variant=case["variant"],
                         request=str(path), frame=str(frame_path), frame_name=frame_name, budget=budget,
                         group=group, extra=dict(working=working or {}, cpu=cpu, adaptive=adaptive)))

    selected = ["native_circle_square_"+variant for variant in
                ["identity", "large", "stretch4", "rotated_stretch4", "shear1"]]
    selected += ["authored_circle_square_"+variant for variant in ["identity", "large", "stretch4", "rotate45"]]
    selected += ["native_circle_pair_"+variant for variant in ["identity", "rotate45"]]
    for name in selected:
        case = by_name[name]
        add(case, "native", IDENTITY, "ablation_oracle_seed", working={"oracleSeed": True})
        add(case, "native", IDENTITY, "ablation_oracle_pool", working={"oraclePool": True})
        add(case, "native", IDENTITY, "ablation_more_refinement", budget=12000)
        add(case, "native", IDENTITY, "ablation_more_profiles", working={"profileTrialBudget": 320000})
        add(case, "native", IDENTITY, "ablation_cpu", cpu=True)
    for case in cases:
        if case["family"] == "example2_actual":
            add(case, "native", IDENTITY, "gui_defaults", budget=60000, adaptive=True)
            if case.get("oracle_count"):
                add(case, "native", IDENTITY, "ablation_actual_oracle", working={"oracleSeed": True})
                if case["variant"] == "layer_copy_6":
                    add(case, "native", IDENTITY, "ablation_actual_oracle_12k", budget=12000, working={"oracleSeed": True})
        if case["family"] in ["native_frame", "native_L", "native_thin_elbow"] and case["variant"] in ["rotate45", "stretch4", "shear1"]:
            add(case, "native", IDENTITY, "ablation_seam_clearance", working={"oracleSeed": True, "oracleClearance": .001})
    for name in ["native_circle_square_identity", "native_circle_square_stretch4",
                 "authored_circle_square_identity", "authored_circle_square_stretch4"]:
        case = by_name[name]
        for value in [.5, 2.]:
            add(case, "native", IDENTITY, "ablation_observation_"+str(value), working={"observationScale": value})
        for value in [.01, .4]:
            add(case, "native", IDENTITY, "ablation_flattening_"+str(value), working={"boundaryTolerance": value})
    for name in ["native_circle_square_identity", "native_circle_square_rotated_stretch4",
                 "authored_circle_square_identity", "authored_circle_square_stretch4"]:
        case = by_name[name]
        document = json.loads(Path(case["request"]).read_text())
        for frame_name in ["native", "uniform", "square"]:
            frame = IDENTITY if frame_name == "native" else centered_frame(document["request"], frame_name == "square")
            add(case, frame_name, frame, "repeat")

    base = json.loads(Path(by_name["native_circle_square_identity"]["request"]).read_text())
    for ratio in [1., 1.5, 2., 3., 4., 6., 8., 12., 16.]:
        for degrees in [0, 15, 30, 45]:
            transform = rotation(degrees) @ scaling(math.sqrt(ratio), 1/math.sqrt(ratio))
            candidate = transformed(base, transform)
            transform = centered_frame(candidate["request"]) @ transform
            document = transformed(base, transform)
            name = f"ratio_sweep_r{ratio}_a{degrees}"
            path = output/"requests"/(name+".json")
            write(path, document)
            g = geometry(document["request"])
            case = dict(name=name, family="native_circle_square_ratio_sweep", variant=f"ratio{ratio}_angle{degrees}",
                        request=str(path), width=float(g["width"]), height=float(g["height"]),
                        principal_aspect=g["principal_aspect"], area=float(g["area"]), perimeter=float(g["perimeter"]),
                        transform=coefficients(transform), anchors=sum(len(loop["points"]) for loop in document["request"]["loops"]),
                        oracle_count=2, component_ratio=ratio, angle=degrees)
            if name not in by_name:
                cases.append(case)
                by_name[name] = case
            for frame_name in ["native", "square", "principal"]:
                frame = IDENTITY if frame_name == "native" else centered_frame(document["request"], True)
                if frame_name == "principal":
                    frame = principal_frame(document["request"])
                add(case, frame_name, frame, "ratio_sweep")
    write(output/"cases.json", cases)
    write(output/"jobs.json", jobs)
    print(json.dumps(dict(cases=len(cases), jobs=len(jobs))), flush=True)


def followup(output):
    cases = json.loads((output/"cases.json").read_text())
    jobs = json.loads((output/"jobs.json").read_text())
    keys = {job["key"] for job in jobs}

    def add(case, document, frame_name, frame, group, budget=3000, extra=None):
        key = f"{case['name']}__{frame_name}__{budget}__{group}"
        if key in keys:
            return
        keys.add(key)
        request_path = output/"ablation_requests"/(key+".json")
        frame_path = output/"frames"/(key+".json")
        write(request_path, document)
        write(frame_path, coefficients(frame))
        jobs.append(dict(key=key, case=case["name"], family=case["family"], variant=case["variant"],
                         request=str(request_path), frame=str(frame_path), frame_name=frame_name,
                         budget=budget, group=group, extra=extra or {}))

    for case in cases:
        if not (case["family"] == "authored_circle_square" or case["family"] == "example2_actual"
                or case["family"].startswith("saved_")):
            continue
        document = json.loads(Path(case["request"]).read_text())
        for frame_name in ["uniform", "square", "principal"]:
            frame = principal_frame(document["request"]) if frame_name == "principal" else centered_frame(document["request"], frame_name == "square")
            add(case, document, frame_name, frame, "corrected_geometry")

    tiny = next(case for case in cases if case["family"] == "example2_actual" and case["variant"] == "layer_copy_18")
    document = json.loads(Path(tiny["request"]).read_text())
    for index in range(10):
        candidate = copy.deepcopy(document)
        points = candidate["request"]["loops"][0]["points"]
        candidate["request"]["loops"][0]["points"] = points[index:]+points[:index]
        add(tiny, candidate, "native", IDENTITY, "anchor_order_"+str(index), budget=60000, extra={"adaptive": True})
    inventory = json.loads((ROOT/"build/normalization_expanded_20261029/cases.json").read_text())
    historical = []
    for file in ["screen.jsonl", "grid.jsonl", "repeat.jsonl"]:
        path = ROOT/"build/normalization_expanded_20261029"/file
        if path.exists():
            historical += [json.loads(line) for line in path.read_text().splitlines()]
    historical = [row for row in historical if row.get("verified") and row.get("count") == 2 and row.get("placements")]
    sources = {entry["name"]: json.loads(Path(entry["request"]).read_text()) for entry in inventory
               if any(row["case"] == entry["name"] for row in historical)}
    targets = [case for case in cases if (case["family"] == "example2_actual" and case["anchors"] == 10)
               or (case["family"] == "authored_circle_square" and case["variant"] in ["large", "stretch16"])]
    for case in targets:
        document = json.loads(Path(case["request"]).read_text())
        target = document["request"]["loops"][0]["points"]
        target_positions = np.asarray([point["position"] for point in target])
        candidates = []
        for row in historical:
            source = sources.get(row["case"], {}).get("request", {}).get("loops", [{}])[0].get("points", [])
            if len(source) != len(target):
                continue
            positions = np.asarray([point["position"] for point in source])
            design = np.column_stack((positions, np.ones(len(positions))))
            transform = IDENTITY.copy()
            transform[:2, :] = np.linalg.lstsq(design, target_positions, rcond=None)[0].T
            mapped = transform_request({"loops": [{"kind": "outer", "points": source}]}, coefficients(transform))["loops"][0]["points"]
            if max(np.linalg.norm(np.asarray(a[field])-np.asarray(b[field]))
                   for a, b in zip(mapped, target) for field in ["position", "incoming", "outgoing"]) > 1e-6:
                continue
            candidate = copy.deepcopy(document)
            candidate["oraclePlacements"] = [dict(shapeId=p["shapeId"], transform=coefficients(transform @ matrix(p["transform"])))
                                              for p in row["placements"]]
            candidate["benchmarkOptions"] = {"oracleSeed": True}
            corner = row.get("boundary", {}).get("maximumCornerDistance", 1)
            score = corner * np.linalg.svd(transform[:2, :2], compute_uv=False)[0]
            if case["variant"] in row["case"]:
                score *= .01
            candidates.append((score, row["case"]+"_"+row["strategy"], candidate))
        candidates.sort(key=lambda value: value[0])
        unique = []
        signatures = set()
        for _, label, candidate in candidates:
            fingerprint = json.dumps(candidate["oraclePlacements"], sort_keys=True)
            if fingerprint not in signatures:
                unique.append((label, candidate))
                signatures.add(fingerprint)
        for index, (label, candidate) in enumerate(unique[:3 if case["family"] == "authored_circle_square" else 1]):
            candidate["oracleSource"] = label
            add(case, candidate, "native", IDENTITY, "historical_oracle_"+str(index))
    write(output/"jobs.json", jobs)
    print(json.dumps(dict(cases=len(cases), jobs=len(jobs))), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["prepare", "run", "ablations", "followup"])
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--group")
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    if arguments.stage == "prepare":
        prepare(arguments.output.resolve())
    elif arguments.stage == "ablations":
        ablations(arguments.output.resolve())
    elif arguments.stage == "followup":
        followup(arguments.output.resolve())
    else:
        run(arguments.output.resolve(), arguments.group)
