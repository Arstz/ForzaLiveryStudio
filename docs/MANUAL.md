# Forza Livery Studio — User Manual

A practical guide to the editor's tools, keyboard shortcuts, panels, and
recommended working pipelines. For build/developer notes see
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
| Import (C_group / C_livery) / Import Guide Layer | *(menu only)* |
| Export (Flat / Nested) | *(menu only)* |
| Exit | *(menu only)* |

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
| Center View on Selection | `F1` |

### Tools

| Tool | Shortcut | Purpose |
| --- | --- | --- |
| Select | `S` | Click/drag to select and move layers and groups. |
| Move | `V` | Reposition the selection; optional auto-select of the clicked layer. |
| Marquee | `F` | Rubber-band select multiple layers. |
| Transform | `T` | Scale/skew the selection via bounding-box handles. |
| Rotate | `R` | Rotate the selection about its centre. |
| Pipette | `I` | Pick a color from visible layers/guides, apply it, and save it to swatches. |

### Options & Window

| Action | Shortcut |
| --- | --- |
| Toggle Flash Selected Layers | `\` |
| Toggle Guide Layers On Top | `` ` `` |
| Toggle Guide Layer Visibility | `Tab` |
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
  than editing each shape in place. Arrow-key nudging works here too.
- **Rotate (`R`)** — rotate the selection about its centre. Enable **Options →
  Transform Relative Mode** if you want the transform/rotate handles to follow
  the selected shape or group rotation.
- **Pipette (`I`)** — click the canvas to sample the visible color under the
  cursor. It samples regular layers and guide layers according to their visual
  stacking, applies the picked color to the current selection, and adds it to the
  project swatches. Guide colors are sampled from original guide pixels, ignoring
  guide opacity.
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
- **Import** (**File → Import…**) — one context-aware importer. Pick a `C_group`
  or `C_livery` file (or drop a file/folder in); the editor detects which it is
  and loads it as an editable group project or a read-only livery viewer.
- **Export** (**File → Export…**) — one exporter. A dialog asks for the format
  (**Flat** or **Nested**) and then a destination folder.
- **Project files** — projects save to a `.3so` container: the editor project
  JSON wrapped in a gzip stream. Legacy plain-JSON (`.json`) projects still open.
- **Guide layers** — import a raster image as an editor-only reference layer
  (**File → Import Guide Layer…**, toolbar **Add Guide Layer**, or drag an image
  from Explorer). Guide layers are stored inside the project file and ignored by
  game export. **Guide Layers On Top** is enabled by default and can be toggled
  with `` ` ``. `Tab` toggles guide-layer visibility.
- **Visibility borders** — Settings can show viewport/placement reference
  borders on the canvas and choose the reference resolution. **Position Limit
  Border** is off by default.
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
  copy/cut/paste, duplicate, and stamp from here or the Edit menu.
- **Properties** — edit name, shape ID, position, scale, rotation, skew,
  opacity, colour, visibility, mask, and lock for the current selection.
  The color picker samples original guide pixels under the cursor, ignoring
  guide opacity. Enable **Options → Show Property Debug** for extra diagnostics.
- **Shapes** — browse and insert vector shapes.
  - Shapes are labelled `Name (ID)` (e.g. `Square (101)`); names come from
    `assets/vector/shape_names.json` and are reparsed on each launch.
  - The search box matches by **ID** or **name** and activates at **3
    characters**; shorter input shows a hint. Search spans every category (and
    matching custom groups).
  - Star a shape to add it to **Favourites**. Use **Add current selection** to
    save the current selection as a reusable **Custom** group.
- **Buffer** — shows the current clipboard contents.
- **Swatches** — project-specific color palette for applying, saving, and
  removing colors.
- **Header** — edit header metadata written into exported projects.

## Recommended Pipelines

### Edit an existing game vinyl

1. **File → Import…** and pick the `C_group` file (or folder) to load a game
   group as an editable project.
2. Edit with the canvas tools and the Layers / Properties docks.
3. **File → Save Project…** (`Ctrl+S`) to keep an editable `.3so` copy.
4. **File → Export…**, choose **Flat**, to write a game-compatible folder.

### Build a new design from shapes

1. **File → New Project** (`Ctrl+N`).
2. Open the **Shapes** dock, search by name or ID, and insert shapes.
3. Arrange with Select/Move, size with Transform, orient with Rotate.
4. Select related shapes and **Group** (`Ctrl+G`); reuse a finished cluster by
   selecting it and choosing **Add current selection** in the Shapes dock.
5. Export when done.

### Trace over a reference image

1. Import the reference via **Import Guide Layer…** (or drag it in).
2. Build your shapes on top; the guide layer never exports to the game.
3. Hide or delete the guide layer before finishing (it is safely ignored by
   export regardless).

### Flat vs. Nested export

- **Flat** is the stable, game-proven export path — use it by default.
- **Nested** preserves group structure, nesting, and masks. It is validated
  in-game for sibling groups, multi-level nesting, and masks, but is not
  byte-identical to the game's own encoding. Prefer it when group semantics must
  survive the round-trip.
