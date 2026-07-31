# Analytic Differential Cover — Minimal-Shape Fill of a Pen Contour

Design + handoff spec for an **opt-in, high-quality Pen fill mode** that covers one
closed, single-colour contour with **as few affine primitive decals as possible**.
Bucket Fill is included because it converts its traced mask into the same editable
Pen contour before filling. The implementation is an independent C++ alternative
to the existing deterministic Pen fill. Fill implementations are grouped under
`src/gui/fill`; the solvers share contour and polygon-mesh infrastructure while
retaining separate selection and optimization policies.

Status: **implemented on the `image_gen` branch.** This document is the behavior
and maintenance specification for the differential-cover subsystem.

---

## 1. Goal and scope

- **Input:** the current valid closed Pen contour, either hand-authored or produced
  by Bucket Fill. The caller flattens the contour into an oriented polygon in
  canvas world coordinates and may also provide a rasterized copy for the
  distance-transform initializer.
- **Output:** an ordered list of **placements**, each a catalog shape id + a 2-D
  **affine transform** (6 DoF), whose opaque union covers as much of the contour as
  the optimizer can reach within its configured contour tolerance. Colour is the
  Pen or Bucket colour.
- **Objective:** minimise the **number of placements** while maximizing legal
  coverage. Complete coverage is preferred but is not a success requirement.
- **Mode:** a persistent **Differential Contour Fill** tool option, off by default.
  When enabled, the normal Pen commit uses this module instead of the deterministic
  Pen fill. It does not refine or wrap the existing result. Runs must be
  repeatable (§9).

Non-goals: lineart/stroke fitting; whole-image segmentation; Fill Regions
integration; gradient/painterly art. The module fills one active contour.

---

## 2. Why a new approach (and why the obvious one fails)

The problem is a **set cover of a target polygon by affine images of a small shape
catalog**, minimising shape count. Two naive framings both fail:

- **Deterministic triangulation** (what the existing fill effectively does for the
  interior): decomposes the region into many non-overlapping triangles/quads. It
  is correct and fast but **cannot use overlap**, so shape count is high on
  organic/non-convex regions. It is a *bad initializer* for a count-minimizing
  solver because it sits in exactly the high-count, non-overlapping local optimum
  we want to escape.
- **Global differentiable area-maximization** (DiffVG/soft-raster style over a
  fixed slot set): optimize N primitives at once to maximize rendered overlap with
  the target. This is slow, prone to local minima, and — critically — **cannot
  guarantee coverage**: maximizing covered area leaves sub-pixel and thin gaps that
  render as holes. Prior experiments confirmed this is not adequate on whole
  images.

**The reframe this doc specifies:** an **explicit-residual greedy covering loop**,
where differentiable optimization is confined to a single well-posed subproblem —
"place *one* primitive to cover as much of the current uncovered residual as
possible, without spilling outside the contour." The residual provides an exact
coverage measurement after every accepted placement. The optimizer decides
where and how large each primitive is, and the loop stops when the residual is
empty, the budget is reached, or no candidate makes sufficient progress. This
removes reliance on a triangle-fan seed and bounds per-step compute.

Placements may overlap each other because every placement has the same opaque
colour. Overlap can lower the shape count without changing the result.

---

## 3. Inputs

The Pen caller provides one contour:

| Name | Type | Meaning |
| --- | --- | --- |
| `mustCover` | polygon set | The flattened active contour. |
| `mayCover` | polygon set | The outward legality envelope derived from the contour tolerance. |
| `colour` | RGBA | The region colour, copied onto every placement. |
| `mask` *(optional)* | binary raster + bbox | Rasterized `mustCover`, for the distance-transform initializer (§7.3). Can be rasterized from `mustCover` internally instead. |
| `boundarySpans` | ordered line/quadratic spans | Original contour topology used to measure straight-boundary dominance and build hard-edge candidates. |

Polygons are simple, closed, with explicit orientation (outer CCW, holes CW, or
whatever convention the chosen boolean library uses — pick one and enforce it).
Coordinates and all area thresholds are in canvas world units. The solver must
not normalize by zoom, guide scale, or a canonical raster resolution.

> The geometric contract is: maximize coverage of `mustCover`, charge spill
> against that original target, and keep the placement union inside `mayCover`.
> Weighted contour coverage uses only numerical area tolerance at that envelope;
> the regular analytic path retains its configured `epsSpill` allowance.

