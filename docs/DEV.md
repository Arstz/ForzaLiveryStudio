# Forza Livery Studio developer guide

Forza Livery Studio is a Qt6/C++20 editor for Forza vinyl groups and liveries.
It imports game assets, edits a hierarchical scene, saves `.3so` projects, and
exports native game containers. The OpenGL car preview uses decoded game models,
materials, and livery masks.

## Documentation

| Reference | Contents |
| --- | --- |
| [Code style](CODESTYLE.md) | Conventions for new and edited code. |
| [User manual](MANUAL.md) | Editor workflows and tool controls. |
| [Fill engines](FILL_ENGINES.md) | Contour conversion, fitting contracts, region generation, and diagnostics. |
| [Contour meshes](MESH.md) | Shared contour topology, connection inference, grouping, and persistence. |
| [Shape configuration](COMPACT_FIT_CONFIG.md) | Compact Fit and Lining catalogs, validation, and deployment. |
| [Group encoding](CGROUP.md) | Group records, transforms, shapes, masks, and game dialects. |
| [Livery encoding](CLIVERY.md) | Sections, paint descriptors, compression, and native export profiles. |
| [Header encoding](HEADER.md) | Header sidecars across Horizon and Motorsport versions. |
| [WGS replacement](WGS.md) | Save-container layout, replacement transactions, and recovery. |

## Build and run

CMake 3.24 or newer, a C++20 compiler, Qt6 Core/Gui/Widgets/OpenGL/
OpenGLWidgets/Svg, and zlib are required. Clipper2 and Potrace are compiled from
`third_party/` and linked into the editor. CUDA acceleration is detected at
configure time; CPU implementations remain available.

### Windows

Install the Qt base, image-format, SVG, and zlib packages for vcpkg's
`x64-windows` triplet, with the Visual Studio C++ build tools installed. Set
`VCPKG_ROOT` to the vcpkg installation; configuration otherwise checks
`C:\vcpkg` and `C:\vcpkg\vcpkg`.

From the repository root:

```powershell
.\tools\build.ps1
.\tools\run.ps1
```

The build script invokes `tools/configure.ps1` and builds Release into
`build/Release`. The executable is `build/Release/ForzaLiveryStudio.exe`.
Close the editor before replacing it during a build.
Pass `-DisablePrivacyPolicy` to the build script to configure privacy policy as
`OFF`; the default remains `ON`.

### Linux

