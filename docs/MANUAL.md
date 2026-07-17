# Forza Livery Studio — User Manual

A practical guide to the editor's tools, keyboard shortcuts, and panels. For
build and developer notes see
[DEV.md](DEV.md).

> All keyboard shortcuts below are the defaults. Every shortcut is rebindable
> from **Window → Settings…** (`Ctrl+K`).

## Keyboard Shortcuts

### File

| Action | Shortcut |
| --- | --- |
| New Project | `Ctrl+N` |
| Open Project (`.3so`) | `Ctrl+O` |
| Save Project (`.3so`) | `Ctrl+S` |
| Save Project As (`.3so`) | `Ctrl+Shift+S` |
| Import Asset / Import Guide Layer / Import Car Model | *(menu only)* |
| Export (grouped C_group) | *(menu only)* |
| Exit | *(menu only)* |

> Every menu-bar action is rebindable in Settings, including the *(menu only)*
> ones that ship without a default shortcut.

### Edit

| Action | Shortcut |
| --- | --- |
| Undo | `Ctrl+Z` |
| Redo | `Ctrl+Y` |
| Copy | `Ctrl+C` |
| Cut | `Ctrl+X` |
| Paste | `Ctrl+V` |
| Group / Ungroup | `Ctrl+G` |
| Ungroup Flat | `Ctrl+Shift+G` |
| Fold All Groups | `Ctrl+Shift+E` |
| Delete Selected | `Del` |
| Stamp (duplicate in place) | `Y` |
| Flip Selection | `Tab` |
| Center View on Selection | `F1` |
| Align Top / Bottom | `Ctrl+Shift+Up` / `Ctrl+Shift+Down` |
| Align Left / Right | `Ctrl+Shift+Left` / `Ctrl+Shift+Right` |
| Align Horizontal Centre | `Ctrl+Shift+C` |
| Align Vertical Centre | `Ctrl+Shift+D` |
| Distribute Vertical / Horizontal | `Ctrl+Shift+V` / `Ctrl+Shift+H` |

### Project

| Action | Shortcut |
| --- | --- |
| Target Car… | *(menu only)* |
| Project Name… | *(menu only)* |
| Creator Name… | *(menu only)* |

### Tools

| Tool | Shortcut | Purpose |
| --- | --- | --- |
| Select | `S` | Click/drag to select and move layers and groups. |
| Move | `V` | Reposition the selection; optional auto-select of the clicked layer. |
| Marquee | `F` | Rubber-band select multiple layers. |
| Transform | `T` | Scale/skew the selection via bounding-box handles. |
| Rotate | `R` | Rotate the selection about its centre. |
| Pipette | `I` | Pick a color from visible layers/guides, apply it, and save it to swatches. |
| Pen | `P` | Draw a closed quadratic path and fill it with contained Primitive shapes. |

### Options & Window

| Action | Shortcut |
| --- | --- |
| Show Selection Flash | `\` |
| Show Guidelines | `Ctrl+;` |
| Lock Guidelines | `Ctrl+Alt+;` |
| Delete All Guidelines | `Ctrl+Alt+Shift+;` |
| Show Guide Layers | *(unbound)* |
| Show Guide Layers On Top | `` ` `` |
| Settings… | `Ctrl+K` |

## Tools & Behaviours

- **Select (`S`)** — primary editing tool. Click a shape or group to select it,
  drag to move. Works together with the layer tree selection.
- **Move (`V`)** — dedicated move tool. Enable **Options → Move Tool
  Auto-Select** to select the layer group you click. When auto-select is off,
  clicking outside the current selection preserves it unless nothing is selected.
  Use arrow keys for precise movement: normal nudges use the Settings value
  (`0.1` by default), and `Shift`+arrow uses the larger Settings value (`1.0` by
  default).
