# Tuning Preset Save Format

Binary tuning presets saved by the player in FH6. Each preset stores a complete
upgrade selection (which parts are installed) plus tuning parameters (gear ratios,
tire pressures, alignment, etc.). Discovered in the Xbox PGS save container.

## Container layout

Each tuning preset is a folder inside `ContainersRoot/`:

```
Tuning_<carId>_<YYYYMMDDHHmmss>/
  header       structured metadata (tuning name, car id, guid)
  Data         598-byte binary blob (upgrade selections + tuning params)
  Thumb.png    preview thumbnail (optional)
```

Examples:

```
Tuning_1108_20260528142725/    BMW M3 E92 — preset "VALIT"
Tuning_1108_20260616124351/    BMW M3 E92 — preset "GOOL"
Tuning_1269_20260525104505/    Ford Capri — preset "Brick"
Tuning_4090_20260601174114/    Porsche 911 — preset "Drift"
```

---

## Header format (version 7, variable-length)

The header is a versioned binary structure containing the preset name and metadata.
All multi-byte values are little-endian.

```
Offset  Size   Field
0x000   4      version (u32 = 7)
0x004   4      nameLen (u32, number of UTF-16 code units)
0x008   N*2    name (UTF-16LE string, no null terminator)
        ...    padding (zeros to align)
        ...    structured metadata fields (see below)
        ...    16-byte GUID
        ...    car ID (u32 LE)
        ...    more metadata
```

### Known header fields

Parsed from 4 sample headers:

| Offset | Size | Field | Values seen | Notes |
|--------|------|-------|-------------|-------|
| 0x000 | 4 | version | 7 | Always 7 |
| 0x004 | 4 | nameLen | 4-23 | UTF-16 code unit count |
| 0x008 | N*2 | name | "VALIT", "GOOL", "Brick", "Drift" | User-defined preset name |
| varies | 4 | unknown_1 | 0 | |
| varies | 4 | unknown_2 | 0x000607ea (395242) | Consistent across some presets |
| varies | 4 | unknown_3 | varies | |
| varies | 4 | unknown_4 | varies | |
| varies | 4 | unknown_5 | 1 | Always 1 |
| varies | 4 | hash_1 | 0x1c6cc732 | Appears to be a hash |
| varies | 4 | unknown_6 | 0x000901fa (590330) | Appears in 3/4 presets |
| varies | 4 | authorLen | 6-13 | UTF-16 code unit count (0 when absent) |
| varies | N*2 | author | "Fr4g3z", "SageCloth5288" | Optional author name |
| varies | 16 | guid | varies | Unique preset identifier |
| varies | 4 | unknown_7 | 0x201 (513) | Always 513 |
| varies | 4 | zero | 0 | Padding |
| varies | 4 | carId | 283648, 1047040, 4110417920 | Car ID (see note) |

**Note:** The car ID in the header appears as a packed u32 that differs from the
car ID in the Data file. For car 1108: header shows 283648 (0x45400), Data shows
1108 (0x0454). The relationship is: `header_carId >> 8 == Data_carId` for some
cars but not all — the exact encoding is not yet fully decoded.

### Header size

| Preset | Header size | Name | Author |
|--------|------------|------|--------|
| 1108_v1 | 123 bytes | VALIT | Fr4g3z |
| 1108_v2 | 137 bytes | GOOL | Fr4g3z |
| 1269 | 177 bytes | Brick | SageCloth5288 |
| 4090 | 123 bytes | Drift | Fr4g3z |

Size varies with name length and presence/absence of author string.

---

## Data format (fixed 598 bytes)

The Data file is a fixed-size binary blob containing the upgrade selections and
tuning parameters. Not encrypted. All multi-byte values are little-endian.

