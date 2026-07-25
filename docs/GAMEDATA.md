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

**Sizing.** The wheel and tire models are normalised identically for every car: the rim outer
and the tire inner bead mate at a canonical **0.1397 m** radius — a 0.2794 m diameter, exactly
11 in — and the axial X (wheel width) is normalised to 0..1. Measured across all 651 per-car
stock wheels and all 527 shared rim styles.

There is **no size anywhere in the scene data**. The carbin wheel part transform is a pure
rotation+translation (unit scale) whose origin is the wheel-centre plane (matching
`carLocator_wheel{LF,RF,LR,RR}`), the part's stored AABB is the normalised mesh AABB, and
`physicsdefinition.bin` holds only mass, inertia tensors and collision point clouds. Locator
and skeleton hub heights are authoring poses, not ride height.

The real dimensions come from `Data_Car` in the game database (see *The car database* below),
stated the way a sidewall states them — section width in mm, aspect ratio in percent, rim
diameter in inches — and are kept in the editor asset `assets/cars/wheel_sizes.json`, keyed by
model code with a `_default` fallback:

```json
"<MODELCODE>": { "front": { "width": 205, "aspect": 40, "rim": 18 },
                 "rear":  { "width": 235, "aspect": 35, "rim": 19 } }
```

`car_preview_widget` looks it up per car and passes a `WheelSizing` into `loadCarBin` and
`appendApproximateTires`. `bakeWheelTransform` scales the rim radially by
`(rimDiameter/2) / 0.1397` and axially to the section width; the tire is built at radius
`rimDiameter/2 + width·aspect` with the same width. Regenerate the asset with
`tools/gen_wheel_sizes.py`.

**Axial anchoring.** The wheel model's axial X runs from the outboard face at 0 to the
inboard rim edge at 1, and the plane the part transform positions is the wheel's mid-width.
Wheel models also carry a local `spindle` bone at a normalised X of their own, but it is not
the anchor: using it sinks the wheel into the arch.

**Where the corner sits.** The carbin's wheel placement is an authoring pose, not the render
pose. Its **Z is trustworthy** — front-to-rear spacing matches `Data_CarBody.ModelWheelbase`
to a median of 0.9 mm — but **X and Y are not**: some cars carry round placeholders (one has
all four wheels at exactly `X = ±1.0`, others sit at `Y = 0`). Those two axes come from the
database instead:

```
wheel centre |X| = ModelTrackOuter/2 - tireWidth/2
wheel centre  Y  = tireRadius - ModelStockRideHeight
```

Measured against each car's collision hull, this puts the outer tyre face a median 22 mm
inside the bodywork, where the carbin's own X leaves it a median 88 mm outside.

A corner is a rigid assembly — wheel, rotor, caliper, suspension arm — so the correction is
computed once from the wheel and applied to every part naming that corner's bone. Otherwise
the brakes stay behind while the wheel moves.

**The corner is a chain.** The suspension arm does not float at the hub: its outboard end
butts against the brake rotor's **inboard face**, which the rotor in turn shares an axis with
the rim. Across the cars that place their arm explicitly, that joint closes to a median of
2 mm. A bone-placed arm is therefore slid along the axle until its outboard extent meets that
face, rather than being left centred on the hub where it passes straight through the disc.
Both extents come from the bundles' own bounding boxes, so no geometry has to be decoded to
find them. A part needing to travel more than `kMaxHubReach` to reach the hub spans more than
one corner — a beam axle, not an arm — and is left where the corner correction put it.

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

## The car database

`media/Stripped/gamedbRC.slt` is a SQLite database (205 tables) holding everything the
`.str` tables only name: per-car specs, upgrade part tables, class and bucket assignments.
It is the authority for numbers that appear nowhere in the scene data.

`Data_Car` has one row per car (651, matching the `Cars/*.zip` set exactly) and 151 columns.
`MediaName` is the car folder code, which is what joins it to everything else on disk. The
columns the editor cares about:

| Column | Meaning |
|--------|---------|
| `MediaName` | `Cars/<CODE>.zip` folder code |
| `FrontTireWidthMM` / `RearTireWidthMM` | Tire section width, millimetres |
| `FrontTireAspect` / `RearTireAspect` | Sidewall height as a percentage of width |
| `FrontWheelDiameterIN` / `RearWheelDiameterIN` | Rim diameter, inches |
| `StockWheelID` | Stock rim style |
| `FrontStockRideHeight` / `RearStockRideHeight` | Stock ride height |

Upgrades that change these live in `List_UpgradeRimSize{Front,Rear}` and
`List_UpgradeCarBodyTireWidth{Front,Rear}`.

### The container

The file is not readable as-is: it is wrapped in an Arxan TransformIT (white-box AES)
container, the same one used for `media/sfsdata` and `media/zipmanifest.xml`. Layout:

```
u8[16]  IV
u32     padding_size          (unencrypted)
u8[16]  header MAC            (unencrypted)
repeat: u8[0x200 | 0x20000] data block (encrypted, CBC)
        u8[16]               block MAC (encrypted)
```

Plaintext size is `blocks · blockSize − padding_size`. The cipher is table-driven — 17
rounds of `keys[4]` + `tables[16][256]` — and those tables are stored obfuscated, so they
cannot be lifted out of the executable statically; a full sweep of `forzahorizon6.exe`
against a known-plaintext oracle finds nothing. Decryption therefore needs an external
tool. The editor does not read the database at runtime: values are extracted once and
checked in as assets.

---

## String table hashes (from strData.md)

The 2,838 unique hashes in `docs/strData.md` are internal `.str` file lookup keys.
They exist **exclusively** inside `.str` files, replicated identically across all 20+
locale zips (`EN.zip`, `DE.zip`, `FR.zip`, `JP.zip`, etc.) — no other game media file
contains them.

