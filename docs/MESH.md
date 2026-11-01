# Contour meshes

Grouping adjacent sibling contours creates a Mesh. Grouping contours with an
existing Mesh extends that Mesh and preserves its scene identity. Contours and
meshes participate in this operation separately from ordinary scene groups.
Grouping with other layer types is rejected. Reparenting contours or meshes
into ordinary groups is also rejected.

A Mesh is a scene leaf with its own transform, visibility, opacity, and lock.
Its contours retain names, Pen or Lining settings, colors, cutouts, handles,
and appearance. Anchor positions belong to a canonical vertex table. Contour
anchors reference that table; handle vectors and point attributes belong to
each contour. Moving a shared vertex updates every incident contour.

## Connection inference

Closed Pen selections are processed together in one constrained Delaunay
triangulation. All exterior and hole boundaries participate in this subdivision.
Curves are adaptively sampled in a normalized Mesh coordinate frame, with
provenance linking each constraint to its original Bezier span and parameter
interval. CDT resolves constraint intersections and coincident edges using
robust orientation and in-circle predicates. Numerical tolerances follow
geometry scale and floating-point precision.

Each triangle receives the complete set of source regions covering its interior.
Overlap ownership is resolved first, using source order as authoritative priority.
The other regions are cut from that ownership before neighbour inference begins.
Every retained source needs an unambiguous material interior.

External boundaries define the neighbour topology. Candidate contacts require
visible, approaching external fronts and sufficient local clearance on both
sides to meet before local self-collision. Existing shared contacts participate
in that topology. A farther contact
is rejected when a sequence of earlier contacts reaches the same neighbour.
Contact times and material dimensions determine admissibility; there is no
fixed world-space merge distance.

Regions advance at equal physical speed. An adaptive, conforming arrival field
in the common subdivision locates collision boundaries. Competing regions
divide a gap by equal arrival time, with all selected regions participating in
the comparison. Growth is limited to supported neighbour collisions; uncontacted
exterior geometry retains its input dimensions. Geometric centres and growth
measurements are included in diagnostics. Each retained growth component must
remain rooted in its source material; detached components are discarded.
Boundary-only attachments require a resolved shared source arc.

Collision paths are pruned by the axial gradient of their half-gap radius.
The dimensionless gradient limit is one third; it measures widening along the
path rather than absolute separation. Retained intervals must belong to a local
short-gap basin. A parallel interval reached through a widening branch cannot
restart that connection unless it establishes its own local minimum.

Each retained collision interval projects onto both source boundaries together.
Projection follows geometric arc length and original Bezier parameters across
sampled constraints. The resulting paired strips cut the source spans at the
collision endpoints, including endpoints inside optimized spans. Source
tangents determine the gradient independently of anchor spacing. Unmatched
extensions retain their original geometry. Fully accepted corridors retain
their complete arrival cells through the sampled path endpoints. Projected
strips apply to corridors that require pruning. Enclosed numerical voids below
the sampling resolution are resolved within their single surrounding owner.

After external growth, a topology pass separates enclosed pockets from the
unbounded exterior and surviving source holes. Pockets reached by growth are
completed by all surrounding owners. A refined local triangulation carries
equal-speed Eikonal arrival fields, constrained to the pocket. Equal-arrival
cells establish shared internal boundaries. This numerical distance field
follows concave geometry through the available region.

Original source coverage remains authoritative throughout labelling. Connected
ownership components are checked for a material interior, with the primary
interior of each source retained. Unsupported fragments and residual pocket
cells are reassigned by connected propagation from surrounding retained owners.
The sampling resolution determines whether a secondary interior is resolved.
Boundary validity is evaluated in each loop's own coordinate frame.

Internal boundaries are processed within their own region. Empty holes contract
as the region grows. Internal components enclosed by self-contact collapse
within the region's growth window when they contain no foreign material.
Occupied internal space remains protected. Internal boundaries do not establish
neighbour contacts; only external-to-external collisions contribute connections.

Reconstruction extracts edges separating different owners, traces exterior and
hole loops, and restores original curve spans where boundary provenance permits.
Shared junctions and retained source corners survive simplification. Shared
chains are simplified once for all incident contours. Unsafe curve restoration
or simplification retains the underlying subdivision boundary. New subdivision
boundaries become explicit connecting segments. A source region
with multiple connected pieces produces multiple contours with its inherited
appearance. Every boundary reference uses the same canonical Mesh vertex table.

Mesh boundaries receive a final simplification pass using the existing cubic
contour fitter and region RDP simplifier. Chains stop at junctions, ownership
changes, authored Hard points, and incompatible boundary handles. Exterior
boundaries and retained curves participate through adaptive curve sampling.
Closed boundaries retain geometrically separated anchors for a valid loop.
Each chain is fitted once in a common Mesh reference frame, then stored with
identical vertices and reversed handles on its incident contours.

The fitter uses bucket-scale accuracy in a normalized reference frame. A bounded
search targets a soft budget of 250 canonical vertices, with displacement capped
by each incident region's width. Bidirectional distance checks bound each fit
against the original chain. Coverage and per-region area error follow the bucket
fitter's allowances; shared connectivity, winding, hole containment, and overlap
are validated together. Failed fits are localized to their incident regions and
retried at higher accuracy before falling back to RDP and their original geometry.
Geometric validity takes precedence over the soft vertex budget.
Unused canonical vertices are removed. Validation reference regions are prepared
once and reused during restoration and simplification retries. Diagnostics record
the achieved budget, fit allowance, coverage loss, regional mismatch, timing, and
the affected regions and validity checks for each retry.