```
Offset  Size   Field
0x000   1      version (u8 = 3)
0x001   1      flags (u8, 0x00 or 0x01)
0x002   2      carId_lo16 (u16 LE, lower 16 bits of car ID)
0x004   2      zero (u16 = 0)
0x006   2      field_A (u16, 0 or 1)
0x008   2      zero (u16 = 0)
0x00A   2      field_B (u16 = 1)
0x00C   1      zero (u8 = 0)
0x00D   var    upgrade_slots[] (u32 LE × N, see below)
...     var    0xFF padding (fills unused upgrade slots)
...     2      separator (u16 = 0x0000)
...     var    tuning_params[] (f32 LE × M, see below)
```

Total: 598 bytes.

### Car ID verification

| carId_lo16 | Car |
|------------|-----|
| 0x0454 (1108) | BMW M3 E92 |
| 0x04f5 (1269) | Ford Capri |
| 0x0ffa (4090) | Porsche 911 |

---

## Upgrade slots

Starting at offset 0x00D, a sequence of u32 LE values — one per upgrade part
category. Each value identifies the specific upgrade variant installed in that slot.
The slot index maps to the `CCarParts` enum (see `GAMEDATA.md` for the full 46-value
enum).

### Upgrade ID encoding

Each upgrade ID is a 32-bit value structured as:

```
Bits 31-16: car-class prefix (identifies the upgrade category group)
Bits 15-0:  variant/level ID (identifies the specific part)
```

**Three prefix categories** observed across all cars:

| Category | Slots | Car 1108 prefix | Car 1269 prefix | Car 4090 prefix |
|----------|-------|-----------------|-----------------|-----------------|
| Body/Exterior | Engine(0), Drivetrain(1), CarBody(2), Brakes(4), SpringDamper(5), AntiSway(6-7), TireCompound(8), RearWing(9), RimSize(10-11), Bumper(34-35), Hood(36), SideSkirts(37), TireWidth(38-39), WeightReduction(40), ChassisStiffness(41), Aspiration(45) | `0x10e8` | `0x135d` | `0x3e68` |
| Engine Internals | Camshaft(12), Valves(13), Displacement(14), Pistons(15), FuelSystem(16), Ignition(17), Exhaust(18), Intake(19), Flywheel(20), OilCooling(23), SuperchargerDSC(28), Intercooler(29) | `0x2aa2` | `0x2b4d` | `0x287b` |
| Drivetrain | Clutch(30), Transmission(31), Driveline(32), Differential(33) | `0x07b0` | `0x2012` | `0x3f99` |

### Special values

| Value | Meaning |
|-------|---------|
| `0xFFFFFFFF` | Slot not applicable to this car (e.g. MotorParts on ICE car, RestrictorPlate) |
| `0xFFFFFF00` | Slot exists but using stock/base part (no upgrade installed) |
| Other | Specific upgrade variant installed |

### Cross-car comparison

Full upgrade slot comparison for 3 cars (1108=BMW M3, 1269=Ford Capri, 4090=Porsche 911):