---

## 4. The shape catalog (verified geometry)

Shapes live in `assets/vector/shape_geometry.json.gz`. Structure:

```json
{
  "shapes": {
    "127": {
      "source": "A_27",
      "size": [85, 128],
      "vertices": [[x, y, alpha], ...],      // local coords, ~128-unit centered box
      "triangles": [[i, j, k], ...]          // indices into vertices
    }
  }
}
```

The fill catalog is the `shape_ids` array in
`assets/differential_shapes.json`. It is loaded when a differential fill starts,
so the deployed asset can be edited without rebuilding the application.

Catalog shapes must be solid, compact residual fillers. Silhouettes with interior
voids or several sharp appendages are excluded because their containment-constrained
placements increase shape count while leaving fragmented residuals.

**Verified structural facts (these are load-bearing for §5):**

- For **every** shape, `tris == verts − 2`, `boundaryEdges == verts`, and no edge
  is shared by more than two triangles. → Each shape is a **single simple polygon**
  (genus 0, no holes, manifold) with a **non-overlapping triangulation**.
- All vertex alphas are `1.0` → shapes are **hard opaque polygons**, no soft-alpha
  edges to handle.
- Coordinates sit in a centered ~128-unit box (well-conditioned for affine maps).

Consequences the implementer can rely on:

1. `area(T(S) ∩ R) = Σ_tri area(T(tri) ∩ R)` is **exact** (non-overlapping tiling,
   no double counting), for any affine `T` and any region `R`.
2. Every triangle is a **convex** clip window → Sutherland–Hodgman applies directly
   even for the non-convex shapes (fang/garlic/tooth/concave arc): just sum over
   their convex triangles.
3. Convex shapes clip against their single boundary polygon for speed; the boundary
   loop is recoverable because `boundaryEdges == verts`. Non-convex shapes retain
   the exact triangle sum.

Load the catalog once into:

```cpp
struct Vec2 { double x, y; };
struct ShapeMesh {
    int id;
    std::vector<Vec2> verts;                 // alpha dropped (all 1.0)
    std::vector<std::array<int,3>> tris;
    std::vector<Vec2> boundary;              // ordered boundary loop (for convex fast path)
    bool convex;
    double area;                             // area of the local silhouette
};
```

(There is an existing `ShapeGeometryStore` loader; using it is fine — it is a data
loader, not fill logic. Loading the JSON directly is equally fine.)

---

## 5. The differentiable kernel — covered/spill area and its gradient

This is the only genuinely new numerical code and the only part with no drop-in
library. It is small (~200 lines). It computes, for one candidate placement,
scalar **covered area** and **spill area** *and their gradients* w.r.t. the 6
affine parameters, using **forward-mode automatic differentiation (dual numbers)**
— no ML framework, no reverse-mode tape.

### 5.1 Parameterization

Affine `θ = (a, b, c, d, e, f)` maps local `u = (ux, uy)` to world:

```
T(u) = ( a·ux + c·uy + e ,  b·ux + d·uy + f )
```

Determinant `det = a·d − b·c`; `area(T(S)) = area(S) · |det|`.

Output affine maps to the game's `QTransform(m11=a, m12=b, m21=c, m22=d, dx=e,
dy=f)` at serialization time (done by the caller, not here).

### 5.2 Forward-mode AD ("jets")

Carry every scalar as a value plus its 6 partials:

```cpp
struct Jet { double v; double g[6]; };      // v = value, g = d v / d θ
// arithmetic overloads: +, −, *, /, with product/quotient rules on g[]
```

The candidate shape's triangle vertices are **linear in θ**, so their jets are
exact and trivial to seed: for local vertex `u`,
`T(u).x = a·ux + c·uy + e` → its jet gradient is `(ux,0,uy,0,1,0)`, etc. Subject
(residual / target) vertices are **constants** during a single primitive's
optimization → jets with zero gradient. Propagating jets through the clip and
shoelace yields `covered.v` and `covered.g[]` in one pass.

### 5.3 Covered area = Σ (residual ∩ triangle), via Sutherland–Hodgman + shoelace

For a fixed subject polygon `R` (the residual — may be non-convex, may have holes;
holes handled by signed areas) and each mapped triangle window `T(tri)`:

1. **Clip** `R` against the triangle's 3 half-planes (Sutherland–Hodgman). The clip
   window must be convex — a triangle always is. Output vertices are either
   original `R` vertices (constant jets) or edge–edge intersection points (jets
   depend on θ through the moving window edge). The intersection point of a
   constant subject edge `p→q` with a moving window edge is a rational expression →
   fully differentiable; propagate jets through it.
2. **Shoelace** the clipped polygon for its signed area (as a jet).
3. Sum signed areas over all triangles of the shape. Because the triangulation
   tiles the shape without overlap, the sum equals `area(T(S) ∩ R)` exactly.

Handle `R` with holes by giving holes negative orientation and summing signed
areas; the clip is linear over the subject's subpaths.

### 5.4 Spill area

```
spill_out(θ) = area(T(S)) − area(T(S) ∩ mustCover)
            = area(S)·|det(θ)|  −  Σ_tri area( T(tri) ∩ mustCover )
