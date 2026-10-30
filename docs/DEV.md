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
| `assets/cars/` | Car registry and preview metadata. |
| `assets/compact_fit_shapes.json` | Compact Fit catalog and task membership. |
| `assets/lining_shapes.json` | Lining catalog and normalized centerlines. |

Catalog changes are loaded per operation; see [configuration deployment](COMPACT_FIT_CONFIG.md#deployment).

## Architecture

CMake builds `fls_core` for codecs and scene data, `fls_profile_compute` for
numeric fitting, and `fls_editor_ui` for the editor. `ForzaLiveryStudio` supplies
the application entry point and links the UI library.

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

Move and resize snapping cache scene outlines at drag start. `geometry_snap.*`
indexes stationary contour segments and geometry alignment coordinates. Drag
corrections are measured in canvas pixels and solved within the active movement
or scale constraints, using each selected leaf's transformed geometry. The solver
returns active alignment and contact indicators with the correction; the canvas
draws this feedback during the drag. One persisted Automatic snapping option
controls both shape and guideline snapping.

The `.3so` container is gzip-compressed scene-tree JSON, currently document
version 4. Project serialization includes guides, figures, swatches, retained
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

The configured game folder accepts either an installation root or its `media`
directory. `game_paths.*` derives `media/Cars`, shared material archives under
`Cars/_library/Materials.zip`, and textures under `Cars/_library/Textures.zip`.
Painttype materials use the `_fmnext/usercustomizable/` archive prefix.

Core decoders read `.modelbin` geometry and materials, assemble `.carbin` parts,
extract zipped car folders, and decode `.swatchbin` textures and `LiveryMasks/`.
Model preparation, material resolution, and enabled texture decoding run outside
the UI thread. Shared material tuning is applied when resolved; native texture
loading is an opt-in setting. Bounded caches retain decoded resources.

The scene renderer rasterizes livery sections into isolated paint regions. The
car shader maps transformed UV3 through the mask atlas into those regions;
registered planar projection supplies coverage for geometry without usable UV3.
Preview coordinate normalization operates on rendering data. Paint bindings,
manufacturer colors, and finish materials drive paint shading. The preview
remains a reference approximation of the game renderer.

### Source map

| Area | Responsibilities and primary files |
| --- | --- |
| `src/main.cpp` | Application startup. |
| `src/core/` scene | `layer.*`, `visual_container.h`, `contour_data.h`, `scene_codec.*`, and `project_codec.*`: hierarchy, visual data, and project serialization. |
| `src/core/` game formats | `cgroup_codec.*`, `livery_codec.*`, `fm_codec.*`, `vinyl_decoder.*`, `header_codec.*`, and `game_export.*`: import, native export, and metadata. |
| `src/core/` resources | `model_*`, `car_scene.*`, `swatchbin.*`, `livery_masks.*`, `paint_finish_catalog.*`, and `game_paths.*`: preview resources and decoding. |
| `src/gui/app/` | `main_window*`, settings, themes, asset lookup, application actions, and I/O coordination. |
| `src/gui/state/` | `editor_state*`, `layer_tree_model.*`, and `scene_view.h`: commands, history, selection, clipboard, and scene views. |
| `src/gui/canvas/` | `project_canvas*`, `canvas_tools.*`, `native_shape_renderer.*`, and `car_model_renderer.*`: interaction, hit testing, drawing, and preview projection. |
| `src/gui/fill/` | Contour conversion, Pen and Lining fitting, Compact Fit, polygon meshing, and region generation. See [fill engines](FILL_ENGINES.md). |
| `src/gui/widgets/` | Docks, properties, import/export dialogs, preview, image I/O, preprocessing, and shape stores. |
| `tools/` | Build/deployment scripts, headless tests, asset generation, and optional inspection tools. |
| `third_party/` | Vendored Clipper2 polygon engine and Potrace bitmap tracer. |

### Core entry points

| Task | Entry points |
| --- | --- |
| Group import | `readCGroupPayload()`, `importCGroupFlat()`, `importCGroupNested()` |
| Physical record framing | `readStandaloneVinylRecords()`, `readVinylRecordStream()`, `mapVinylSections()` |
| Livery import | `readLiveryPayload()`, `buildLiverySections()`, `importCLivery()` |
| Motorsport import | `decodeFM2023RawGroup()`, `readFM2023LiveryPayload()`, `decodeFM2023LiverySections()`, `importFM2023Asset()` |
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

`CMakeLists.txt` defines the available tests and fixture requirements. Local
assets under `tests/` are optional and untracked; some reference checks are
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