| Slot | Part | 1108 | 1269 | 4090 |
|------|------|------|------|------|
| 0 | Engine | 0x10e82100 | 0x135d0d00 | 0x3e689300 |
| 1 | Drivetrain | 0x10e82000 | 0x135d0900 | 0x3e689000 |
| 2 | CarBody | 0x10e82000 | 0x135d0800 | 0x3e689000 |
| 3 | Motor | 0xffffff00 | 0xffffff00 | 0xffffff00 |
| 4 | Brakes | 0x10e823ff | 0x135d0bff | 0x3e6893ff |
| 5 | SpringDamper | 0x10e82500 | 0x135d0d00 | 0x3e689500 |
| 6 | AntiSwayFront | 0x10e82000 | 0x135d0b00 | 0x3e689300 |
| 7 | AntiSwayRear | 0x10e82000 | 0x135d0b00 | 0x3e689300 |
| 8 | TireCompound | 0x10e82900 | 0x135d1100 | 0x3e689900 |
| 9 | RearWing | 0x10e82100 | 0x135d0900 | 0x3e689200 |
| 10 | RimSizeFront | 0x10e82300 | 0x135d0800 | 0x3e689000 |
| 11 | RimSizeRear | 0x10e82300 | 0x135d0800 | 0x3e689000 |
| 12 | Camshaft | 0x2aa21000 | 0x2b4df000 | 0x287b4800 |
| 13 | Valves | 0x2aa21000 | 0x2b4df300 | 0x287b4b00 |
| 14 | Displacement | 0x2aa21000 | 0x2b4df300 | 0x287b4900 |
| 15 | PistonsCompression | 0x2aa21000 | 0x2b4df000 | 0x287b4800 |
| 16 | FuelSystem | 0x2aa21000 | 0x2b4df300 | 0x287b4b00 |
| 17 | Ignition | 0x2aa21000 | 0x2b4df300 | 0x287b4b00 |
| 18 | Exhaust | 0x2aa21000 | 0x2b4df000 | 0x287b4800 |
| 19 | Intake | 0x2aa21000 | 0x2b4df000 | 0x287b4b00 |
| 20 | Flywheel | 0x2aa21200 | 0x2b4df000 | 0x287b4b00 |
| 21 | Manifold | 0xffffff00 | 0xffffff00 | 0xffffff00 |
| 22 | RestrictorPlate | 0xffffffff | 0xffffffff | 0xffffffff |
| 23 | OilCooling | 0x2aa210ff | 0x2b4df0ff | 0x287b4aff |
| 24 | SingleTurbo | 0xffffff00 | 0x2b4df400 | 0xffffff00 |
| 25 | TwinTurbo | 0xffffffff | 0xffffff00 | 0xffffffff |
| 26 | QuadTurbo | 0xffffffff | 0xffffffff | 0xffffffff |
| 27 | SuperchargerCSC | 0xffffffff | 0xffffffff | 0xffffffff |
| 28 | SuperchargerDSC | 0x2aa211ff | 0xffffffff | 0x287b49ff |
| 29 | Intercooler | 0x2aa21100 | 0x2b4df0ff | 0xffffff00 |
| 30 | Clutch | 0x07b0c000 | 0x2012f000 | 0x3f9943ff |
| 31 | Transmission | 0x07b0c400 | 0x2012f700 | 0x3f994400 |
| 32 | Driveline | 0x07b0c000 | 0x2012f300 | 0x3f994300 |
| 33 | Differential | 0x07b0c300 | 0x2012f300 | 0x3f994300 |
| 34 | FrontBumper | 0x10e82100 | 0x135d0900 | 0x3e689200 |
| 35 | RearBumper | 0x10e82000 | 0x135d0900 | 0x3e689000 |
| 36 | Hood | 0x10e82000 | 0x135d0800 | 0x3e689000 |
| 37 | SideSkirts | 0x10e83400 | 0x135d0800 | 0x3e689000 |
| 38 | TireWidthFront | 0x10e82300 | 0x135d0800 | 0x3e689000 |
| 39 | TireWidthRear | 0x10e82300 | 0x135d0800 | 0x3e689000 |
| 40 | WeightReduction | 0x10e82100 | 0x135d0b00 | 0x3e689100 |
| 41 | ChassisStiffness | 0x10e82100 | 0x135d0b00 | 0x3e689300 |
| 42 | Ballast | 0xffffff00 | 0xffffff00 | 0xffffff00 |
| 43 | MotorParts | 0xffffffff | 0xffffffff | 0xffffffff |
| 44 | WheelStyle | 0xffffffff | 0xffffffff | 0xffffffff |
| 45 | Aspiration | 0x10e820ff | 0x135d08ff | 0x3e6893ff |

**Observations:**
- Motor(3), Manifold(21), RestrictorPlate(22), MotorParts(43), WheelStyle(44) are
  universally stock or N/A across all 3 cars
- SingleTurbo(24) is only installed on car 1269 (Ford Capri) — other cars use NA
- SuperchargerDSC(28) is installed on 1108 and 4090 but not 1269
- The 0xFF suffix (byte 1 of the lower 16 bits) appears to indicate "max level" or
  a special tier — it appears on Brakes, OilCooling, SuperchargerDSC, Clutch, and
  Aspiration across the sample cars

