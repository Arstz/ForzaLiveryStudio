# Fill engines

Technical contracts for the contour and image-generation code in
`src/gui/fill/`. [DEV.md](DEV.md) covers build and architecture;
[COMPACT_FIT_CONFIG.md](COMPACT_FIT_CONFIG.md) defines catalog configuration.
Tool interaction is described in [MANUAL.md](MANUAL.md).

## Shared representation

Pen and Lining use the scene's hard/soft contour points, cubic handles, and saved
figure data. Pen requests can contain an outer contour and interior cutouts;
Lining requests describe an open centerline with anchor widths and endpoint caps.
Validation and contour conversion precede native-shape fitting.

A primitive carries a native shape ID and its opaque silhouette. A placement
pairs that ID with an affine transform. Fill results include placements, coverage
measurements, cancellation state, and errors. GUI insertion turns accepted
placements into ordinary scene groups. Saved figures preserve the authored paths
separately from generated shapes.

Leeway supplies geometry in which a fill may change hidden support while quality
checks measure the visible result. The editor accepts an opaque vector shape,
a closed saved Pen path, or their union within a group. Saved paths supply an
explicit permitted area. World transforms, cutouts and visibility apply to the
combined geometry. The active target contour cannot supply its own leeway.
Callers supply leeway explicitly. Cancellation discards generated placements;
failure handling depends on the selected engine.

## Contour conversion and Analytical Pen

`bucket_fill.*` selects raster pixels using RGBA tolerance. Pixel-square boundary
tracing produces closed loops; cubic fitting retains persistent corners and
aligned smooth tangents. Displacement, selected-pixel coverage, cutout topology,
and whole-contour validity constrain simplification. Conversion runs in guide
coordinates before its scene transform, independently of canvas zoom.

`svg_vector_objects.*` extracts supported filled SVG object paths directly into
editable contours. SVG features without a reliable object contour use raster
selection. The source guide remains vector artwork in the project.

`pen_fill.*` validates compound contours, fits affine native primitives along
curved boundaries, and prepares the remaining polygonal core. `polygon_mesh.*`
normalizes polygons, checks intersections, performs deterministic ear clipping,
and merges compatible triangle pairs into Squares. Analytical output proceeds
from the boundary inward under a point-count-derived shape budget.

## Compact Fit

The editor uses curve-first initialization from `compact_fit.*` and
`profile_fit.*`. Catalog profiles fit long boundary spans with endpoint and
tangent constraints; straight, corner, and interior tasks supply complementary
proposals. Ranked candidates form a seed, followed by bounded repair, affine
refinement, count reduction, and final verification.

With leeway, boundary search also simplifies hidden chains between hard
anchors, including chains with soft points and curved spans. It tests direct
connections and new corners at intersections of the retained edge directions.
Each replacement must reduce the anchor count,
retain valid compound contours, cover required paint and remain within
spill-free support. Search effort and extension lengths are bounded. The
working contours supply additional straight and corner candidates, plus a
single affine Square candidate for a completed parallelogram. Selection and
final verification use the original coverage requirements. Authored contours
and exposed curves are preserved. Profile diagnostics report accepted
shortcuts, intersections, anchor counts and generated candidates under
`leewayContour`.

Selection and refinement assess the complete placement union. Coverage repair
uses missing interior support and observed contour defects. Reduction reuses
candidate geometry and the joint unique support of neighboring placements.
A final exact-union pass removes redundancy and consolidates placements while
preserving the emitted union on the clipping grid.

### Quality and work limits

Defaults are defined by `compact::FillOptions` in `compact_fit.h`:

| Option | Default | Meaning |
| --- | --- | --- |
| `boundaryAllowance` | `2.0` | Maximum outward support distance in world units. |
| `inwardAllowance` | `0.5` | Allowed inward boundary corridor in world units. |
| `areaErrorRatio` | `0.02` | Maximum combined missing and spill area relative to target area. |
| `observationScale` | `1.0` | World-unit scale for observed contour and topology measurements. |
| `shapeBudget` | `3000` | Caller placement-count budget. |
| `evaluationBudget` | `60000` | Refinement score-evaluation budget. |
| `profileTrialBudget` | `0` | Automatic profile-search allocation. |
| `adaptiveBudget` | `true` | Extend search in rounds according to complexity and measured progress. |
| `timeBudgetMilliseconds` | `0` | Automatic time allowance; a positive value sets the search deadline. |
| `retainFailedFill` | `false` | Retain available placements after a quality failure. |

