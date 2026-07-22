# Advancing Front Region Fill Plan

## Purpose

Add a third **Advancing Front** result to **Fill Regions**. It is an experimental,
initially hidden comparison variant alongside the existing visible **Safe** and hidden
**Dangerous** variants. Safe and Dangerous planning and fitting remain unchanged.

Advancing Front starts from the Safe region-layer plan and its cyclic closed RDP
contours at epsilon 1.9. It tries to cover each planned region with a small number of
affinely transformed Forza primitives before sending the uncovered remainder through
the existing polygon-mesh fallback.

## Residual and front model

- The immutable target is the Safe unit's `QPainterPath`.
- Accepted primitive silhouettes form a vector coverage union.
- The exact residual is `target - coverage` and is rebuilt with vector path booleans.
- Every accepted placement is retained; coverage is periodically rebuilt from those
  original placements to avoid accumulating boolean-operation drift.
- A front is a contour of a newly exposed residual component. After every accepted
  primitive, all residual components are extracted again.
- Safe output and residual fallback retain cyclic RDP epsilon 1.9 geometry. Advancing-
  front seed selection instead uses at most 48 curvature-significant controls from a
  0.35-pixel guide, while each candidate is fitted and compared against the dense
  residual boundary between those controls.
- Only newly exposed boundary sections receive adaptive guide simplification. The exact
  residual remains the source of truth. Guide trials use epsilons 1.9, 2.5, 3.5, 5.0,
  and 7.5 and retain the fewest points whose local mask DSSIM remains within the
  configured limit. Junctions and topology-changing points remain locked.
- Original holes, other-colour regions, and transparent background are forbidden target
  space. A small configured leakage is nevertheless permitted by DSSIM.
- A candidate may overlap already accepted shapes from the same unit. Only newly
  covered residual area contributes to its area score; redundant overlap receives a
  small efficiency penalty.

## Component scheduling

After each residual rebuild:

1. Test newly exposed components for a complete one-shape fit.
2. Prefer the eligible one-shape result with the highest score, breaking equal scores
   by greater component area and then deterministic contour order.
3. Otherwise process the largest residual component, with deterministic equal-area
   ordering.
4. Discard disconnected residual pieces from smallest to largest while their cumulative
   omission remains within the independent omission-DSSIM budget.
5. When advancing-front search stalls or exhausts its work budget, fill the remaining
   residual with the Safe cyclic-RDP contour tolerance, exact triangulation,
   compatible Square merging, and Circle/ellipse replacement pass.

Retain the already computed Safe placements as the per-unit baseline. An advancing-
front unit replaces them only when its complete, topology-valid output uses fewer
shapes. Otherwise the comparison unit copies Safe exactly.

The complete-component shortcut accepts a primitive when leakage DSSIM and
candidate-versus-component DSSIM are within their configured limits and coverage is at
least 98 percent. Any remainder after that shortcut goes directly to the fallback rather
than restarting expensive front search.

## Candidate generation and fitting

- Seed selection is deterministic rather than random.
- Rank front edges by curvature significance and unprocessed arc length. Test the best
  16 seed edges per iteration and grow from each seed in both contour directions.
- Begin with two adjacent front points, add one point per span, and fit every supported
  region primitive at each step.
- Supported transforms are translation, rotation, non-uniform scale, skew, and mirrored
  affine variants. Perspective is not allowed. Reject near-singular transforms and
  scale condition ratios above 20:1.
- Use all span samples in least-squares refinement. Evaluate actual transformed
  silhouettes and every usable primitive-boundary orientation.
- Keep the best span seen. Stop growing after at least six tested span sizes when three
  consecutive spans score at least 0.02 below the best. A span is capped at the smaller
  of 64 front points and half of the active contour.
- Sample paired front and primitive arcs at approximately 0.5 image pixels, clamped to
  32 through 256 samples. A sample matches when its nearest point on the opposite arc is
  within 1.9 image pixels.
- Contour parity is the harmonic mean of front recall and primitive-arc precision,
  multiplied by continuous tangent alignment and the local leakage penalty, then clamped
  to `[0, 1]`.
- Candidate score is `0.75 * contourParity + 0.25 * normalizedArea`. Normalize newly
  covered area by the largest raw newly covered area observed during that seed's complete
  growth cycle, then rescore the stored candidates.