- **Marquee (`F`)** — drag a rectangle to select every layer it touches.
- **Transform (`T`)** — drag edge/corner handles to scale, side handles to skew.
  For a group or multi-shape selection the values transform the selection's
  bounding box about its centre (each child keeps its relative position) rather
  than editing each shape in place. `Tab` flips the current selection through the
  scale-sign states. Arrow-key nudging works here too.
- **Rotate (`R`)** — rotate the selection about its centre. Enable **Options →
  Transform Relative Mode** if you want the transform/rotate handles to follow
  the selected shape or group rotation.
- **Pipette (`I`)** — click the canvas to sample the visible color under the
  cursor. It samples regular layers and guide layers according to their visual
  stacking, applies the picked color to the current selection, and adds it to the
  project swatches. Guide colors are sampled from original guide pixels, ignoring
  guide opacity.
- **Pen (`P`)** — place a closed vector contour which is converted into ordinary
  Forza Primitive layers. The first point is a hard corner. Single-click places a
  soft quadratic control and double-click places a hard point; click the first
  point again to close and fill the path. Adjacent hard points form a line,
  hard–soft–hard forms a quadratic curve, and consecutive soft points share an
  implied midpoint. `Backspace` removes the latest point and `Escape` cancels the
  path or a running fill. Self-intersecting contours are highlighted and must be
  corrected before closing.

  Pen uses only **Square** (`101`), **Circle** (`102`), and **Triangle** (`103`),
  including affine rectangle and ellipse forms. It follows the contour inward:
  outward curved boundaries receive fitted Circle or Ellipse placements and become
  chords, then the remaining core is deterministically meshed with Triangle and
  compatible Square placements. Overlap is allowed, curve fits use a bounded spill
  tolerance, and generated shape count is capped at twice the Pen point count. The
  generated layers share the last selected shape color when the corresponding
  new-shape behavior is enabled (otherwise white) and are placed in one **Pen Fill**
  group as a single undoable operation. Filling runs in the background.
- **Place Text** (toolbar) — build a line of text from the vector font glyphs.
  Clicking the toolbar button opens a dialog to pick a **font** (Arial, Magneto,
  Freestyle, Pristina, EnglishMT, BrushMT, Impact, Playbill, TimesNewRoman,
  Elephant, CenturyGothic) and type the **text**. On confirm the glyph shapes are
  laid out proportionally in a line at the current view centre and grouped into a
  new group named after the text. Text is case-sensitive (upper and lower letters
  use different glyphs). Supported characters are `A–Z a–z 1–9 0 ! ? @ &` and the
  lowercase symbols `$ £ ¥ € ( ) ¢ * # + % ; : ,`; a space inserts a gap and any
  other character is skipped (reported afterwards).
- **Numeric property drag** — in the Properties dock, drag a numeric field's
  label left/right (or up/down) to scrub its value live.
- **Align / Distribute** (**Edit → Align** / **Edit → Distribute**) — arrange the
  current selection by its shape geometry. Each selected top-level unit — a whole
  group counts as one, loose shapes/guides count individually — is snapped to a
  shared edge (**Top/Bottom/Left/Right**) or a shared **Horizontal/Vertical Centre**.
  **Distribute Vertical/Horizontal** evens the gaps between unit bounds; selecting
  a single group distributes that group's own direct children. Align needs ≥2 units,
  distribute ≥3.
- **Project menu** — edit one project field at a time: **Target Car…** (the car a
  livery targets), **Project Name…**, and **Creator Name…** (also saved as the
  default creator for new projects). Changing the target car reloads its matching
  model in the 3D preview.
- **Import** (**File → Import…**) — browse folders in the built-in asset explorer.
  Importable folders show their available metadata and thumbnail; selecting one
  routes it to the matching importer. The explorer restores its last location,
  search text, and asset-type filter.