```

Both terms are jets (the first via `|det|`, the second via §5.3 against
`mustCover`). `spill_out` is the area of the placement lying **outside** the active
contour; the objective drives it toward 0. Exact legalization separately rejects
coverage outside `mayCover`.

### 5.5 Per-placement objective

```
score(θ) = covered(θ, residual)
         − λ_spill · spill_out(θ)
         + boundedFeaturePotential(θ)
```

Maximize by gradient ascent (Adam is fine; §7.4). `covered` pulls the shape to
swallow residual; `λ_spill` (large, e.g. 4–16× the coverage weight) keeps it inside
`mayCover`. No term is needed to prevent degeneracy — maximizing coverage inflates
the shape on its own.

### 5.6 Non-smoothness (kinks) — expected, benign

`score(θ)` is **C0 continuous but only piecewise-C1**: a kink occurs when the
intersection's combinatorics change (a residual vertex crosses a triangle edge, or
a triangle vertex crosses a residual edge). Within a piece the jet gradient is
exact; at a kink AD returns a valid one-sided gradient. Adam tolerates this — it is
far milder than the global visibility discontinuity DiffVG must edge-sample,
because only **one** convex-clip primitive moves against a **fixed** residual.
Optional robustness: a tiny softening margin on the half-plane tests, or a couple
of random restarts (§7.4) if a region proves finicky. No special discontinuity
machinery is required.

### 5.7 GPU execution

`useGpu` selects the first available evaluation backend in this order:

1. CUDA double-precision optimizer evaluation.
2. Direct3D 11 single-precision legalization with double-precision CPU Adam
   evaluation.
3. Parallel double-precision CPU evaluation.

CUDA is an optional build capability controlled by `FLS_ENABLE_CUDA`. When a CUDA
compiler is found, the build compiles the clipping kernel for the GPU
architectures supported by that toolkit and deploys the CUDA runtime beside the
application. The residual and catalog geometry stay in device buffers for a
greedy step. Covered/spill values and all six partials are evaluated in double
precision on CUDA for both Adam and legalization. Adam's small state update
remains in stable host order, and task results are reduced in stable order on the
CPU. Weighted jobs add their compact feature-potential gradient during that
host update and preserve the assigned anchor while CUDA evaluates shrink
batches. Optimized transforms are legalized at fixed checkpoints and when a
trajectory terminates; exact job winners are then verified against the outward
envelope on the CPU.

The Direct3D evaluator compiles the analytic clipping kernel for shader model 5.
It uses single precision, so it remains an acceleration and screening layer:
Adam's trajectory uses the parallel double-precision CPU kernel, legalization
uses grouped Direct3D dispatches, and competitive results are legalized again
with the CPU kernel.

Both GPU paths preserve exact CPU verification of each job winner. Exact residual
subtraction and final coverage measurement remain in Clipper2. Backend
initialization or evaluation failure advances to the next backend, and an
interrupted optimizer step is recomputed through a precision-safe path. Failure
reasons remain in the profile. Backend selection does not alter placement budget,
optimizer iteration counts, restart counts, legalization steps, or stopping
thresholds.

Evaluation requests that exceed a backend dispatch capacity are divided into
smaller batches and evaluated recursively on that backend. Batching preserves
every requested candidate and calculation; it is not a placement clock,
calculation cap, or reduced optimizer budget.

Feature-weighted requests currently use the parallel CPU evaluator. The CUDA
and Direct3D paths remain active for unweighted comparison runs until their
feature-potential values and gradients have parity tests.

---

## 6. The exact residual loop

Keep an exact `residual` polygon so coverage and stopping state are measured
geometrically rather than inferred from the optimizer's score.

```
residual   ← mustCover                      // exact polygon
placements ← []
while area(residual) > ε_area and |placements| < budget:
    (shapeId, θ, gain) ← bestPlacement(residual, mayCover, catalog)   // §7
    if gain < ε_gain:                        // no primitive makes progress
        stalled ← true
        break
    footprint ← polygon(T_θ(shape[shapeId])) # exact, via boolean lib
    placements.append({shapeId, θ})
    residual ← residual  −  footprint        # exact boolean subtraction