The editor scales its coverage defaults once for each region. It uses the
area-equivalent circle diameter relative to a 256-observation-unit reference;
cutout area is subtracted. Uniform enlargement therefore increases outward and
inward distances linearly, while rotation and translation leave them unchanged.
The dialog shows the resulting world-unit distances and permits an outward
override. Low-level fill APIs continue to take explicit world-unit allowances.
`scaleCoverageToRegion` applies the editor's preparation to replay requests.
It tightens geometry flattening when a smaller corridor needs more precision,
while preserving observation scale, corner limits and the area-error percentage.

Geometric checks use triangle unions on a `1e-6` clipping grid and reconstructed
float transforms. Interior support and the exterior envelope constrain accepted
changes. Boundary measurements compare displacement, protected corners, tangent
alignment, turning, and component/hole counts. Target and output receive the same
observation operator; original geometry remains authoritative for interior
support and protected-corner distances. These sampled measurements provide
quality criteria rather than a perceptual-equivalence guarantee.

Authored junction positions and one-sided tangent directions accompany the
flattened geometry. Polygon-only corners use support at a fixed observation
distance and retain one prominent turn feature per junction, so short edges,
curve subdivision and tiny rounded joins do not erase or duplicate a junction. Visible
target corners match fitted corners one to one within corresponding, equally
oriented boundary loops. Corner position and local angle have separate checks;
a nearby smooth boundary cannot substitute for a missing corner.

Matched junctions divide each loop into ordered spans. Smooth-boundary samples
project onto the corresponding target span rather than another nearby branch.
Near a junction, comparison samples align their arc distance to the matched
junction. Junction projection and neighborhoods use the observed boundary;
the separate position check uses the original geometry.
The smooth-turn check excludes matched junction neighborhoods, whose position
and angle remain constrained by the corner checks. Inconsistent junction order
fails correspondence. Observation distances, the corner-position limit and the
angular limits remain independent of region size. Diagnostics report matched
and missing corners, corner displacement, corner-angle error and excess turn
on smooth spans separately.
Leeway cut vertices are not authored corners. Corner checks require a fully
visible observation neighborhood, and turn comparisons omit samples whose
tangent support is occluded. The occlusion neighborhood uses the fixed
observation scale; it does not grow with the coverage allowances.
The hidden coverage guard uses the same integer-grid interior-support operation
as the coverage checks.

Small placement sets can correct matched junctions with simultaneous affine
updates. A damped least-squares step uses the junction offsets of the complete
union; backtracking checks the emitted transforms against coverage, corner and
topology constraints. Each correction call uses at most 256 trials and four
iterations, with up to eight placements and sixteen corners. These trials share
the caller's existing evaluation and cancellation limits. Near-feasible covers
of three or more shapes remain eligible for this refinement.

The solver can select a tighter outward allowance within the requested maximum.
Stages share bounded work and retain feasible checkpoints. CPU and GPU trials
contribute to the operation's work accounting. Progress reflects consumed work;
setup and verification costs also depend on contour complexity.

Adaptive refinement starts with 3,000 to 12,000 evaluations. Authored anchor count,
holes, components and seed count increase the initial allowance and ceiling, up to the caller's
`evaluationBudget`. Remaining work is issued in growing rounds on the same
objective, caches and placement state. Lower verified shape count or improved
incomplete coverage resets patience; two stalled rounds stop further extensions.
A verified one-shape fit stops count search. The automatic search allowance ranges
from 3 to 20 seconds. Deadlines are cooperative: an in-progress geometry operation
and final output verification can finish after the allowance.

Internal deadlines return the best verified checkpoint. Explicit user
cancellation clears placements. Whole-shape discovery uses an affine frame based
on the region's area covariance. Ellipse arcs and straight boundary directions
propose native circles and parallelograms in this frame. The proposals return to
the original metric before envelope checks, coverage scoring or refinement.
Conditioning does not change the requested distance allowances.

Discovery retains up to 24 base candidates per primitive, plus up to 24 circle
candidates adjusted for the native polygon's chord error. Joint selection tests single
shapes and pairs, followed by up to eight greedy covers of at most eight shapes.
A beam of up to 16 partial covers searches for shorter alternatives to those
multi-shape covers.
Authored straight edges remain eligible as support lines regardless of their
length. Curve-flattening chords do not replace those edges. Opposing support
lines can propose all corners of a parallelogram together, including corners
inside leeway; the complete proposal must pass the same output checks.
Every accepted cover passes the full original coverage, contour, corner and
topology checks with reconstructed output transforms. Near-feasible covers can
use up to four refinement trials, limited to 4,800 evaluations or one fifth of
the caller's evaluation budget. Discovery has a cooperative four-second deadline;
an explicit operation deadline reduces that allowance and includes discovery
time. Unselected candidates remain available to native refinement, which retains
its evaluation allowance. These bounds limit search work; they do not guarantee
the minimum shape count for arbitrary contours.

