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

Leeway describes occluding geometry above a fill. It permits support underneath
that geometry while quality checks measure the visible result. Callers supply
leeway explicitly. Cancellation discards generated placements; failure handling
depends on the selected engine.

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

Selection and refinement assess the complete placement union. Coverage repair
uses missing interior support and observed contour defects. Reduction reuses
candidate geometry and the joint unique support of neighboring placements.
A final exact-union pass removes redundancy and consolidates placements while
preserving the emitted union on the clipping grid.

Callers can supply an allowed domain to constrain outward support while keeping
the visible contour and its fitting allowances separate. Interior repair can
recover part of a concave gap with native primitives when a single enclosing
placement cannot fit. An infeasible GPU seed reserves work for exact coverage
repair before final verification.

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
| `retainFailedFill` | `false` | Retain available placements after a quality failure. |

Geometric checks use triangle unions on a `1e-6` clipping grid and reconstructed
float transforms. Interior support and the exterior envelope constrain accepted
changes. Boundary measurements compare displacement, protected corners, tangent
alignment, turning, and component/hole counts. Target and output receive the same
observation operator; original geometry remains authoritative for interior
support and protected-corner distances. These sampled measurements provide
quality criteria rather than a perceptual-equivalence guarantee.

The solver can select a tighter outward allowance within the requested maximum.
Stages share bounded work and retain feasible checkpoints. CPU and GPU trials
contribute to the operation's work accounting. Progress reflects consumed work;
setup and verification costs also depend on contour complexity.

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