- **Import Car Model** (**File → Import Car Model…**) — load a `.modelbin`,
  `.carbin`, or zipped car folder into the **3D Preview** dock, which shows the
  current vinyl mapped through the model's livery UVs and coverage masks (reference
  only; the in-game material and lighting may differ). Opening a livery auto-loads
  its matching car from the **car models
  folder** set in Settings (prompted once if unset); **Discard current model on
  livery open** (Settings, on by default) controls whether that replaces the
  currently loaded model.
- **Export** (**File → Export…**) — writes a grouped `C_group` folder while
  preserving structure, nesting, and masks. Source-backed livery projects write a
  `C_livery` folder with ordinary section artwork flattened to world-space shapes;
  sections containing masks retain their mask structure. When a car model is loaded,
  livery export renders `bigThumb.webp` from the textured 3D preview.
- **Project files** — projects save to a `.3so` container: the editor project
  JSON wrapped in a gzip stream. Legacy plain-JSON (`.json`) projects still open.
- **Guide layers** — import a raster image as an editor-only reference layer
  (**File → Import Guide Layer…**, toolbar **Add Guide Layer**, or drag an image
  from Explorer). Guide layers are stored inside the project file and ignored by
  game export. **Options → Guides → Show Guide Layers On Top** is enabled by
  default and can be changed with `` ` ``. All guide layers can be shown or hidden
  together from the same menu without changing their individual visibility.
- **Rulers and guidelines** — canvas rulers track world coordinates while the
  view is panned or zoomed. `Alt`+left-click a ruler to add a project guideline,
  drag its ruler marker to reposition it, or right-click the marker to remove it.
  Guideline visibility, locking, clearing, shortcuts, and color are configurable
  from **Options → Guides** and Settings.
- **Visibility borders** — Settings can show viewport/placement reference
  borders on the canvas and choose the reference resolution. **Position Limit
  Border** is off by default.
- **Transform anchors** — Settings can hide transform handles during a drag.
  They remain visible by default.
- **Swatches** — saved colors are project-specific. Click a swatch to apply it
  to the current selection, click `+` to save the current selection color, and
  middle-click or right-click a swatch to remove it.
- **Drag & drop** — drop a project (`.3so`/`.json`), a `C_group`/`C_livery`
  file/folder, or an image file from Explorer straight onto the window.
- **Flash Selected Layers (`\`)** — briefly flash the current selection on the
  canvas to locate it.

## Panels

- **Canvas** — the OpenGL editing surface; pan and zoom to navigate.
- **Layers** — the layer/group/guide tree with thumbnails and visibility / mask
  / lock badges. Reorder siblings by internal drag/drop; group, ungroup, delete,
  copy/cut/paste, duplicate, and stamp from here or the Edit menu. Livery section
  labels display leaf counts that update after structural edits.
- **Properties** — edit name, shape ID, position, scale, rotation, skew,
  opacity, colour, visibility, mask, and lock for the current selection.
  The color picker samples original guide pixels under the cursor, ignoring
  guide opacity. Enable **Options → Show Property Debug** for extra diagnostics.
- **Shapes** — browse and insert vector shapes.
  - Shapes are labelled `Name (ID)`; names come from
    `assets/vector/shape_names.json` and are reparsed on each launch.
  - The search box matches by **ID** or **name** and activates at **3
    characters**; shorter input shows a hint. Search spans every category (and
    matching custom groups).
  - Star a shape to add it to **Favourites**. Use **Add current selection** to
    save the current selection as a reusable **Custom** group.
- **Buffer** — shows the current clipboard contents.
- **Swatches** — project-specific color palette for applying, saving, and
  removing colors.
- **Project** — summary of the open project (name, creator, source, layer/group
  counts, and the target car for liveries). Edit these via the **Project** menu.
- **Header** — edit header metadata written into exported projects.
- **3D Preview** — an orbit-camera view of an imported car with the current vinyl
  mapped onto its paint. Drag to orbit, middle-drag to pan, wheel to zoom. Material
  and lighting are a reference approximation rather than the in-game render. Press
  `U` while the preview has focus to cycle its projection diagnostics.
