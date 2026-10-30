"""Summarize verified outputs and counterfactual controls from the affine study."""

import argparse
import base64
import collections
import csv
import hashlib
import html
import json
import math
from pathlib import Path
import statistics


def failures(row):
    diagnostics = row.get("diagnostics", {})
    boundary = diagnostics.get("boundaryQuality", {})
    target = diagnostics.get("targetBoundary", {})
    result = []
    if row.get("area_error", 0) > diagnostics.get("areaErrorLimit", float("inf")):
        result.append("area")
    if diagnostics.get("missingInteriorArea", 0) > 0 or diagnostics.get("missingBeyondInward", 0) > 0:
        result.append("interior")
    if row.get("outside_envelope", 0) > 0:
        result.append("outward")
    if boundary.get("maximumCornerDistance", 0) > diagnostics.get("cornerDistanceLimit", .5):
        result.append("corners")
    if boundary.get("maximumExcessTurnDegrees", 0) > math.degrees(diagnostics.get("maximumExcessTurnLimit", .6)):
        result.append("turn")
    if boundary.get("energy", 0) > diagnostics.get("boundaryEnergyLimit", float("inf")):
        result.append("energy")
    if boundary.get("cornerDefects", 0) > diagnostics.get("cornerDefectLimit", float("inf")):
        result.append("defects")
    if any(boundary.get(key) != target.get(key) for key in ["holes", "components"] if key in target):
        result.append("topology")
    return result


def paired(rows, first, second):
    a = {row["case"]: row for row in rows if row["frame_name"] == first}
    b = {row["case"]: row for row in rows if row["frame_name"] == second}
    result = collections.Counter()
    for case in a.keys() & b.keys():
        left, right = a[case], b[case]
        if left.get("verified") and right.get("verified"):
            result["both_verified"] += 1
            result["second_fewer" if right["count"] < left["count"] else
                   "second_more" if right["count"] > left["count"] else "equal"] += 1
        else:
            result["first_only" if left.get("verified") else "second_only" if right.get("verified") else "both_failed"] += 1
    return dict(result)