### Value-string cross-reference (key+value search)

Searched the actual string **values** (not hashes) from strData.md inside all game media.
Results by table:

| Table | Strings searched | Found in game media | Location |
|-------|-----------------|--------------------:|----------|
| `List_LiveryMaterials` | Gloss, Matte, Semigloss, Chrome, Metallic | ✓ | `.materialbin` in `Cars/_library/Materials.zip` |
| `List_LiveryMaterials` | Carbon Fiber Matte/Polished, Candy Paint, etc. | ✗ | — |
| `Livery_Decals` | Brembo, Recaro, Sparco, HKS, Pirelli, etc. | ✗ | — |
| `Data_Car` | Aventador SVJ, Skyline 2000GT-R, etc. | ✗ | — |
| `UpgradeTypes` | Engine Swap, Camshaft, Tire Compound, etc. | ✗ | — |
| `WheelCategories` | Stock/Sport/Multi Piece Rim Style, etc. | ✗ | — |

**Key finding:** Paint finish names from `List_LiveryMaterials.str` appear as literal
strings inside `.materialbin` files in `Cars/_library/Materials.zip`:

```
Cars/_library/Materials.zip!painttypes/aluminumsemigloss.materialbin  → "Gloss"
Cars/_library/Materials.zip!painttypes/brasssemigloss.materialbin     → "Gloss"
Cars/_library/Materials.zip!painttypes/normalpaintmatte.materialbin   → "Matte"
Cars/_library/Materials.zip!painttypes/normalpaintsemigloss.materialbin → "Gloss"
Cars/_library/Materials.zip!painttypes/camoclassicdesertmatte.materialbin → "Matte"
Cars/_library/Materials.zip!painttypes/steelsemigloss.materialbin     → "Gloss"
Cars/_library/Materials.zip!painttypes/twotonematte.materialbin       → "Matte"
Cars/_library/Materials.zip!painttypes/wooddarkmatte.materialbin      → "Matte"
```

This confirms `List_LiveryMaterials.str` values are used as material type identifiers
inside the engine's material binary format. No other `.str` table values were found
outside `.str` files — car names, upgrade labels, decal brands, and wheel categories
are purely UI/display strings resolved at runtime.

### How the hashes work

Each `.str` file contains two parallel tables (keys and values) linked by shared hash.
The hash is derived from the key string (e.g. `IDS_DisplayName_1`) and used for O(1)
lookup. The game engine reads `.str` files at runtime and builds an in-memory
hash→string table; no other game data file references strings by hash.

### Locale replication

Every hash appears in all locale zip archives. For example, `0x40E1C739`
(`IDS_DisplayName_1`) was found in all of:

```
EN.zip, DE.zip, FR.zip, JP.zip, BR.zip, CHS.zip, CHT.zip, CZ.zip,
DK.zip, EL.zip, ES.zip, FI.zip, GB.zip, HU.zip, IT.zip, KO.zip,
MX.zip, NL.zip, NO.zip, PL.zip, PT.zip, RU.zip, SV.zip, TR.zip
```

This is expected — each locale has its own translated values for the same keys.

### Source code references

Only one `.str` file is explicitly mentioned in source:

| File | Reference | Location |
|------|-----------|----------|
| `List_LiveryMaterials.str` | Comment only | `src/core/paint_finish_catalog.h:22` |

All other `.str` files are not referenced in source — they exist purely as game data
consumed at runtime.

### Material hashes (in source, not in strData.md)

The editor does use hardcoded hashes, but these are **material parameter hashes**
(64-bit FNV-1a), not string table hashes (32-bit). They live in
`src/core/material_hashes.h`:

| Constant | Hash | Purpose |
|----------|------|---------|
| `kBodyPaint` | `0xF7DBE8A7C839A675` | Body paint material binding |
| `kHoodPaint` | `0x6AC1E9D87FE5D953` | Hood paint binding |
| `kMirrorPaint` | `0x1E5FF0F50C741122` | Mirror paint binding |
| `kSpoilerPaint` | `0xCD48110253EE319A` | Spoiler/wing paint binding |
| `kWindowGlass` | `0x9582FD1BA2FFF9A4` | Window glass binding |
| `kBrakeCaliper` | `0xA5495E0A43DF55B9` | Brake caliper binding |

Additional arrays: `kLiveryMaterials` (7 entries), `kLiveryPanels` (11 entries),
`kFrontWheelPaint` / `kRearWheelPaint` (5 each), plus parameter hashes for base
color, emissive, opacity, gloss, metallic, flake amount, and texture paths.

These material hashes are used by `project_codec.cpp` to identify livery paint slots
when reading/writing `.livery` project files — they are unrelated to the `.str` string
table hashes.

---

## Rims (wheels)

### Shared rim models

Located in `Cars/_library/scene/wheels/` — one `.zip` per aftermarket rim style,
each containing a `.modelbin` mesh + AO texture:

```
Cars/_library/scene/wheels/BBS_CIR.zip
  BBS_CIR_WheelLF.modelbin           (3.17 MB)
  Textures/AO/BBS_CIR_wheelLF_AO.swatchbin
```

**446 rim styles** from manufacturers: ADV, AME, ASA, BBS, BRA, COM, CRA, DUB, DYM,
ENK, F52, FIK, GRA, HAL, HOL, HRE, IFO, KMC, KON, KOS, LEX, MIC, MOD, MOM, MOT,
OET, OZ, RAC, ROT, RO, RSW, SPE, TEA, TEN, TIT, TSW, VOL, VOR, WED, WEL, WOR.

Each car also has its **stock wheel** as a `.modelbin` inside its own zip:
```
Cars/BMW_M3E92_08.zip → Scene/_library/Scene/Wheels/BMW_M3E92_08_wheelLF.modelbin
```

