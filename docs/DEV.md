# Forza Livery Studio developer guide

Forza Livery Studio is a Qt6/C++20 editor for Forza vinyl groups and liveries.
It imports native game assets, edits a hierarchical scene, saves `.3so` projects,
and exports game containers. Its OpenGL car preview uses decoded game models,
materials, and livery masks.

## Documentation

| Reference | Contents |
| --- | --- |
| [Code style](CODESTYLE.md) | Conventions for new and edited code. |
| [User manual](MANUAL.md) | Editor workflows and tool controls. |
| [Fill engines](FILL_ENGINES.md) | Contour conversion, fitting, region generation, and diagnostics. |
| [Contour meshes](MESH.md) | Shared contour topology, grouping, and persistence. |
| [Shape configuration](COMPACT_FIT_CONFIG.md) | Compact Fit and Lining catalogs, validation, and deployment. |
| [Group encoding](CGROUP.md) | Group records, transforms, shapes, masks, and game dialects. |
| [Livery encoding](CLIVERY.md) | Sections, paint descriptors, compression, and export profiles. |
| [Header encoding](HEADER.md) | Header sidecars and metadata. |
| [WGS replacement](WGS.md) | Save containers, replacement transactions, and recovery. |

## Build and run

Requirements are CMake 3.24 or newer, a C++20 compiler, Qt6
Core/Gui/Widgets/OpenGL/OpenGLWidgets/Svg, and zlib. Xbox 360 encryption uses
Windows CNG or OpenSSL Crypto on other platforms. Clipper2, Potrace, and the
libmspack LZX decoder are vendored in `third_party/`. CUDA fill acceleration is
optional; CPU implementations remain available.

### Windows

Install the Visual Studio C++ build tools and Qt base, image-format, SVG, and
zlib packages for vcpkg's `x64-windows` triplet. Set `VCPKG_ROOT` to the vcpkg
installation; the scripts also check `C:\vcpkg` and `C:\vcpkg\vcpkg`.

From the repository root:

```powershell
.\tools\build.ps1 -DisablePrivacyPolicy
.\tools\run.ps1
```

The build script configures CMake and builds Release into `build/Release`.
The executable is `build/Release/ForzaLiveryStudio.exe`. Close the editor before
building so the executable can be replaced.

`-DisablePrivacyPolicy` permits importing locked game content for development.
Omit it for the default public configuration, which rejects locked content.
Livery export validates per-section shape counts against the supported limits.

### Linux

