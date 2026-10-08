import argparse
import copy
import csv
import json
from pathlib import Path
import subprocess


REPOSITORY = Path(__file__).resolve().parent.parent


def expanded_configuration(baseline, additions):
    configuration = copy.deepcopy(baseline)
    enabled = set(configuration["shape_ids"] + configuration["reserve_shape_ids"])
    for task, ids in additions.items():
        selected = configuration["tasks"][task]
        if not isinstance(selected, list):
            raise ValueError(f"Task {task} uses a selector; use an explicit list to append IDs")
        for shape_id in ids:
            if shape_id not in enabled:
                configuration["shape_ids"].append(shape_id)
                enabled.add(shape_id)
            if shape_id not in selected:
                selected.append(shape_id)
    return configuration


def replay(executable, request, configuration, allowance, log):
    command = [str(executable), "--profile-retained", str(request), str(allowance),
               "--shape-config", str(configuration)]
    result = subprocess.run(command, cwd=REPOSITORY, capture_output=True, text=True)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    diagnostics = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    if result.returncode or not diagnostics:
        raise RuntimeError(f"Replay failed; see {log}")
    return diagnostics[-1]


def measurement(request, name, diagnostics):
    quality = diagnostics.get("boundaryQuality", {})
    return {
        "request": str(request),
        "configuration": name,
        "count": diagnostics["replayCount"],
        "seconds": diagnostics["replayElapsedMilliseconds"] / 1000,
        "missingInteriorArea": diagnostics.get("missingInteriorArea"),
        "missingBeyondInward": diagnostics.get("missingBeyondInward"),
        "outsideEnvelope": diagnostics.get("outsideEnvelope"),
        "tangentEnergy": quality.get("tangentEnergy"),
        "turnEnergy": quality.get("turnEnergy"),
        "cornerDefects": quality.get("cornerDefects"),
        "maximumCornerDistance": quality.get("maximumCornerDistance"),
        "boundaryEnergy": quality.get("energy"),
        "error": diagnostics.get("replayError", ""),
        "shapeCounts": diagnostics["replayShapeCounts"],
        "smallFootprints": diagnostics.get("replayFootprintsBelow", {}),
    }


def save_measurements(directory, measurements):
    (directory / "summary.json").write_text(json.dumps(measurements, indent=2) + "\n", encoding="utf-8")
    fields = [field for field in measurements[0] if field not in ("shapeCounts", "smallFootprints")]
    with (directory / "summary.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(measurements)


def main():
    parser = argparse.ArgumentParser(description="Compare Compact Fit shape additions using isolated configurations")
    parser.add_argument("--request", type=Path, action="append", required=True)
    parser.add_argument("--executable", type=Path, default=REPOSITORY / "build/Release/fls_compact_fit_tests.exe")
    parser.add_argument("--base", type=Path, default=REPOSITORY / "assets/compact_fit_shapes.json")
    parser.add_argument("--variants", type=Path, default=REPOSITORY / "tools/fixtures/compact_fit_shape_trials.json")
    parser.add_argument("--output", type=Path, default=REPOSITORY / "build/compact_fit_shape_trials")
    parser.add_argument("--only", action="append")
    parser.add_argument("--allowance", type=float, default=2.0)
    args = parser.parse_args()
    baseline = json.loads(args.base.read_text(encoding="utf-8-sig"))
    additions = json.loads(args.variants.read_text(encoding="utf-8-sig"))
    if args.only:
        unknown = set(args.only) - additions.keys()
        if unknown:
            parser.error(f"Unknown variants: {', '.join(sorted(unknown))}")
        additions = {name: tasks for name, tasks in additions.items() if name in args.only}
    configurations = {"baseline": baseline}
    configurations.update({name: expanded_configuration(baseline, tasks) for name, tasks in additions.items()})
    args.output.mkdir(parents=True, exist_ok=True)
    for name, configuration in configurations.items():
        (args.output / f"{name}.json").write_text(json.dumps(configuration, indent=2) + "\n", encoding="utf-8")
    measurements = []
    for index, request in enumerate(args.request):
        for name in configurations:
            print(f"Running {request.name}: {name}", flush=True)
            diagnostics = replay(args.executable.resolve(), request.resolve(),
                                 (args.output / f"{name}.json").resolve(), args.allowance,
                                 args.output / f"{index:02d}_{request.stem}_{name}.log")
            row = measurement(request, name, diagnostics)
            measurements.append(row)
            save_measurements(args.output, measurements)
            print(f"{name}: {row['count']} shapes, {row['seconds']:.2f}s, "
                  f"missing interior {row['missingInteriorArea']}, tangent {row['tangentEnergy']}", flush=True)
    print(f"Comparison written to {args.output.resolve()}")


if __name__ == "__main__":
    main()
