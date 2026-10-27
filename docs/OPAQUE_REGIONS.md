# Opaque region experiment

`fls_opaque_regions` audits a solid-fill SVG against a reference render, then
simplifies the filled geometry and its draw order. It runs without opening the
editor. Build with `tools/build.ps1`; the executable is in `build/Release`.

```powershell
build/Release/fls_opaque_regions.exe baseline.svg --reference splat.png --output artifacts
build/Release/fls_opaque_regions.exe baseline.svg --reference splat.png --output artifacts --strategy supports --passes 2
build/Release/fls_opaque_regions.exe baseline.svg --reference splat.png --original original.png --output artifacts --strategy supports --absorb-colors
build/Release/fls_opaque_regions.exe baseline.svg --reference splat.png --output artifacts --abstract-colors --passes 1 --chord-spans 1
build/Release/fls_opaque_regions.exe --self-test
python tools/opaque_regions/cli_test.py build/Release/fls_opaque_regions.exe
```

Inputs must have identical native pixel dimensions. The tool does not resize
them. The reference must come from the same checkpoint as the SVG. An optional
original raster can be supplied with `--original`. Use a separate output
directory; artifact paths that would overwrite an input are rejected.

## Filled project comparison

`generate.py` evaluates region proposals after the actual Compact Fit engine
fills them. Its contour document is an internal input. The review outputs are
editable `.3so` projects, native-rendered PNGs, differences, and `report.json`:

```powershell
python tools/opaque_regions/generate.py palette_project --svg palette_project/native/optimized.svg --output filled_comparison
python tools/opaque_regions/rate_distortion_test.py
python tools/opaque_regions/topology_transactions_test.py
python tools/opaque_regions/fit_repair_test.py
python tools/opaque_regions/curve_pipeline_test.py build/Release/fls_opaque_regions.exe
python tools/opaque_regions/budgeted_test.py
python tools/opaque_regions/hidden_contours_test.py
python tools/opaque_regions/compact_output_test.py build/Release/fls_opaque_regions.exe
python tools/opaque_regions/generate_test.py
```

Use the existing Python research environment. Explicit polygon-seed diagnostics
use Shapely's constrained Delaunay triangulation. Input dimensions and palette remain fixed. Use a new output
directory outside the input project. Internal geometry and fitting diagnostics
are retained under `internal/`.

### Whole-image shape budget

The default `--strategy budgeted` applies `--total-shape-budget` to the complete
filled image. Its default is 3,000 native shapes. The native `--fill-only` mode
exposes the same limit as `--compact-fit-total-shape-budget`. This limit is
separate from the per-region fitting limit. An oversized result cannot become
an accepted project, including with `--scene-validated-fills`.

Geometry search allocates complexity by the change in error against the
unchanged source raster. Proposals include straight contour chords, removal of
thin branches, hole filling, same-fill unions and enclosing shapes. Joint
proposals change draw order while completing contours, or fill a lower cutout
and restore its visible colors at later positions. Same-fill items can retain
different layer positions. Added hidden area has no penalty. There is no region
quota.

The topology cleanup uses current visible owners for cutout contents. It inserts
their paint above the completed region and below the existing upper stack.
Small fragments can transfer to adjacent existing colors when source pixels
support that choice. Important fragments remain in the donor while other
fragments transfer. Complete scene checks charge both donor and recipients.
Hidden boundary chains can shorten through the available upper coverage.
`--cleanup-edits` bounds this search, independently of the output count.

The final ordering pass freezes complete visible ownership, rebuilds all
overlap dependencies, and topologically sorts available items by geometry
complexity. Same-color items can retain separate positions. The ordering report
records the fixed regions, graph edges, resulting order and contour costs.

The search charges the complete replacement geometry, including restoration
patches. It uses polygon triangulation counts as a preliminary planning estimate, with
additional costs for connected regions and holes. These estimates are not
reported as measured Forza counts. The final native project supplies the actual
shape count. Failed native trials feed another topology cleanup and curve
recovery pass. The final shape limit applies to measured placements. Compact Fit
receives empty leeway and an explicit common domain that bounds outward support.
Topology cleanup charges 256 per hole and 32 per connected region, in addition
to its polygon estimate. Search effort is bounded separately from output complexity.

Before native filling, the Bucket cubic contour fitter recovers mixed line and
Bezier spans. `--curve-tolerance` sets its native-pixel tolerance, default one.
Each replacement must retain valid filled geometry inside the domain and pass
the cumulative source-image budgets. Ownership operations use consistent
polygon copies on the native clipping grid; emitted curves retain their control
points. Straight spans retain hard corners. The curve route does not spend spare
triangle counts on additional polygon vertices. Setting the curve tolerance to
zero retains the polygon diagnostic route.

Every final region uses `profile::fillRegion`, the editor's curve-first Compact
Fit entry point, with GPU work enabled and the deployed catalog. The default
evaluation budget is 60,000 per region. The tool records profile initialization,
GPU diagnostics, actual shape usage and any fitting warnings. Explicit
`--compact-fit-cpu` selects native CPU diagnostics. Triangle initialization
requires the explicit polygon-seed option. Native comparison renders use four
samples per pixel axis to measure curved boundaries.

After fitting, a scene pass selects native placements by same-color coverage at
the output renderer's sample positions. Appearance groups share coverage while
placements retain their original stack positions. An exact native sample
comparison restores any placements needed for the original appearance. A second
pass removes placements completely covered by later fitted geometry. The pass
requires pixel-identical before/after renders through the output renderer;
otherwise it retries with geometric clearance or retains the original scene.
Empty paint groups are removed. The total shape cap applies to the saved scene
after this validated reduction. Reports keep generated counts, removed counts,
final counts and retained region mappings separate. Fitting still receives
empty leeway. Internal unculled renders and culling differences document the
comparison.

The native image dimensions and fill colors remain fixed. Simplification changes
contours instead of blurring the raster. `--source-local-growth` bounds error
growth in each sliding source-image window during structural search and final
validation; its default is 32 in 8-bit RGB RMSE. `--source-rmse-growth` bounds final whole-image error
relative to the source-to-opaque baseline. The budgeted strategy defaults to 8;
the bidirectional comparison retains its default of 1. These quality limits
remain configurable independently of the hard native-shape cap. Conversion from the
derived contours to Compact Fit targets, target-to-filled error, original
baseline-to-filled error, and source-to-filled error are reported separately.
`--render-rmse` and `--local-rmse` bound the final fitter's error against the
derived contours. The external domain remains fixed and the actual filled
silhouette is checked independently.

When native rendering exceeds a local error limit, the budgeted route can
append bounded correction contours from the derived guide or original opaque
baseline. It preserves existing Bezier paths and checks the cumulative source
budgets before refitting. Every correction uses the same GPU Compact Fit entry
point and counts toward the total shape cap. These repairs do not relax the
final image-error limits.

Full-scene renders reconcile incremental error accounting at checkpoints.
`stack_checkpoint.json.gz` retains the current geometry, source hashes and
optimization measurements; `--resume-stack` continues a matching checkpoint.
`--max-edits` bounds search work. Successful output is `after.3so` and
`after.png`. A result within the shape budget that fails a quality check is
labelled `budget_candidate.3so`; it does not replace an accepted output.

