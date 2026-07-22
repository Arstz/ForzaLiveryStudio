# Forza GUI Editor

Standalone Qt6/C++ editor for Forza vinyl projects. It imports supported game
assets, edits vector and guide layers, saves editor project containers, and
exports grouped `C_group` folders and source-backed `C_livery` folders.

## Functionality

- Import supported asset folders through one context-aware **Import…** action.
  Its minimal filesystem browser identifies importable folders, shows available
  header metadata and thumbnails, routes the selection to the matching decoder,
  and restores its last folder and filters when reopened.
- Open/save editor projects as a `.3so` container: the editor project JSON
  wrapped in a gzip stream. The document is the unified scene tree (v2: a recursive
  `root` of kind-discriminated layer nodes); legacy v1 flat documents and plain-JSON
  (`.json`) projects still load and are upgraded to v2 on save. Imported livery
  paint-material colors, selectors, and finishes are stored as project metadata.
- Drag/drop projects (`.3so`/`.json`), `C_group`/`C_livery` files/folders, and
  image guide layers from Explorer.
- Edit layers with Select, Move, Marquee, Transform, Rotate, Pipette, Pen, and Lining
  canvas tools. Pipette returns to the previously used tool after a successful pick.
  Pen builds a simple closed hard/soft quadratic contour,
  fits affine vector primitives along curved boundaries, and prepares an interior
  boundary before meshing the remaining area.
  The polygonal core uses deterministic ear clipping and compatible Square merging.
  Placements are emitted from the boundary inward under a `2 * point count` shape
  cap, and the result is an ordinary single-colour scene group.
  Lining builds an editable open hard/soft quadratic centreline, expands it to a
  constant-width ribbon, and selects a ranked sequence from its dedicated
  Primitive catalog. Selection follows the authored point structure, ranks
  centreline agreement before width coverage, emits one primary placement per
  authored span, and requires connected overlap before producing an ordinary
  single-colour scene group. The completed sequence normalizes placement widths
  to its curve coverage and derives asymmetric placement orientation
  from the authored path direction. Each fit writes its selection and transform
  diagnostics to `lining_fill.log` beside the executable.
- Use Move tool auto-select from the Options menu to select clicked layer groups.
  **Allow Move Outside Bounding Box** is on by default, letting Move and Transform
  drag the current selection from outside its bounds and giving that selection
  priority over auto-select. Disable it to retain bounds-gated interaction.
- Nudge selected layers/guides precisely in Move or Transform with arrow keys;
  normal and Shift step sizes are configurable in Settings.
- Read world coordinates from pan/zoom-aware canvas rulers and manage persistent
  horizontal and vertical guidelines from their ruler markers.
- Edit layer properties: name, shape ID, position, scale, rotation, skew,
  opacity, color, visibility, mask, and lock state. Direct numeric input accepts
  arithmetic expressions with parentheses and `+`, `-`, `*`, `/`, and `%`.
  The Scale X/Y link applies the same proportional change to both axes.
- Edit a group or multi-shape selection's position, scale, rotation, and skew as
  a unit: the values transform the selection's bounding box (about its centre)
  rather than each shape in place. A group also carries its own transform frame in
  the scene tree — transforming a whole group (on the canvas or via the property
  panel) accumulates the group's frame while its descendant leaves stay baked, so
  the group owns its transform and rendering composes it down the tree.
- Livery logos are first-class raster layers (a `Shape` with a `RasterContainer`),
  rendered uniformly with vector shapes on the canvas, in the 3D car preview, and in
  the layer-tree / clipboard / shapes-browser thumbnails.
- Drag numeric property labels vertically for live value changes.
- Use editor-only raster guide layers for references; guide layers are saved in
  the project container and ignored by game export. Guide layers can render above
  or below shapes, can be shown or hidden together, and can be sampled by the
  Pipette/color picker ignoring guide opacity.
