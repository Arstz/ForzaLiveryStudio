# Compact Fit shape configuration

Compact Fit reads `assets/compact_fit_shapes.json` beside the executable at the
start of each operation. Changes take effect on the next operation without a
rebuild or restart. An operation keeps its initial configuration snapshot.
If the deployed file is absent, the loader checks `assets/compact_fit_shapes.json`
under the current working directory. An invalid deployed file produces an error
and does not fall back to another configuration.

The repository file supplies deployment defaults. To apply repository edits
without compiling, run `tools/update_compact_fit_config.ps1`. It copies only this
JSON file to `build/Release/assets`. Alternatively, edit the deployed file
directly. A later application build redeploys the repository defaults.

The experimental `assets/compact_fit_shapes_combined.json` profile combines
the task additions from the comparison tool. Activate it with
`tools/update_compact_fit_config.ps1 -ConfigurationPath assets/compact_fit_shapes_combined.json`.
Run the helper without `-ConfigurationPath` to restore the default profile.

The default curve task includes Pill `2133`. Its smooth ends participate in the
same affine profile fitting and exact coverage checks as the other silhouettes.

## Catalog fields

- `schema_version`: must be `1`.
- `shape_ids`: enabled opaque native silhouettes.
- `reserve_shape_ids`: additional enabled silhouettes, disjoint from `shape_ids`.
  The legacy comparison solver retains its reserve-search behavior. Compact Fit
  task membership controls eligibility for both lists.
- `tasks`: must contain every task below. An empty task array disables that
  task's new shape proposals.

The global catalog limits every task. Removing an ID from both catalog lists
disables that shape even if a task array still mentions it. Geometry must already
exist in the native shape library. This configuration selects existing shapes;
it does not define new geometry.

## Tasks

| Key | Shape selection |
| --- | --- |
| `curves` | Catalog perimeter profiles used to initialize curved spans. |
| `interior` | Interior and thin-body seed proposals. |
| `corners` | Affine corner proposals. |
| `straight_edges` | Rectangle span proposals; accepts Square `101` or `[]`. |
| `corner_triangles` | Endpoint triangle proposals; accepts Triangle `103` or `[]`. |
| `replacements` | General CPU count-reduction searches. |
| `cutout_replacements` | Additional CPU replacements when the target has holes. |
| `group_replacements` | GPU pair replacements and exact boundary-cluster replacements. |
| `whole_region` | Single-placement recognition candidates. |
| `gap_patches` | Bounded interior gap patches. |
| `residual_repair` | Residual repair replacements, insertions, and connectors. |
| `patch_consolidation` | Rounded patch consolidation; accepts Circle `102` or `[]`. |
| `exact_replacements` | Exact rectangle consolidation; accepts Square `101` or `[]`. |

Task arrays list eligible IDs. Interior, corner, and gap-patch proposals retain
array order. Other searches retain their geometric rankings and deterministic
tie breaks. A task controls new catalog choices. Refinement can still move or
grow existing placements, and exact deletions can remove redundant placements.
Task changes can alter runtime, count, and quality because proposals share a
bounded evaluation budget. Coverage and quality checks remain active.

Boundary-cluster compaction reuses eligible candidates from the seed pool.
New cluster fits use shapes selected by both `curves` and `group_replacements`.
The fit follows the principal direction of the required support and tightens
its transverse scale against the permitted envelope. Exact union checks
preserve interior coverage and reject contour-quality regressions.

The corner fitter derives anchors from usable silhouette corners. Adding a
shape to `corners` enables those proposals. Straight-edge proposals currently
derive their transform from a rectangle side and its inward normal. Other
silhouettes require a fitter that identifies suitable straight sides and their
interior orientation before they can participate in `straight_edges`.

A task can instead use an object with these optional fields:

- `all`: includes every enabled catalog shape.
- `multiple_contours`: includes enabled silhouettes with more than one contour.
- `exclude`: removes these IDs from that task only.

`all` and `multiple_contours` combine by inclusion. Exclusions are applied last.
Selector results follow catalog ID order. All flags must be booleans. IDs must
be integers, unique within each list, and present in the native geometry library.
Unknown fields and missing tasks produce an error.

Logs include the loaded file path, its SHA-256 hash, and effective task lists.
These identify the configuration snapshot used for a fit.

## Thin regions and Lining

**ImgGen → Detect Lining** detects strokes in the selected image guide and saves
a separate guide with transparent pixels outside the detected strokes. The
source guide keeps its image and transform. Detection runs in the background
and supports cancellation through the existing fill controls.

The detector searches for narrow contrast ridges in four directions. Its
default width limit is 0.8% of the smaller image dimension, bounded to 3–24
source pixels. It uses a luminance contrast of 12, requires stronger support
within each connected stroke, removes components smaller than eight pixels,
and assigns up to six representative stroke colors. Local contrast supports
grey interior stripes. Transparent source pixels stay empty. The reusable
`extractLining` API exposes width, contrast, area and color limits and returns
both pixel masks and traced contours for image generation. Detection describes
local image geometry; shading and narrow colored details can also qualify as
strokes. Check the detected guide before fitting the regions.

`lining_detection.log` records detection parameters, pixel and component counts,
and runtime. Detection and fitting have separate diagnostics.
The thin fitting API accepts protected empty geometry and an optional original
stroke geometry for thickness measurement. A padded fitting target retains
that original thickness reference. Image trials protect fully transparent
source pixels and the interior of cutouts while allowing padding along the
surrounding strokes. Curves are flattened in source coordinates before fitting.
The human examples supply alignment and style
references; their coverage is not a completion requirement for source detection.