Install Qt6 development packages with SVG support, zlib, OpenSSL, CMake, and
a C++20 compiler, then run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/ForzaLiveryStudio
```

### Configuration and assets

`CMakeLists.txt` defines build options and targets. `FLS_ENABLE_CUDA` defaults to
`ON` and detects an available CUDA toolchain. `FLS_PRIVACY_POLICY` defaults to
`ON`; tests and helper tools default to `OFF`.

The Windows configure script resets tests, helper tools, and experimental menus
to the public configuration. For a custom build, run `tools/configure.ps1` first,
apply options with `cmake -S . -B build`, then build directly with CMake.

The build validates and deploys `assets/` beside the executable. Native geometry
and shape names live in `assets/vector/`, raster decals in `assets/raster/`, and
UI resources in `assets/icons/` and `assets/themes.json`. Car metadata and paint
catalogs live in `assets/cars/`. Compact Fit and Lining use
`assets/compact_fit_shapes.json` and `assets/lining_shapes.json`; keep these
catalogs consistent with the deployed geometry. See
[configuration deployment](COMPACT_FIT_CONFIG.md#deployment).

## Architecture

CMake builds `fls_core` for codecs and scene data, `fls_profile_compute` for
numeric fitting, and `fls_editor_ui` for the editor. `ForzaLiveryStudio` supplies
the application entry point and links the UI library.

### Scene and project state

`Project::root` is the canonical `fls::scene::Group`. Groups own child layers,
and local transforms compose through the hierarchy into world transforms.
Visual content refers to native shape IDs or stores raster imagery. Guides and
editable Pen and Lining figures are retained as project content.

`EditorState` owns the project and its indexes. The command dispatcher validates
and commits edits, coordinates selection and undo history, and updates the
canvas, layer model, and preview. Reparenting preserves world placement.
Authored visibility is independent of active-section isolation. Interactive
transforms and snapping operate on cached scene geometry.

The `.3so` format stores gzip-compressed scene-tree JSON, including guides,
figures, meshes, swatches, retained header data, and livery paint state. Autosave
updates saved projects; per-instance recovery documents also preserve unsaved
work.

### Import and export

Game codecs decode native payloads into the shared scene representation.
`VinylTreeDecoder` owns the common group and section traversal, while format
profiles supply record dialects and semantics. `vinyl_framing` handles physical
records and boundaries separately from section mapping and editable geometry.
Compression is resolved before scene decoding.

The [Xbox 360 codecs](XBOX360.md) handle authenticated file envelopes and
big-endian vinyl records. They feed the same vinyl tree and scene importer.

`vinyl_encoding` converts scene placement into native records. Export profiles
select framing, headers, paint layout, masks, and compression. Grouped export
retains nested groups and composes scene transforms. Livery export derives
section occupancy from visible content. Figures and guides remain editor
content.

Destination formats are documented in
[native export profiles](CLIVERY.md#native-export-targets). WGS replacement uses
the transactional pipeline described in [WGS.md](WGS.md).

### Contours and fill

Pen, Lining, and image-derived regions feed the fill engines in `src/gui/fill/`.
They convert contours into native shapes using analytical or catalog-based
fitting. Saved contours and shared meshes retain editable topology in the
project. Algorithm contracts and diagnostics belong in
[FILL_ENGINES.md](FILL_ENGINES.md), mesh rules in [MESH.md](MESH.md), and catalog
configuration in [COMPACT_FIT_CONFIG.md](COMPACT_FIT_CONFIG.md).

### Game resources and preview

Per-game settings accept an installation root or its `media` directory.
`game_paths.*` resolves car folders and shared material and texture archives.
Automatic model loading prefers the imported livery's game, then searches other
configured games. The car registry provides availability, model names, and
wheel sizing for each title.

FH3 PC asset lookup recognizes the obfuscated media layout. It translates file
and ZIP entry names at runtime using the
[FH3 filename mapping](https://github.com/oxmaulmike2581/FH3DecryptNames/blob/master/FH3DecryptNames/Program.cs#L261).
Archive extraction writes decoded names into the preview's temporary directory.

Core decoders read `.modelbin` geometry and materials, assemble `.carbin` parts,
and decode `.swatchbin` textures and livery masks from game archives. Model
preparation, material resolution, and optional texture decoding run outside the
UI thread. Resource caches and shared preparation avoid repeated decoding.

Model imports index the car package once and resolve references by path without
regard to case. If directories have moved, the longest unique matching path
suffix identifies the resource. Ambiguous references remain unresolved.
Material parameters resolve from shader defaults through the material resource,
model instance and scene override. The preview maps material textures through
their authored coordinate channels and transforms. The headless car-load
profile reports resource resolution and livery mask coverage.

The scene renderer rasterizes livery sections into paint regions. The car shader
maps model UVs through the livery mask atlas into those regions, with planar
projection providing fallback coverage. Part selection drives preview geometry,
paint bindings, and the canvas unwrap. Paint finishes and manufacturer colors
come from the paint catalogs; paint edits participate in project persistence,
undo, and livery export. The preview approximates the game renderer.

### Source map

| Location | Responsibility |
| --- | --- |
| `src/main.cpp` | Application startup. |
| `src/core/` | Scene data, project serialization, game codecs, headers, WGS transactions, and resource decoding. |
| `src/gui/app/` | Main window, settings, themes, runtime asset lookup, and application I/O. |
| `src/gui/state/` | Commands, history, selection, clipboard, scene views, and the layer model. |
| `src/gui/canvas/` | Interaction, hit testing, snapping, drawing, and car rendering. |
| `src/gui/fill/` | Contour conversion, fitting, meshing, and region generation. |
| `src/gui/widgets/` | Docks, properties, dialogs, preview controls, image preprocessing, and shape stores. |
| `tools/` | Build and deployment scripts, asset generation, and inspection tools. |
| `tools/tests/` | Test suites, shared fixtures, and benchmark scripts. |
| `third_party/` | Clipper2, Potrace, and libmspack. |