Install Qt6 development packages with SVG support, zlib, CMake, and a C++20
compiler through the distribution package manager, then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ForzaLiveryStudio
```

Qt6 and zlib are resolved from system packages. Windows-specific integrations
are guarded in the source.

### Configuration

| CMake option | Default | Purpose |
| --- | --- | --- |
| `FLS_ENABLE_CUDA` | `ON` | Detect and build optional fill accelerators. |
| `FLS_PRIVACY_POLICY` | `ON` | Reject imports carrying the native locked-content state. |
| `FLS_BUILD_TESTS` | `OFF` | Build test executables and register CTest checks. |
| `FLS_BUILD_HELPER_TOOLS` | `OFF` | Build model, decal, and finish inspection tools. |
| `FLS_BUILD_LIVERY_COMPARE` | `OFF` | Include the livery comparison tool when helper tools are enabled. |
| `FLS_ENABLE_IMGGEN_MENU` | `OFF` | Expose experimental image-generation actions. |

`tools/configure.ps1` applies the public editor configuration on every scripted
build, resetting tests, helper tools, and ImgGen to `OFF`. For a custom build,
run that script first, apply the required options with `cmake -S . -B build`,
and use `cmake --build build --config Release` directly.

The privacy policy checks the decoded payload: the group lock marker at `0x1D`
or the livery source-state field at `0x08`. Published header metadata is retained
separately. Private development configuration can set `FLS_PRIVACY_POLICY=OFF`.
Livery export validates per-section shape counts against the supported limits.

### Runtime assets

The build deploys `assets/` beside the executable through a validated staging
and replacement process. Deployment preserves the previous assets for recovery
if publication fails. Geometry and fill catalogs must be deployed together.
Restart the editor after repairing runtime assets loaded during initialization.

| Asset path | Role |
| --- | --- |
| `assets/vector/` | Native shape geometry and `shape_names.json` registry. |
| `assets/raster/` | Packed raster decals. |
| `assets/icons/` | Editor icons and cursors. |
| `assets/themes.json` | Theme definitions and UI color defaults. |
| `assets/cars/` | Car registry and preview metadata. |
| `assets/cars/paint_colors.json` | Per-title manufacturer colors and native finish definitions. |
| `assets/compact_fit_shapes.json` | Compact Fit catalog and task membership. |
| `assets/lining_shapes.json` | Lining catalog and normalized centerlines. |

Car registry entries store canonical `model`, `name`, and `year` values, with
game availability in `version`. Their optional `games` map contains only fields
that differ from the canonical values. Wheel sizing uses the same inheritance
rule, with per-game overrides applied recursively to the canonical axle fields.

Catalog changes are loaded per operation; see [configuration deployment](COMPACT_FIT_CONFIG.md#deployment).

## Architecture

CMake builds `fls_core` for codecs and scene data, `fls_profile_compute` for
numeric fitting, and `fls_editor_ui` for the editor. `ForzaLiveryStudio` supplies
the application entry point and links the UI library.

### GUI appearance

On Windows, `title_menu_bar.*` integrates the main menus and dialog captions
through WinAPI client-area calculation. An application event filter installs
dialog caption bars in their root layouts. File, color, and message dialogs use
Qt surfaces. Dialog controls follow their existing window flags, while custom
popup, dock, and toolbar chrome stays independent. The frame retains native side
and bottom resize borders, and Qt measures the frame using the native border
geometry. DWM owns frame painting, with native surface, text, and dark-mode attributes following
the application theme. Frame colors refresh with theme and activation changes.
Native frame configuration runs once when the native handle is assigned,
before Qt creates its native child windows. Client-area calculation follows the
current native styles during window state transitions. Native activation updates
preserve custom caption painting.
Caption state and system commands follow the native window state. Native
geometry notifications and state classification remain separate when the
client area fills a monitor. Queued reconciliation lets Qt finish native move
and resize transitions before refreshing its state and the caption controls.
State notifications preserve Qt's normal-window geometry. Saved maximized
layouts normalize their state during restoration.
Resize borders retain hit-test priority.
Theme-painted window controls occupy the right corner; the application logo
precedes the menus, and the title remains available to the operating system.
Caption glyphs use the theme foreground, and hover uses the shared selection
colors. Native system commands remain enabled. A native hit-test filter routes
menu actions and caption buttons through standard client input. Empty menu-bar
space supplies the native drag and system-menu region, and window edges supply
resize regions. Hit testing only classifies regions. Screen, font, and style
changes update the control geometry. Menu padding uses font metrics to center
text within the caption row.
The canvas container and OpenGL dock bodies use native child windows on Windows.
Render hosts become native after their OpenGL widget subtrees are assembled so
Qt selects OpenGL surfaces during window-wrapper creation. OpenGL composition
belongs to those render hosts, while the main window uses raster presentation.
Native ancestor windows provide clipping and coordinate inheritance for render
hosts through dock reparenting. Geometry, dock activation, visibility, and
native-surface exposure changes schedule a shared repaint of the visible widget
tree. Repainting starts after UI initialization and runs at low event priority
after pending layout work. The repaint pass activates layouts, synchronizes
native child bounds with Qt layout coordinates in physical pixels, invalidates
visible widgets, and flushes the backing store together. Native bounds are
measured through WinAPI and corrected when they differ from the layout. Surface
observers follow native-window recreation and track exposure transitions. Native
client-area resizing requests a complete redraw.
Menu mnemonic underlines are hidden while keyboard shortcuts remain available.
The application color scheme follows the active UI theme. Other platforms use
the standard menu bar and window frame.

`theme_manager.*` owns the theme registry, semantic UI colors, component color
roles, control metrics, the Qt palette, and the shared stylesheet. The selector
uses stable string IDs from `assets/themes.json`. Each definition supplies an
`id`, a display `name`, a `dark` classification, and a `colors` object keyed by
the roles in `uiColorRoles()`. Add definitions to extend the selector. Definitions
load at startup; restart after changing the file.

Mahogany Dark and Mahogany Light use charcoal or cream and blush surfaces with
sage accents. Dark and Light use the legacy neutral palettes with blue accents.
Missing semantic colors use the built-in fallback palette. Component roles
inherit semantic colors unless the definition or user supplies a color. Shared
color overrides also update inherited component colors. Colors accept Qt color
strings, including `#AARRGGBB` for alpha.