### Bidirectional comparison

`--strategy bidirectional` retains the earlier fixed-ownership comparison.
The search compares bidirectional hidden-contour edits, same-fill grouping,
cutout completion, and promotion of covered colors to later paint positions.
Hidden-chain chords can remove or add painted territory within the occlusion
envelope. A coordinated completion charges both the simpler lower contour and
every required upper repair. Complete visible color geometry and the external
silhouette are validated. Dependencies use fixed visible ownership and are
rebuilt after geometry changes. Unchanged source curves remain available to the
contour exporter. Hidden area has no ratio cap or size penalty.

`--ownership-tolerance` sets a native-coordinate boundary band, default 0.25
pixels. Color ownership can change inside that band; the complete rendered
result must still pass the global, local, and source-error checks. Zero retains
exact visible ownership. Geometry differences inside and outside the band are
reported separately. The original raster is not resized or filtered.

`--window-start` and `--window-stop` select input paint positions for proposal
generation. `--search-states` bounds search work and `--shortlist` selects how
many complete proposals reach fitting. Neither option limits output regions.
The search uses complete-stack contour complexity to form a diverse shortlist.
An exploratory completion can reach measurement even when its SVG proxy grows.
Its rendering and ownership checks still apply. Final selection uses
`--weight-shapes * actual_shapes + --weight-regions * actual_regions`, after
rendering and source-error checks. Geometry scores alone cannot establish an
improvement. The retained baseline is preferred on a measured-cost tie.
The default region weight is 256 and the default shape weight is 1. There is
no region quota; an additional draw region must earn its cost through simpler
filled geometry elsewhere in the stack.

Each fill receives the complete derived contour with empty leeway. The batch
generator uses the editor's curve-first GPU pipeline. `--polygon-seeds` selects
the explicit polygon initializer for comparison. The original Bezier contour
remains the target and quality reference.
Seed construction uses adaptive native-coordinate geometry; it does not
resample the image. Failed fills remain explicit.

`--compact-fit-evaluations`, `--compact-fit-shape-budget`,
`--compact-fit-boundary`, `--compact-fit-inward`, and
`--compact-fit-area-ratio` expose the fitter's work and quality settings.
`--compact-fit-workers` bounds concurrent independent fits. Project groups are
restored to their derived draw order after fitting. The batch default is 6,000
evaluations per region; changing work effort does not change quality limits.
Seed polygons come from the native Compact Fit target conversion. Its image
error against the opaque input is reported separately. Native transform
reconstruction validates triangle coverage before optimization. Failed polygon
seeds receive one retry with subdivided long edges. Successful regions retain
their cached results. The retry
uses the same target contours, work settings, and quality limits.
`--render-rmse`, `--local-rmse`, `--max-error`, and `--source-rmse-growth`
guard the rendered result. Cumulative candidate error is checked against the
original opaque baseline. Reports separate source-to-opaque error from fitting
and optimization error.

The native `--fill-only` mode saves a project, reloads it, and renders its actual
placements through `NativeShapeRenderer` in a hidden OpenGL context. Each input
region has its own ordered group; identical fills can retain separate stack
positions. Each group also retains a hidden editable Pen contour. All emitted
shapes have solid colors and full opacity. Scene-render
antialiasing and fitting allowances remain separate from shape opacity.
Native comparison renders use four samples per pixel axis for boundary
antialiasing. Saved images retain the input dimensions. Source pixels are not
resampled. The native CLI exposes `--compact-fit-render-samples`.

Per-region verification is required by default. The optional
`--scene-validated-fills` mode can evaluate retained placements with region
quality warnings. Every region must supply placements within its shape budget.
Cancellation, missing placements, and fatal errors cannot pass. The complete
native render must pass all image and silhouette limits before selection.
Reports retain every region warning and state the verification scope.

Per-region results are cached by engine, catalog, target geometry, polygon seed,
and fitting settings. `--compact-fit-cache` can share that cache across runs.
Each finished region also updates a progress report. Missing placements and
fatal fitting errors cannot win final selection. Retained quality warnings
require explicit scene validation as described above. A valid
baseline is retained when no complete candidate improves its measured score.
When no complete fit passes validation, the report states that no valid filled
output is available. Diagnostic counts do not imply a complete generated image.
`search_plan.json.gz` stores validated complete-stack proposals before fitting.
`--resume-search` reuses that search with the same opaque baseline.

## Shared-shade raster prototype

`tools/opaque_regions/posterize.py` starts from the original raster. This optional
research CLI uses NumPy, SciPy, Pillow, and OpenCV. It learns shared color
families with at most four solid shades each. Distinct chromatic residuals can
introduce a family even when their frequency is low. Every subsequent pixel and
paint item uses the same palette.

```powershell
python tools/opaque_regions/posterize.py source.png --output tonal
python tools/opaque_regions/posterize_test.py
build/Release/fls_opaque_regions.exe tonal/stacked/manifest.json --raster-regions --reference tonal/posterized.png --original tonal/source.png --output tonal/vector --conversion-rmse 4.5 --conversion-max-error 128 --passes 0
```

