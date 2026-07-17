# Forza GUI Editor

Standalone Qt6/C++ editor for Forza vinyl projects. It imports supported game
assets, edits vector and guide layers, saves editor project containers, and
exports grouped `C_group` folders.

## Functionality

- Import supported asset folders through one context-aware **Import…** action.
  Its minimal filesystem browser identifies importable folders, shows available
  header metadata and thumbnails, routes the selection to the matching decoder,
  and restores its last folder and filters when reopened.
- Open/save editor projects as a `.3so` container: the editor project JSON
  wrapped in a gzip stream. The document is the unified scene tree (v2: a recursive
  `root` of kind-discriminated layer nodes); legacy v1 flat documents and plain-JSON
  (`.json`) projects still load and are upgraded to v2 on save.
- Drag/drop projects (`.3so`/`.json`), `C_group`/`C_livery` files/folders, and
  image guide layers from Explorer.
- Edit layers with Select, Move, Marquee, Transform, Rotate, Pipette, and Pen
  canvas tools. Pen builds a simple closed hard/soft quadratic contour,
  fits Circle or affine Ellipse primitives along outward curved boundaries, and
  replaces those boundaries with chords before meshing the remaining interior.
  The chordal core uses deterministic ear clipping and compatible Square merging.
  Placements are emitted from the boundary inward under a `2 * point count` shape
  cap, and the result is an ordinary single-colour scene group.
- Use Move tool auto-select from the Options menu to select clicked layer groups.
  When auto-select is off, clicking outside the selected bounds preserves the
  current selection unless no selection exists.
- Nudge selected layers/guides precisely in Move or Transform with arrow keys;
  normal and Shift step sizes are configurable in Settings.
- Read world coordinates from pan/zoom-aware canvas rulers and manage persistent
  horizontal and vertical guidelines from their ruler markers.
- Edit layer properties: name, shape ID, position, scale, rotation, skew,
  opacity, color, visibility, mask, and lock state.
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
- Store project-specific color swatches in the `.3so` project document.
- Manage layer/group trees with thumbnails, visibility/mask/lock badges,
  grouping, ungrouping, deletion, sibling reordering, copy/cut/paste, duplicate,
  and stamp.
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
  preview thumbnail, and draft/imported header handling. Livery export is
  temporarily blocked in the UI while artwork synthesis remains incomplete.
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
  currently loaded model or keeps it. Native car texture loading is an opt-in preview
  setting and is disabled by default.
- Configure UI theme, canvas and guideline colors, layout, keybinds, behavior options, guide
  visibility borders, transform-drag anchors, nudge step sizes, the car models folder,
  and the discard-model option. Every menu-bar action can be bound to a hotkey in the
  keybind settings, even those with no default shortcut.
  Fresh settings default to the Dark theme with theme-default canvas colors.
- Collapse dock areas from the dock title-bar collapse buttons. The button appears on
  only one dock per area (never duplicated across split docks) and is hidden on
  floating docks.
- Show selected-layer flash with `\` or from the Options menu.
- Switch Transform Relative mode from the Options menu when transform handles
  should follow the selected shape or group rotation.

Grouped export is the canonical GUI export path. It preserves group semantics:
each group is written with a
translation-only origin transform and shapes packed relative to it, mask groups are
emitted as `60` records with per-shape trailing mask flags, and nested groups carry
their own child-type bitmaps. It is not byte-identical to the game's own encoding. Livery
(`C_livery`) encoding remains available in the core for development, but the UI
rejects livery export until artwork synthesis is complete.

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
    framed 9-byte separate-transform trailers), three-byte group controls and
    variable child bitmaps, registry-backed vector ID validation, custom-logo stats
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
    (select/move/marquee/transform/rotate/pipette/pen) as
    `CanvasTool` subclasses;
    `pen_fill.*` owns quadratic contour validation, Primitive silhouettes,
    outward-curve fitting, bounded core construction, and union-area reporting;
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
  `import_locations.*`)
  - `car_preview_widget`: the dockable 3D car preview `QOpenGLWidget` — orbit
    camera, its own scene/car renderers, and a configurable livery paint texture regenerated
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
    tool; header editing; and image decode (Qt plus Windows WIC) / guide-image
    encode / accepted suffixes.
## Core Entry Points

- `readCGroupPayload()` / `writeCGroupFile()`
- `getLayerData()`
- `decodeGroup()` / `buildTree()` / `validateTree()` / `flattenGroup()`
- `importCGroupFlat()` / `importCGroupNested()`
- `importCLivery()` / `readLiveryPayload()` / `buildLiverySections()`
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