Settings groups UI parts by their resolved color, including alpha. Each palette
entry shows a swatch and its main element's name, with other parts in the hover
text. Editing an entry applies the
color to all its parts; resetting restores each part's theme default. Groups
refresh after color changes. The theme color reset clears all overrides for the
selected theme. Changes preview immediately; accepting saves them and canceling
restores the previous colors and theme. Overrides persist under `ui/themeColors/<id>`.
Canvas background preferences persist per theme under `ui/canvas/themes/<id>`.
Existing theme selections and canvas preferences migrate to Mahogany themes.
Custom widget painting uses the resolved theme roles.
Interactive controls and their highlights use a shared 5 px corner radius.
Dock panels use borderless bodies, flat title actions, and one-pixel header
separators. Dock surfaces explicitly paint the panel background. Folded dock
areas paint their exposed space with the same background and their inner gap
with the shared resize-divider color and extent. Painting spans the dock layout
along each area's orientation and follows dock-corner ownership. These
separators are static decoration. Floating docks
reserve an edge margin for resizing, and title bars expose close and collapse
controls.
Theme changes refresh cached icons across dock titles and tab bars. Square action
controls share explicit size constraints, and input editors share surface colors.
Native control glyphs use theme foregrounds. Checkbox and button dimensions follow
shared size roles. Body text uses the system font with a one-point increase;
secondary text uses the system size. Fields, buttons, checkbox indicators,
and Settings help buttons share a 27 px size. Dock tabs occupy 32 px including
their border and highlight spacing. A common inset derived
from font ascent, descent, and capital height centers visible text within each
control. Embedded editors use their parent field's geometry. Vertical centering
preserves horizontal text alignment. Embedded field action buttons use the
field's geometry and text-sized icons. Toolbar icons
match the font's pixel size; button widths follow text and icon metrics with
shared padding. Icon-only toolbar buttons and Settings help buttons are square.
Hover highlights use the shared lighter selection fill without an outline.
Selected controls use the selection fill. Toolbar controls, dock tabs, dock
title actions, menus, and caption controls leave a one-pixel visual gap beside
separator lines. Caption controls retain their full mouse targets.
Dock tabs use text-sized icons and keep selected and inactive labels aligned.
Dock content insets follow the editor layout across floating and tabbed states.
Reset Layout unfolds dock areas, clears their saved collapse state, restores
the default dock arrangement, and fits oversized windows to the active screen.
Theme changes derive fonts from the system baseline. Guideline color is
on the Theme settings page and retains its existing persisted preference.
Resize dividers fill the full grab region with their theme color. Tool settings use
the shared control size, a constant padded row height,
and a movable horizontal toolbar that supports floating. Floating toolbar width
follows active contents above a minimum width. Tool Settings reserves symmetric
vertical padding and the horizontal scrollbar's height so controls fit in the
viewport. Tool Settings shares the toolbar background role. Menu and toolbar
edges, Tool Settings, dock tabs, and widget titles use one-pixel separators from
the Border color role, with layout space
reserved in the toolbar contents for those separators. Toolbar stylesheet
padding stays zero so the drag handle and action layout share the same origin.
`theme_toolbar.*` reapplies layout margins after show, orientation, font, and
style changes so separator spacing follows the active layout.
The status bar uses its own surface role and occupies a fixed 32 px row directly
below the 2D canvas inside the central editor area.
Dock headers indicate keyboard focus, and semantic widget states
share theme-aware text and border colors. Swatch styling derives readable text from
the displayed color. Canvas and preview background preferences remain persisted
through the theme settings.