- A residual split is allowed. Penalize fragmentation, especially newly created tiny
  components; pieces inside the omission-DSSIM allowance may instead be discarded.

## DSSIM and raster checks

- Local decisions use color-independent grayscale coverage masks because extraction
  already separates regions strictly by color.
- Rasterize at 4x supersampling and downsample for comparisons.
- A local comparison ROI is the complete affected bounding box plus eight source-image
  pixels of padding.
- Track leakage and omission with independent DSSIM limits, both initially 0.01.
- Leakage compares cumulative coverage with that coverage clipped to the immutable
  target. Apply a soft score penalty as leakage approaches its limit and reject a local
  candidate beyond the limit.
- Omission is accumulated when small residual components are discarded and stops before
  its independent limit would be exceeded.
- The final fully rendered, color-aware comparison is diagnostic. If its global DSSIM
  exceeds 0.01, retain the produced shapes and write a warning rather than replacing the
  result. Independently require at least 99 percent target coverage and at most two
  percent raster leakage. Structural failure, incomplete coverage, excessive leakage,
  or failure to reduce the per-unit shape count restores the Safe placements.

## Search performance and determinism

- Candidate evaluation has two stages. Run cheap geometric fitting and contour-distance
  scoring for every primitive/span combination, then perform full affine refinement and
  4x DSSIM only for the best candidates. The finalist count is a detached tuning value,
  initially 12. Classify each dense span as straight, consistently curved, or mixed and
  test only compatible primitive families.
- Use the existing Safe placements as the count baseline. The first advancing candidate
  receives at most 256 cheap jobs and must cover at least 80 percent of two Safe shapes.
  If it cannot, copy Safe immediately without residual remeshing. Later candidates must
  continue replacing remaining Safe geometry, and the final strict shape-count guard
  remains authoritative.
- Candidate jobs run independently on `max(1, hardware_concurrency - 1)` worker threads.
  Gather results and sort by stable deterministic keys before selection.
- Use deterministic evaluation budgets rather than wall-clock deadlines. Budgets scale
  with the post-RDP point count across all active contours:
  - cheap fits: `4096 + 512 * pointCount`, capped at 250000;
  - DSSIM refinements: `128 + 16 * pointCount`, capped at 8192.
  The bases, multipliers, and caps are independent tuning values.
- On a stall, bounded backtracking removes recent placements and tries the next-ranked
  candidate. Initial detached limits are rollback depth 2 and four alternatives per
  step.
- Cancellation is checked between candidate batches, DSSIM evaluations, residual
  rebuilds, and fallback stages.

All numeric behavior controls live together in an `AdvancingFrontOptions` structure or
named constants so they can be adjusted without changing search logic.

## Output, progress, and diagnostics

- Insert **Advancing Front** as the third generated variant and start it hidden.
- Preserve Safe plan draw order and region grouping. Shapes within one region keep their
  acceptance order; fallback placements follow advancing-front placements.
- Keep the existing Dangerous Differences heatmap unchanged so it continues to compare
  Safe and Dangerous only.
- The existing progress UI reports the outer region position plus advancing-front
  residual percentage and cumulative candidate count, so a difficult region never
  appears stuck at an unchanged counter.
- Overwrite `advFront.log` beside the executable at the beginning of every Fill Regions
  run. Its header records timestamp, image size, worker count, and every tuning value.
- Log per-region source mapping, original and guide point counts, cheap/pruned/refined
  candidate counts, accepted primitive counts by type, leakage and omission DSSIM,
  discarded residual area, component splits, backtracks, budget exhaustion, fallback
  shape count, elapsed time by phase, and final diagnostic DSSIM.
- Individual rejected-candidate traces stay disabled behind a detached verbose flag.
  Aggregate rejection reasons are always logged.

## Verification

- Build the Release target and run the existing focused tests.
- Confirm Safe and Dangerous shape counts and rendered output are unchanged for the same
  input.
- Confirm the generated Region Fill container contains visible Safe, hidden Dangerous,
  and hidden Advancing Front groups.
- Confirm cancellation remains responsive and progress changes within a long region.
- Confirm `advFront.log` is overwritten and contains configuration, per-region, fallback,
  and summary records.
- Compare Advancing Front visually by toggling its group and inspect any final-guard
  warning rather than silently discarding its shapes.