- Preprocess one selected, unlocked guide from **ImgGen → Preprocess Image…**.
  The Qt/C++ pipeline starts from the `anime_detail` settings, performs
  edge-preserving smoothing, median flattening, and circular-hue HSV palette
  clustering, then buckets clusters below the configurable minimum color share
  into their nearest retained color. It then converts visible pixels to a 16-bit
  palette-index image; detail selection, edge cleanup, line preservation, and
  optional region flattening operate only on those labels. Sobel magnitude from
  the smoothed source produces a one-pixel-dilated edge mask. Local detail can
  select a different retained label in region interiors, fades toward detected
  edges, and is disabled throughout the edge mask by default. Output therefore
  remains strictly palette-only.
  Fully transparent source pixels remain transparent, while every partially or
  fully visible pixel becomes fully opaque. The advanced speckle threshold is the
  maximum component size eligible for 3×3 or 5×5 label-majority replacement over
  zero to two edge-cleanup passes. Optional flat fills replace labels inside large
  edge-bounded regions with the region's dominant label. Dark thin-line separation
  is enabled by default: it derives a dedicated line color, reserves a palette
  slot, and keeps detected line labels immutable; the color can be overridden.
  Colors chosen with the color dialog or either preview's eyedropper are locked
  while HSV clustering fills the remaining color limit. Importing project
  swatches switches to a fixed palette containing those swatches plus any manual
  additions; fixed-palette output uses only those exact colors. Colors can be
  removed from either palette. The final palette is added to project swatches as
  part of the same undoable edit. The final retained-color count is stored on the
  guide and becomes the palette cap for region extraction. The dialog shows
  the original beside an asynchronously regenerated downscaled preview, exposes
  four main controls and keeps the remaining numeric controls under **Advanced
  settings**. Both views share wheel zoom and mouse-drag panning, with **Fit**,
  **100%**, and double-click-to-fit navigation. **OK** processes the original
  resolution, preserves dimensions, produces binary alpha, and replaces
  the guide image as one undoable edit. **Create Regions** and **Fill Regions**
  are also in the **ImgGen** menu. Create Regions merges components below its
  persistent ImgGen area slider into the adjacent component with the closest RGB
  colour before tracing. Region filling runs without blocking the main window.
  The cleaned label raster produces a conservative back-to-front layer plan:
  connected exact-colour regions are combined, nearby exact-colour regions can
  be joined through a bounded foreground-only bridge, enclosed exact-colour regions
  can share a unit, and small enclosed regions are incorporated into their surrounding
  backdrop while remaining as later overlay units. Rasterized coverage establishes
  the draw dependencies needed to restore differently coloured ownership above an
  expanded backdrop. A dependency cycle or failed raster comparison rolls back one
  contributing nearby, adjacent, containment, or absorption operation and rebuilds
  the plan, preserving independent unions. Nearby bridges prefer simple ownership:
  paths crossing more than one differently coloured source are rejected, while a
  directly adjacent pair cannot reconnect through the nearby-merge path after its
  adjacent operation is suppressed.
  The complete plan is compared pixel-for-pixel with the existing vector-rendered
  baseline before fitting. Fill Regions retains two comparison variants: the
  visible **Safe** group rolls back operations until that comparison is exact, while
  the hidden **Dangerous** group keeps the first plan that differs from the baseline
  so its optimizations can be inspected manually. Dangerous additionally joins
  same-colour closed contours through a hierarchical straight-corridor graph. A
  candidate exists only when outer-boundary contact pixels have direct straight-line
  visibility entirely through one connected foreground lineart component. Winding
  routes, differently coloured regions, and transparent background are impassable.
  Progressive corridor widths are clipped to eligible lineart pixels and ranked by
  traced contour-point cost before estimated shape count. Spatially separated straight
  contacts can be bundled when their combined expansion further simplifies the merged
  contour. Each accepted bridge is re-evaluated against the currently expanded groups;
  additional contacts must lower that group's current fitting cost, and a bridge cannot
  overlap pixels claimed by another expanded group. Deterministic Kruskal levels then
  produce successively larger merged contours. The log records straight visibility,
  attachment count, expanded-group cost, corridor width, and hierarchy level. Dangerous also tries
  square morphological closing at several radii and a convex contour. A geometry
  candidate is eligible only when every filled pixel was already the exact same
  colour or classified lineart; candidates entering another colour or transparent
  background are rejected. The candidate with the fewest traced path elements is
  retained. Connector candidates are rasterized in their local bounds and cached across
  dependency-cycle rebuilds; morphological and convex trials run only after dependency
  recovery reaches its final plan. Dangerous disables optimization safety: it retains
  every bridge, containment, absorption, morphology, and convex operation even when
  validation reports changed pixels or their layer dependencies contain cycles. Cycles
  are completed with a deterministic forced draw order, preferring enclosing backdrops
  before their absorbed overlay regions when possible. The log marks this policy with
  an explicit warning because the resulting output may overlap other colours, hide
  regions, or have broken ordering. Dangerous removes the area limit for
  enclosed-region absorption. The
  enclosing backdrop fills through an absorbed region and that region remains a
  later, topmost overlay unit.
  Both variants are inserted under one
  **Region Fill** container and can be compared by toggling their visibility. If no
  validation difference exists, both variants contain the same plan. The fitted
  output comparison is also saved as a visible **Dangerous Differences** guide at
  the top of the source guide's parent: red is Safe-only coverage, cyan is
  Dangerous-only coverage, and yellow means both variants cover the pixel but render
  different colours. Fitting runs in parallel on
  half of the available CPU threads, writes results into fixed plan-index slots,
  restores draw order before insertion, reports planning, bridge scoring, dependency
  recovery, simplification, and completed/total fitting phases in the status-bar
  progress bar, and remains cancellable with **Esc** during planning and fitting.
  Potrace curves
  use the same optimized Pen-point and
  curve-Primitive fitter as Bucket Fill. Dense traces are no longer skipped at
  a fixed point-count cutoff: removable cubic junctions use a 1.1-pixel cap,
  preserve orientation and non-crossing single-contour topology, and are
  accepted only when a native-size, 4x-supersampled raster comparison remains
  at or below DSSIM 0.001. Soft runs whose exact maximum offset remains within
  the same cap are reduced to the chord between their hard endpoints. An unsafe
  candidate is backed off through a bounded adaptive search. Curve candidates on
  dense contours are ranked by visual
  importance and evaluated under a point-count-scaled budget, preventing the
  curve search from consuming the whole per-region deadline. If a soft-run
  reduction cannot be fitted, the fitter retries the hard-only reduction before
  retrying the unmerged Potrace controls and using a mesh fallback. Unavoidable
  fallbacks retain the hard-only contour for the mesh simplification stage. If Pen
  rejects a contour before producing optimized points, the original traced outer
  contour is recovered and meshed instead of dropping the unit. Fallbacks apply a
  0.45-pixel RDP pass, accept it only when the
  result remains a valid non-crossing polygon, and retry the original contour if
  simplified triangulation fails. Large polygonal cores evaluate transformed Circle
  primitives as ellipses. A core within 0.5 percent symmetric-difference of an ellipse
  is replaced by that single shape; fully contained ellipses can also replace multiple
  mesh primitives they cover. Per-unit and summary logs report the selected core
  ellipse count. Scene insertion keeps the
  result in one **Region Fill** container with one child group per filled unit.
  Before tracing, progressive line-classification votes identify narrow connected
  fringe components and absorb each chain into its strongest shared-boundary color
  neighbor. Candidate widths are normalized to the median recognized stroke width,
  and persistent historical evidence can classify a component independently of
  final-mask adjacency. A chain enclosed by recognized lineart is incorporated into
  that mask.
  `region_layer.log` records separate Safe and Dangerous sections with source-to-unit
  mappings, adjacent and nearby unions, hierarchical contour connectors and levels,
  absorptions, per-rebuild rollback reasons, operation-specific suppression counts,
  foreign-owner bridge rejections, bridge widths and path-element scores,
  morphological/convex selections, selective geometry suppressions, point reductions,
  Dangerous cycle breaks, dependency counts,
  validation, and fallback state.
  `region_fill.log`
  records every planned
  unit result, its source labels, hard and soft point reductions, the fitted Safe vs.
  Dangerous heatmap difference count, and the largest
  unit's original, optimized, and flattened contour point counts plus DSSIM.
  `region_points.log`
  records every raw path element, optimized Hard/Soft Pen point, and flattened
  optimized-contour point for that largest region. `region_extract.log` records
  progressive segmentation counts, line-vote persistence, cleanup decisions,
  output ordinals, and per-component topology and colour metrics.