return placements, area(residual)
```

- `residual`, `footprint`, and the subtraction are **exact** polygon booleans
  (non-differentiable — that is fine; they run *between* optimization steps, and
  `residual`/`mayCover` are constant *during* a step, which is what lets §5 treat
  the subject as constant jets).
- Transform each catalog boundary into the boolean library's outer-path
  orientation before subtraction or union. Affine reflection reverses source
  winding but does not change a solid placement into a hole.
- Termination: each accepted step removes a positive area from `residual`; the loop
  ends when the residual is negligible, the budget is hit, cancellation is
  requested, or candidate gain falls below `epsGain`.
- `epsArea` is a small world-unit area threshold so negligible residues do not
  spawn shapes.

**Polygon boolean library:** use **Clipper2** (C++, permissive Boost Software
License, robust integer-based booleans). Vendor it under `third_party/`. Note:
Clipper2 scales coordinates to integers for robustness — use it only for the exact
residual/footprint booleans. Keep the §5 differentiable clip in floating point as
its own code path (it needs derivatives, which Clipper2 does not provide).

---

## 7. Choosing each placement (the greedy step)

`bestPlacement(residual, mayCover, catalog)` returns the shape+affine that removes
the most residual this step.

### 7.1 Candidate shapes — global and component-local routing

Do **not** optimize every catalog shape on each step. Rank the catalog against
the residual descriptor and optimize only the three closest shapes, then keep
the best:

- Compute the residual and catalog bounding-box aspect ratios and occupied-area
  fractions.
- Rank by logarithmic aspect-ratio distance plus weighted occupied-area distance.
- Break equal descriptor distances by shape id for deterministic selection.
- When the residual has multiple connected components, repeat the descriptor
  comparison for the component containing the distance-transform seed. Add at
  most one component-local candidate when its descriptor match is decisively
  better than the global route.

The component-local candidate may replace a global candidate only when it has
both a decisive exact-area advantage and a decisively simpler resulting residual.
It cannot extend a step after all global candidates stall. The router therefore
keeps per-step cost bounded regardless of catalog growth while avoiding count
inflation from weak component-local gains.

### 7.2 Structural prepasses

Before greedy optimization, line-like contour spans are clustered into two
nonparallel affine axes. Eligible contours are snapped into the corresponding
oblique coordinate frame, divided into occupied grid cells, and solved as a
deterministic minimum rectangle cover. Each rectangle is translated and shrunk
independently until its exact footprint is legal against the outward envelope.

A rectangle plan covering at least 98 percent of the target completes directly.
A legal plan covering at least 90 percent also completes when the residual's
maximum inscribed radius is within the coordinate snapping tolerance. This
distinguishes boundary detail from a missing structural region. Other legal
plans above the same coverage floor seed the greedy solver with their residual.
The configured catalog must contain Square for this pass.

The first greedy step adds one oriented-bounds initialization for every
catalog-shape/component pair. This gives affine-equivalent or near-equivalent
components an opportunity to be removed with one placement before the local
medial-axis search dominates.

The original contour spans also provide a straight-boundary ratio. A
hard-edge-dominated contour produces fixed polygon-mesh candidates using the
catalog's square and triangle geometry. Mixed contours retain every mesh
candidate. The complete mesh is first legalized coherently by scaling every
piece around the target center, preserving shared seams. A legal mesh covering
at least 98 percent completes directly. When that plan is unavailable, its
pieces pass through the same exact coverage and spill checks as optimized
candidates.

Incomplete structural and mesh results remain candidate-generation passes. They
compete with differentiable candidates through the common exact selector.

### 7.3 Initialization (per optimized candidate) — medial axis, **not** triangulation

Good init is what makes the optimizer fast and local-minimum-free:

- Compute the **distance transform** of `residual`. Its maximum is the deepest
  interior point `p*` with clearance `r*` (the largest inscribed circle).
- Seed the candidate so its silhouette is roughly the largest-inscribed instance at
  `p*`: circle → center `p*`, radius `r*`; square/triangle → centered at `p*`,
  scaled ~`r*`, rotated to the local residual orientation (PCA of residual pixels
  near `p*`); concave shapes → align the shape's concavity to the nearest residual
  boundary feature.
- This is the classic medial-axis "cover with maximal inscribed shapes" seed and
  the analog of LIVE's component-wise path initialization. It yields an overlapping,
  low-count cover and keeps each Adam run to a short warm descent.

### 7.4 Optimize (per candidate)

- Ascend `score(θ)` (§5.5) with Adam over the 6 params. ~100–300 iters; stop early
  on small gradient norm.
- Optionally 1–3 random restarts (jitter θ) and keep the best `score`; makes the
  step robust to kinks without a framework.
- Reject a candidate whose `covered < epsGain`. Legalization uses the configured
  outside-area allowance for the regular analytic path and numerical area
  tolerance for the weighted contour envelope. Spill against `mustCover` remains
  an objective and reporting term.
- CUDA evaluates the double-precision Adam gradients and legalization batches
  when available. Direct3D accelerates legalization while all but one available
  CPU thread evaluates Adam. GPU candidates are verified by the CPU kernel before
  acceptance. Initial jitter is generated in stable job order before dispatch,
  results are written to fixed slots, and selection is performed in that same
  order so scheduling cannot change the output.

### 7.5 Select

Compute exact residual subtraction for every legal candidate, find the maximum
gain, and discard candidates below the configured `areaWindowRatio`. Rank the
remaining candidates by newly represented exposed-union feature weight,
boundary distance, residual complexity, exact gain, Tversky similarity, shape
id, and affine transform. A candidate cannot lose feature weight already
represented by the current union. The accepted placement stores its exact
subtraction gain, newly represented feature ids, and exposed contour-arc
contribution.

### 7.6 Redundancy pruning and local adjustment

After the greedy loop stops, run an exact backward-elimination pass:

1. Measure each placement's unique covered area by subtracting the union of all
   other placements from the target.
2. Rank placements from least to greatest unique area, with deterministic
   shape/transform tie-breaks.
3. Tentatively remove the least-essential placement. Accept immediately when
   exact coverage and outside-area tolerances remain satisfied.
4. Otherwise, order spatially overlapping survivors by their overlap with the
   removed footprint. Re-optimize each survivor from its existing affine
   transform against the exact area not covered by the other fixed placements.
   Retain only adjustments that monotonically reduce the trial residual and
   satisfy the outside-area tolerance.
5. Accept the removal when the adjusted trial satisfies the fixed coverage and
   outside-area limits, then recompute unique areas and restart. Stop only when
   an entire pass removes nothing or cancellation is requested.

The coverage floor is the greater of the completed greedy cover minus the
absolute area tolerance and the compact-cover threshold. The outside-area
ceiling is fixed from the completed greedy cover. Repeated removals therefore
cannot accumulate loss beyond those fixed limits. Survivor adjustment uses the
selected CUDA, Direct3D, or CPU optimizer without a placement clock, calculation
cap, or separate iteration reduction.

Weighted pruning also compares complete metric snapshots. It preserves the
accepted represented-feature weight, Tversky floor, outward-distance limit, and
legality-envelope constraint. Final ownership is reassigned from the exposed
placement union.

When the pruned result is below the compact coverage threshold, compute the exact
repair target as `postPruneResidual - prePruneResidual`. Run one additive greedy
cover on that target using the normal catalog, routing, optimizer, legality
checks, and remaining placement budget. Append the resulting placements to the
pruned cover. A result meeting the compact threshold retains its accepted
shape-count tradeoff. The repair result is not pruned again.

---

## 8. Budget, stopping, and partial results

- **Budget:** `budget` caps placements. The default remains high because this is
  an explicitly slow mode, and cancellation must be checked throughout routing,
  optimization, and residual subtraction.
- **No fallback:** a stalled or budget-limited run does not call another fill,
  triangulate the residual, discard it as occluded, or fabricate a placement.
- **Inactivity timeout:** stop after 60 seconds without an accepted placement or
  prune operation. Timeout uses the same exact partial-result finalization as
  user cancellation.
- **Partial insertion:** a nonempty placement list is a usable result even when
  `residualArea > epsArea`. The caller inserts it, clears the active contour, and
  reports the uncovered world-unit area plus the stopping reason. A zero-placement
  result inserts nothing and leaves the active contour available. Cancelling an
  active differential fill retains and inserts the best exact cover completed
  before cancellation.
- **Progress:** report exact covered-area progress after every accepted placement
  and every accepted prune operation. Elapsed wall time updates independently
  throughout the run.
- **Profiling:** every differential run records total wall time, greedy setup,
  parallel candidate-batch wall time, cumulative candidate worker time, Adam
  evaluation time, legalization time, exact residual updates, final measurement,
  evaluation counts, worker configuration, evaluation backend, GPU adapter,
  GPU failure reason, GPU batch and intersection-task counts, and cumulative GPU
  evaluation wall time in `pen_fill.log`. It also records whole-component jobs,
  generated hard-edge candidates, feature-aware jobs and rejections, complexity
  selections, and accepted placements
  from component-local, whole-component, and hard-edge routes. Prune wall time,
  passes, attempts, survivor optimizations, adjusted placements, and removed
  placements are recorded separately. The pre-prune and post-prune residual
  areas, repair-target area, repair placements, repaired area, remaining newly
  exposed area, and repair wall time are also recorded. Cumulative worker
  durations overlap while jobs execute in parallel and are not elapsed wall time.
  Structural diagnostics record the decision reason, explained boundary
  fraction, grid and rectangle counts, coverage, residual, spill, and whether the
  plan completed or seeded the run. Final shape IDs, covered areas, and affine
  transforms, owned feature ids, exposed contour arcs, exact area terms,
  Tversky similarity, boundary distances, outward distance, Boundary F-score,
  and feature/tangent errors are recorded with the result.

---

## 9. Repeatability (determinism is relaxed, not abandoned)

The optimizer is non-deterministic in principle, but the module must be
**run-to-run repeatable** so regenerating a livery does not reshuffle decals:

- Fixed RNG seed for restarts/jitter (seed derivable from region id), with
  separate streams for optional routes so rejected candidates do not perturb
  later global restarts.
- Fixed iteration counts and a fixed candidate/shape ordering.
- Deterministic tie-breaks (lowest shape id, then lexicographic θ).

Bit-exact reproducibility across machines is **not** required; stable output on one
build is.

---

## 10. Module interface

```cpp
namespace cover {

struct Affine { double a, b, c, d, e, f; };     // maps to QTransform(a,b,c,d,e,f)

struct Placement {
    int   shapeId;
    Affine xf;
};

struct FillInput {
    // Exact polygons in canvas world space; outer CCW, holes CW.
    Polygons mustCover;
    Polygons mayCover;                            // outward legality envelope
    QVector<ContourSpan> boundarySpans;
    // Optional prebuilt raster of mustCover for the DT initializer.
    QImage mask;
    QRectF maskBounds;
};

struct FillOptions {
    int    budget        = 100000;               // max placements
    double spillWeight   = 8.0;                   // λ_spill
    double epsArea       = 0.25;                  // world², residual-empty threshold
    double epsGain       = 1.0;                   // world², min progress per step
    double epsSpill      = 0.25;                  // world², max tolerated outside
    int    adamIters     = 200;
    double adamLr        = 0.05;
    int    restarts      = 2;
    double inactivityTimeoutSeconds = 60.0;
    double boundaryTolerance = 0.1;
    double areaWindowRatio = 0.875;
    double tverskyAlpha = 0.35;
    double tverskyBeta = 1.0;
    double featureWeight = 1.0;
    int    featureRestarts = 12;
    uint64_t seed        = 0;                     // 0 → derive from region
    bool   useRouter     = true;                  // §7.1
    bool   useGpu        = true;                  // optimizer backend, §5.7
    bool   useWeightedContour = false;
};

struct FillProfile {
    double totalWallSeconds = 0.0;
    double greedySetupWallSeconds = 0.0;
    double candidateBatchWallSeconds = 0.0;
    double candidateWorkerSeconds = 0.0;
    double adamEvaluationWorkerSeconds = 0.0;
    double legalizationWorkerSeconds = 0.0;
    double residualUpdateWallSeconds = 0.0;
    double finalMeasurementWallSeconds = 0.0;
    double gpuEvaluationWallSeconds = 0.0;
    double pruneWallSeconds = 0.0;
    double repairWallSeconds = 0.0;
    double prePruneResidualArea = 0.0;
    double postPruneResidualArea = 0.0;
    double repairTargetArea = 0.0;
    double postRepairNewGapArea = 0.0;
    double repairCoveredArea = 0.0;
    QString evaluationBackend;
    QString gpuAdapter;
    QString gpuError;
    uint64_t candidateJobs = 0;
    uint64_t adamEvaluations = 0;
    uint64_t legalizationEvaluations = 0;
    uint64_t gpuBatches = 0;
    uint64_t gpuIntersectionTasks = 0;
    uint64_t wholeComponentJobs = 0;
    uint64_t hardEdgeCandidates = 0;
    uint64_t featureCandidateJobs = 0;
    uint64_t featureCandidateRejections = 0;
    uint64_t selectionInsufficientGainRejections = 0;
    uint64_t selectionEnvelopeRejections = 0;
    uint64_t selectionOutwardDistanceRejections = 0;
    uint64_t selectionFeatureRejections = 0;
    uint64_t pruneAttempts = 0;
    uint64_t pruneOptimizations = 0;
    int greedySteps = 0;
    int complexitySelections = 0;
    int localComponentPlacements = 0;
    int wholeComponentPlacements = 0;
    int hardEdgePlacements = 0;
    int prunedPlacements = 0;
    int adjustedPlacements = 0;
    int prunePasses = 0;
    int repairSteps = 0;
    int repairPlacements = 0;
    int workerThreads = 0;
};

struct FillResult {
    std::vector<Placement> placements;
    Polygons residual;
    FillProfile profile;
    CoverErrorMetrics metrics;
    double residualArea = 0.0;
    double coveredArea = 0.0;
    double outsideArea = 0.0;
    bool budgetHit = false;
    bool stalled = false;
    bool cancelled = false;
    bool timedOut = false;
    QString error;
};

struct FillProgress {
    int placementCount = 0;
    double targetArea = 0.0;
    double coveredArea = 0.0;
    double residualArea = 0.0;
    double elapsedSeconds = 0.0;
};

FillResult analyticCoverFill(const FillInput& in,
                             const std::vector<ShapeMesh>& catalog,
                             const FillOptions& opt,
                             const std::function<bool()>& cancelled = {},
                             const std::function<void(const FillProgress&)>&
                                 progress = {});

} // namespace cover
```

Colour is not in `Placement` — the caller stamps the region colour onto every
returned placement when building decals.
---

## 11. Implementation and validation layout

The implementation is divided by responsibility:

- `differential_cover.cpp` owns solver orchestration, progress, cancellation,
  timeout handling, and repair dispatch.
- `differential_cover_geometry.cpp` owns catalog construction, target and shape
  feature extraction, legality-envelope construction, polygon conversion, exact
  booleans, and the differentiable area kernel.
- `differential_cover_candidates.cpp` owns initialization, routing, CPU/GPU
  optimization dispatch, legalization, and candidate selection.
- `differential_cover_metrics.cpp` owns placement unions, exact cover state,
  area/Tversky summaries, boundary distances, exposed feature matching and
  ownership, and incremental placement gains.
- `differential_cover_strategies.cpp` owns coherent structural and mesh plans.
- `differential_cover_postprocess.cpp` owns pruning, survivor adjustment, and
  profile merging.
- `differential_cover_gpu.cpp` and the CUDA sources own accelerated evaluators.
- `differential_cover_internal.h` is the private contract between those units;
  public callers use only `differential_cover.h`.

Future cover measurements belong in the metrics unit so every selection path
and post-processing stage compares the same snapshot. Candidate objective
extensions belong in the candidate unit, and coherent-plan policy remains in
the strategy unit.

1. **Catalog adapter** — `ShapeGeometryStore` loads `shape_geometry.json.gz`; the
   cover module reconstructs indexed `ShapeMesh` data for the configured ids, validates
   the §4 invariants, and recovers each boundary loop.
2. **Jet type + differentiable clip kernel (§5)** — `Jet` with 6 partials;
   Sutherland–Hodgman clip of a constant subject against a moving convex triangle;
   shoelace; `covered` and `spill_out` with gradients. **Unit-test the gradient
   against finite differences** — this is the correctness lynchpin.
3. **CUDA evaluator (§5.7)** — evaluate the optimizer and legalization kernel in
   double precision, retain stable CPU reduction, and verify values and gradients
   against the CPU kernel.
4. **Direct3D 11 compute evaluator (§5.7)** — batch legalization intersections,
   verify its values and gradients against the double-precision kernel, and
   retain automatic fallback.
5. **Clipper2 2.0.1** is vendored under `third_party/clipper2`; fixed six-decimal
   integer conversion wraps residual, footprint, union, and subtraction booleans.
   The initializer rasterizes world coordinates independently.
6. **Single greedy step (§7)** — structural prepasses, global and component-local
   routing, DT initialization, Adam, residual-complexity ranking, and exact
   selection.
7. **Residual loop (§6) + stopping (§8) + repeatability (§9).**
8. **Redundancy pruning (§7.6)** — exact unique-area ranking, tentative removal,
   local survivor adjustment, fixed acceptance limits, convergence, and one
   additive repair cover of newly exposed residual.
9. **Pen commit integration** sits behind the persistent, default-off
   Differential Contour Fill option. Bucket requires no separate solver integration
   because its traced region already becomes an editable Pen contour.

The optimizer caches subject bounds for the duration of each greedy step, rejects
triangle/contour pairs whose bounds do not intersect, reuses clipping buffers, and
transforms each triangle only once when evaluating covered and legal area. These
are exact work-reduction paths: they do not change iteration counts, restarts,
candidate routing, placement budget, or stopping thresholds.

Validate the returned union by checking: (a) reported coverage matches the
union, (b) it stays within `mayCover` up to `epsSpill`, (c) boundary and feature
metrics remain finite, and (d) placement and residual counts are repeatable.

Use `build/Release/pen_fill.log` as the integration reference contour. The test
loads its request points, rebuilds the same Bucket-derived Pen contour in world
coordinates, runs the analytic method with a fixed seed, and verifies that it
produces placements, reports finite coverage/residual metrics, respects the spill
tolerance, and repeats the same placement ids and transforms within floating-point
tolerance. The log is a local test asset and is not compiled into the application.

---

## 12. Prior art / references

The differentiable kernel is a documented technique (differentiable polygon
clipping for rotated-IoU), not novel research; the greedy-few-shapes architecture
mirrors LIVE. None is a drop-in for the affine-catalog C++ case, but they are the
reference implementations to crib correctness from:

- **Differentiable Sutherland–Hodgman + shoelace** (the §5 method):
  <https://github.com/mhdadk/sutherland-hodgman>
- **DGAL — header-only C++ differentiable geometry (polygon intersection):**
  <https://github.com/cmpute/dgal>
- **Differentiable Computational Geometry for 2D/3D ML** (rotated-IoU basis):
  <https://arxiv.org/pdf/2011.11134>
- **LIVE — layer-wise image vectorization, minimal path count, component-wise init**
  (architecture precedent): <https://ma-xu.github.io/LIVE/> ·
  <https://github.com/Picsart-AI-Research/LIVE-Layerwise-Image-Vectorization>
- **Clipper2** (exact polygon booleans, permissive): the residual/boolean engine.

---

## 13. Open risks

- **Boolean robustness/perf.** The exact `residual` loop leans on Clipper2 booleans
  per step. Clipper2 is robust, but many steps on complex regions add up; if it
  becomes the bottleneck, cache `mayCover` clips and consider a scanline/raster
  residual for the *progress test* while keeping exact booleans only for the final
  subtraction.
- **Affine-catalog ceiling.** Twelve affine shapes cap how low the count can go;
  organic regions benefit most, geometric ones barely — so gate the HQ mode to fire
  only where a cheap pre-estimate says the region is count-heavy. Richer shapes are
  a separate axis (a closed-form-fittable catalog extension), out of scope here.
- **Kinks on difficult residuals.** Expected benign (§5.6); restarts are the
  escape hatch. If a contour defeats the optimizer, return the measured partial
  result rather than looping.
- **Controlled spill.** The contour tolerance supplies a bounded outward
  envelope. Spill remains measured against the original target, and the maximum
  outward distance and outside-envelope area remain hard constraints.