### Scene and project state

`Project::root` is the canonical `fls::scene::Group`. Groups own child layers;
shapes and guides carry their visual content. Local transforms compose through
the parent chain into world transforms. Vector content refers to native shape
IDs; raster content holds decal or guide imagery. Saved figures retain editable
Pen or Lining contours and participate in scene editing and project undo.

`EditorState` owns the project and its indexes. The command dispatcher validates
and commits edits, coordinates selection and history, and emits updates for the
canvas, layer model, and preview. Reparenting preserves world placement through
recalculated local transforms. Authored visibility is stored independently of
active-section isolation.

Live transforms update affected render entries and their active-section copies
in place. Drag targets retain their starting local and parent frames throughout
the gesture. Selection bounds use cached world transforms, and the canvas shares
one selection-box calculation between property controls and painting until the
selection or geometry changes.

The persisted Hover shape outline option controls canvas hover outlines and is
enabled by default. Selected-shape outlines have their own setting.

The headless GUI command harness accepts `--profile-transforms <report.json>`
to measure CPU input processing with snapping disabled. Rendering time is
excluded from these measurements.

Move and resize snapping cache scene outlines at drag start. `geometry_snap.*`
indexes stationary contour segments and geometry alignment coordinates. Drag
outlines use fully opaque shape layers with hard alpha boundaries. Shape alpha
eligibility is reused by visual identifier for the duration of the drag. Drag
corrections are measured in canvas pixels and solved within the active movement
or scale constraints, using each selected leaf's transformed geometry. The solver
returns active alignment and contact indicators with the correction; the canvas
draws this feedback during the drag. One persisted Automatic snapping option
controls both shape and guideline snapping.

The `.3so` container is gzip-compressed scene-tree JSON, currently document
version 5. Project serialization includes guides, figures, meshes, swatches, retained
header data, and livery paint state. Autosave writes associated projects; locked
per-instance scratch documents support crash recovery, including unsaved work.

### Import and export

Game codecs decode record streams into the shared scene representation.
`VinylTreeDecoder` owns the common group and section walk; format profiles supply
record dialect and semantic interpretation. The shared `vinyl_framing` reader and
writer own child headers, optional attributes, boundary metadata, and physical
record spans. Section mapping and editable geometry are separate from physical
occupancy. Motorsport block
compression is resolved before the shared decoder runs.

The shared `vinyl_encoding` helpers convert scene placement and serialize shape
records, standalone prefixes, and current-generation group transitions. Export
profiles select record markers, mask state, and destination framing.

Grouped export composes scene transforms while retaining nested groups and mask
semantics. Livery export derives section occupancy from visible scene content
and selects the destination's framing, header, paint layout, and compression.
Figures and guides remain editor content. Native encodings can differ while
representing equivalent scene geometry.