A failed compact search reports its failed checks. Triangulated covers remain
available to the diagnostic tool and are not substituted for Compact Fit output.
Count regression checks constrain successful compact results independently of the
general shape budget. Geometry validity alone does not establish a compact result.

Extensions repair the current failures: exact boundary transforms address contour
continuity and protected corners, while coverage repair addresses missing interior
support. Progress includes constraint improvement as well as verified count
reduction. Automatic profile seeding and refinement have separate time allowances;
a stopped seed phase retains selected candidates for refinement. An explicit time
allowance continues to bound the combined seed and refinement phases.

Compact Fit searches in observation units, relative to the analytic outer bounds'
center. Each closed contour starts at its lexicographically smallest anchor.
Contour tolerances, coverage allowances, leeway, preferred coverage and supplied
placements use the same coordinate conversion. Equivalent uniform changes of
units require scaling all distance settings, including `observationScale` and
the contour's boundary tolerance. Public distance and area measurements return
in project units. `coordinateFrame` records the origin and observation unit;
dimensionless scores and internal diagnostic coordinates remain in solver units.
Returned placements receive native transform rounding and another coverage check
in the original project coordinates. Coordinate conversion can change clipping-grid
rounding. Conversion first tries bounded clearance adjustments, then repairs the
existing placements in project coordinates within the unused work allowance.
Conversion repair cannot increase the placement count.
A bounded clearance adjustment can recover coverage lost during rounding; it
must pass the complete original quality requirements.

GPU refinement starts from substantially covered seeds and shares a bounded part
of the repair allocation. Exact repair and
count reduction continue under the remaining stage allocations after a feasible
GPU result. Failed GPU attempts retain their consumed work in the shared budget.

The GUI enables failed-fill retention: available Compact Fit placements can be
inserted with a reported warning when verification fails. Headless callers use
strict rejection by default. Errors before placements exist leave the authored
contour available. Compact Fit emits opaque shapes and rejects mask requests.

### Computation and geometry

`fls_profile_compute` batches independent profile fitting on CUDA when available,
with bounded parallel CPU fallback. GPU refinement reuses resident occupancy and
boundary fields; exact CPU geometry validates accepted results and checkpoints.
CPU and GPU results can differ numerically. Jobs are committed in planned order.

`compact_fit_geometry.cpp` wraps Clipper2 operations. Prepared operands, local
clipping projections, spatial indexes, and bounded caches reduce repeated union
and boundary work. Geometry storage participates in cache ownership so changed
placements invalidate their derived data. The legacy catalog-cover solver and
Analytical-seeded comparisons remain in the headless tool.

## Lining and closed thin regions