Validation checks the reconstructed geometry against the assigned subdivision,
retains singly owned source material and the combined input material, measures
remaining overlap, and verifies simple boundaries, hole containment, winding,
and shared connectivity. Changes are committed atomically. Curve restoration
is subject to the scale-dependent approximation budget; the implementation does
not assume the papers' formal guarantees for exact polygonal input.

Selections containing open paths retain the ordered arc-correspondence pipeline.
Its regional repair uses bounded gap and overlap ribbons, material-clearance
checks, and atomic topology validation. Open paths do not define filled-region
coverage labels.

### Research references

- Ledoux and Arroyo Ohori, [Edge-matching polygons with a constrained triangulation](https://3d.bk.tudelft.nl/hledoux/pdfs/11_ostrava.pdf), 2011: regional repair through spatial labels.
- Arroyo Ohori, Ledoux, and Meijers, [Validation and Automatic Repair of Planar Partitions Using a Constrained Triangulation](https://www.dgpf.de/pfg/2012/pfg2012_5_Arroyo-Ohori.pdf), 2012: reconstruction of valid boundaries from labelled regions.
- Ledoux and Arroyo Ohori, [Solving the horizontal conflation problem with a constrained Delaunay triangulation](https://3d.bk.tudelft.nl/hledoux/pdfs/17_jgsy_em.pdf), 2017: conflation through a common labelled subdivision.
- Amenta, Bern, and Eppstein, [The Crust and the Beta-Skeleton: Combinatorial Curve Reconstruction](https://ics.uci.edu/~eppstein/pubs/p-crust.html), 1998: geometric feature size and reconstruction topology.
- Shaked and Bruckstein, [Pruning Medial Axes](https://freddy.cs.technion.ac.il/wp-content/uploads/2018/01/Pruning-Medial-Axes.pdf), 1998: radius-based medial-axis regularization and rate pruning. The collision-path cutoff adapts these ideas to the reconstructed gap field.
- Kimmel and Sethian, [Computing Geodesic Paths on Manifolds](https://math.berkeley.edu/~sethian/2006/Papers/sethian.kimmel.geodesics.pdf), 1998: Eikonal distance propagation on triangulated domains. Interior completion uses a numerical arrival-field adaptation constrained to each pocket.

## Diagnostics

Each reconstruction attempt during Mesh grouping writes diagnostics beside the
executable:

- `mesh_reconstruction.log`: timestamped JSON lines with run summaries, stage, outcome, overlap measurements, and decision counts.
- `mesh_reconstruction_latest.json`: the latest detailed report, including source entry identifiers, input scene geometry, accepted output geometry, coverage labels, ownership assignments, rejection reasons, tolerances, and timing.

The latest report is replaced atomically. Detailed decision records are bounded;
aggregate counts include omitted records. Coordinates and areas use Mesh-local
units unless a field explicitly indicates normalization. The application uses
its local-data `diagnostics` directory when the executable directory is not
writable. Diagnostic I/O does not affect reconstruction acceptance.

Closed-region reports identify the `topology_wavefront` pipeline and include
neighbour decisions, collision times, per-region growth, self-collapse and
discarded detached areas,
geometric centres, subdivision vertices, coverage sets, final owners, and
output-to-source labels. Retained polygonal boundaries are identified by source
label. The triangle snapshot has a record limit and reports omitted triangles.
Corridor profiles include retained and discarded lengths, gradients, basin
rejections, projection breaks, and bounded endpoint records with source span
parameters. Reports also count interior source cuts and resolved numerical
voids. The summary log excludes the detailed subdivision and corridor profiles.
Interior reports include completed pocket areas and owners, protected openings,
arrival updates and travel distances, reassigned component counts and areas,
residual void repairs, and discarded unrooted material.
The `boundarySimplification` record includes canonical vertex counts before and
after reduction, chain counts, cubic and RDP results, validation attempts,
displacement tolerance, and timing.

## Editing and persistence

Selecting a Mesh exposes its canonical anchors across all faces and holes.
Shared edges are drawn and picked once. Drag an anchor to move it, and use
Shift-click to extend the point selection. Selected anchors expose their curve
handles. Anchor movement updates each canonical position once; shared handle
edits update the oriented controls of every incident face.

Ctrl-click a boundary to split its curve and insert one shared anchor. Right-click
an anchor, or press Backspace with points selected, to remove anchors from all
incident loops. Deletion retains shared junctions, closed-loop cardinality, and
mesh connectivity. Coordinate changes pass through the complete Mesh world
transform. Each gesture has one project history entry; cancelling restores its
original geometry. Mesh transforms and appearance remain available through the
regular editing controls. Ungrouping dissolves the Mesh into independent contours
with their current geometry.

Project document version 5 stores meshes, canonical vertices, contour references,
and per-contour attributes. Loading validates references and connectivity.
Cloning, clipboard operations, and project history preserve topology. Meshes
remain editor content during native game export.