---

## Tuning parameters

After the upgrade slots and 0xFF padding, a `u16 0x0000` separator marks the start
of the tuning parameter section — an array of f32 LE values.

### Parameter count

| Car | Parameters | Notes |
|-----|-----------|-------|
| 1108 (BMW M3) | 27 | |
| 1269 (Ford Capri) | 32 | 5 more than other cars |
| 4090 (Porsche 911) | 27 | Identical count to 1108 |

### Parameter values

Car 1108 (BMW M3, preset "VALIT"):

| Index | Value | Likely meaning |
|-------|-------|---------------|
| 0 | 0.304211 | Gear ratio or tire pressure |
| 1 | 0.518421 | Gear ratio or tire pressure |
| 2 | 0.150000 | Alignment or brake balance |
| 3 | 0.050000 | Small adjustment parameter |
| 4 | 0.025000 | Fine adjustment |
| 5 | 0.470000 | Mid-range parameter |
| 6 | 0.500000 | 50% — likely a balance/ratio |
| 7 | 0.000000 | Zeroed/disabled |
| 8 | 0.251652 | |
| 9 | 0.181250 | |
| 10 | 0.000000 | Zeroed/disabled |
| 11 | 0.344211 | |
| 12 | 0.582105 | |
| 13 | 0.600000 | |
| 14 | 0.395000 | |
| 15 | 1.000000 | Maximum / 100% |
| 16 | 0.500000 | 50% balance |
| 17 | 0.664855 | Identical across 1108 and 4090 |
| 18 | 0.400362 | Identical across 1108 and 4090 |
| 19 | 0.257246 | Identical across 1108 and 4090 |
| 20 | 0.170290 | Identical across 1108 and 4090 |
| 21 | 0.115942 | Identical across 1108 and 4090 |
| 22 | 0.085145 | Identical across 1108 and 4090 |
| 23 | 0.057971 | Identical across 1108 and 4090 |
| 24 | -1.000000 | Sentinel / unset |
| 25 | -1.000000 | Sentinel / unset |
| 26 | -1.000000 | Sentinel / unset |

**Pattern:** Indices 17-23 are identical across cars 1108 and 4090, suggesting they
are global defaults or shared parameters. The final 3 values (-1.0) appear to be
sentinel/end markers (present in all 3 cars). Indices 0-16 vary per car and are
likely car-specific tuning values.

### Preset versioning

When a preset is updated (e.g. 1108_v1 "VALIT" → 1108_v2 "GOOL"), the upgrade
slots remain identical — only the tuning parameters change. Between the two 1108
presets, only 13 bytes differed, all in the float section (indices 1, 3, 6, 7).

---

## Relationship to .carbin upgrades

The upgrade IDs in the tuning Data file correspond to the `Upgrade.id` values
defined in the car's `.carbin` file. The `.carbin` defines all possible upgrades
per slot; the tuning preset stores which specific upgrade ID is installed.

The architecture:

1. `.carbin` = static catalog of all possible upgrades per slot (read from game media)
2. Tuning Data = player's selection of which upgrade ID is installed per slot
3. At render time, the engine matches the stored upgrade ID against the `.carbin`
   upgrade list to find the corresponding `SharedCarModel` → loads the right mesh

Livery files (`C_livery`) are completely independent — they store only vinyl shapes
and paint materials, not upgrade parts.

---

## Source code references

The tuning data format is not currently parsed by ForzaLiveryStudio. Relevant
existing code:

| File | Relevance |
|------|-----------|
| `src/core/car_scene.cpp` | `PartInstance` struct, `carbinReadParts()` — reads `.carbin` upgrade definitions |
| `src/gui/widgets/car_preview_widget.cpp` | Paint slot resolution, wheel model path detection |

A future tuning codec would need to:
1. Read the 13-byte header to extract car ID
2. Parse the upgrade slot u32s and map them to CCarParts indices
3. Match upgrade IDs against `.carbin` Upgrade.id values
4. Parse the tuning parameter floats
