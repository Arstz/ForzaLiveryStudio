# Compact Fit and Lining shape configuration

Shape catalogs select the native silhouettes available to fitting operations.
Compact Fit uses `assets/compact_fit_shapes.json`; Lining and the closed
thin-region API use `assets/lining_shapes.json`. Engine behavior and API quality
options are documented in [FILL_ENGINES.md](FILL_ENGINES.md).

## Deployment

Each operation loads a configuration snapshot from `assets/` beside the
executable. When that file is absent, the loader checks `assets/` under the
current working directory. An invalid deployed file reports an error. Edits take
effect on the next operation without restarting or rebuilding.

A build deploys repository defaults. To copy only the Compact Fit configuration
into an existing `build/Release/assets` directory, run:

```powershell
.\tools\update_compact_fit_config.ps1
```

`-AssetsDirectory` selects a different deployed asset directory.
`-ConfigurationPath` selects an alternate source file. The repository includes
`assets/compact_fit_shapes_combined.json` for experimental task additions.
Running the helper without `-ConfigurationPath` restores the default catalog.
Edit deployed `lining_shapes.json` directly to try Lining catalog changes;
a later build redeploys the repository version.

## Catalog schema

| Field | Meaning |
| --- | --- |
| `schema_version` | Required; must be `1`. |
| `shape_ids` | Enabled opaque native silhouettes. |
| `reserve_shape_ids` | Additional enabled silhouettes, disjoint from `shape_ids`. |
| `tasks` | Required task selections listed below. |
| `stroke_shape_ids` | Preferred stroke silhouettes for Lining. |
| `centerlines` | Normalized centerline definitions for Lining silhouettes. |

The combined shape lists bound every task. Removing an ID from both lists
disables it globally. Geometry must exist in the deployed native shape library
and supply opaque painted area. Catalog configuration selects existing geometry.
The reserve list retains its separate search role in the legacy comparison
solver; general Compact Fit uses task membership for eligibility.

IDs must be integers and unique within each list. Unknown fields, unavailable
geometry, overlapping catalog lists, and missing task selections produce errors.
Logs identify the loaded path, SHA-256 hash, and effective task membership.

## Tasks

Every task key is required. A value is an array of eligible IDs or a selector
object. An empty array disables new proposals for that task.

| Key | Role | Restricted selection |
| --- | --- | --- |
| `curves` | Perimeter profiles for curved-span initialization. | |
| `interior` | Interior and thin-body seeds. | |
| `corners` | Affine proposals anchored to silhouette corners. | |
| `straight_edges` | Rectangle span proposals. | Square `101` or `[]`. |
| `corner_triangles` | Endpoint triangle proposals. | Triangle `103` or `[]`. |
| `replacements` | General CPU count reduction. | |
| `cutout_replacements` | Additional replacements for regions with holes. | |
| `group_replacements` | GPU pair and boundary-cluster replacements. | |
| `whole_region` | Single-placement recognition. | |
| `gap_patches` | Bounded interior gap patches. | |
| `residual_repair` | Residual replacements, insertions, and connectors. | |
| `patch_consolidation` | Rounded patch consolidation. | Circle `102` or `[]`. |
| `exact_replacements` | Exact rectangle consolidation. | Square `101` or `[]`. |

Selector objects accept these optional fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `all` | Boolean | Include every enabled catalog shape. |
| `multiple_contours` | Boolean | Include enabled shapes with multiple contours. |
| `exclude` | ID array | Remove IDs from this task after inclusion. |

Inclusion flags combine; exclusions apply last. Selectors follow catalog order.
Explicit arrays preserve proposal order for interior, corner, and gap-patch
searches; other searches use their geometric rankings and deterministic ties.
Restricted tasks validate the resulting selection against the required shape ID.

Tasks control new catalog proposals. Refinement can move or grow retained
placements and delete redundant ones. New boundary-cluster profile fits use the
intersection of `curves` and `group_replacements`. Catalog changes affect count,
quality, and runtime through a shared bounded search budget.

## Lining catalog

Lining uses the same schema, deployment lookup, and validation rules. Its catalog
also requires explicit `centerlines` for every enabled silhouette. Keys are shape
IDs and values are arrays of `[x, y]` pairs normalized to painted bounds.
Coordinates must be finite and within `[0, 1]`, with at least two points and
distinct endpoints. `stroke_shape_ids` selects preferred strokes from the enabled
catalog.

`tools/generate_lining_centerlines.py` derives definitions from opaque mesh
triangle strips. Definitions can also be edited directly. The open-path fitter
matches these centerlines; the closed-region API uses region and silhouette
geometry. Their quality limits and search budgets are API settings described in
[fill engines](FILL_ENGINES.md#lining-and-closed-thin-regions).

## Compare configurations

Build `fls_compact_fit_tests` with `FLS_BUILD_TESTS=ON`.
`--shape-config <path>` selects an explicit general Compact Fit catalog for a
headless run while leaving the deployed editor configuration unchanged.

Run `python tools/tests/compact_fit/compare_compact_fit_configs.py --request <contour-log>` to compare
the baseline against task additions in `tools/tests/fixtures/compact_fit_shape_trials.json`.

| Argument | Purpose |
| --- | --- |
| Repeated `--request` | Compare multiple contour requests. |
| `--variants` | Select a different task-addition file. |
| `--only` | Select named variants. |
| `--allowance` | Use one outward allowance for every fit. |
| `--output` | Select the result directory; defaults to `build/compact_fit_shape_trials`. |

Trials run sequentially and write generated configurations, raw logs, shape usage,
and JSON/CSV measurements. Evaluate coverage and recorded quality failures
alongside count and runtime. Headless replay and thin-region comparison modes
are listed in [fill diagnostics](FILL_ENGINES.md#diagnostics-and-replay).