### Motion-blur slots

Wheel models carry a second set of meshes on `blur_lip` / `blur_rim` slots (1,000 meshes
across 609 cars) bound to `_fmnext/specialcase/wheelblur`. They are full-rim-radius discs
sitting in a thin slab at the outboard face — the geometry the game swaps in for a spinning
wheel — and they cover the opening the spokes and brake are seen through, so a static render
drops them. The `black` slot is different: it is a tube running the length of the barrel
(`_fmnext/specialcase/blackhole`), and stays.

### Wheel paint channels

Resolved from `_library/Materials.zip` via `car_preview_widget.cpp:319-339`:

| Slot name | Material path |
|-----------|---------------|
| `rim` / `rim2` | `_fmnext/wheel/wheelpaint.materialbin` |
| `black` | `_fmnext/specialcase/blackhole.materialbin` |
| `lip`, `hub`, `lug`, `inner_rim`, `detail`, `detail2`, `valve_cap` | `wheelmaterials/aluminum_machined_satin.materialbin` |

Additional wheel materials in `Materials.zip`: `wheelmaterials/chrome_machined_satin.materialbin`,
`wheelmaterials/metalicpaint_lightsilver.materialbin`, `wheelmaterials/paint_matteblack.materialbin`.

### Per-car rim sizes

Stored in `assets/cars/wheel_sizes.json` — keyed by model code, per axle: tire section
width (mm), aspect ratio (%), rim diameter (in). Generated from `Data_Car` in the game
database by `tools/gen_wheel_sizes.py`; falls back to `_default` if no entry exists.

---

## Tires

### Shared tire archives

Located in `Cars/_library/scene/tires/` — **197 tire `.zip` files**, each containing
left/right `.modelbin` meshes. Key tire types from filenames:

| Category | Examples |
|----------|----------|
| Road/Street | `tire_road.zip`, `tire_street.zip`, `tire_sport.zip` |
| Performance | `tire_semi_slick.zip`, `tire_slick.zip` |
| Wet | `tire_WET_b.zip`, `tire_WET_c.zip` |
| Off-road | `tire_offRoad.zip`, `tire_offRoadMT.zip` |
| Rally | `tire_rally.zip`, `tire_rallyow.zip` |
| Snow | `tire_snow.zip` |
| Drag | `tire_drag.zip` |
| F1 | `tire_F1.zip`, `tire_slick_F1.zip` |
| Vintage | `tire_vintage.zip`, `tire_vintageRace.zip` |
| Branded | `tire_c_Michelin.zip`, `tire_c_Pirelli_a.zip`, `tire_c_Dunlop.zip` |
| Special | `tire_whiteWall.zip`, `tire_GYWhite.zip`, `tire_DUALLY_c.zip` |

Each zip contains: `tireL_b.modelbin` (left) + `tireR_b.modelbin` (right).

### Tire materials

From `Cars/_library/Materials.zip`:
- `genericassetmaterials/tiretread_*.materialbin` — tread variants (a, b, c, f, g, h, i, offroad, rally, snow, vintage)
- `genericassetmaterials/tiresidewall_*.materialbin` — sidewall variants
- `_fmnext/tires/tires_pg.materialbin` — PG tire material
- `_fmnext/tires/tires_pg_sidewall.materialbin` — PG sidewall

### Approximate tire generation

The editor stands a scaled `tire_b` template in for the real tire. Its shape is the
template's; its dimensions come from the axle's tire spec — radius
`rimDiameter/2 + width·aspect`, width the section width — so only the tread pattern is
approximate. The rim mesh bounds are still measured, to find the lip the tire seats
against. `kCanonRimRadial = 0.1397f` is the normalised radius both models mate at.

**The sidewall is stretched, not scaled.** The template has a fixed bead-to-tread
proportion (0.1401 / 0.2248 = 0.623), which only matches a real tire at one aspect ratio.
Scaling it as a whole therefore lands the bead wherever the tread scale puts it instead of
on the rim: 845 of 1,302 axles end up off by more than 20 mm, opening a gap of +76 mm on
tall sidewalls (315/80 R17) and burying the rim by −35 mm on low-profile ones. Each vertex
is instead remapped by radius,

```
r' = rimRadius + (r - beadRadius) · (tireRadius - rimRadius) / (treadRadius - beadRadius)
```

so the bead seats on the rim and the tread reaches the tire radius exactly, whatever the
aspect ratio. Normals are transformed anisotropically to match — scaled by the sidewall
factor along the radius and by the local hoop factor around it.

---

## Upgrade parts

### How upgrades are stored

Upgrade parts are embedded **inside each car's `.zip` archive** as variant `.modelbin`
files. The `.carbin` scene manifest defines the complete upgrade hierarchy.

**Key insight: Livery files (`C_livery`) contain NO upgrade data.** They only store vinyl
shapes and paint materials for body panels. Upgrades are determined entirely by the
`.carbin` scene manifest at render time.

### Part placement and bones

A `CarRenderModel` carries both a transform and the name of a bone. The two are alternatives,
not a composition:

- **Transform with a translation** — the world placement, used as-is. The bone names the rig
  parent that animates it; applying it as well double-transforms the part.
- **Transform with no translation, plus a bone** — placed entirely by that bone. The name
  belongs to the *car* skeleton (`Scene/<CODE>_skeleton.modelbin`, path in the carbin header),
  not to the part's own bundle. This is how the suspension arms are placed on 425 of 624 cars
  (1,672 parts, all `controlArm_*`); resolving it against the part's local skeleton finds
  nothing and collapses them onto the car origin.

The car skeleton is a **bind pose**, not the render pose — where a car states both, they
differ by up to 0.34 m — and its corner bones are largely a shared template: 302 of 482 cars
repeat the same `controlArm_LF` value. It therefore supplies orientation only; a bone-placed
corner part takes its position from the corner (see *Where the corner sits*).

