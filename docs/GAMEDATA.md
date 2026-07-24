# Game Media Layout

Where the resources the editor reads live inside a Forza game install, and how the
editor locates them. The editor is pointed at a single **game folder** (the install
root, e.g. `…/steamapps/common/ForzaHorizon6`, or its `media` directory); every path
below is derived from it by `src/core/game_paths.*`.

Most game data is packed in `.zip` archives. The editor reads individual entries in
memory with `readZipEntry`/`readZipEntries` (`src/core/zip_extract.*`); car folders are
extracted to a temp directory before loading. Archive entry paths are lower-case and use
forward slashes.

## Install root

```
<game>/
  media/                  all graphical/data assets (the "media folder")
```

`gameMediaDir()` accepts either the root or the `media` directory: if the folder
directly contains `Cars`, it is treated as `media`; otherwise `media` is appended.

## media/ (top level)

Only the parts the editor uses are listed; the folder also holds audio, world,
cinematics, particles, UI, and other runtime data.

```
media/
  Cars/                   per-car archives + the shared _library
  Livery/                 shared decal/vinyl artwork archives
  Stripped/
    StringTables/         localized string tables (finish names live here)
    gs/swatchbins.zip     shared swatch textures
```

## media/Cars/

```
Cars/
  <CODE>.zip              one archive per car, named by model code (e.g. ALF_4C_14.zip)
  _library/               assets shared across all cars
```

`gameCarsDir()` → `media/Cars`. A livery's target car id maps (via the editor's car
registry) to a model code; `findCarModelPath()` searches `Cars` recursively for
`<code>.zip` (or `.carbin`/`.modelbin`).

### A per-car `<CODE>.zip`

```
<CODE>.zip
  <CODE>.carbin           the car definition (part list + per-part transforms)
  Scene/**/*.modelbin     geometry/material bundles for each referenced part
  Manifest.xml            per-model material + texture binding map (see below)
  LiveryMasks/            Masks.xml + body-side coverage swatchbins
  Locators.xml            named locator transforms (wheel/bumper landmarks)
  ManufacturerColors.bin  factory paint presets
  LightPresets.bin        light rig
  <CODE>.avpins           physics/attachment pins
  carscene_<CODE>_build_report.html   asset build report
```

`Manifest.xml` is the authoritative per-part visual tuning: under
`<NonUpgradeablePart>` / `<UpgradeablePart>` groups, each `<Model path=…modelbin>`
lists its `<Material path=…materialbin>` set and `<Swatchbin path=…swatchbin>` set,
all pointing into the shared `_library`. The editor does **not** parse it — each
exterior mesh already carries its materialbin path in its own modelbin (see the code
map for `resolveExteriorMaterials`) — but it is the ground truth for which library
materials a part uses.

### media/Cars/_library/

```
_library/
  Materials.zip           shared materials, including the customizable paint finishes
  Textures.zip            shared textures, including paint-finish swatchbins
  Shaders.zip             compiled shaders
  scene/wheels/*.zip      shared wheel models
  scene/tires/tire_b.zip  shared tire model template (tireL_b/tireR_b.modelbin)
```

`Materials.zip` also holds the shared non-paint material library the exterior/wheel
parts bind to, e.g. `_fmnext/metal/*` (aluminum, steel, chrome, gold…),
`_fmnext/rubber/*`, `_fmnext/tires/*`, `_fmnext/wheel/wheelpaint*`,
`_fmnext/specialcase/{blackhole,wheelblur}`, and `wheelmaterials/*`.

### Wheels and tires

Wheel and tire modelbins are special: they reference materials only by **slot name**
(wheels: `rim`, `rim2`, `hub`, `inner_rim`, `lip`, `detail`, `detail2`, `valve_cap`,
`black`, `blur_lip`; tires: `tread`, `scaling_text`) and carry **no materialbin path**
in the modelbin (exterior panels do). The binding is a fixed shared-library
convention — rims → `_fmnext/wheel/wheelpaint`, `black` → `_fmnext/specialcase/blackhole`,
metal slots → `wheelmaterials/aluminum_machined_satin`, tread → `_fmnext/tires/tires_pg`.
Rims are paintable (their colour/finish come from the livery paint state, keyed by a
per-channel wheel paint hash). The car's real per-corner tires are not shipped as loose
meshes here; the shared `tire_b` template is scaled to fit each rim.

## Paint finishes (the customizable paint materials)

`gamePaintMaterialsArchive()` → `media/Cars/_library/Materials.zip`. The customizable
paint materials the player applies to a livery live under two parallel prefixes inside
that archive:

```
Materials.zip
  _fmnext/usercustomizable/<name>.materialbin   full set (57), the editor's source
  painttypes/<name>.materialbin                 a partial/legacy set (69)
```

The editor reads `_fmnext/usercustomizable/` because it contains the complete set
(including `candypaint` and the `metallicpaint*` variants that `painttypes/` omits).
Each `.materialbin` is a Grub bundle of shader parameters (base colour, gloss, F0
reflectance → metallic, flake amount, and `.swatchbin` texture references) — decoded by
`decodeMaterialBundle` in `src/core/model_material.*`. The materials themselves carry no
finish-code number.

### Paint-finish textures

`gamePaintTexturesArchive()` → `media/Cars/_library/Textures.zip`. The pattern/flake
textures referenced by the paint materials (carbon-fibre weave, camo, brushed-metal
normals, tint maps) are `.swatchbin` files under:

```
Textures.zip
  userpaint/swatches/<name>.swatchbin
  _fmnext/userpaint/swatches/<name>.swatchbin
  genericmaps/swatches/<name>.swatchbin
```

They are referenced from a material parameter by GUID-suffixed leaf name (e.g.
`damascus_001_nrml_f302c9c1-….swatchbin`). Decoded by `src/core/swatchbin.*`.

## The finish enumeration

A livery's paint state stores, per paintable region, a `finish` code (u32) — a global
paint-material enumeration, not a hash and not present in the `.materialbin` files. The
authoritative code→name list is a string table:

```
media/Stripped/StringTables/<LANG>.zip
  List_LiveryMaterials.str
```

`<LANG>` is a locale code (`EN`, `DE`, `FR`, …). The `.str` is a hashed string table:
a header, then two `(hash, offset)` record tables and two string pools — one for keys,
one for localized values. Keys are `IDS_DisplayName_<code>` (plus
`IDS_PrimaryColorDisplayName_<code>` / `IDS_SecondaryColorDisplayName_<code>`); joining
the two tables by shared hash yields `finish code → display name`. Display names map to
the `usercustomizable` material stems (mostly by normalization; a few rename, e.g.
"Blob Camo" → `camoclassic*`, "Realistic Camo Woodland" → `camorealforest*`, "Steel
Damascus" → `damascus`).

The resolved table is encoded in `src/core/paint_finish_catalog.*` (`paintFinishTable()`):
56 finishes covering solid/semigloss/matte/metallic paint, two-tone, metals
(aluminum/brass/copper/steel/chrome/gold/zinc/damascus/galvanized), carbon fibre and
kevlar, camo families (classic/digital/realistic), spyshot, wood, prismacolor, and candy.
`metalflakelarge` exists in the materials but is unused by the finish list.

## media/Livery/

Shared livery artwork used by all cars (referenced by decal/vinyl id), not per-car:

```
Livery/
  Decals.zip / DecalsHiRes.zip   raster decal artwork
  Vinyls.zip                     vector vinyl shapes
```
