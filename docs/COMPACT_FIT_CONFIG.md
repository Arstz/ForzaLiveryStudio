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
| `group_replacements` | GPU replacements for nearby groups of small placements. |
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