`thin_fit.*` shares the Lining catalog and numeric fitting infrastructure across
open strokes and closed thin regions. These entry points have distinct quality
objectives; catalog membership is documented in
[Lining configuration](COMPACT_FIT_CONFIG.md#lining-catalog).

### Open Lining paths

`thin::fillLiningPath()` fits explicit catalog centerlines to authored spans.
Hard anchors delimit spans; whole-span fits precede subdivision. Full and trimmed
profiles propose affine transforms, then bounded pose optimization adjusts the
assembled stroke. Anchor widths interpolate through cubic segments, and endpoint
caps participate in the requested ribbon geometry.

Continuity and curve accuracy take priority over count, followed by area
coverage. Verification checks connected support, centerline deviation, width,
tangents, and contour quality. Native taper is allowed within body-width and
join constraints. Cleanup can adjust adjacent placements together while retaining
those constraints. Failed verification blocks insertion.

The outside margin defaults to `0.5` local widths per edge; the permitted corridor
has width `1 + 2 * margin` times the requested width. Stored-spine fitting permits
an offset of `0.08 + min(0.25, margin / 2)` local widths. Guide-support checks
retain a separate `0.08` width allowance. Hard-point tangents allow twenty degrees;
smooth subdivision joins retain five degrees. Width and contour thresholds live
in `thin_fit.cpp`.

### Closed thin regions

`thin::fillRegion()` and `thin::fillPolygons()` fit closed regions through
whole-region recognition, boundary and medial profiles, and interior candidates.
Growth, gap repair, neighbor replacement, and pruning operate within an envelope
based on local width and the caller's outward allowance. Leeway excludes occluded
areas from visible checks. Final verification enforces coverage, envelope
containment, connectivity, and cutout preservation.

| API option | Default | `thin::qualityOptions()` |
| --- | --- | --- |
| Minimum coverage | `0.98` | `0.98` |
| Preferred thickness ratio | `1.15` | `1.6` |
| Maximum thickness ratio | `1.6` | `2.0` |
| Profile trial budget | `320000` | `2000000` |
| Pose evaluation budget | `0` | `120000` |
| Pose time budget | `120000` ms | `180000` ms |

Preferred thickness guides selection and growth within the maximum envelope.
Search limits are API options, separate from catalog configuration. Budget
exhaustion proceeds to final verification with the accepted placements.

## Image regions

The experimental ImgGen pipeline combines guide preprocessing, region extraction,
layer planning, and native-shape generation:

1. `image_preprocessor.*` smooths and quantizes the guide to a palette, with
   edge-aware cleanup and line preservation. Locked colors and project swatches
   constrain palette selection. The resulting guide and palette are one undoable edit.
2. `region_extract.*` merges small connected color regions, classifies lineart,
   and traces outlines through Potrace. The result retains source labels and
   foreground/lineart masks for planning and comparison.
3. `region_layer_plan.*` combines eligible same-color regions, expands backdrops
   beneath later overlays, and derives draw dependencies from raster ownership.
4. `region_fill_batch.cpp` fits planned units in parallel, restores draw order,
   and returns grouped placements and a difference heatmap for scene insertion.

The **Safe** plan rolls back operations until it matches the vector-rendered
baseline pixel-for-pixel. **Dangerous** retains aggressive contour expansion and
forced ordering despite validation differences or dependency cycles. It can alter
colors, coverage, and layer order. Both variants are retained beneath **Region
Fill**, with Safe visible initially. **Dangerous Differences** compares the fitted
outputs: red indicates Safe-only coverage, cyan Dangerous-only coverage, and
yellow differing colors where both cover a pixel.

Region fitting uses cyclic polygon simplification and meshing, with Pen recovery
and bounded contour simplification when needed. Pen conversion checks topology,
boundary deviation, and supersampled DSSIM. Planning and fitting are cancellable;
progress reports the current phase and completed units.

## Diagnostics and replay

| Log | Contents |
| --- | --- |
| `compact_fit.log` | Replayable request/result, configuration hash, work and timing, placement history, coverage, contour metrics, and retained-failure status. |
| `lining_fill.log` | Widths and margin, span fits, joint quality, coverage, contour metrics, shape usage, and timings. |
| `region_extract.log` | Segmentation, line classification, cleanup, and component metrics. |
| `region_layer.log` | Safe/Dangerous plans, draw dependencies, contour operations, rollbacks, and validation. |
| `region_fill.log` | Per-unit fitting results, simplification measurements, and fitted-output differences. |
| `region_points.log` | Raw and optimized contour points for the largest region. |

Build `fls_compact_fit_tests` with `FLS_BUILD_TESTS=ON`. Its diagnostic modes include:

| Arguments | Purpose |
| --- | --- |
| `--profile-fit <contour-log>` | Replay curve-first fitting used by the editor. |
| `--compact-fit <contour-log>` | Replay the Analytical-seeded optimizer. |
| `--shape-config <path>` | Select the general Compact Fit catalog for the run. |
| `--lining-tests`, `--thin-tests`, `--thin-quality-tests` | Run focused fitting checks. |
| `--thin-fit <contour-log>` | Fit a logged contour through the thin-region API. |
| `--thin-reference <project> [comparison-project]` | Compare a project using the default thin-region options. |
| `--thin-reference-quality <project> [comparison-project]` | Use the quality preset. |
| `--thin-reference-quality-seed <project>` | Inspect candidate selection before refinement. |
| `--thin-color-reference-quality <project> [comparison-project]` | Fit ordered opaque color runs with occlusion leeway. |

Thin operations load their own catalog. `FLS_PROFILE_GEOMETRY` enables geometry
operation timings in headless replay. Catalog comparison is described in
[configuration trials](COMPACT_FIT_CONFIG.md#compare-configurations). Assess quality
and coverage together with placement count and runtime.