PGS targets cover FH2–FH6, FM5–FM7, and Motorsport 2023. Destination-specific
section and paint handling is specified in [native export profiles](CLIVERY.md#native-export-targets).
FH5 WGS export replaces a registered destination slot through the transactional
pipeline in [WGS.md](WGS.md).

### Game resources and preview

Each supported game has an installation-folder setting that accepts an installation
root or its `media` directory. `game_paths.*` probes both `media/Cars` and
`media/pcfamily/Cars`, with shared material archives under
`Cars/_library/Materials.zip`, and textures under `Cars/_library/Textures.zip`.
Painttype materials use the `_fmnext/usercustomizable/` archive prefix.
Automatic model loading prefers the imported livery's game, then searches the
other configured games in descending release order. It uses the car registry's
availability and per-game model names. Model preparation applies the selected
game's wheel-sizing overrides.
The previous single-folder setting migrates to the FH6 setting. The preview's
finish library continues to use the configured FH6 resources.
The target-car chooser shows the catalog's supported versions beside each name.
It scans configured car folders once per opening and matches per-game model
names. The chooser uses themed text and warning colors to indicate model availability.

Core decoders read `.modelbin` geometry and materials, assemble `.carbin` parts,
extract zipped car folders, and decode `.swatchbin` textures and `LiveryMasks/`.
The ZIP reader supports stored, Deflate, and XMem-framed LZX entries (method 21).
LZX decoding uses the vendored libmspack decoder, retaining history between
frames and validating decoded sizes and ZIP CRCs.
Model preparation, material resolution, and enabled texture decoding run outside
the UI thread. Shared material tuning is applied when resolved; native texture
loading is an opt-in setting. Bounded caches retain decoded resources. BC7
decoding shares its graphics device and shaders across textures.

Unwrap preparation shares projection and mask paths across levels of detail,
then schedules independent jobs for each part and side on workers assigned to
each load according to Qt's available CPU thread count. Superseded loads cancel
pending jobs.
Concurrent requests for the same model share preparation, and unchanged models
already loaded in the preview are reused. Paint edits update render parameters
while retaining uploaded mesh geometry.

The preview drawer contains individual paint-slot rows whose active mesh bindings
have non-zero triangle area. Slot discovery follows the renderer's LOD, part
selection, and paint-binding rules. Finish choices and input
color counts come from the title's paint database. Generic paint finishes appear
directly in the material menu; special materials appear in its final hover
submenu. Color buttons appear for generic finishes according to their input
count. The custom color popup shares live RGB, HSV, and hexadecimal values with
a saturation/value square and offers native manufacturer colors and project
swatches as color presets. Live changes update render parameters, with one undo entry committed
when the popup closes.
Cancellation restores the paint state captured when the popup opened.

Finish assets merge shader defaults with material parameters. Masked finishes
retain separate mask textures and linear thread tints. Finish sampling
retains the material's UV source and tiling independently of livery coverage.
Woven finishes use transformed UV coordinates, mask-directed anisotropic
reflections, and separate surface and clear-coat roughness.
Native normal mapping derives its tangent frame from transformed texture
coordinates. Woven finishes use authored tangents with handedness decoded as a
unit sign. Authored mip levels and anisotropic texture filtering retain texture detail across viewing
angles. Native material, manufacturer, and thread colors convert to display color
before preview lighting.

The canvas keeps one rasterized unwrap and wireframe overlay in memory. Layer
edits reuse that image; changes to the view, display density, overlay geometry,
or visibility settings regenerate it.

Unchanged ZIPs share temporary extraction directories through reference-counted
leases. A preview retains its most recent extraction until replacement or
destruction; directories are removed after their final lease is released.

The headless loading profiler measures CPU preparation without opening the
editor or creating an OpenGL context:

```powershell
.\build\Release\ForzaLiveryStudio.exe --profile-car-load <car.zip> --repeat 2 --output <report.json>
```

Add `--textures` to include native texture decoding. `--verify-unwrap` compares
all computed outlines, wireframes, and part bindings with the serial calculation.
Verification time is excluded from preparation timings. Unwrap stage timings
sum worker time; `unwrap_ms` measures elapsed time. GPU upload and rendering
time are outside this report.

Set `FLS_MODEL_LOAD_REPORT` to a JSON file path to record the latest GUI load
through its first presented frame. The report includes CPU preparation,
dispatch delay, GUI adoption, mesh and mask upload, and livery texture creation.
`FLS_PERF_LOG` also emits the completed load report through Qt logging.

The scene renderer rasterizes livery sections into isolated paint regions. The
car shader maps transformed UV3 through the mask atlas into those regions;
registered planar projection supplies coverage for geometry without usable UV3.
Preview coordinate normalization operates on rendering data. Mask atlases retain
their native pixel spacing when padded to the preview canvas; their region
coordinates and origins translate with the canvas center. Paint bindings,
manufacturer colors, and finish materials drive paint shading. The preview
remains a reference approximation of the game renderer.

### Source map

| Area | Responsibilities and primary files |
| --- | --- |
| `src/main.cpp` | Application startup. |
| `src/core/` scene | `layer.*`, `visual_container.h`, `contour_data.h`, `scene_codec.*`, and `project_codec.*`: hierarchy, visual data, and project serialization. |
| `src/core/` game formats | `cgroup_codec.*`, `livery_codec.*`, `vinyl_decoder.*`, `header_codec.*`, and `game_export.*`: import, native export, and metadata. |
| `src/core/` resources | `model_*`, `car_scene.*`, `swatchbin.*`, `livery_masks.*`, `paint_finish_catalog.*`, and `game_paths.*`: preview resources and decoding. |
| `src/gui/app/` | `main_window*`, settings, themes, asset lookup, application actions, and I/O coordination. |
| `src/gui/state/` | `editor_state*`, `layer_tree_model.*`, and `scene_view.h`: commands, history, selection, clipboard, and scene views. |
| `src/gui/canvas/` | `project_canvas*`, `canvas_tools.*`, `native_shape_renderer.*`, and `car_model_renderer.*`: interaction, hit testing, drawing, and preview projection. |
| `src/gui/fill/` | Contour conversion, Pen and Lining fitting, Compact Fit, polygon meshing, and region generation. See [fill engines](FILL_ENGINES.md). |
| `src/gui/widgets/` | Docks, properties, import/export dialogs, preview, image I/O, preprocessing, and shape stores. |
| `tools/` | Build/deployment scripts, asset generation, and optional inspection tools. |
| `tools/tests/` | Test suites in individual directories, shared fixtures, and benchmark scripts. |
| `third_party/` | Vendored Clipper2 polygon engine and Potrace bitmap tracer. |

### Core entry points

| Task | Entry points |
| --- | --- |
| Group import | `readCGroupPayload()`, `importCGroupFlat()`, `importCGroupNested()` |
| Physical record framing | `readStandaloneVinylRecords()`, `readVinylRecordStream()`, `mapVinylSections()` |
| Livery import | `readLiveryPayload()`, `buildLiverySections()`, `importCLivery()` |
| Motorsport import | Shared group and livery entry points accept native `data` files. |
| Group export | `buildFlatPayload()`, `buildNestedPayload()`, `exportFlatProjectFolder()`, `exportNestedProjectFolder()` |
| Livery export | `buildLiveryGyvl()`, `encodeCLiveryPayload()`, `exportCLivery()` |
| Project persistence | `projectToJson()`, `projectFromJson()`, `encodeProjectDocument()`, `decodeProjectDocument()` |
| Headers | `parseHeader()`, `buildHeader()`, `defaultDraftHeader()` |
| Preview resources | `gameMediaDir()`, `gameCarsDir()`, `PaintFinishLibrary::load()` |

## Validation and diagnostics

Enable tests after the normal configure step, then build and run the relevant
CTest selection:

```powershell
cmake -S . -B build -DFLS_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

`CMakeLists.txt` defines the available tests and fixture requirements. Test
sources and supporting scripts live in `tools/tests/<suite>/`, with shared data under
`tools/tests/fixtures/`. Local assets under the root `tests/` directory are
optional and untracked; some reference checks are
registered only when their fixtures exist at configuration time.

Codec tests compare decoded geometry, masks, shape order, section membership,
and project round trips. Native-reference checks distinguish byte equality from
scene equivalence and normalize header identity/publication metadata. State and
widget tests exercise commands, history, selection, and startup. Asset deployment
checks cover staged replacement and release catalog completeness, with isolated
working directories for source-fallback checks.

`fls_vinyl_framing_tests` checks attribute presence bits, opaque boundary state,
stored root bitmap extents, physical occupancy, and truncated records. Passing
fixture directories as arguments also checks complete byte coverage and direct
child counts for compressed group and livery payloads. Section mapping
disagreements are reported independently of physical framing failures.

Use the [fill diagnostics](FILL_ENGINES.md#diagnostics-and-replay) for contour
requests, quality measurements, and catalog comparisons. Helper builds provide
`fls_model_dump`, `fls_pack_decals`, and `fls_finish_dump`; the latter accepts
`--library <gameFolder>` or `--material <file>`. GUI appearance and in-game
acceptance require user verification.
