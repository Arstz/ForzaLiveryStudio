"""Check constructive native covers against the original Compact Fit verifier."""

import argparse
import json
import math
from pathlib import Path
import random
import subprocess
import time


def polygon(points, kind="outer"):
    return dict(kind=kind, points=[dict(kind="hard", position=list(p), explicitHandles=True,
                                      incoming=[0, 0], outgoing=[0, 0]) for p in points])


def prepare(output, manifest):
    cases = json.loads(manifest.read_text())
    directory = output/"procedural"
    directory.mkdir(parents=True, exist_ok=True)
    rng = random.Random(447129)

    def add(name, loops):
        path = (directory/(name+".json")).resolve()
        request = dict(curveModel="cubic-anchors-v1", boundaryTolerance=.1, loops=loops)
        path.write_text(json.dumps(dict(request=request)))
        cases.append(dict(name="procedural_"+name, family="procedural", request=str(path)))

    for index in range(40):
        count = rng.randint(8, 40)*2
        outer = []
        for point in range(count):
            angle = point*math.tau/count
            radius = rng.uniform(65, 100) if point % 2 else rng.uniform(28, 55)
            outer.append((radius*math.cos(angle), radius*math.sin(angle)))
        horizontal = [1, .0625, 16, 4, 1][index % 5]
        vertical = [1, .0625, 16, .125, 1][index % 5]
        angle = rng.uniform(0, math.tau)
        shift = (12345, -9876) if index % 5 == 4 else (0, 0)

        def transform(point):
            x, y = point[0]*horizontal, point[1]*vertical
            return (x*math.cos(angle)-y*math.sin(angle)+shift[0],
                    x*math.sin(angle)+y*math.cos(angle)+shift[1])

        loops = [polygon([transform(p) for p in outer])]
        if index % 3 == 0:
            loops.append(polygon([transform((10*math.cos(j*math.tau/8), 10*math.sin(j*math.tau/8)))
                                  for j in range(8)], "cutout"))
        add("star_"+str(index), loops)

    rectangle = lambda left, top, right, bottom: [(left, top), (right, top), (right, bottom), (left, bottom)]
    add("touch_corner", [polygon(rectangle(-64, -64, 0, 0)), polygon(rectangle(0, 0, 64, 64))])
    add("touch_edge", [polygon(rectangle(-64, -64, 0, 64)), polygon(rectangle(0, -64, 64, 64))])
    add("notch_touching_edge", [polygon(rectangle(-64, -64, 64, 64)), polygon(rectangle(-12, -64, 12, 0), "cutout")])
    add("holes_touch_corner", [polygon(rectangle(-64, -64, 64, 64)), polygon(rectangle(-40, -40, 0, 0), "cutout"),
                               polygon(rectangle(0, 0, 40, 40), "cutout")])
    add("crossed_outline", [polygon([(-64, -64), (64, 64), (-64, 64), (64, -64)])])
    for width in [.1, .3, .5, 1, 2, 8]:
        add("corridor_"+str(width), [polygon([(-64, -64), (-64+width, -64), (-64+width, 64-width),
                                              (64-width, 64-width), (64-width, -64), (64, -64), (64, 64), (-64, 64)])])
    for gap in [.001, .1, .3, 1, 4]:
        add("holes_gap_"+str(gap), [polygon(rectangle(-64, -64, 64, 64)),
            polygon(rectangle(-48, -40, -gap/2, 40), "cutout"), polygon(rectangle(gap/2, -40, 48, 40), "cutout")])
    for count in [2, 4, 8, 16]:
        add("components_"+str(count), [polygon(rectangle(index*24, 0, index*24+16, 16)) for index in range(count)])
    for count in [2, 4, 8, 16]:
        add("holes_"+str(count), [polygon(rectangle(-128, -128, 128, 128))]+
            [polygon(rectangle(-104+(i % 4)*56, -104+(i//4)*56, -80+(i % 4)*56, -80+(i//4)*56), "cutout")
             for i in range(count)])
    (output/"cases.json").write_text(json.dumps(cases, indent=2))
    print("Prepared", len(cases), "cases", flush=True)


def run(output, executable):
    cases = json.loads((output/"cases.json").read_text())
    raw = output/"adaptive_raw"
    raw.mkdir(exist_ok=True)
    attempts_path, solutions_path = output/"attempts.jsonl", output/"solutions.jsonl"
    done = {r["case"] for r in map(json.loads, solutions_path.read_text().splitlines())} if solutions_path.exists() else set()
    for case in cases:
        if case["name"] in done:
            continue
        total_ms = 0
        best = None
        attempts = 0
        for epsilon in [.25, .125, .05, .02, .005, 0]:
            source_invalid = False
            for mode in ["centered", "rawedges", "slabs", "canonical_slabs"]:
                if source_invalid and mode != "canonical_slabs":
                    continue
                if mode == "canonical_slabs" and not source_invalid:
                    continue
                arguments = [str(executable), "--constructive-cover", case["request"], mode, str(epsilon)]
                start = time.perf_counter()
                try:
                    process = subprocess.run(arguments, capture_output=True, timeout=30)
                except subprocess.TimeoutExpired as error:
                    process = subprocess.CompletedProcess(arguments, -1, error.stdout or b"", error.stderr or b"")
                label = case["name"]+"_"+mode+"_"+str(epsilon)
                (raw/(label+".stdout.txt")).write_bytes(process.stdout)
                (raw/(label+".stderr.txt")).write_bytes(process.stderr)
                lines = [line for line in process.stdout.decode(errors="replace").splitlines() if line.startswith("{")]
                result = json.loads(lines[-1]) if lines else dict(verified=False, error=process.stderr.decode(errors="replace")[-300:])
                result.update(case=case["name"], command=arguments, mode=mode, epsilon=epsilon,
                              wall_seconds=time.perf_counter()-start, timeout=process.returncode == -1)
                total_ms += result.get("total_ms", result["wall_seconds"]*1000)
                attempts += 1
                with attempts_path.open("a") as stream:
                    stream.write(json.dumps(result)+"\n")
                if result.get("verified"):
                    best = result
                    break
                error = result.get("error", "")
                source_invalid = any(text in error for text in (
                    "only have one outer path", "boundaries cross", "cannot overlap or contain",
                    "path crosses itself"))
            if best:
                break
        solution = dict(case=case["name"], family=case["family"], verified=bool(best), attempts=attempts,
                        accumulated_ms=total_ms, count=best["count"] if best else None,
                        selected_command=best["command"] if best else None,
                        selected_ms=best["total_ms"] if best else None)
        with solutions_path.open("a") as stream:
            stream.write(json.dumps(solution)+"\n")
        print(solution["case"], solution["verified"], solution["count"], attempts, total_ms, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("prepare", "run"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=Path("build/normalization_expanded_20261029/cases.json"))
    parser.add_argument("--executable", type=Path, default=Path("build/Release/fls_compact_fit_tests.exe"))
    args = parser.parse_args()
    if args.stage == "prepare":
        prepare(args.output, args.manifest)
    else:
        run(args.output, args.executable.resolve())