This is an experimental combination inspired by
[PosterChild](https://cragl.cs.gmu.edu/posterchild/),
[region-merging energies](https://arxiv.org/html/2409.15940v1), and
[rolling guidance](https://www.cse.cuhk.edu.hk/leojia/projects/rollguidance/).
It is not an author-code reproduction. Color labeling uses deterministic
checkerboard coordinate descent followed by connected-region transactions.
Each region transaction reduces the combined perceptual fitting error,
boundary cost, and connected-component cost. There is no output-region quota
or area-only deletion. The final transaction energy is recomputed independently
of its cached gains.

All processing retains the native pixel grid. `--guidance-scale` controls
texture abstraction in native pixels, default 4; zero disables it. A color
guided filter supplies the rolling guidance. The original raster remains the
error reference. `--guidance-range` and `--guidance-iterations` control that
filter. `--shades-per-family` accepts 1 through 4. `--hue-width`,
`--neutral-chroma`, and `--shade-error` control family and shade extraction.
`--boundary-weight`, `--component-weight`, and `--label-sweeps` control spatial
labeling. Structurally supported labels are locked using the guidance image and
`--edge-contrast`. These locks do not preserve every original texture edge.

The raster output includes `source.png`, `structure.png`, `palette_only.png`,
`spatial.png`, `posterized.png`, `difference.png`, `labels.npz`, and `report.json`.
The report separates original-to-structure error from original-to-posterized
error, and records all family memberships and component histograms. Opaque
inputs and binary transparent domains are supported; fractional input opacity
is rejected. The domain mask remains unchanged during labeling.

Paint construction compares several deterministic color orders. Lower supports
may cover colors drawn later. Every trial is composited on the native grid and
must reproduce the complete final label map exactly. The selected raster order
uses connected components, holes, and contour corners as a cost proxy. This
proxy does not establish the best native encoding or predict Compact Fit
placements. `flat/manifest.json` retains disjoint color masks;
`stacked/manifest.json` contains the selected overlapping paint masks.

`--raster-regions` traces those binary masks using the vendored Potrace library.
`--trace-tolerance` and `--trace-corner-threshold` expose curve fitting controls;
rectangular masks retain exact corners. Speckle deletion is disabled. Native
Pen conversion decomposes compound color paths into actual Bucket targets.
Color groups and selectable path counts remain separate. Conversion and later
structural error budgets are enforced normally. The example command exposes
the experimental raster-to-vector limits explicitly; every image error remains
in the report. The raster posterization is not an appearance-preserving edit of
the original source. Inspect its tonal abstraction and detail retention before
using the resulting guide.

Viewport clipping retains unchanged interior Bezier spans. Boolean clipping
uses adaptive polygons, and curve restoration only selects spans inside the
viewport. Closure and Pen-target checks run on the resulting complete geometry.

### Original-source boundary refinement

An optional second raster pass fits existing region boundaries to the original
pixels while retaining the palette and input paint order:

```powershell
python tools/opaque_regions/refine.py tonal --output tonal_sharp
python tools/opaque_regions/boundary_refine_test.py
build/Release/fls_opaque_regions.exe tonal_sharp/stacked/manifest.json --raster-regions --reference tonal_sharp/posterized.png --original tonal_sharp/source.png --output tonal_sharp/vector --passes 0
```

Use a separate, non-nested output directory. `--band` limits motion to a band
around the input boundaries, in native pixels, default 8. `--sweeps` controls
iterations, default 12; zero retains the input labels. `--boundary-weight`
penalizes irregular boundaries, default 12. `--edge-scale` controls how original
color contrast weakens that penalty, in scaled Oklab units, default 6.

Candidate pixels take a neighboring label. Each accepted move reduces original
RGB error and the local source-fitting objective. Local topology checks retain
both four- and eight-connected topology for individual colors and every
cumulative mask in the fixed paint order. The complete rebuilt supports must
retain their component and hole counts. Labeling and opacity preserve the
source domain. No missing interior feature is created by this pass.

`unrefined.png` and `refinement_difference.png` isolate boundary motion. The
report retains input hashes, source error before and after, and source-aligned
gradient response at strong original edges. That response is a numerical edge
agreement measure, not visual approval. A decreased response retains the input
map. Inherited guidance locks describe the earlier labeling stage; any of their
labels changed during refinement are reported explicitly.

The refined masks still need the native export audit. Tracing may change the
number of Bucket paths even when raster topology is retained. Finer contours
can increase segment count and raster-to-vector coverage error; all conversion
and structural gates remain in effect. Full-artwork Compact Fit measurements
are separate from these geometry metrics.

### Original-source color fitting

`recolor.py` fits colors after the region geometry is established. Its SVG input
must be a validated, closed, solid-fill export from the same region project:

```powershell
python tools/opaque_regions/recolor.py tonal_sharp --svg tonal_sharp/vector/optimized.svg --output tonal_color
python tools/opaque_regions/recolor.py tonal_sharp --svg tonal_sharp/vector/optimized.svg --output tonal_mean --method mean
python tools/opaque_regions/palette_refit_test.py
```

The default `source` method considers colors observed in the original pixels
assigned to each visible label. Candidates near the weighted geometric median
and RGB mean are evaluated by mean Oklab distance. A global RGB error constraint
limits the permitted tradeoff. `--source-rmse-growth` sets its growth allowance
in eight-bit channels, default zero. The input palette remains an available
fallback. The `mean` method supplies an RGB least-squares control. Reports also
record the fixed-label RGB error floor and both mean and RMS Oklab distances.

Palette family membership and its limit of four slots remain fixed. Each slot
can change its RGB value. Hidden support pixels do not contribute to the fit.
An observed source representative can retain a dominant hue that an average
would dilute. A single fill still approximates all source colors assigned to
its label; mixed source hues cannot all be reproduced by changing that fill.

`candidate.svg` changes only path fills. Path commands, transforms, fill rules,
opacity, order, and dimensions retain their input values. Masks and labels are
unchanged. Coincident fitted colors can share an appearance group while their
draw items retain separate stack positions. No geometry reduction is claimed
for this stage.

Outputs include `input.svg`, `candidate.svg`, `before_palette.png`,
`posterized.png`, `palette_difference.png`, the unchanged masks, and a report
with per-slot RGB values and input hashes. Use a separate, non-nested output
directory. Export correctness is checked against a render of the recolored
candidate; original-source color error is measured separately. Exact agreement
with that export reference does not imply exact reconstruction of the source.

### Selective hidden completion and contour fitting

`simplify.py` constructs new paint supports from a validated opaque SVG and the
existing palette project. It uses the Python research environment with NumPy,
SciPy, OpenCV, Pillow, PySide6 and Shapely. It does not install dependencies.

```powershell
python tools/opaque_regions/simplify.py tonal_color --svg tonal_color/vector/optimized.svg --output tonal_simple
python tools/opaque_regions/simplify_test.py
```

The native audit executable must already exist. `--native-tool` selects its
path. The input must use native coordinates, closed M/L/C/Z paths, solid opaque
fills and the project palette. Nested output directories are rejected.

Visible ownership comes from complete filled SVG geometry in reverse paint
order. Ownership remains fixed during support construction. The initial paint
supports retain the validated source coverage. The search tests
hole filling, chords, rectangles, convex hulls, component hulls and closing at
several native-pixel scales. A lower support may cover later original owners.
It does not automatically absorb every available connected area. Every trial
rebuilds dependencies from fixed ownership; color-level cycles reject the trial. Equal
geometry savings favor proposals that block less neighboring simplification.

After color-level completion, components receive separate draw positions. Local
completion and exact-color grouping can place the same fill at different stack
positions. A component trial can resolve an ordering conflict by transferring
the conflicting original owners into separate upper paint positions. Their
complete visible regions move together; surviving lower supports retain their
other owners. The combined replacement must reduce encoded cost and pass a
fresh dependency check. Same-fill overlap imposes no ordering edge. Unresolved
cycles are logged and rejected. There is no region-count quota or hidden-area ratio limit. The
greedy search uses the encoded line/cubic, component, path and hole cost;
`--passes` bounds search sweeps, default two. Final complexity is measured again
by the native exporter, including required decomposition.
`--passes 0` fits contours while retaining the input draw positions. It also
retains fully occluded support items, which can affect boundary antialiasing.
`--weight-paths` defaults to 256 and `--weight-holes` to 96. Components cost one
additional unit each; lines cost one and cubics cost three. The same weights
are passed to both native audits. These weights express a topology preference,
not a limit on the number of output regions.

An existing full-domain background retains its first draw position. Enclosed
regions initially remain assigned to that foundation. A completion can transfer
their ownership into upper restoration items when necessary. The combined
transaction charges those added items before acceptance. The foundation is
excluded from component merging.

The native `supports` strategy also tests partial-region repainting. Each
completion retains an exact visible-geometry alternative with its original
cutouts. Recursive repainting compares its combined cost against that
alternative. Repeated color recursion terminates with exact geometry; an
unhelpful hole fill leaves the valid layer available. Diagnostics report these
fallbacks together with the final ownership and ordering checks.
`--visible-area-tolerance` bounds both missing original-color coverage and the
final filled-geometry difference per color. Its default zero uses
`--area-tolerance`, which also sets the positive-intersection threshold.
Silhouette and image-error limits remain separate checks.

### Joint region and order search

`--search bottom-up` constructs alternative stacks while choosing their painted
geometry and grouping. The final visible color masks stay fixed. Visible
ownership can be repartitioned between paint positions of the same color.
Painted coverage and committed visible territory are tracked separately, so a
simple lower layer can cover details that receive separate upper paint paths.
Every added repaint and every disconnected component contributes to cost.

An initial grouping pass starts from the original paint shapes. It tests
same-color source pairs against fixed visible ownership, completes their joint
geometry and rebuilds the complete dependency graph after each accepted merge.
Coordinated proposals can move a blocking region above a merge and complete its
geometry again. Acceptance charges both changed shapes. The foundation stays in
place. Cycles reject a merge. Remaining compatible items receive a deterministic
topological order, preferring simpler geometry when several items are ready.
Groups can retain several stack positions of the same color.
Numerical score changes and disconnected SVG wrappers do not count as topology
improvements.

```powershell
python tools/opaque_regions/simplify.py tonal_color --svg tonal_color/vector/optimized.svg --output tonal_stack --search bottom-up --weight-holes 512
python tools/opaque_regions/bottom_up_test.py
python tools/opaque_regions/stack_export_test.py
```

The beam search compares complete color groups, connected visible components
and source ownership fragments. Proposals include filled holes, convex hulls,
axis-aligned and rotated envelopes, and contour completions at several scales.
Candidates can exploit uncommitted upper coverage. Committed territory blocks
later paints of other colors. The initial full-domain foundation can remain
uncommitted, allowing its enclosed details to be restored above completed
regions. Same-color paints may occupy several stack positions.

Search cost includes connected paint regions, holes, polygon points, reflex
corners, turning reversals and deviation from preferred simple turning angles.
`--weight-angles` defaults to four. This is a geometry surrogate; final acceptance
still requires lower actual exported encoding cost and the existing native
Bucket, silhouette and final-render checks. Hidden area has no ratio penalty.
There is no region-count quota or required geometric primitive. Available upper
coverage decreases as the stack grows; a strict monotonic complexity constraint
is not imposed on individual layers.

`--beam-width` defaults to four and `--search-states` to 96. The state setting
bounds grouping attempts and beam expansions separately. These bound search
effort, not output region count. `--window-start` and `--window-stop` select a
zero-based, half-open range of source draw positions; omission searches the
complete stack. Paths outside the window retain their original curve commands
and fill rules during candidate generation. Their coverage remains part of the
complete-stack validation. A source path that becomes completely hidden can be
removed; diagnostics retain its source position. `--passes` applies to the
selective search.

The working domain is the selected supports' convex hull clipped to the original
silhouette. Visible lower-color territory in this domain can receive upper
repaints; lower source paths remain in place. This allows local cutout removal
without including the entire foundation in the replacement window. Candidate
priority balances painted cost, visible area covered and the complexity of newly
committed territory. Consecutive same-color paints are combined geometrically,
then completed together on subsequent expansions. Bounded finishing attempts
test complete stacks when the beam stops with unresolved visible territory.
Candidate ranking estimates the remaining visible repaint cost. Coverage hidden
by fixed upper layers does not contribute to this estimate. Simple interior
cores can defer intricate portions of a connected visible region.

Direct cutout proposals group enclosed areas by the visible lower color that
they expose. They fill those cutouts in the lower paint and complete upper
repaint shapes through available coverage. The full stack is checked again.
Repaints can make earlier source paths redundant; removing those paths is part
of the same transaction. Combined cost includes every new connected paint shape.

Saved joint-search plans include the new visible ownership partition and source
path references. Resuming validates their aggregate color masks against the
original baseline before checking dependencies. Reports retain search effort,
per-layer topology, paint area, committed visible area and complete-stack error.
When no cheaper valid stack exists in the explored states, the original SVG is
retained. This experimental search does not prove a globally optimal order.

Contour fitting favors lines, stable corners and few cubic spans. A penalty on
the cubic third derivative discourages unnecessary variation. Full closed
contours and filled geometry are checked, including cutouts and neighboring
components. A failed fit tightens only the affected ring or retains its
polygonal boundary. Rectangular domains constrain fitted control points to the
canvas. Unchanged components and individual closed contours retain their
original Bezier encoding when it costs less than the fitted replacement.
Intact cubic spans can also be recovered inside contours with edited closures.
Nonzero source paths must pass a complete filled-geometry equivalence check
before their contours can be reused under the output fill rule.
This is an experimental implementation inspired by
[depth-ordered completion](https://arxiv.org/html/2409.06648v1) and
[perceptual contour fitting](https://www.cs.ubc.ca/labs/imager/tr/2020/ClipArtVectorization/),
not an author-code reproduction.

`--boundary-tolerance` bounds fitting displacement in native pixels, default
one. `--fairness` controls fitting regularization, default one. Visible boundary
motion is allowed within these limits; the cumulative masks' topology is not
locked. No image is resized. The palette and fully opaque solid fills remain
fixed.

`--flatten-tolerance` sets the adaptive curve approximation used for geometry
operations, default 0.05 native pixels. `--area-tolerance` sets positive-area
intersection checks, default 0.000001 square native pixels. The same values are
passed to both native audits. Ownership assignment retains smaller regions;
an intersection threshold does not delete their IDs.
Positive-area overlay checks use a native-pixel grid of 0.00000001, matching
the native Boolean precision. Exact containment is checked before subtraction.

Final-output rendering checks cumulative input-to-output RGB RMSE, every
sliding 16-pixel window, peak error, original-source RMSE growth, alpha coverage
and complete silhouette area. Defaults are `--render-rmse 4`, `--local-rmse 24`,
`--max-error 255`, `--source-rmse-growth 0.25` and
`--silhouette-tolerance 0.001`. The peak allowance permits high-contrast edge
motion inside the geometric band; global and local error checks still apply.
Boundary antialiasing does not change shape opacity. A failed gate or absent
encoded-complexity reduction retains the input SVG in `optimized.svg`.
Fitting retries at progressively tighter displacement tolerances without
relaxing the image-error limits. When a coarser fit fails and a finer fit passes,
up to three interval refinements test intermediate tolerances. The cheapest
passing encoding is retained, with its own render and tolerance recorded.

The native executable audits the baseline before optimization and the candidate
afterward. Its separate conversion gate is 0.5 RMS / 16 peak, without a
structural bypass. Reports separate original-source loss, baseline-to-candidate
change and native export error. They include actual Bucket anchors, lines,
cubics, paths, holes, components, ordering restrictions, silhouette difference
and runtime. Turning-direction reversal density is a roughness diagnostic; it
also counts genuine features and hidden boundaries. Compact Fit placement
counts remain a separate measurement.

Artifacts include before/after renders, candidate and accepted SVGs, difference
images, ownership-order diagnostics and both native audit reports. Rejected
candidates remain available for inspection. Compressed support plans retain
geometry and ownership before and after component optimization. `--resume-plan`
validates the input and ownership hashes, complete geometry and ordering before
reusing a plan. A global-stage plan continues component optimization according
to `--passes`; a complete plan repeats only fitting and export validation.

## Export audit and appearance validation

The audit compares Qt SVG rendering, captured paths, and a closed-path export
roundtrip. It checks opacity, fill rules, transforms, and paint order. Fractional
boundary coverage is reported separately from fill opacity. The source-to-opaque
comparison measures loss from replacing splat coverage and alpha compositing
with opaque curve interiors. Nonopaque fills are rejected; their forced-opaque
conversion error is measured separately.

Conversion must pass `--conversion-rmse` and `--conversion-max-error`, with
8-bit channel defaults 2 and 32. A failed conversion retains the baseline and
returns exit code 3. `--structural` explicitly permits a failed source conversion
for a structural experiment on an opaque SVG. The report retains the failed
conversion result. Opacity, capture, roundtrip, and baseline-to-optimized
validation remain required. Source conversion error and structural error are
reported separately.

The final guide is rendered through Qt SVG at native dimensions. Structural
limits are `--render-rmse` and `--render-max-error`, defaulting to 0.5 and 16 in
8-bit channels. Alpha error is also bounded. Errors are cumulative against the
unchanged baseline. If no lower-cost result passes, the baseline is retained.
Execution errors return 2; rejected baselines return 3; valid optimized or
unchanged results return 0.

SVG strategies require each paint object to pass the editor's SVG-to-Pen
conversion: one closed outer contour with valid cutouts. The audit also checks
that SVG winding rules and Pen cutouts describe the same filled geometry.
Disconnected contours cannot count as one Bucket target. Reports include the
actual Pen anchor count, hard and soft anchors, and conversion failures before
and after export. Native crossing predicates use coordinate precision separately
from curve sampling tolerance.

Boolean clipping, collinear cleanup, and curve matching share ClipperD's binary
coordinate grid. The report records its actual step, approximately
7.45e-9 native pixels. Curve recovery starts at original span anchors where
available, preserving complete unchanged Beziers across a Boolean merge.

Captured paths are explicitly clipped to the native SVG viewport before ownership
and color processing. The report and `viewport.svg`/`viewport.png` artifacts
measure this step against the original SVG render. Off-canvas paint contributes
no ownership or Bucket contour. Clipping preserves original curve spans where
possible and must pass the rendering limits.

Before structural edits, incompatible source paths are resolved into filled
components. Unchanged valid paths retain their curves. Boolean boundaries reuse
original curve spans when their complete flattened geometry agrees; other
boundaries use straight segments. A triangulation fallback and bounded cuts
handle components that still fail conversion. Unconvertible area can be
discarded only within the cumulative intersection-area tolerance; the report
records that area, and complete rendering and silhouette checks still apply.

Degenerate cutouts that fail native Pen conversion can be filled within the
same cumulative geometry budget. Repeated-vertex excursions receive complete
filled-geometry checks before removal. Point contacts can receive bounded
junction repairs or cuts through the contact. Reports distinguish these changes
from fallback triangulation and retain native failure geometry for diagnostics.

`bucket_candidate.svg`, its render, and `bucket_difference.png` show this
conversion separately from optimization. `bucket_baseline.svg` is saved only
after conversion passes the structural rendering and silhouette limits.
Splitting paint commands can change antialiasing even when the filled geometry
is identical. A failed candidate stays diagnostic; it is never reported as a
validated baseline. An unconvertible input remains available in `baseline.svg`.
Successful conversion without structural improvement has status `normalized`.

`input_to_topology_baseline` measures the cumulative changes from viewport
clipping, optional color abstraction, and native-target conversion.
`topology_baseline_to_optimized` measures structural edits against that validated
baseline. `optimization_difference.png` shows only this second comparison.
The existing `baseline_to_optimized_paths` field measures the complete input-SVG
to final-SVG difference, including normalization.

## Incremental geometry and ordering

`--absorb-colors` runs local color and geometry transactions after native target
validation and before the selected `incremental` or `supports` strategy. It
merges adjacent ownership regions using a neighbouring fill or an existing
palette color that improves the estimated source fit. Each
accepted transaction reduces actual emitted contour cost and passes cumulative
rendering checks. A failed proposal retains earlier accepted transactions.
Joint class proposals can substitute all remaining items of one color with an
existing neighboring fill while merging a compatible pair geometrically. The
unchanged items retain their own contours and stack positions. A proposal must
reduce combined geometry cost and pass the complete source checks; a palette
substitution alone has no geometry saving. Each adjacency sweep tests the first
available anchor for each directed color pair.
If the merged support completely covers other substituted items' fixed visible
ownership, a joint proposal transfers that ownership to the support and removes
their draw items. The complete filled regions receive containment checks. The
support's dependencies are rebuilt against the remaining fixed owners. An
ordering conflict retains the separate substituted items and their positions.
This mode has no palette-only fallback and cannot be combined with
`--abstract-colors`.

Supply `--original` to judge source fidelity against the original raster;
otherwise absorption uses the checkpoint render. `--absorption-source-growth`
limits the increase in source RMSE over the validated native baseline and
defaults to zero. At zero, each transaction must also preserve or improve the
current source RMSE. Later topology edits retain the accepted color stage's
source-quality gain. `--absorption-edge-contrast` sets the native-pixel source
gradient threshold for protected edges, default 24. Supported edge contrast
must retain `--contrast-retention`, with a small raster quantization allowance.
`--abstraction-local-rmse` limits newly added source error within each local
window. Cumulative guide error remains bounded by `--render-rmse` and
`--render-max-error`. The local source-damage limit measures newly added source
error; a correction that reduces source error can change the imperfect guide
more than that local limit.

`--color-tolerance` and `--detail-scale` rank eligible adjacent colors by
contrast, visible area, and shared boundary support. They impose no output
region count or area cutoff. A predicted source improvement can also make a
proposal eligible; actual final-output rendering must still pass every guard.
Merged supports also try enclosing rectangles and convex hulls. Candidate
geometry ranks by encoded savings, then added area and fixed-owner dependency
count. Estimated source error breaks remaining ties between palette fills.
`--absorption-passes` controls adjacency sweeps,
default two; zero disables those transactions independently of `--passes`.
Adjacency and ownership retain native dimensions. Untouched curve spans remain
available during geometric unions.

Absorption also transfers individual fixed ownership components between paint
items. It can remove a local color fragment while retaining other components
owned by the same source item. Compatible neighbors can join through the
transferred territory. Other donor components covered by the new support either
transfer under the source checks or receive separate opaque restoration items.
The donor retains its other ownership and can simplify its contour. These are
combined-cost transactions with rebuilt fixed-owner dependencies. Source errors
inside one connected ownership component can remain when correction needs a new
partition. Retained fragments and source error remain visible in the report.

`absorbed.svg`, `absorbed.png`, `absorption_difference.png`, and `absorption.json`
record the accepted color stage separately from subsequent support construction.
`absorption_regions_before.svg` and `absorption_regions_after.svg` contain the
fixed visible ownership relabelled by accepted merges. Their component and
area histograms count visible fragments separately from emitted Bucket targets.
`absorption_target.png` records the decoded quality target used by the editor;
`absorption_input.png` records the validated native baseline for independent
pixel comparisons.
The report includes connected paths, exact fills, contour cost, source error,
protected edge checks, ordering, and individual rejection causes. Native target
validation and the final renderer remain required after both stages.

`--strategy supports` reconstructs the complete image using unchanged colors.
It starts from the fixed visible union of each color and tries deterministic
color orders. A top-down pass connects and simplifies each color through
territory whose owners will be drawn above it. A forward pass constructs broad
color supports and repaints covered earlier colors in later positions. Filling
an internal cutout and repainting its contents form one proposal. External
transparent holes remain part of the locked domain.

Domain normalization can remove closed cracks only within the cumulative
silhouette-area tolerance. The report records removed hole and excursion counts
and the complete filled-geometry difference. All accepted reconstructions are
still checked against the original domain. The bottom color can use its own
simplified support; a full-domain underlay is one option rather than a
requirement.

Sparse straight bridges can connect same-color components through covered
territory. Each bridge must pass complete containment and native Pen checks.
Both the bridged support and its hole-filled completion compete with the
unconnected geometry. Hidden area remains a tie-breaker after encoded cost.

Proposals are scored by connected target count, emitted segments and control
points, holes, and dependencies. Disconnected compound contours incur the cost
of all their components. Native conversion runs before final scoring and counts
any required decomposition. Every accepted path passes the native Pen converter.
Color appearances can occupy multiple layer positions; there is no region-count
or palette-size target. Reports include `single_path_fills`,
`multiple_path_fills`, and the complete position list for each color.

Ownership is reassigned from fixed original color territory after construction
and native conversion. The dependency graph is rebuilt for each complete
candidate. Complete filled geometry, the external silhouette, and final SVG
rendering must pass validation. Rejected complete candidates remain diagnostic;
the unchanged baseline remains active when no lower-cost reconstruction passes.
`--passes` bounds the number of forward color orders in this strategy, while
`--shortlist` bounds the retained deterministic order variants.

Valid constructions that reduce the complete objective enter the cleanup pool.
Those with the lowest cost, fewest paths, fewest holes, and fewest
encoded coordinates form a cleanup shortlist. Each receives incremental cleanup for connected
grouping, cutouts, and support completion. This pass uses the original color
ownership assigned to the constructed draw positions, plus cumulative original
image guards. `--passes` and `--chord-spans` also apply to this cleanup.
Component-split search is disabled; color construction already assigns separate
draw positions where needed.

Complete final visible geometry is compared with every original color union.
The maximum per-color symmetric difference must stay within `--visible-area-tolerance`;
its default of zero uses `--area-tolerance`. This visible-geometry budget is
separate from numerical intersection and containment decisions.
The sum and maximum are reported separately. Silhouette checks retain their own
area budget. Unclipped rectangle and hull proposals can absorb sub-tolerance
Boolean cracks only when these geometry limits and the final renderer permit it.

`support_trials.json` records each completed order trial as it finishes.
`fixed_color_ownership.svg` contains the unchanged ownership polygons used for
validation. `support_latest_plan.svg` is the latest construction before native
conversion; it is diagnostic and may contain rejected geometry.

Merging requires a connected, Pen-compatible support. Proposals include
silhouette-clipped rectangles and hulls, connected domain supports, and
completions clipped around ownership that must remain below the proposed item.
Hidden support area has no cost penalty. The objective measures emitted geometry
and independent paint targets rather than compound SVG containers.

An ordering conflict can trigger a joint transaction: trim blocking supports
away from the candidate's fixed original ownership, split the retained support
into connected targets, and rebuild its dependencies. Each retained target must
cover its assigned original ownership. The transaction passes only when combined
encoded cost falls, the final DAG is acyclic, and rendering remains within budget.
Other proposals retain separate positions for the same fill when that costs less.

Global planners also use connected visible ownership rather than source-object
ownership. Original source contours remain available as draw options, including
an option that owns all their visible components. Individual components can move
to different stack positions. Search savings allocate each original source's
cost across its visible components; acceptance measures the complete emitted
scene. Fixed ownership is computed against intersecting later source supports.
Point-touching Boolean boundaries can receive small same-fill bridges as native
completion candidates. Their complete added area must stay within the
intersection tolerance. Native conversion, fixed ownership, silhouette checks,
and rendering limits remain required. Unavailable-owner diagnostics identify the
source, native conversion error, and committed blockers when planning stops.

Complete global proposals are saved in `ordering_candidate.svg`, its render,
and a difference image, including proposals rejected by rendering or cost checks.
Their report entry states whether the candidate was selected and records its
native conversion audit. The validated baseline remains the active output when
no improvement passes.

Chord search tries short chains and longer chains at coarse intervals. Area and
chord tests screen proposals before complete filled-geometry validation.
`--completion-candidates` bounds retained alternatives per contour. Search limits
do not set a palette or region-count target. Native export checks run again after
serialization and SVG capture.

### Color and region abstraction

`--abstract-colors` enables color abstraction before geometry and ordering
optimization. There is no palette-size or region-count target.

The stage partitions complete visible vector geometry into connected regions.
Adjacency lengths use native pixel resolution. Vector contact tests also connect
subpixel regions without a pixel-center sample. Boolean operations retain the
complete adaptively flattened geometry.

Palette consolidation first recolors the original source paths. Each original
color must remain within `--color-tolerance` of its palette color in
Oklab. Palette refitting checks every original member. Significant original
boundaries must retain at least `--contrast-retention` of their original Oklab
contrast, including chromatic contrast. The default retention is 0.75.
A boundary is significant when its original contrast exceeds the effective
color tolerance. These constraints update against the current neighboring colors.

Regional merging then tries to absorb eligible neighboring color variations.
For color distance squared `d2`, effective area `a = A * B / (A + B)`, contact
length `l`, and detail scale `s`, its ranking score is
`d2 * a / (a + s*l + s*s)`. Every merge also checks original-member color drift
and significant-boundary contrast. A small area or long shared boundary can
improve a candidate's rank; the same appearance constraints still apply.
`--color-tolerance` defaults to 5 on an Oklab scale of 0 to 100.
`--detail-scale` defaults to 24 native pixels. Regional color drift is bounded by the full effective color tolerance, weighted
by a conservative bound on native pixel coverage. Coverage is measured with antialiasing at native resolution. A fully covered
probe establishes a bound of 1; other regions receive a complete native crop
measurement. Out-of-view regions conservatively retain a bound of 1.
Significant-boundary tests use the smaller coverage bound of the two regions.
Palette consolidation retains unweighted color and contrast bounds. Reports
include both raw and coverage-weighted regional color drift and the coverage
bound for each original visible region. These coverage
measurements describe pixel footprints; emitted shape fills remain opaque. Similar merged regions can reuse a
palette entry when all original-member and boundary checks pass. Regional
merges choose an existing palette fill. Their flat-color RGB error against
each original member, with the same conservative coverage weighting, is also
bounded by `--abstraction-local-rmse`. Complete rendered image errors remain
unweighted and receive separate validation.

The regional source-support export keeps the original overlaps for comparison.
A source whose visible components receive different colors gets correction
paths before its original upper occluders. These source-support candidates
receive independent image measurements and acceptance flags. Small contrasting
components stay present; geometry cost does not trigger component deletion.

Opaque reconstruction uses fixed original visible ownership with the selected
regional colors. Its filled domain comes from the original source stack. The
largest visible color starts with a domain support. Other colors receive
primary draw items in deterministic contour-cost-per-area order.

Each primary color tries filled visible ownership, original source supports,
rectangles, convex hulls, and per-component rectangles and hulls. A support can
cover previously committed ownership when correction shapes repaint those
original-color intersections above it. The intersections can split a connected
original region into separate layer positions. Corrections close covered holes
and recursively restore protected colors inside them. Nesting and cost bounds
limit the proposal search; there is no output region-count target.

Correction geometry can also complete rectangles, hulls, and curved-boundary
closures through its own original color or future regions. Added territory is
checked against complete fixed ownership. Every unchanged matching Bezier span
remains available for reconstruction. All correction items participate in the
combined encoded-geometry cost.

Final required territories come from the fixed original color regions and their
last matching draw positions. A final ownership DAG is rebuilt and sorted with
deterministic ties. Geometrically redundant items can be removed while retaining
an opaque domain support. Cycles and missing required ownership reject the
proposal. Same-color appearances retain separate draw positions when needed.

`--abstraction-overlap` defaults to one native pixel. It adds an optional padded
support proposal beneath later regions. All output fills remain fully opaque.
Coverage changes are measured separately from color approximation.
`--chord-spans 0` skips the curved-closure search while retaining the rectangle,
hull, hole-filling, and restoration proposals.

A reconstruction that fails native peak or local error checks can restore the
validated palette source stack inside those failed areas. Failed pixels and
sliding windows select a mask on the native pixel grid. The mask receives solid
source shapes in their original order; clipped boundaries recover matching
Bezier spans and use straight closures. Repeated failures compare source and clipped native
coverage; affected full source bounds can join the restoration mask. Restoration
masks fill their internal holes. Up to three restoration attempts are
allowed. Reports retain the repair domain, restored path count, image errors,
and runtime. Reconstruction must reduce combined encoded geometry cost against
both the regional source candidate and the safe source stack, while retaining
or reducing the safe stack's hole count. A failed result falls back to the
validated source stack. A regional source-support export must preserve or reduce
the palette stack's geometry cost and hole count before it can be selected. Fixed ownership is rebuilt from the accepted output
before subsequent topology edits.

Same-color grouping tries whole-color and dependency-compatible sets first.
If a large set fails validation, it also tries smaller batches in source draw
order. This retains separate stack positions and cumulative boundary coverage
where a single compound path would change the native render.

`--abstraction-boundary-tolerance` defaults to 0.25 native pixels. It applies to
hidden correction and coverage geometry while preserving required visible territory and the
external domain. Matching original Bezier spans are recovered after Boolean
operations. Curve matching uses the Boolean coordinate grid. Recovered filled
geometry is checked against the Boolean result; unmatched contours retain the
adaptively flattened geometry. The color transform follows the
[Oklab reference implementation](https://bottosson.github.io/posts/oklab/).

`--abstraction-rmse` and `--abstraction-max-error` bound cumulative opaque-to-final
error, with defaults 12 and 64 in 8-bit channels. `--abstraction-local-rmse`
defaults to 12. It bounds the worst RGB RMSE in every sliding window of
`--abstraction-local-window` native pixels, default 16 by 16. The check covers
all pixel offsets, including windows that cross a fixed tile boundary. Alpha
and complete silhouette checks also remain required. During incremental edits,
the unchanged opaque input remains an additional appearance reference. Every
transaction checks its local windows against that input, with enough surrounding
pixels to cover windows crossing the edited bounds. Full checkpoints recheck
the complete input-relative budget.

Palette-only, regional source-support, and opaque reconstruction results receive
separate validation and reports. Regional colors with valid member and contrast
bounds can be tested through opaque reconstruction even when their source-support
export fails rendering checks. Only a reconstruction that passes final rendering,
silhouette, dependency, and geometry-cost checks can replace that export.
A failed reconstruction retains the validated source-support or palette result.
`--skip-region-reconstruction` retains recolored source supports for comparison.
If neither result passes and reduces color or geometry complexity, the stage
retries with a smaller color tolerance, then retains the original baseline.
The subsequent topology stage fixes ownership from the accepted paint stack.
Final validation reapplies cumulative global, peak, local, and silhouette limits
against the unchanged opaque input.

### Fixed-appearance topology

The default `--strategy incremental` starts with the validated stack. Visible
ownership is fixed from that baseline and partitioned into connected components,
including their holes. Components retain their original source IDs and colors.
Appearance groups remain separate from draw items and their layer positions.

Each transaction replaces one or more draw items. Every replacement must retain
all its assigned components and remain inside the original filled domain.
Coverage of other fixed components produces dependencies on their current draw
items. Every trial rebuilds that mapping and deterministically sorts the graph.
Cycles are rejected. Hidden support area has no ratio limit or penalty.

Single-item proposals include all cyclic boundary chains up to `--chord-spans`
(default 8), independent loop removal, hole filling, visible-region completion,
rectangles, and convex hulls. Chains are enumerated from short to long without
the old initial-proposal cap. Accepted changes are applied immediately, and
proposals are regenerated from the changed contour. Unchanged Bezier spans are
retained. Boolean boundaries recover original spans and adaptive subdivisions
when their flattened chains match.

Exact-fill grouping tries whole color classes, compatible subsets of the
current ownership DAG, nearby pairs, and growing neighborhood groups. A group
transaction must preserve or reduce the total number of logical holes. Grouping runs before individual contour edits in each sweep. New groups
participate in subsequent merge and contour passes. Source paths, Boolean unions, visible unions, hulls, and rectangles are
scored as actual replacement geometry. Component splitting can give parts of
one source different layer positions and reconnect compatible parts with other
same-fill items.

A cycle can trigger a joint completion and occluder split. Assigned components
covered by the proposed completion can move into a later draw item while the
remaining components retain their original support geometry. This search is
limited to two conflicting draw items and `--component-limit` ownership
components (default 8). A joint transaction must reduce combined cost and pass
the same validation as any other edit. Ownership inside a connected component
is not subdivided further. Hole completion also tries a joint restoration of all directly covered
conflicting ownership components. The original supporting items retain their
remaining ownership; enclosed components receive later draw positions. This
deterministic hole-restoration transaction uses the complete covered set.
Its combined geometry cost, fixed-ownership DAG, and final rendering must pass
the same checks. Colors remain fixed during this topology stage.

`--passes` (default 20) bounds sweeps and successive edits on one contour. A
sweep without accepted changes stops the search. Candidate geometry and its
fixed-ownership intersections are cached between attempts.

## Transaction validation

Local checks validate complete filled geometry, including holes and components.
Clipper2 uses adaptively flattened polygons. `--flatten-tolerance` defaults to
0.05 native pixels; `--area-tolerance` controls positive-area tests in square
native pixels.
`--silhouette-tolerance` bounds complete filled-domain difference. Boolean
precision is 8 decimal digits, represented internally by Clipper2's binary
scaling. Valid SVG self-crossings are normalized according to their fill rule.

A transaction is rendered through Qt SVG in a pixel-aligned native-resolution
rectangle. The rectangle includes edited items and items whose relative order
changes. The crop retains the original pixel scale. Error is checked against
the original baseline, including cumulative whole-image RMSE and alpha error.

`--checkpoint-edits` (default 32) controls full-scene checks. A checkpoint renders
the complete SVG and recomputes filled-domain difference. If it fails, the
transaction batch is restored and replayed with full checks after each edit.
Valid independent edits survive; dependent edits whose inputs were removed by
a rejected edit are skipped. Final output receives full geometry, ordering, and
render validation. The baseline is retained when no improvement passes.
`--progress` prints checkpoint counts, cost, and elapsed time to standard error.

## Cost and path counts

The objective counts emitted selectable paths, line/cubic commands, cubic
control points, holes, components, and optional ordering edges. Weights
`--weight-paths`, `--weight-segments`, `--weight-controls`, and
`--weight-components` default to 1. `--weight-holes` defaults to 4;
`--weight-ordering` defaults to 0. Set `--weight-controls 0 --weight-holes 1`
to use the earlier objective.

Replacing a cubic with a line removes two encoded control points even if the
segment count stays constant. Reports therefore include line and cubic counts,
`bezier_control_points`, and `svg_coordinate_pairs`, which counts each move,
line endpoint, cubic endpoint, and cubic control point. These are geometry
metrics and a search proxy, not a Compact Fit placement prediction.

The report also includes total path count, subpaths, exact fills, paths above
one per fill, maximum per-path complexity, dependency edges, and longest
ordering chain. One path per exact fill is a preference subject to topology
and layer positions. A transaction may add a draw item if its combined weighted
geometry cost decreases. Group wrappers do not change these geometry counts.

A compound replacement can retain several original closed contours. Compare its
path count with its component, segment, and coordinate counts to assess how much
geometry the transaction removed. Full-artwork Compact Fit measurements establish
the resulting placement count.

Bucket reads the Bezier contours directly from `optimized.svg`. Compact Fit
receives the complete emitted shapes with no leeway geometry supplied here.

## Compact Fit measurements

`--compact-fit-limit N` benchmarks both complete scenes when neither exceeds N
paths. With `--absorb-colors`, it compares the validated native input before color
transactions with the final guide. `--compact-fit-sample N` benchmarks up to N accepted artwork transactions,
ranked by cost reduction. A sample contains its replaced and replacement paint
shapes; surrounding occluders are excluded from the fitting workload. It is a
measurement of fitting those shapes, separate from full-artwork appearance
validation. Samples are limited to eight replaced and sixteen replacement items.

Both modes call the editor's curve-first Compact Fit entry point with GPU work
enabled, the deployed catalog and empty leeway. `--compact-fit-evaluations`
controls its per-path evaluation budget, default 60,000. Reports retain per-path
diagnostics, runtime, and placement counts.
`verified_placements` is available when every path passes the engine's bounded
approximation checks. `placements` uses the stricter rule that every uncovered
residual must be empty. Failed or partial attempts remain separately labelled.

Verified fitting runs save `.3so` projects and PNG renders and their error
against the respective input shapes. Sample directories also retain internal
`before.svg`, `after.svg`, and `report.json`. Benchmarking is disabled by default.
Use `generate.py` for final project selection and native-rendered quality checks.

SVG fill validity does not guarantee Pen-path compatibility. Filled self-crossings
and disconnected outer contours can pass SVG validation while Compact Fit rejects
their raw contours. SVG strategies normalize the baseline and validate each
emitted target through the native Pen converter. The benchmark reports native
conversion and fitting failures separately.

## Earlier strategies

`--strategy current`, `difficulty`, and `savings` retain the earlier complete-stack
comparison. `--compare-strategies` applies to those strategies and evaluates all
three. Their initial candidate cap is `--completion-candidates` (16). Difficulty
weights remain available for that comparison; the incremental pass selects by
combined encoded geometry cost. `--passes 0` skips the subsequent topology sweeps. An enabled color abstraction
and reconstruction stage still runs and receives full validation.

`--strategy legacy` retains the native polygon-mesh experiment. It measures
Square/Triangle decomposition, reconstructed float32 transforms, and native
shape-record bytes. Native weights `--weight-native-items` and
`--weight-native-segments` default to 0. Its native rendering gates are separate
from Compact Fit and cannot be bypassed with `--structural`. Other strategies
validate emitted SVGs and report native mesh counts as unmeasured.

## Artifacts and checkpoint audit

Outputs include closed opaque SVGs, before/after PNGs, absolute difference
images, ordering diagnostics, and a JSON report. Color abstraction adds
`palette_only.svg`, `palette_only.png`, and `palette_difference.png` for the
source-support palette stage. `regional_candidate.svg` and `regional_candidate.png` retain the regional
source-support attempt, including rejected attempts; `regional_difference.png`
records its error. `topology_candidate.svg` and `topology_candidate.png` record
the grouping and ordering proposal before local source restoration.
`reconstruction_candidate.svg` and `reconstruction_candidate.png` record the
proposal after restoration, including rejected proposals. Matching difference
images and rejection measurements remain available. Candidate files are
experimental artifacts; `optimized.svg` contains the accepted guide.
`abstracted.svg` and `abstracted.png` contain the accepted
opaque reconstruction or its validated source-support or palette fallback. `abstraction.json`
records the attempted stages, cost and hole checks, color-drift and boundary-contrast measurements,
local image error, and fallback decisions. `topology_difference.png` isolates
subsequent geometry and ordering changes. Strategy reports include cost,
image error, rejection, and runtime. Accepted stack steps include owners,
completion type, required upper owners, difficulty, savings, neighbor regret,
and ordering conflicts caused by committing visible owners.
Optional complete Compact Fit runs add fitted SVG and PNG renders.

`tools/paper5_export_audit.py` reads the existing author-code research environment
and checkpoint. It re-renders the checkpoint, records Gaussian opacity
statistics, and exports legacy and corrected depth order. It compares Gaussian
foreground alpha with opaque path coverage after excluding the background.
Corrected sorting uses float32 sigmoid depths and reverses stable front-to-back
ties. Boundary/interior depth interleaving is reported because a single curve
path cannot represent it. A translated variant aligns integer CUDA sample
centers with Qt SVG pixel centers. The helper does not train or download packages.

```powershell
python tools/paper5_export_audit.py checkpoint-directory --output audit-directory
```

Use the interpreter from the existing CUDA research environment. Set
`--experiment-root` to use an environment at another location.