Corner membership comes from the bone-name suffix (`spindleLF`, `hubLR`, `controlArm_RF`),
which is consistent with the placement on all but 2 of 2,493 wheel instances.

### .carbin binary format (verified from ForzaTechStudio reference)

The `.carbin` format uses a tree structure:

```
Scene (header: version, GUID, ordinal, mediaName, skeletonPath, LODFlags)
├── NonUpgradableParts[]    (fixed geometry — body, interior, windows)
│   └── PartEntry (type byte + Part)
│       └── Part (version, type, CarRenderModel[], AABB)
└── UpgradableParts[]       (geometry that changes with upgrade level)
    └── UpgradablePart (version, type, Upgrade[], SharedCarModel[])
        ├── Upgrade (version, level, isStock, id, carBodyId, parentIsStock, [models], AABB)
        └── SharedCarModel (upgradeIds[], CarRenderModel)
```

**Version history:**
- FH4/FH5: Scene v6, Model v16-18
- FH6: Scene v7, Model v21
- FM2023: Scene v10-11, Model v21+

**UpgradablePart binary layout:**
```
u16    version
u32    type (CCarParts enum)
u32    upgradeCount → Upgrade[]
u32    sharedModelCount → SharedCarModel[]   (version >= 3 only)
```

**Upgrade binary layout:**
```
u16    version
u8     level          (0 = stock)
bool   isStock
i32    id             (unique upgrade ID, referenced by SharedCarModel)
i32    carBodyId      (links to car body database entry)
bool   parentIsStock
CarRenderModel[]  (version < 3 only — inline models)
AABB               (version >= 2 — bounding box)
```

**SharedCarModel binary layout (v3+, FH5/FH6):**
```
u32    upgradeCount → i32[] upgradeIds   (which upgrade levels this model appears with)
CarRenderModel
```

### Complete CCarParts enum (46 values)

| Value | Name | Description |
|-------|------|-------------|
| 0 | Engine | Engine geometry |
| 1 | Drivetrain | Drivetrain components |
| 2 | CarBody | Primary car body shell |
| 3 | Motor | Motor parts (EV) |
| 4 | Brakes | Brake geometry |
| 5 | SpringDamper | Suspension spring/damper |
| 6 | AntiSwayFront | Front anti-roll bar |
| 7 | AntiSwayRear | Rear anti-roll bar |
| 8 | TireCompound | Tire geometry |
| 9 | RearWing | Rear wing/spoiler |
| 10 | RimSizeFront | Front wheel rims |
| 11 | RimSizeRear | Rear wheel rims |
| 12 | Camshaft | Internal engine upgrade |
| 13 | Valves | Internal engine upgrade |
| 14 | Displacement | Internal engine upgrade |
| 15 | PistonsCompression | Internal engine upgrade |
| 16 | FuelSystem | Internal engine upgrade |
| 17 | Ignition | Internal engine upgrade |
| 18 | Exhaust | Exhaust system |
| 19 | Intake | Intake system |
| 20 | Flywheel | Flywheel upgrade |
| 21 | Manifold | Manifold upgrade |
| 22 | RestrictorPlate | Restrictor plate |
| 23 | OilCooling | Oil cooling |
| 24 | SingleTurbo | Single turbo |
| 25 | TwinTurbo | Twin turbo |
| 26 | QuadTurbo | Quad turbo |
| 27 | SuperchargerCSC | Centrifugal supercharger |
| 28 | SuperchargerDSC | Displacement supercharger |
| 29 | Intercooler | Intercooler |
| 30 | Clutch | Clutch upgrade |
| 31 | Transmission | Transmission upgrade |
| 32 | Driveline | Driveline upgrade |
| 33 | Differential | Differential upgrade |
| 34 | FrontBumper | Front bumper |
| 35 | RearBumper | Rear bumper |
| 36 | Hood | Bonnet/hood |
| 37 | SideSkirts | Side skirts |
| 38 | TireWidthFront | Front tire width |
| 39 | TireWidthRear | Rear tire width |
| 40 | WeightReduction | Weight reduction |
| 41 | ChassisStiffness | Chassis stiffness |
| 42 | Ballast | Ballast (FM2023+ only) |
| 43 | MotorParts | Motor upgrade parts |
| 44 | WheelStyle | Wheel style geometry |
| 45 | Aspiration | Aspiration upgrade |

**Note:** Pre-Ballast games (FH4/FH5) use V1 enum mapping where values >= 42 are
shifted down by 1. The `CCarPartsHelper.FromV1()/ToV1()` methods handle this.

### Example: BMW M3 E92 upgrade parts

From `BMW_M3E92_08.zip`:

```
Scene/Exterior/BumperF/bumperF_a.modelbin     (861 KB) — stock front bumper
Scene/Exterior/BumperF/bumperF_b.modelbin     (1.3 MB) — upgrade variant
Scene/Exterior/BumperF/bumperF_c.modelbin     (2.2 MB) — upgrade variant
Scene/Exterior/BumperF/bumperF_race.modelbin  (712 KB) — race variant
Scene/Exterior/BumperR/bumperR_a.modelbin     (776 KB) — stock rear bumper
Scene/Exterior/BumperR/bumperR_b.modelbin     (1.2 MB) — upgrade variant
Scene/Exterior/BumperR/bumperR_c.modelbin     (802 KB) — upgrade variant
Scene/Exterior/Hood/hood_a.modelbin           (314 KB) — stock hood
Scene/Exterior/SideSkirts/skirtL_a.modelbin   (136 KB) — stock side skirt
Scene/Exterior/SideSkirts/skirtL_c.modelbin   (706 KB) — upgrade variant
Scene/Exterior/Trunk/wing_a.modelbin          (83 KB)  — stock wing
Scene/Exterior/Trunk/wing_b.modelbin          (144 KB) — upgrade wing
Scene/Exterior/Trunk/wing_c.modelbin          (92 KB)  — upgrade wing
Scene/Exterior/Trunk/wing_e.modelbin          (816 KB) — upgrade wing
Scene/Exterior/Trunk/wing_race.modelbin       (692 KB) — race wing
Scene/Interior/Floor/rollCage_a.modelbin      (171 KB) — roll cage
Scene/_library/Scene/Brakes/Calipers/BMW_caliperLF_006.modelbin  (47 KB)
Scene/_library/Scene/Brakes/Rotors/BMW_rotorLF_006.modelbin      (47 KB)
```