- Store project-specific color swatches in the `.3so` project document.
- Manage layer/group trees with thumbnails, visibility/mask/lock badges,
  grouping, ungrouping, deletion, sibling reordering, copy/cut/paste, duplicate,
  and stamp. Livery section labels show live leaf counts after structural edits;
  switching sections reuses cached tree rows and keeps root guide layers available.
- Browse and insert vector shapes from the Shapes dock.
- Place text as a line of vector font glyphs (toolbar **Place Text**): pick one of
  the 11 fonts, type a string, and the glyph shapes are laid out proportionally at
  the view centre and grouped into a new group named after the text. A Monospace
  flag (persisted in QSettings) instead advances every glyph by a fixed cell — the
  font's average glyph width, computed separately for upper- and lower-case.
- Edit header metadata for exported projects, or edit individual project fields
  directly from the **Project** menu: **Target Car…** (the car a livery is for),
  **Project Name…**, and **Creator Name…** (the last also persists as the default
  creator for new projects).
- Align and distribute the current selection from the **Edit** menu. **Align**
  (Top/Bottom/Left/Right/Horizontal Centre/Vertical Centre) snaps each selected
  top-level unit — a whole group counts as one unit, loose leaves individually —
  to a shared edge or centre line.
  **Distribute** (Vertical/Horizontal) evens the gaps between unit bounds
  along one axis; a lone selected group distributes its own direct children. Both
  evaluate world-space shape geometry.
