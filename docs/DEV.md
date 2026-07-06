# Forza GUI Editor

Standalone Qt6/C++ editor for Forza vinyl `C_group` projects. It imports game
files, edits vector layers and guide layers, saves editor project JSON, and
exports flat game-compatible folders.

## Functionality

- Import a `C_group` or `C_livery` source (file or folder) through one
  context-aware **Import…** action that detects group vs livery and loads it as
  an editable project or a read-only multi-section viewer.
- Open/save editor projects as a `.3so` container: the editor project JSON
  wrapped in a gzip stream. Legacy plain-JSON (`.json`) projects still load.
- Drag/drop projects (`.3so`/`.json`), `C_group`/`C_livery` files/folders, and
  image guide layers from Explorer.
- Edit layers with Select, Move, Marquee, Transform, Rotate, and Pipette canvas
  tools.
- Use Move tool auto-select from the Options menu to select clicked layer groups.
  When auto-select is off, clicking outside the selected bounds preserves the
  current selection unless no selection exists.
- Nudge selected layers/guides precisely in Move or Transform with arrow keys;
  normal and Shift step sizes are configurable in Settings.
- Edit layer properties: name, shape ID, position, scale, rotation, skew,
  opacity, color, visibility, mask, and lock state.
- Edit a group or multi-shape selection's position, scale, rotation, and skew as
  a unit: the values transform the selection's bounding box (about its centre)
  rather than each shape in place.
- Drag numeric property labels vertically for live value changes.
- Use editor-only raster guide layers for references; guide layers are saved in
  the project container and ignored by game export. Guide layers can render above
  or below shapes, can be toggled from the keyboard, and can be sampled by the
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
- Edit header metadata for exported projects.
- Export through one **Export…** action whose dialog offers Flat or Nested. Flat
  writes a rebuilt `C_group`, copied sidecars, preview thumbnail, and
  draft/imported header handling. Nested (grouped) export preserves group
  structure, nesting, and masks.
- Configure UI theme, canvas colors, layout, keybinds, behavior options, guide
  visibility borders, and nudge step sizes.
  Fresh settings default to the Dark theme with theme-default canvas colors.
- Collapse dock areas from the dock title-bar collapse buttons.
- Toggle selected-layer flash with `\` or from the Options menu.
- Switch Transform Relative mode from the Options menu when transform handles
  should follow the selected shape or group rotation.

Flat export is the stable game export path. Nested export (the Nested option in
the **Export…** dialog) preserves group semantics: each group is
written with a translation-only origin transform and shapes packed relative to
it, mask groups are emitted as `60` records with per-shape trailing mask flags,
and nested groups carry their own child-type bitmaps. It is validated in-game for
sibling groups, multi-level nesting, and masks, but is not byte-identical to the
game's own encoding.

## Build

```powershell
.\tools\configure.ps1
.\tools\build.ps1
```

The scripts use `VCPKG_ROOT`, defaulting to `C:\vcpkg` or `C:\vcpkg\vcpkg`,
and target `x64-windows`. Build output is written to `build\Release`.

Runtime assets live in `assets` (repo root). The build copies that folder next
to the editor executable, including `assets/vector/shape_geometry.json` and
`assets/vector/shape_names.json`.

## Run

```powershell
.\tools\run.ps1
```

Or run the generated editor executable from `build\Release`.

## Repository Layout

- `src/`  EC++ sources (`core/` library, `gui/` editor split into
  `app/ state/ canvas/ widgets/`, and `main.cpp`).
- `assets/`  Eruntime XPM icons and `vector/` shape data, copied next to the
  executable at build time.
- `tools/`  Ebuild/utility scripts (`configure.ps1`, `build.ps1`, `run.ps1`,
  `gen_xpm.ps1`, `gen_xpm.py`).
- `docs/`  Ethis file, `MANUAL.md` (end-user shortcuts/tools), and format notes
  (`CGROUP.md`, `HEADER.md`).

## Code Map

- `src/main.cpp`
  - Application startup.
- `src/core/`
  - Binary codecs, vinyl tree decoding, livery decoding, project JSON, flat and
    nested payload export, matrix math, header parse/build, and shape registry.
- `src/gui/app/` (`main_window.*`, `dock_chrome.*`, `settings_dialog.*`,
  `theme_manager.*`, `gui_assets.*`, `gui_constants.h`, `perf_utils.h`)
  - Main editor shell, menus/toolbars/docks, import/export actions, drag/drop,
    project save/load, unsaved-change prompts, and selection synchronization;
    plus app settings, theming, dock title chrome (including
    the dock-area collapse buttons), asset/icon loading, opt-in perf timing, and
    cross-cutting UI constants.
- `src/gui/state/` (`editor_state.*`, `editor_state_selection.cpp`,
  `editor_state_clipboard.cpp`, `editor_state_grouping.cpp`,
  `editor_state_snapshot.cpp`, `editor_state_internal.h`,
  `editor_state_util.cpp`, `layer_tree_model.*`, `scene_entry.h`)
  - Project ownership, index cache, lock/visibility/mask propagation, and
    shared Qt signals in the core file; selection, clipboard/duplicate/insert,
    group/ungroup/reorder, and undo/redo snapshots in the per-concern units;
    helpers shared between units in `gui::detail`; the layer/group/guide tree
    model; and the `EntryRef` Shape/Guide unification with shared
    transform/local-rect helpers and the bounds accumulator.
- `src/gui/canvas/` (`project_canvas.*`, `canvas_tools.*`,
  `native_shape_renderer.*`, `drag_cursors.*`)
  - OpenGL canvas, pan/zoom, hit testing, guide layer rendering, selection
    overlays, color sampling, visibility borders, keyboard nudging, and cursor
    handling; per-tool interaction strategies
    (select/move/marquee/transform/rotate/pipette) as `CanvasTool` subclasses;
    OpenGL shape rendering; and layer drag cursors.
- `src/gui/widgets/` (`property_panel.*`, `layer_tree_view.*`,
  `layer_state_delegate.*`, `color_palette_widget.*`, `shapes_browser_widget.*`,
  `shape_geometry_store.*`, `shape_name_store.*`, `font_glyphs.*`,
  `clipboard_buffer_widget.*`,
  `header_metadata_widget.*`, `livery_section_bar.*`, `image_io.*`,
  `import_locations.*`)
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
- `buildTree()` / `validateTree()` / `flattenGroup()`
- `importCGroupFlat()` / `importCGroupNested()`
- `importCLivery()` / `readLiveryPayload()` / `buildLiverySections()`
- `buildFlatPayload()` / `buildNestedPayload()`
- `exportFlatProjectFolder()` / `exportNestedProjectFolder()`
- `projectToJson()` / `projectFromJson()`
- `encodeProjectDocument()` / `decodeProjectDocument()` (`.3so` gzip container)
- `parseHeader()` / `buildHeader()` / `defaultDraftHeader()`