### Brake components

From `_library/Scene/Brakes/` inside each car zip:
- `Calipers/<MFR>_caliper{LF|LR|RF|RR}_<ID>.modelbin`
- `Rotors/<MFR>_rotor{LF|LR|RF|RR}_<ID>.modelbin`

Brake materials from `Materials.zip`:
```
_fmnext/brake/brakerotor_ch1.materialbin
_fmnext/brake/rotoredge_ch1.materialbin
_fmnext/brake/upgrade_brakes/chrome_ch1_brake.materialbin
_fmnext/brake/upgrade_brakes/gunmetal_ch1_brakes.materialbin
_fmnext/brake/upgrade_brakes/metalpowder_ch1_brake.materialbin
_fmnext/brake/upgrade_brakes/paintedmetal_brake.materialbin
_fmnext/brake/upgrade_brakes/plastic_ch1_brake.materialbin
```

### Livery mask for upgrades

Each car has `LiveryMasks/wing.swatchbin` (and similar) that define paintable
regions on upgrade parts.

---

## Textures

### .swatchbin format (the "Grub" bundle)

All game textures — car paint patterns, AO maps, normal maps, decals, world
textures — use the `.swatchbin` container format (also `.pb` for world textures).
The container is called "Grub" internally.

**Container header:**
```
u32     magic (0x47727562 = "Grub")
u16     version_major (typically 1)
u16     version_minor (0 or 1)
u32     blob_count
...     blob_headers[] (per-blob: tag, offset, size, metadata)
...     blob_data[]
```

**Texture blob tags:**
- `TXCB` (0x54584342) — texture data blob
- `TXCH` (0x54584348) — texture content header (width, height, platform, encoding,
  slice offset, transcoding info)

**Platform check:** Platform 0 = PC (supported). Non-zero = Xbox/Durango tiled
format (not supported by the editor).

### Supported texture encodings

| ID | Format | BPP | Notes |
|----|--------|-----|-------|
| 0 | BC1 (DXT1) | 4 | RGB, 4:1 compression |
| 1 | BC2 (DXT3) | 8 | RGBA, explicit alpha |
| 2 | BC3 (DXT5) | 8 | RGBA, interpolated alpha |
| 3 | BC4 unsigned | 4 | Single channel |
| 4 | BC4 signed | 4 | Single channel |
| 5 | BC5 unsigned | 8 | Two-channel (normal maps) |
| 6 | BC5 signed | 8 | Two-channel (normal maps) |
| 7 | BC6H unsigned | 8 | HDR |
| 8 | BC6H signed | 8 | HDR |
| 9 | BC7 | 8 | High-quality RGBA |
| 13 | R8G8B8A8 | 32 | Uncompressed RGBA |
| 14 | B5G6R5 | 16 | 16-bit RGB |
| 15 | B5G5R5A1 | 16 | 16-bit RGBA |
| 19 | R8 | 8 | Single channel |
| 20 | A8 | 8 | Alpha only |
| 21 | R8G8 | 16 | Two channel |
| 22 | BC7 High Quality | 8 | BC7 variant |

Software decoders exist for BC1-BC5, R8G8B8A8, R8, A8, R8G8. BC7 uses D3D11
GPU-accelerated decompression (with WARP fallback). BC6H, B5G6R5, B5G5R5A1,
DCT, and procedural encodings are detected but not yet decoded.

Source: `src/core/swatchbin.h/.cpp` (686 lines)

### Texture locations

| Archive | Entries | Contents |
|---------|---------|----------|
| `Cars/_library/Textures.zip` | 17,658 | Car-specific: symbols/badges (diffuse, normal, opacity, emboss), AO maps, caliper AO. Organized by car model, quality level (1-5). 212 bytes to 4 MB. |
| `Stripped/gs/swatchbins.zip` | 21,015 | World/environment textures (`.pb` extension, same Grub container). Characters, geology, LED flipbooks, masks, moss, snow, rain, garage UI. |
| `Livery/Decals.zip` | 536 | Decal textures: 268 `.modelbin` + 268 `.swatchbin` pairs (3D decal meshes with textures). |
| `Livery/DecalsHiRes.zip` | 188 | High-resolution decal meshes (`.modelbin` only, no separate textures). |
| `Livery/Vinyls.zip` | 1,480 | Vector vinyl shapes (`.modelbin` only). Letters A-Z, numbers 0-9, symbols. No textures — use paint/livery system for colouring. |

### Paint-finish textures

The pattern/flake textures referenced by paint materials (carbon-fibre weave, camo,
brushed-metal normals, tint maps) are `.swatchbin` files in `Textures.zip`:

```
Textures.zip
  userpaint/swatches/<name>.swatchbin
  _fmnext/userpaint/swatches/<name>.swatchbin
  genericmaps/swatches/<name>.swatchbin
```

Referenced from material parameters by GUID-suffixed leaf name (e.g.
`damascus_001_nrml_f302c9c1-….swatchbin`). Decoded by `src/core/swatchbin.*`.

---

## Materials (.materialbin format)