- Export through one **Export…** action that writes a grouped (nested) `C_group`
  folder — preserving group structure, nesting, and masks — plus copied sidecars, a
  preview thumbnail, and draft/imported header handling. The same action exports
  source-backed livery projects while preserving visible section group structure,
  nesting, and masks. With a
  car model loaded, livery export writes `bigThumb.webp` from the textured car render.
- Preview a car in 3D with the current vinyl applied: **Import Car Model…**
  decodes a `.modelbin` (single model), a `.carbin` (full car - referenced parts
  assembled with their per-part transforms), or a zipped car folder (`.zip`) and
  shows it in the **3D Preview** dock.
  The vinyl scene is rasterized into a configurable paint-canvas texture (4x by default).
  Imported sections retain their XML origins and are stored in isolated paint regions.
  Mesh UV channel 3 is transformed by the corresponding per-mesh texture-coordinate
  vector from the model bundle. The fragment shader converts the transformed coordinates
  from backing-texture addressing into the mask atlas, resolves them against the supplied
  `*.swatchbin` masks, and remaps the covered section into its paint region. Meshes without
  this UV data use the geometry projection registered from the
  model scene, XML axes, mask boundaries, part classifications, and locator transforms.
  Section-coordinate orientation is normalized while building and sampling the isolated
  paint regions; decoded project geometry remains unchanged.

  Only the highest LOD of each part is uploaded (names ending `_LOD1`.. are dropped) to
  avoid body overdraw. The swatch masks are uploaded to the GPU as a 2x upsampled
  texture array. Imported `C_livery` section geometry is not rewritten for the preview;
  texture-coordinate conversion is performed in the car shader's sampling path while
  mask sampling remains in the original swatch orientation. The preview updates live as
  layers are edited.
  A "reference only" note is pinned in the preview's corner because its material and
  lighting remain a simplified representation of the in-game renderer.
  When a livery project is opened, the matching car model is auto-loaded from a
  user-configured **car models folder** (matched by the livery's target car id → the
  car registry's model code, searched recursively). If the folder is unset the app
  prompts once to pick it; it can also be set in Settings. A **Discard current model on
  livery open** option (on by default) controls whether opening a livery replaces the
  currently loaded model or keeps it. Changing the target car always reloads the
  matching preview model. Native car texture loading is an opt-in preview
  setting and is disabled by default.
- Configure UI theme, canvas, preview-background and guideline colors, layout,
  keybinds, behavior options, guide
  visibility borders, transform-drag anchors, nudge step sizes, the car models folder,
  and the discard-model option. Every menu-bar action can be bound to a hotkey in the
  keybind settings, even those with no default shortcut.
  Buffer and layer-preview backgrounds independently support theme-default,
  checkerboard, and custom modes. The toolbar can use a vertical icon-only layout.
  Fresh settings default to the Dark theme with theme-default canvas colors.
- Collapse dock areas from the dock title-bar collapse buttons. The button appears on
  only one dock per area (never duplicated across split docks) and is hidden on
  floating docks.
- Show selected-layer flash with `\` or from the Options menu.
- Switch Transform Relative mode from the Options menu when transform handles
  should follow the selected shape or group rotation.

Grouped `C_group` export is the canonical group export path. It preserves group
semantics: each group is written with a
translation-only origin transform and shapes packed relative to it, mask groups are
emitted as `60` records with per-shape trailing mask flags, and nested groups carry
their own child-type bitmaps. It is not byte-identical to the game's own encoding.

Livery (`C_livery`) export is available through the core and GUI for source-backed
projects. Visible section groups are emitted as nested records with composed group and
shape transforms. Mask ancestry and trailing shape-mask state remain represented. A
terminal masked shape uses the trailing `01` marker after the section walk. Structured
sections emit inherited mask leads, single-byte group-to-shape state transitions, and
complete remnants between populated section slots.

## Build

### Windows

```powershell
.\tools\configure.ps1
.\tools\build.ps1
```

The scripts use `VCPKG_ROOT`, defaulting to `C:\vcpkg` or `C:\vcpkg\vcpkg`,
and target `x64-windows`. Build output is written to `build\Release`.
Public builds enable `FH6_PRIVACY_POLICY` by default, which blocks importing
locked game content. Private development builds can opt out at configure time:

```powershell
cmake -S . -B build -DFH6_PRIVACY_POLICY=OFF
```

Livery export shape-limit enforcement is controlled by `ENFORCE_SHAPE_LIMITS`
and defaults to `ON`. Top, Left, and Right sections allow up to 3000 shapes;
the remaining sections allow up to 1000. Configure with
`-DENFORCE_SHAPE_LIMITS=OFF` when this validation is not required.

### Linux (Arch)

**Prerequisites:**

Install build dependencies:

```bash
# Arch (SteamOS)
sudo pacman -S qt6-base qt6-tools qt6-svg zlib cmake gcc ninja

# Ubuntu/Debian
sudo apt install qt6-base-dev qt6-tools-dev qt6-svg-dev zlib1g-dev cmake g++ ninja-build

# Fedora
sudo dnf install qt6-qtbase-devel qt6-qttools-devel qt6-qtsvg-devel zlib-devel cmake gcc-c++ ninja-build
```

**Configure:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

CMake will automatically detect Qt6 and ZLIB from system packages.
Public builds enable `FH6_PRIVACY_POLICY` by default, which blocks importing
locked game content. Private development builds can opt out with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DFH6_PRIVACY_POLICY=OFF
```

**Build:**

```bash
cmake --build build
```

Build output is written to `build/ForzaLiveryStudio`.

### Runtime Assets

Runtime assets live in `assets/` (repo root). The build copies that folder next
to the editor executable, including `assets/vector/shape_geometry.json.gz` and
`assets/vector/shape_names.json`.

## Run

### Windows

```powershell
.\tools\run.ps1
```

Or run the generated editor executable from `build\Release\ForzaLiveryStudio.exe`.

### Linux

```bash
./build/ForzaLiveryStudio
```

## Cross-Platform Notes

The codebase is designed to build on both Windows (via vcpkg) and Linux (via system packages). Platform-specific code (Windows COM for image decoding, Windows libraries) is guarded with `#ifdef Q_OS_WIN` and is automatically skipped on Linux. Qt provides portable abstractions for all GUI and system operations.

## Repository Layout

- `src/`  EC++ sources (`core/` library, `gui/` editor split into
  `app/ state/ canvas/ widgets/`, and `main.cpp`). The core library target is
  `fls_core`.
- `assets/`  Eruntime XPM icons and `vector/` shape data, copied next to the
  executable at build time.
- `tools/`  Ebuild/utility scripts (`configure.ps1`, `build.ps1`, `run.ps1`,
  `gen_xpm.ps1`, `gen_xpm.py`) plus optional dev-only console harnesses
  (`livery_compare`, `model_dump`, `pack_decals`). The harnesses are gated behind
  `FH6_BUILD_HELPER_TOOLS` (OFF by default) and are not built for shipping. Note: the
  scripts are Windows-only; on Linux, use CMake directly.
- `docs/`  Ethis file, `MANUAL.md` (end-user shortcuts/tools), and consolidated
  format notes for Forza Horizon and Forza Motorsport containers, groups,
  liveries, and headers.

## Code Map

- `src/main.cpp`
  - Application startup.
- `src/core/`
  - Binary codecs, vinyl tree decoding, livery decoding, project JSON, flat and
    nested payload export, matrix math, header parse/build, and shape registry.
    `VinylTreeDecoder` provides the shared group and livery decode pipelines.
    Format profiles supply record normalization, root-header handling, terminal
    padding, and post-decode state while both formats use the same tree walkers.
    The FM container reader concatenates sequential compressed blocks and extracts
    the complete tagged stream before entering that pipeline.
    The livery decoder handles the embedded `gyvl` transform dialect (including
    framed 9-byte separate-transform trailers), 16-bit child and bitmap-block counts,
    reserved group bytes, registry-backed vector ID validation, custom-logo stats
    weighting, and section-root boundary guards before converting the decoded tree
    into scene layers.
  - `layer.*` / `visual_container.h`: the unified scene-object hierarchy in
    namespace `fh6::scene` — `Layer` (id, `Transform2D`, opacity, visibility/lock,
    parent) subclassed by `Shape` / `GuideLayer` / `Group`. A group carries its own
    transform and owns its children (`std::vector<unique_ptr<Layer>>`), so transforms
    compose down the tree (`worldMatrix()`); leaf visuals are stored separately as a
    `VisualContainer` — `VectorContainer` (shape id) or `RasterContainer` (a logo
    decal's pixels/id, or a guide's inline image). `clone()` deep-copies for undo.
  - `scene_codec.*`: recursive `kind`-discriminated tree JSON for the canonical
    `Project::root` scene. Imports/load populate `Project::root`, save serializes it
    directly, rendering/editor state walk it directly, and export derives payload records
    from visible scene leaves.
  - `raster_decals.*`: the indexed decal-pixel pack, plus `sharedRasterDecals()` — a
    process-wide lazily-loaded pack shared by every surface that draws logos (GL
    canvas, layer-tree/clipboard/browser thumbnails).
  - `model_bundle.*` / `model_geometry.*` / `model_material.*`: read-only decoder
    for ForzaTech "Grub" bundles (`.modelbin`) — blob/metadata parsing, nested
    material resources, typed shader parameters, and per-mesh geometry dequant
    (positions/normals/UVs/indices, bone transforms, texture-coordinate transforms).
    Material defaults retain their native texture slots and UV tiling for preview use.
    Resolved material defaults and decoded textures are retained in bounded runtime caches,
    while uploaded native textures are reused for the lifetime of the preview context.
    No proprietary DLL is required.
  - `car_scene.*`: `.carbin` reader (ported from CarbinParser) — parses the part
    list, resolves each referenced `.modelbin` next to the carbin, and bakes each
    part's transform (× its own skeleton bone) into a merged `CarModel`. Stock
    selections form the displayed mesh set; all variants also form a projection-only
    mesh set used to register the universal livery masks. Part classifications and
    sibling locator transforms and material paint bindings are retained.
  - `zip_extract.*`: minimal ZIP extractor for car model archives. The 3D preview
    extracts `.zip` car folders to a temporary directory, then reuses the normal
    `.carbin` and sibling-asset loading path.
  - `swatchbin.*`: decoder for `.swatchbin` livery-mask textures (a Grub bundle
    wrapping a TXCB texture blob). Decodes PC-format (linear) BC4/R8 masks to a
    single-channel `SwatchMask`; Xbox/Durango tiled textures are unsupported.
  - `livery_masks.*`: parses a car's `LiveryMasks/` folder — `Masks.xml` per-side
    planar-projection parameters plus the body-side coverage swatchbins — into a
    `LiveryMaskSet` used for atlas coverage and projection fallback.
- `src/gui/app/` (`main_window.*`, `dock_chrome.*`, `settings_dialog.*`,
  `theme_manager.*`, `gui_assets.*`, `gui_constants.h`, `perf_utils.h`)
  - Main editor shell, menus/toolbars/docks, import/export actions, drag/drop,
    project save/load, unsaved-change prompts, and selection synchronization;
    plus app settings, theming, dock title chrome (including
    the dock-area collapse buttons), asset/icon loading, opt-in perf timing, and
    cross-cutting UI constants.
- `src/gui/state/` (`editor_state.*`, `editor_state_selection.cpp`,
  `editor_state_clipboard.cpp`, `editor_state_grouping.cpp`,
  `editor_state_snapshot.cpp`, `layer_tree_model.*`, `scene_view.h`)
  - Project ownership, index cache, lock/visibility/mask propagation, and
    shared Qt signals in the core file; selection, clipboard/duplicate/insert,
    group/ungroup/reorder, and undo/redo snapshots in the per-concern units;
    the layer/group/guide tree model; and shared scene transform/local-rect
    helpers plus the bounds accumulator.
- `src/gui/canvas/` (`project_canvas.*` plus its per-concern implementation units
  `project_canvas_hit.cpp` / `project_canvas_drag.cpp` / `project_canvas_cursor.cpp` /
  `project_canvas_paint.cpp` / `project_canvas_events.cpp` and the shared
  `project_canvas_internal.h`; `canvas_tools.*`, `native_shape_renderer.*`,
  `car_model_renderer.*`, `drag_cursors.*`)
  - OpenGL canvas, pan/zoom, coordinate rulers, project guidelines, hit testing, guide layer rendering, selection
    overlays, color sampling, visibility borders, keyboard nudging, and cursor
    handling. `ProjectCanvas` is one class declared in `project_canvas.h` but split
    across translation units by concern (lifecycle/view in `project_canvas.cpp`;
    hit-testing/selection-box in `_hit`; drag gestures in `_drag`; cursor resolution
    in `_cursor`; painting/overlay/flash/guides in `_paint`; input events in
    `_events`), mirroring the `editor_state_*` layout; the anonymous-namespace helpers
    shared between those units live in `project_canvas_internal.h`. Read paths walk the
    scene through the shared `forEachSceneShape`/`forEachSceneGuide` iterators, which
    hide the EditorState-render-entries vs. canvas-local-tree backends. Per-tool
    interaction strategies
    (select/move/marquee/transform/rotate/pipette/pen/lining) as
    `CanvasTool` subclasses;
    `pen_fill.*` owns quadratic contour validation, Primitive silhouettes,
    outward-curve fitting, bounded core construction, and union-area reporting;
    `lining_fill.*` owns open-centreline construction, semantic span fitting,
    stroke containment, and overlap-connected lining Primitive selection;
    `polygon_mesh.*` owns polygon normalization, intersection validation,
    deterministic ear clipping, maximum compatible square pairing, and exact
    Square/Triangle affine placement;
    OpenGL shape rendering (`native_shape_renderer`, which walks the `scene::Group`
    tree with a matrix stack — composing each group's frame into its descendants and
    dispatching vector vs raster off the visual container — and also exposes
    `renderSceneToTexture` for the 3D preview); the perspective/depth car mesh
    renderer (`car_model_renderer`) whose fragment shader applies the livery through
    transformed mesh UV3, converts backing-texture coordinates into the mask atlas,
    resolves coverage from the swatch texture array, normalizes section coordinates,
    samples separate packed paint regions, and shades non-paint materials with native
    diffuse, alpha, normal, surface, and emissive maps. Meshes without usable UV3 fall back to
    registered planar projection using the model scene, mask boundary, part metadata,
    and locator landmarks. It uploads only each part's highest LOD; and layer drag cursors.
- `src/gui/widgets/` (`property_panel.*`, `layer_tree_view.*`,
  `layer_state_delegate.*`, `color_palette_widget.*`, `shapes_browser_widget.*`,
  `shape_geometry_store.*`, `shape_name_store.*`, `font_glyphs.*`,
  `clipboard_buffer_widget.*`, `car_preview_widget.*`,
  `header_metadata_widget.*`, `livery_section_bar.*`, `image_io.*`,
  `image_preprocessor.*`, `image_preprocess_dialog.*`,
  `import_locations.*`)
  - `car_preview_widget`: the dockable 3D car preview `QOpenGLWidget` — orbit
    camera, framebuffer thumbnail capture, its own scene/car renderers, and a configurable livery paint texture regenerated
    from `EditorState` change signals. Imported `C_livery` sections are rendered into
    isolated packed regions derived from `Masks.xml`; section axes are normalized in the
    preview copy and sampling path without mutating decoded section/group transforms.
  - Dockable panels and their support: property editing (single/multi/group/
    guide, live color, numeric-label dragging, mixed values); the tree view with
    sibling-only drag/drop reordering, row badges, and thumbnails; the
    project-specific swatches panel; the shape browser with theme-aware previews,
    categories, favorites, and insertion;
    vector geometry loading and human-readable shape names
    (`assets/vector/shape_names.json`, reparsed each launch); the shared font
    glyph-block table (`font_glyphs.*`) mapping characters to font-letter shape
    ids and merged browser sections, used by both the browser and the Place Text
    tool; header editing; image decode (Qt plus Windows WIC) / guide-image encode /
    accepted suffixes; and the label-based, edge-aware C++ guide preprocessing
    engine plus its live-preview dialog.
## Core Entry Points

- `readCGroupPayload()` / `writeCGroupFile()`
- `getLayerData()`
- `decodeGroup()` / `buildTree()` / `validateTree()` / `flattenGroup()`
- `importCGroupFlat()` / `importCGroupNested()`
- `importCLivery()` / `readLiveryPayload()` / `buildLiverySections()`
- `buildLiveryGyvl()` / `encodeCLiveryPayload()` / `exportCLivery()`
- `importFM2023Asset()` / `decodeFM2023RawGroup()` /
  `readFM2023LiveryPayload()` / `decodeFM2023LiverySections()`
- `buildFlatPayload()` / `buildNestedPayload()`
- `exportFlatProjectFolder()` / `exportNestedProjectFolder()`
- `projectToJson()` / `projectFromJson()` (v2 scene-tree JSON; v1 flat loader kept)
- `Project::root` (`scene::Group`, canonical runtime scene tree)
- `scene::sceneTreeToJson()` / `scene::sceneTreeFromJson()`
- `encodeProjectDocument()` / `decodeProjectDocument()` (`.3so` gzip container)
- `parseHeader()` / `buildHeader()` / `defaultDraftHeader()`

## Privacy Policy Build Flag

`FH6_PRIVACY_POLICY` defaults to `ON` for public builds. When enabled, imports
reject locked payloads only: `C_group` byte `0x1D == 0x21` and `C_livery`
little-endian `u32` at offset `0x08 == 1`. Published header metadata is not used as a
denial signal because legitimate users may edit their own published items.