def write_reports(directory):
    rows = [json.loads(line) for line in (directory/"results.jsonl").read_text().splitlines()]
    jobs = json.loads((directory/"jobs.json").read_text())
    cases = {case["name"]: case for case in json.loads((directory/"cases.json").read_text())}
    main = {(row["case"], row["frame_name"]): row for row in rows if row["group"] == "main"}
    for row in rows:
        if row["group"] == "corrected_geometry":
            main[row["case"], row["frame_name"]] = row
    effective = list(main.values())
    groups = collections.Counter(row["group"] for row in rows)
    comparison = {f"{a}_versus_{b}": paired(effective, a, b)
                  for a, b in [("uniform", "square"), ("uniform", "principal"), ("native", "uniform")]}
    undo = collections.defaultdict(set)
    for row in effective:
        if row["frame_name"] == "undo":
            undo[row["family"]].add(row.get("working_count"))
    repeats = [row for row in rows if row["group"] == "repeat"]
    old_main = {(row["case"], row["frame_name"]): row for row in rows if row["group"] == "main"}
    repeat_matches = sum((row.get("count"), row.get("verified")) ==
                         (old_main[row["case"], row["frame_name"]].get("count"),
                          old_main[row["case"], row["frame_name"]].get("verified")) for row in repeats)
    per_frame = {}
    for frame in sorted({row["frame_name"] for row in effective}):
        selected = [row for row in effective if row["frame_name"] == frame]
        per_frame[frame] = dict(cases=len(selected), verified=sum(bool(row.get("verified")) for row in selected),
                               median_solver_ms=statistics.median(row["total_ms"] for row in selected if "total_ms" in row))
    actual = []
    for case in cases.values():
        if case["family"] != "example2_actual":
            continue
        matching = [row for row in rows if row["case"] == case["name"]]
        default = next(row for row in matching if row["group"] == "gui_defaults")
        witnesses = [row for row in matching if row.get("verified") and row.get("count", 999) <= 2
                     and row["group"] != "unit_equivalence"]
        actual.append(dict(contour=case["variant"], anchors=case["anchors"],
                           registration_residual=case.get("affine_registration_residual"),
                           default_count=default["count"], default_verified=default["verified"],
                           two_shape_verified=bool(witnesses), witness_keys=[row["key"] for row in witnesses]))
    sweep = {frame: [row for row in rows if row["group"] == "ratio_sweep" and row["frame_name"] == frame]
             for frame in ["native", "square", "principal"]}
    sweep_summary = {frame: dict(cases=len(values), verified=sum(bool(row.get("verified")) for row in values),
                                 working_counts=dict(collections.Counter(str(row.get("working_count")) for row in values)),
                                 original_oracles_verified=sum(bool(row.get("oracle_verified")) for row in values))
                     for frame, values in sweep.items()}
    sources = ["src/gui/fill/profile_fit.cpp", "src/gui/fill/compact_fit.cpp",
               "src/gui/fill/compact_fit_coordinates.cpp", "src/gui/fill/compact_fit_geometry.cpp"]
    root = Path(__file__).resolve().parents[1]
    summary = dict(completed=len(rows), planned=len(jobs), cases=len(cases), group_counts=dict(groups),
                   frame_summary=per_frame, paired_comparisons=comparison,
                   working_pass_original_fail=sum(row.get("working_verified", False) and not row.get("verified", False) for row in effective),
                   undo_working_counts={key: sorted(value) for key, value in undo.items()},
                   repeat_matches=repeat_matches, repeat_cases=len(repeats), example2=actual, ratio_sweep=sweep_summary,
                   timeouts=sum(bool(row.get("timeout")) for row in rows),
                   protocol="Controlled runs: 3000 refinement evaluations, adaptive scheduling off, profile work recorded separately. Default, CPU, larger-budget and unit-equivalence runs are separate groups.",
                   correction="Hard-to-hard spans are straight in the editor. Original control-point-based frames for authored/saved paths were superseded by corrected_geometry runs. Native polygon controls and ratio sweep were unaffected.",
                   limitations=["Selected contours are not a random sample of all possible regions.",
                                "A supplied two-shape solution proves feasibility, not a global minimum.",
                                "An invalid supplied candidate does not prove two-shape infeasibility.",
                                "The oracle-pool ablation replaces the normal reuse pool with the supplied candidates; it does not isolate availability from candidate competition.",
                                "Saved cases.json, jobs.json and request files are the frozen replay inputs; prepare generates a new corpus with the current sampler.",
                                "Timing is a single-machine measurement; process startup and oracle checks are excluded from solver milliseconds.",
                                "The old seven-anchor example2 encoding is included but was not registered pointwise to the five ten-anchor copies."],
                   production_source_sha256={name: hashlib.sha256((root/name).read_bytes()).hexdigest() for name in sources})
    (directory/"summary.json").write_text(json.dumps(summary, indent=2)+"\n")
    flat = []
    for row in rows:
        seed = row.get("working_diagnostics", {}).get("profileSeed", {})
        flat.append(dict(case=row["case"], family=row["family"], variant=row["variant"], group=row["group"],
                         frame=row["frame_name"], budget=row["budget"], verified=row.get("verified", False),
                         count=row.get("count"), working_verified=row.get("working_verified"),
                         oracle_verified=row.get("oracle_verified"), oracle_count=row.get("oracle_count"),
                         initial_selection=seed.get("initialSelectionCount"), greedy_count=seed.get("greedyCount"),
                         seed_count=seed.get("count"), body_centers=seed.get("bodyCenters"),
                         thin_branch=seed.get("thinRegionCandidates"), curve_jobs=seed.get("curveJobs"),
                         curve_candidates=seed.get("curveCandidates"), body_candidates=seed.get("bodyCandidates"),
                         profile_trials=seed.get("profileTrials"), search_scale=seed.get("searchScale"),
                         solver_ms=row.get("total_ms"), failed_checks=",".join(failures(row))))
    with (directory/"results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(flat[0]))
        writer.writeheader()
        writer.writerows(flat)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    ratios = [1., 1.5, 2., 3., 4., 6., 8., 12., 16.]
    angles = [0, 15, 30, 45]
    figure, axes = plt.subplots(1, 3, figsize=(12, 6), constrained_layout=True)
    for axis, frame in zip(axes, ["native", "square", "principal"]):
        values = np.full((len(ratios), len(angles)), np.nan)
        lookup = {}
        for row in sweep[frame]:
            case = cases[row["case"]]
            i, j = ratios.index(case["component_ratio"]), angles.index(case["angle"])
            lookup[i, j] = row
            if row.get("verified"):
                values[i, j] = row["count"]
        axis.imshow(values, vmin=2, vmax=5, cmap="YlGnBu", aspect="auto")
        for (i, j), row in lookup.items():
            axis.text(j, i, str(row.get("count", "?")) if row.get("verified") else "FAIL", ha="center", va="center", fontsize=9)
        axis.set_xticks(range(len(angles)), angles)
        axis.set_yticks(range(len(ratios)), [str(value) for value in ratios])
        axis.set_title(frame+" search frame")
        axis.set_xlabel("Rotation (degrees)")
        axis.set_ylabel("Component stretch ratio")
    figure.suptitle("One exact circle-and-square family: verified shape count\nEvery cell has a verified two-shape witness; 3000 refinement evaluations")
    figure.savefig(directory/"stretch_rotation.png", dpi=160)
    figure.savefig(directory/"stretch_rotation.svg")
    plt.close(figure)

    image = base64.b64encode((directory/"stretch_rotation.png").read_bytes()).decode()
    table_rows = "".join("<tr>"+"".join("<td>"+html.escape(str(value))+"</td>" for value in [
        row["contour"], row["anchors"], row["default_count"], row["default_verified"], row["two_shape_verified"]])+"</tr>" for row in actual)
    escaped_data = json.dumps(flat).replace("<", "\\u003c")
    content = f'''<!doctype html><meta charset="utf-8"><title>Compact Fit transformation study</title>
<style>body{{font:16px system-ui;max-width:1250px;margin:32px auto;padding:0 16px;color:#18212b}}table{{border-collapse:collapse;font-size:13px}}td,th{{border:1px solid #ccd3da;padding:6px;text-align:left}}img{{width:100%}}.scroll{{overflow:auto;max-height:650px}}select,input{{margin:8px;padding:6px}}pre{{white-space:pre-wrap;background:#f3f5f7;padding:16px}}</style>
<h1>Compact Fit: transformations and shape count</h1>
<p>{len(rows)} fits across {len(cases)} cases. All six example2 paths were included as one intended circle-and-square family; five are exact affine copies with registration residual below 1e-12.</p>
<p>The production solver was not changed for this study. Counts are accepted only after verification in original coordinates. FAIL means a returned candidate did not pass those checks.</p>
<h2>Controlled stretch and rotation</h2><img src="data:image/png;base64,{image}">
<p>Non-uniform normalization can help or hurt. Principal-axis whitening made this sweep consistently use three shapes, while a verified two-shape construction exists in every cell.</p>
<h2>Actual example2 paths</h2><table><thead><tr><th>Contour</th><th>Anchors</th><th>Default count</th><th>Default valid</th><th>Verified two-shape witness found</th></tr></thead><tbody>{table_rows}</tbody></table>
<h2>What the counterfactual tests establish</h2>
<ul><li>Direct two-shape starting placements distinguish a feasible construction from the search's ability to discover it.</li>
<li>Replacing the normal reuse pool with supplied exact candidates recovers some low-count fits, but not all. This combines candidate availability and competition effects. Local combination/replacement limits are a separate bottleneck.</li>
<li>Scaling all dimensional settings with a uniform change of units restores matching results. Keeping fixed pixel limits changes relative accuracy requirements.</li>
<li>Working-frame verification can pass while original-coordinate verification fails. A scalar normalized allowance is not an affine transformation of the original error corridor.</li>
<li>Candidate generation uses discrete center, angle and ratio choices; grid and thin-region thresholds change after transforms. The observed count is not a monotone function of aspect ratio.</li>
<li>Cyclic start-anchor changes alter the default count without changing the closed curve, showing deterministic coordinate/order sensitivity.</li>
<li>Tiny overlapping clearance repairs native-float seams in the constructed touching-rectangle controls without adding shapes.</li></ul>
<h2>Matched comparisons and protocol</h2><pre>{html.escape(json.dumps({"paired":comparison,"frames":per_frame,"repeat_matches":repeat_matches,"repeat_cases":len(repeats),"timeouts":summary['timeouts'],"protocol":summary['protocol'],"correction":summary['correction'],"limitations":summary['limitations']},indent=2))}</pre>
<h2>All trials</h2><p>Filter family, group or frame. Solver milliseconds exclude process startup and oracle verification. Profile-generation work is distinct from the refinement evaluation allowance.</p>
<select id="family"><option value="">All families</option></select><input id="query" placeholder="Case, group or frame"><span id="number"></span><div class="scroll"><table id="results"></table></div>
<script>const data={escaped_data};const family=document.querySelector('#family'),query=document.querySelector('#query');[...new Set(data.map(r=>r.family))].sort().forEach(v=>family.add(new Option(v,v)));const columns=['case','group','frame','verified','count','oracle_verified','seed_count','body_centers','thin_branch','curve_jobs','profile_trials','solver_ms','failed_checks'];function draw(){{const values=data.filter(r=>(!family.value||r.family===family.value)&&(!query.value||JSON.stringify(r).toLowerCase().includes(query.value.toLowerCase())));document.querySelector('#number').textContent=values.length+' rows';const table=document.querySelector('#results');table.replaceChildren();let head=table.insertRow();columns.forEach(c=>{{let th=document.createElement('th');th.textContent=c;head.appendChild(th)}});values.forEach(r=>{{let tr=table.insertRow();columns.forEach(c=>{{tr.insertCell().textContent=r[c]??''}})}})}}family.onchange=query.oninput=draw;draw();</script>'''
    (directory/"report.html").write_text(content, encoding="utf-8")
    print(json.dumps({"completed":len(rows),"planned":len(jobs),"comparisons":comparison,"repeats":repeat_matches,"example2":actual}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, nargs="?", default=Path("build/transformation_study_20261030"))
    write_reports(parser.parse_args().directory.resolve())