`.materialbin` files are also Grub bundles, containing nested blob records for
material definitions. 1,855 entries in `Cars/_library/Materials.zip`, plus 264 in
`Stripped/gs/bin.zip`.

### Material blob tags

| Tag | Name | Description |
|-----|------|-------------|
| `MATI` | MaterialResource | Resource path string |
| `MATL` | MaterialLinks | Up to 3 linked paths (version-dependent) |
| `DFPR` | DefaultMaterialParameters | Default parameter values |
| `MTPR` | MaterialParameters | Instance parameter overrides |

### Parameter types

| Type | Description |
|------|-------------|
| `Vector` / `Color` / `Swizzle` | float4 values (base colour, emissive, etc.) |
| `Float` | Single float (gloss, metallic, flake amount, opacity) |
| `Bool` / `Int` | Boolean/integer flags |
| `Texture2D` | Texture path reference (with path hash) |
| `Sampler` | Address mode U/V + filter mode |
| `ColorGradient` | Array of float4 color stops |
| `Vector2` | 2-component vector (texture tiling U/V) |

### Material categories in Materials.zip

| Prefix | Count | Contents |
|--------|-------|----------|
| `_fmnext/usercustomizable/` | 57 | Customizable paint finishes (the editor's primary set) |
| `painttypes/` | 69 | Partial/legacy paint finish set |
| `_fmnext/metal/` | — | Metal materials (aluminum, steel, chrome, gold) |
| `_fmnext/rubber/` | — | Rubber materials |
| `_fmnext/tires/` | — | Tire materials |
| `_fmnext/wheel/` | — | Wheel paint materials |
| `_fmnext/specialcase/` | — | blackhole, wheelblur |
| `_fmnext/brake/` | — | Brake rotor/caliper materials |
| `wheelmaterials/` | — | Wheel-specific materials (chrome, satin, matte) |
| `genericassetmaterials/` | — | Tire tread/sidewall variants |
| `amw_badges/` | — | Manufacturer badge materials |
| `driver/` | — | Driver suit, helmet, visor materials |

Source: `src/core/model_material.h/.cpp` (326 lines)

---

## Shaders

### Compiled DX12 shaders (bin.zip)

`Stripped/gs/bin.zip` contains 15,481 entries of compiled shader binaries:

| Extension | Count | Description |
|-----------|-------|-------------|
| `.pcdxil.pso` | 7,275 | Pixel Shader Objects (DXIL compiled) |
| `.pcdxil.vso` | 7,275 | Vertex Shader Objects (DXIL compiled) |
| `.pcdxil.lso` | 604 | Library Shader Objects (ray-tracing/compute) |
| `.materialbin` | 264 | Material definition bundles |
| `.shaderbin` | 31 | Shader binary bundles |
| `.xml` | 32 | Shader configuration/p permutation configs |

Named by material/light-scenario combination (e.g.
`barnfindvehicle_alphablendcubemaplightscenario.pcdxil.pso`). Organized by hash
directory (`_0x00000000/`, `_0x00000001/`, …). Sizes: 3 KB (shadow depth) to 57 KB
(debug shaders).

### Per-material-class shader archives

`Cars/_library/shaders/` contains 33 `.zip` archives, each with compiled shader
variants for a specific car material class:

| Archive | Purpose |
|---------|---------|
| `car_automotive_paint.zip` | Automotive paint finish shaders |
| `car_livery.zip` | Livery/applied decal shaders |
| `car_livery_transmissive.zip` | Transmissive livery materials |
| `car_standard.zip` | Standard opaque materials |
| `car_standard_coated.zip` | Clear-coated materials |
| `car_standard_emissive.zip` | Emissive/glow materials |
| `car_standard_fabric.zip` | Fabric/cloth materials |
| `car_glass.zip` / `car_glass_detailed.zip` | Glass materials |
| `car_metal_colorshift.zip` | Color-shifting metallic |
| `car_mirror.zip` | Mirror reflections |
| `car_brakerotor.zip` | Brake rotor heat glow |
| `car_tires_pg.zip` | Tire materials |
| `car_wheelblur.zip` | Wheel spin-blur effect |
| `car_digitalscreen.zip` | Dashboard digital screens |
| `car_lens.zip` | Lens flare effects |
| `car_reflector.zip` | Reflector materials |
| `car_window.zip` / `car_window_limotint.zip` | Window tint |
| `usercustomizable.zip` | Customizable paint materials |
| `traffic.zip` | Traffic car materials |

**Note:** The project uses its own OpenGL 3.3 GLSL PBR shaders (~370 lines in
`src/gui/canvas/car_model_renderer.cpp`) instead of the game's DX12 shaders. The
game shaders are not parsed or used by the editor.

---

## Lighting

### DefaultLightControllers.xml (719 lines)

A complete **car light controller VM** (virtual machine) specification defining how
car lights behave in response to player input and game state.

**VM architecture:**
- Registers: `d` (destination), `p0/p1/p2` (parameters), `c` (constant)
- Operations: `mov`, `movc`, `frac`, `sat`, `add`, `sub`, `mul`, `mulc`, `div`,
  `mod`, `min/max`, `sg/slc` (comparisons), `madd`, `lerp`
- Sequence generators (`seq0-3`) with triggers and timed steps
- Slew rate per controller (0.0 = instant/LED, 0.1 = filament bulb ramp)

**Input signals (car state):**

| Signal | Description |
|--------|-------------|
| BrakeAmount | Brake pedal position (0-1) |
| SteeringAmount | Steering angle |
| Speed | Vehicle speed |
| Brake | Brake on/off |
| Headlight | Headlight on/off |
| Sidelight | Sidelight on/off |
| DayTimeRL | Daytime running lights |
| Highbeam | High beam on/off |
| Fog | Fog light on/off |
| Reverse | Reverse gear |
| Engine | Engine running |
| Hazard | Hazard lights |
| IndicatorL / IndicatorR | Turn signals |

**Light output controllers (30+):**

| Controller | Description |
|------------|-------------|
| `Offset_BrakeLightAmount` | Simple brake on/off |
| `Offset_ReverseLightAmount` | Reverse light on/off |
| `Offset_HeadlightAmount` | Headlight on/off |
| `Offset_DayTimeRLAmount` | DRL on/off |
| `Offset_HighBeamAmount` | max(headlight×1.0, highbeam×2.0) |
| `Offset_FogLightAmount` | Fog light on/off |
| `Offset_RightTurnLightAmount` / `Left` | Indicator blink sequences |
| `Offset_HazardLightAmount` | Hazard blink |
| `Offset_SequentialBrake1/2/3` | Sequential brake light with timing delays |
| `Offset_SequentialIndicator{Left,Right}{1,2,3}` | Sequential indicator sweeps |
| `Offset_Brakepulse` | High-speed brake pulse at >1 m/s |
| `Offset_BrakeIndicator{Left,Right}` | Combined brake+indicator blending |
| `Offset_BrakeTailIndicator{Left,Right}` | Tail light + indicator blending |
| `Offset_DRLBrake` / `DRLBrakeIndicator{Left,Right}` | DRL dimming when braking |
| `Offset_LightsFlashingFrequency{1,2,3,4}` | Engine-triggered flash patterns |
| `Offset_ElectroLuminescentLivery` | Electro-luminescent livery effect |
| `Offset_Numberplatelight` | License plate light (max(sidelight, headlight) × 0.5) |
| `Offset_EngineStart` | Engine start indicator |

Source: `Cars/_library/DefaultLightControllers.xml`

### GlobalCarAttributes.xml (44 lines)

Global rendering parameters for all cars:

```xml
<GlobalCarAttributes Version="17" RainLightScale="12">
```

| Parameter | Value | Description |
|-----------|-------|-------------|
| `DarkeningFactor` | 0.9 | Shadow map darkening |
| `CockpitDarkening` | 0.4 | Cockpit interior shadow intensity |
| `HeadlightPitchAI` | 0 | AI headlight pitch offset |
| `HeadlightPitchPlayer` | -0.15 | Player headlight pitch offset |

Also contains extensive rain/snow particle tuning (separate configs for Exterior,
Interior, Dynamic, SideWindow, 3rdPerson views) and per-surface water/snow
accumulation parameters (hood, bumper, cockpit, exterior lens). Damage system
defines dent/scrape speed-to-radius mapping.

Source: `Cars/_library/GlobalCarAttributes.xml`

### InterVehicleEmissiveBalancing.xml (110 KB)

Per-vehicle emissive intensity tuning. Not currently parsed by the editor.

---

## Save data (PGS container)

Player save data uses the **PGS (PlayFab GameSave)** system. Each save is a
directory of named blobs inside the Xbox/Steam local storage path.

**Save locations:**
- Xbox: `C:\XboxGames\GameSave\pgs\u_<XUID>_<DeviceId>\25\ContainersRoot\`
- Steam: `C:\Users\<user>\AppData\Local\ForzaHorizon6\LocalStorage_Shared\User_<Id>\`

### Container structure

```
ContainersRoot/
  SaveVersion/SaveVersion     plaintext UUID (40 bytes)
  User_<Id>/                  user profile directory
    2535448520738610Meta      metadata (564 bytes, encrypted)
    C_ProfileData             profile data (330 KB, encrypted)
    C_ProfileBackup           profile backup (2 MB, encrypted)
    VersionFlags              version flags (564 bytes, encrypted)
    CampaignThumb_*           campaign thumbnails (21-36 KB each)
    TransactionLogFile        transaction log (8 bytes)
  User_<Id>_Backup/           backup profile
    C_ProfileBackup           backup (encrypted)
  Tuning_<carId>_<timestamp>/ tuning presets (UNENCRYPTED)
    header                    preset metadata
    Data                      598-byte binary (upgrades + tuning params)
    Thumb.png                 preview thumbnail
  LayerGroup_<id>_<timestamp>/ livery groups
    header                    group metadata
    C_group                   livery data (zlib compressed)
    thumb.webp                thumbnail
    render_preview.png        render preview (optional)
  Livery_<carId>_<timestamp>/ individual liveries
    header                    livery metadata
    C_livery                  livery data
    bigThumb.webp             thumbnail
  GarageLayout_<guid>/        garage layout
    GarageLayoutData          encrypted (7.99 entropy)
    header                    layout metadata
  Estate_<guid>/              estate (home) data
    EstateData                encrypted (8.00 entropy, max)
    header                    estate metadata
  C_GarageLayoutsDataContainer/
    C_GarageLayoutsDataBlob   encrypted (7.64 entropy)
  ToExport/                   exported livery groups
```

### Encryption status

| Blob | Size | Entropy | Status |
|------|------|---------|--------|
| `C_ProfileData` | 330 KB | — | Encrypted (PGS) |
| `C_ProfileBackup` | 2 MB | 8.0 | Encrypted (PGS) |
| `GarageLayoutData` | 26 KB | 7.99 | Encrypted (PGS) |
| `EstateData` | 261 KB | 8.00 | Encrypted (PGS) |
| `C_GarageLayoutsDataBlob` | 564 B | 7.64 | Encrypted (PGS) |
| `VersionFlags` | 564 B | 7.63 | Encrypted (PGS) |
| `2535448520738610Meta` | 564 B | — | Encrypted (PGS) |
| **Tuning Data** | 598 B | **3.7** | **Plaintext** |
| **C_livery** | varies | — | **Plaintext** |
| **C_group** | varies | — | **Zlib compressed** |
| **header** | varies | — | **Plaintext** (UTF-16 metadata) |
| **SaveVersion** | 40 B | — | **Plaintext** (UUID) |

The PGS encryption is per-account-per-device; the game binary contains the
decryption keys. The Tuning data and livery data are the only readable blobs.

### C_group format (livery groups)

Each `C_group` blob has an 8-byte header followed by zlib-compressed data:

```
u32     compressed_size (size of zlib payload, = file_size - 8)
u32     decompressed_size
byte[]  zlib compressed payload (magic 0x78 0x9C at offset 8)
```

Decompressed data starts with the `gyvl` magic (0x6779766C) — the livery group
format. See `src/core/project_codec.cpp` for parsing.

### Tuning data format

See `docs/TUNING.md` for the complete tuning preset binary format specification.

---

## NerdData

`NerdData/NerdData.json` (564 bytes) — despite the `.json` extension, this is an
**encrypted binary blob** containing network/session versioning data.

### File structure

| Offset | Size | Content |
|--------|------|---------|
| 0x00 | 4 | Magic `B7 92 A6 69` (key selection tag, not embedded key) |
| 0x04 | 4 | Possibly IV or version indicator (`61 3F 41 34`) |
| 0x08 | 556 | 35 × 16-byte AES ciphertext blocks + 4 trailing bytes |

| Property | Value |
|----------|-------|
| File size | 564 bytes (4-byte header + 560 bytes = 35 AES blocks) |
| Shannon entropy | 7.63 / 8.0 (near-random) |
| Block uniqueness | All 35 blocks unique (not ECB mode) |

### What it contains (after decryption)

The decrypted content is **JSON** with at minimum a `DataVersion` field.
The string table adjacent to the NerdData path references `DataVersion` and
`GameSession`, confirming this is **network matchmaking/session version data**
used by the "Nerd" networking subsystem.

### Encryption details

**Crypto library:** Botan 3.9.0 (statically linked in the executable)
- AES with CBC/CTR/GCM modes available
- KDF options: PBKDF2, HKDF, Argon2, scrypt
- Windows BCrypt API also imported as fallback

**Executable location:** `S:\SteamLibrary\steamapps\common\ForzaHorizon6\forzahorizon6.exe` (175 MB)
**Internal codename:** `forte_main` (Playground Games P4 depot at `D:/p4/forte_main/`)

**String references in executable:**
- `0x068396C0`: `game:\media\NerdData\`
- `0x068396D8`: `NerdData.json`
- `0x068396F0`: `DataVersion`, `GameSession`
- `0x06839720`: `4.0.3.798` (data version string)

**Error codes in executable:**
- `NERD_E_NERDDATA_JSON_LOAD_FAIL`
- `NERD_E_NERDDATA_JSON_PARSE_FAIL`
- `NERD_E_NERDDATA_JSON_DATA_VERSION_MISSING`

### Why it can't be decrypted

1. Magic bytes `B792A669` are **NOT hardcoded** in the exe — they're runtime-derived
2. AES key is derived at runtime (likely from game binary via anti-tamper / key derivation)
3. Key is in `.text` (code) section, not `.rdata` (data) — not scannable via strings
4. To find the key, need to reverse-engineer the function at `0x068396C0` (NerdData path reference)
5. No public decryption tool exists for FH6 NerdData

### Comparison to FH5 encryption

FH5 uses **Arxan TransformIT (GuardIT)** with 16-byte IV headers and MAC verification.
FH6 likely uses the same or similar scheme, but with updated keys. The
[ForzaTech-crypto-tool](https://github.com/Doliman100/ForzaTech-encryption-tool)
supports FM6Apex through FH5 only — FH6 support would require extracting new keys
from the executable using the IDA Pro debugging workflow described in the
[XeNTaX thread](https://web.archive.org/web/20231012075845/https://forum.xentax.com/viewtopic.php?t=19015&start=15#p193855).

---

## Other game data locations

| Path | Contents |
|------|----------|
| `Cars/_library/Materials.zip` | All `.materialbin` files (paint, wheel, tire, brake, glass, etc.) |
| `Cars/_library/Textures.zip` | Shared textures (AO maps, gauges, wheel textures, etc.) |
| `Cars/_library/Shaders.zip` | Compiled shaders |
| `Cars/_library/GlobalCarAttributes.xml` | Global car tuning params (blur, shadows, rain drops) |
| `Cars/_library/DefaultLightControllers.xml` | Lighting defaults (719-line VM spec) |
| `Cars/_library/InterVehicleEmissiveBalancing.xml` | Per-vehicle emissive tuning (110 KB) |
| `Cars/_library/*.avpins` | Animation clips (door open, hood, trunk, etc.) |
| `Stripped/gs/bin.zip` | Shaders (7275 .pso + 7275 .vso + 604 .lso) + 264 `.materialbin` |
| `Stripped/gs/swatchbins.zip` | 21,015 texture swatches + shaders |
| `Stripped/gs/snapnodes.zip` | 274 track scene snap nodes |
| `Stripped/RC0.zip` | Decal swatchbins (thumbnail textures) |
| `Stripped/EntityModel.zip` | Entity model data |
| `Stripped/gamedbRC.slt` | Game database (SQLite in a TransformIT container — see *The car database*) |
| `NerdData/NerdData.json` | Encrypted binary blob (564 bytes, AES-encrypted) |
| `Livery/Decals.zip` | Decal textures (536 entries) |
| `Livery/DecalsHiRes.zip` | High-res decal meshes (188 entries) |
| `Livery/Vinyls.zip` | Vinyl textures (1,480 entries) |
| `Physics/TireEffectsDefinitions.xml` | Tire smoke/surface effects per surface type |
| `Physics/surfaceTypes.xml` | Surface type definitions (binary) |
| `UI/` | UI assets |