**Compact Fit — Thin Regions** and the Lining tool read
`assets/lining_shapes.json` beside the executable on each operation. The same
schema and validation rules apply. Edit `build/Release/assets/lining_shapes.json`
to try changes without rebuilding or restarting. A build redeploys the
repository defaults. If the deployed file is absent, the loader checks the
current working directory; an invalid deployed file produces an error.

The thin backend first tries one Square for a straight span and the
`whole_region` shapes for a complete region. Open Lining paths also try the
`curves` shapes across the full centreline. If one placement cannot cover the
region within the margin, a bounded profile search uses `curves`, `corners`,
`straight_edges`, `corner_triangles` and `interior`. The default catalog enables
native shapes, including triangles. Triangle proposals follow contour corners
and spans; the backend does not construct a triangle mesh. Whole-region matching uses
affine hull anchors as well as bounding boxes, so rotation and heavy skew do
not require a preferred shape ID.

The overlap envelope follows local stroke width. The GUI quality preset limits
thickness to 2 times the original, with the supplied outward margin as an additional
absolute cap. The API permits thickness ratios above 1 and at most 2. Tapered
tips receive a small geometric tolerance. Open Lining paths use their specified
width; closed regions estimate width from inward boundary crossings. Candidate
selection penalizes extra ink while favoring placements that cover long spans.
Medial paths and partial source profiles support matching across junctions.
Candidate paths also connect compatible tangent directions through junctions.
Stable noncollinear anchors support inflected curves. Growth uses cached local
geometry, and gap repair keeps original cutout edges distinct from new holes.
The quality search also recognizes affine native arcs in polygon contours.
Recognized shapes must fit the original painted geometry. When they cover most
of the region, selection retains these strokes before filling the residual.
A native cover with at least 99.5% area coverage and 99.9% interior coverage
skips the broad profile search. Selection checks these thresholds again before
skipping residual body proposals and the expensive pose refinement. Growth,
gap repair, cleanup, native replacements, merging and final checks still run.
Curve screening uses
a raster resolution based on stroke width, and quality profile fits allow more
numerical iterations and trimmed ends.
Preferred padding contributes to selection without requiring coverage of all
padding pixels.
At least 98% of the selected region must remain covered. Existing placements
can grow toward a preferred thickness of 1.6 times the original, within the
same envelope. This is a soft target that preserves coverage and continuity.
Width growth keeps selected pixels that are already covered intact.
Placements that add little coverage are removed before and after gap repair
when protected interior coverage, connectivity and cutouts remain intact in the
complete union. Quality selection gives additional weight to the stroke interior.
Small gaps first try expanding a nearby placement. Leeway represents layers above the
lining: generated shapes can continue underneath, and visible coverage checks
exclude those areas. Square `101` in
`gap_patches` enables significant gap proposals, and Square `101` in
`exact_replacements` enables rectangle consolidation. Shapes in
`group_replacements` can consolidate neighboring placements through reused
profile candidates and bounded local affine fits. Quality refinement also adjusts
neighboring groups through translation, rotation, scale and shear.
Group membership follows geometric contact and the fraction of overlap.
Nearby placements can close very small gaps with fine scale adjustments;
rectangle repair includes the precision of the emitted float transform.
Separate native regions are matched individually before the profile search. Width growth takes priority
over length adjustments at joins. The remaining task lists are reserved for
the general Compact Fit backend. Search budgets, the coverage target and
geometric tolerances are API options, separate from the shape configuration.
Default API options use the shorter search, with preferred thickness 1.15 and
maximum thickness 1.6. `thin::qualityOptions()` supplies the GUI preset:
2,000,000 profile trials, 120,000 pose evaluations and a 180-second time budget
for pose refinement. Whole-region fits can finish before these stages. Budget
exhaustion proceeds to final verification with the accepted placements.

The headless test tool accepts `--thin-tests`, `--thin-quality-tests`,
`--thin-fit <contour-log>` and
`--thin-reference <project> [comparison-project]`. Use
`--thin-reference-quality <project> [comparison-project]` for the quality preset.
`--thin-reference-quality-seed <project>` stops after candidate selection and
reports the seed measurements without running the refinement passes.
`--thin-color-reference-quality <project> [comparison-project]` fits opaque color
runs separately, treats later opaque runs as leeway, and retains translucent
source placements and their opacity in the comparison. Reference commands
report fitting phases as well as final measurements.
Thin operations use their
own catalog. The explicit `--shape-config` argument applies to general Compact
Fit operations.

## Compare configurations

The headless `fls_compact_fit_tests` tool accepts `--shape-config` followed by an
explicit configuration path. This leaves the deployed editor configuration
unchanged. Build the headless target with `FLS_BUILD_TESTS=ON` before using it.

Run `python tools/compare_compact_fit_configs.py --request <contour-log>` to
compare the baseline against additions defined in
`tools/fixtures/compact_fit_shape_trials.json`. Repeat `--request` to compare
multiple contours. `--variants` selects another task-addition file, `--only`
selects named variants, and `--allowance` sets one fixed allowance for every fit.

Each variant appends IDs to explicit task arrays and enables missing IDs in the
global catalog. Generated configurations, raw logs, shape usage counts, and
JSON/CSV measurements are written to `build/compact_fit_shape_trials`, or the
directory selected by `--output`. Trials run sequentially. Retained results can
have quality failures; inspect the recorded error and coverage metrics together
with count and runtime before choosing a configuration.
