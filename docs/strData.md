# Car-Related String Tables (FH6)

Parsable from `media/Stripped/StringTables/<LANG>.zip`, each `.str` is a hashed
string table. Keys are lookup IDs (e.g. `IDS_DisplayName_4`); values are the
localised text. Keys and values share the same index and are linked by a common
hash.

All files below were parsed from the **EN** locale. Every table has
**FH6 flags = `0x00020000`** (different from FH5's `0x0000008C`).

---

## .str binary format

```
Offset  Size   Description
0x0000  2      Magic: 00 08
0x0002  126    Table name, null-terminated, padded with zeros to 0x80 bytes
0x0080  4      FH6 flags (u32 LE) — always 0x00020000 in FH6
0x0084  4      values_section_offset (u32 LE) — always 0x8C
0x0088  4      keys_section_offset (u32 LE) = 0x8C + values_section_size
0x008C  ...    VALUES section
        ...    KEYS section
```

Each section (VALUES and KEYS):

```
+0x00  u32  section_size   = 12 + 8*count + blob_size
+0x04  u32  blob_size      (size of the null-terminated string blob)
+0x08  u32  entry_count
+0x0C  8*N  entries        (each: u32 hash, u32 offset_into_blob)
+...   var  string blob    (null-terminated UTF-8 strings)
```

Keys and values are linked by **index**: `keys[i].hash == values[i].hash`.

The parser script is at `tools/Pickle/parse_str.py`.

---

## File inventory

| File | Size (B) | Entries | Offset | Description |
|------|---------|---------|--------|-------------|
| `List_CarMake.str` | 7,990 | 178 | 96–187 | Car manufacturer names (Acura…Zenvo) |
| `List_FamilyBody.str` | 2,559 | 46 | 188–238 | Body style families |
| `List_FamilyModel.str` | 1,670 | 36 | 239–279 | Model families (Civic, Supra, 911…) |
| `List_FamilySpecial.str` | 371 | 5 | 280–289 | Door count (2/3/4/5-door) |
| `CarClasses.str` | 1,900 | 16 | 290–302 | Performance class names + descriptions (D…X) |
| `List_LiveryMaterials.str` | 8,818 | 168 | 303–372 | Paint finish names + primary/secondary colour labels |
| `WheelCategories.str` | 1,485 | 10 | 373–382 | Rim style categories |
| `CarBuckets.str` | 2,190 | 49 | 383–436 | Car type bucket names (Cult Cars, Hypercars…) |
| `CarDetails.str` | 4,654 | 66 | 437–507 | Car detail field labels + help strings |
| `CarFlow.str` | 16,892 | 262 | 508–774 | Car selection UI strings |
| `CarHornCategory.str` | 402 | 3 | 775–782 | Musical / Sound Effects / Standard |
| `Data_Car.str` | 66,485 | 1,302 | 783–2089 | Car data labels |
| `DefaultGarageLayout.str` | 1,618 | 16 | 2090–2110 | Default garage layout labels |
| `Livery_Decals.str` | 33,394 | 708 | 2111–2823 | Brand/logo display names for decals |
| `PaintableGroups.str` | 3,531 | 80 | 2824–2908 | Paint region names + error messages |
| `UpgradeTypes.str` | 16,283 | 106 | 2909–3019 | Upgrade type labels + descriptions |
| `Upgrades.str` | 21,809 | 496 | 3020–3527 | Upgrade part names |

---

## Key enumerations (full data)

### List_CarMake — 178 manufacturers

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C739 | IDS_DisplayName_1 | Acura |
| 0x40E1C6B9 | IDS_DisplayName_2 | Aston Marti
| 0x40E1C639 | IDS_DisplayName_3 | Audi |
| 0x40E1C5B9 | IDS_DisplayName_4 | Bentley |
| 0x40E1C539 | IDS_DisplayName_5 | BMW |
| 0x40E1C4B9 | IDS_DisplayName_6 | BMW |
| 0x40E1C439 | IDS_DisplayName_7 | Buick |
| 0x40E1C3B9 | IDS_DisplayName_8 | Cadillac |
| 0x40E1C339 | IDS_DisplayName_9 | Chevrolet |
| 0x70E387A0 | IDS_DisplayName_16 | Honda |
| 0x70E38720 | IDS_DisplayName_17 | Hyundai |
| 0x70E38020 | IDS_DisplayName_19 | Jaguar |
| 0x70E344A0 | IDS_DisplayName_20 | Koenigsegg |
| 0x70E34420 | IDS_DisplayName_21 | Lamborghini |
| 0x70E345A0 | IDS_DisplayName_22 | Lancia |
| 0x70E34520 | IDS_DisplayName_23 | Lexus |
| 0x70E346A0 | IDS_DisplayName_24 | Lotus |
| 0x70E34620 | IDS_DisplayName_25 | Maserati |
| 0x70E347A0 | IDS_DisplayName_26 | Mazda |
| 0x70E34720 | IDS_DisplayName_27 | McLaren |
| 0x70E340A0 | IDS_DisplayName_28 | Mercedes-Benz |
| 0x70E34020 | IDS_DisplayName_29 | MINI |
| 0x70E304A0 | IDS_DisplayName_30 | Mitsubishi |
| 0x70E30420 | IDS_DisplayName_31 | Nissan |
| 0x70E305A0 | IDS_DisplayName_32 | Opel |
| 0x70E306A0 | IDS_DisplayName_34 | Pagani |
| 0x70E307A0 | IDS_DisplayName_36 | Peugeot |
| 0x70E300A0 | IDS_DisplayName_38 | Pontiac |
| 0x70E30020 | IDS_DisplayName_39 | Porsche |
| 0x70E2C420 | IDS_DisplayName_41 | Renault |
| 0x70E2C520 | IDS_DisplayName_43 | Saleen |
| 0x70E2C720 | IDS_DisplayName_47 | Shelby |
| 0x70E2C0A0 | IDS_DisplayName_48 | Subaru |
| 0x70E2C020 | IDS_DisplayName_49 | Toyota |
| 0x70E284A0 | IDS_DisplayName_50 | TVR |
| 0x70E285A0 | IDS_DisplayName_52 | Volkswagen |
| 0x70E28520 | IDS_DisplayName_53 | Volvo |
| 0x70E286A0 | IDS_DisplayName_54 | Alfa Romeo |
| 0x70E28620 | IDS_DisplayName_55 | Land Rover |
| 0x70E28020 | IDS_DisplayName_59 | Holden |
| 0x70E247A0 | IDS_DisplayName_66 | Radical |
| 0x70E205A0 | IDS_DisplayName_72 | Noble |
| 0x70E20720 | IDS_DisplayName_77 | De Tomaso |
| 0x70E200A0 | IDS_DisplayName_78 | DeLorean |
| 0x70E1C4A0 | IDS_DisplayName_80 | GMC |
| 0x70E1C7A0 | IDS_DisplayName_86 | AMG Transport Dynamics |
| 0x70E1C0A0 | IDS_DisplayName_88 | Jeep |
| 0x70E1C020 | IDS_DisplayName_89 | Plymouth |
| 0x70E184A0 | IDS_DisplayName_90 | Abarth |
| 0x70E187A0 | IDS_DisplayName_96 | Ultima |
| 0x70E18720 | IDS_DisplayName_97 | Austin-Healey |
| 0x70E180A0 | IDS_DisplayName_98 | MG |
| 0x71C24938 | IDS_DisplayName_102 | Hennessey |
| 0x71C24AB8 | IDS_DisplayName_105 | Lincoln |
| 0x71C24BB8 | IDS_DisplayName_107 | Ariel |
| 0x71C24C38 | IDS_DisplayName_108 | KTM |
| 0x71C208B8 | IDS_DisplayName_111 | BAC |
| 0x71C2C9B8 | IDS_DisplayName_123 | Ram |
| 0x71C2CA38 | IDS_DisplayName_124 | Datsun |
| 0x71C2CB38 | IDS_DisplayName_126 | SRT |
| 0x71C289B8 | IDS_DisplayName_133 | Zenvo |
| 0x71C28BB8 | IDS_DisplayName_137 | Mercedes-AMG |
| 0x71C28C38 | IDS_DisplayName_138 | HSV |
| 0x71C28CB8 | IDS_DisplayName_139 | Meyers |
| 0x71C34838 | IDS_DisplayName_140 | Penhall |
| 0x71C348B8 | IDS_DisplayName_141 | Polaris |
| 0x71C34938 | IDS_DisplayName_142 | Alumicraft |
| 0x71C34B38 | IDS_DisplayName_146 | Reliant |
| 0x71C308B8 | IDS_DisplayName_151 | RJ Anderson |
| 0x71C3C938 | IDS_DisplayName_162 | Can-Am |
| 0x71C3CB38 | IDS_DisplayName_166 | Funco Motorsports |
| 0x71C388B8 | IDS_DisplayName_171 | Peel |
| 0x71C38938 | IDS_DisplayName_172 | Formula Drift |
| 0x71C049B8 | IDS_DisplayName_183 | Rimac |
| 0x71C04A38 | IDS_DisplayName_184 | Apollo |
| 0x71A24838 | IDS_DisplayName_200 | Playground |
| 0x71A24B38 | IDS_DisplayName_206 | DeBerti |
| 0x71A209B8 | IDS_DisplayName_213 | SIERRA Cars |
| 0x71A20AB8 | IDS_DisplayName_215 | Wuling |
| 0x71A20C38 | IDS_DisplayName_218 | Gordon Murray Automotive |
| 0x71A2C838 | IDS_DisplayName_220 | Jimco |
| 0x71A2C8B8 | IDS_DisplayName_221 | RIVIAN |
| 0x71A2C9B8 | IDS_DisplayName_223 | Casey Currie Motorsports |
| 0x71A2CB38 | IDS_DisplayName_226 | Schuppan |
| 0x71A289B8 | IDS_DisplayName_233 | Lucid |
| 0x71A28CB8 | IDS_DisplayName_239 | Autozam |
| 0x71A3C938 | IDS_DisplayName_262 | GR |

### List_FamilyBody — 46 body style families

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C739 | IDS_DisplayName_1 | None |
| 0x40E1C6B9 | IDS_DisplayName_2 | 1960s Grand Prix |
| 0x40E1C639 | IDS_DisplayName_3 | 1970s Grand Prix |
| 0x40E1C5B9 | IDS_DisplayName_4 | 1980s Grand Prix |
| 0x40E1C539 | IDS_DisplayName_5 | 1990s Grand Prix |
| 0x40E1C4B9 | IDS_DisplayName_6 | American Motorsport |
| 0x40E1C439 | IDS_DisplayName_7 | American Muscle Revival |
| 0x40E1C3B9 | IDS_DisplayName_8 | American Stock Cars |
| 0x40E1C339 | IDS_DisplayName_9 | American Street Muscle |
| 0x70E384A0 | IDS_DisplayName_10 | Classic Sport Compact |
| 0x70E38420 | IDS_DisplayName_11 | Classic Supermini |
| 0x70E385A0 | IDS_DisplayName_12 | Coupster |
| 0x70E38520 | IDS_DisplayName_13 | Early Grand Touring |
| 0x70E386A0 | IDS_DisplayName_14 | Early GT Racing |
| 0x70E38620 | IDS_DisplayName_15 | Early Hot Hatch |
| 0x70E387A0 | IDS_DisplayName_16 | Early Protoype Racing |
| 0x70E38720 | IDS_DisplayName_17 | Early Sport Compact |
| 0x70E380A0 | IDS_DisplayName_18 | Executive Sport |
| 0x70E38020 | IDS_DisplayName_19 | German Touring Cars |
| 0x70E344A0 | IDS_DisplayName_20 | Grand Touring Legends |
| 0x70E34420 | IDS_DisplayName_21 | GT Racing |
| 0x70E345A0 | IDS_DisplayName_22 | Iconic Sports Cars |
| 0x70E34520 | IDS_DisplayName_23 | Middle Grand Touring |
| 0x70E346A0 | IDS_DisplayName_24 | Modern FWD Hot Hatch |
| 0x70E34620 | IDS_DisplayName_25 | Modern Grand Prix |
| 0x70E347A0 | IDS_DisplayName_26 | Modern Grand Touring |
| 0x70E34720 | IDS_DisplayName_27 | Modern Hypercar |
| 0x70E340A0 | IDS_DisplayName_28 | Modern Indy |
| 0x70E34020 | IDS_DisplayName_29 | Modern Muscle Car |
| 0x70E304A0 | IDS_DisplayName_30 | Modern Sport Compact |
| 0x70E30420 | IDS_DisplayName_31 | Modern Sports Cars |
| 0x70E305A0 | IDS_DisplayName_32 | Prewar Grand Prix |
| 0x70E30520 | IDS_DisplayName_33 | Pro Stock Drag Racing |
| 0x70E306A0 | IDS_DisplayName_34 | Production Rally |
| 0x70E30620 | IDS_DisplayName_35 | Prototype1 Racing |
| 0x70E307A0 | IDS_DisplayName_36 | Racing Icons |
| 0x70E30720 | IDS_DisplayName_37 | Racing Trucks |
| 0x70E300A0 | IDS_DisplayName_38 | Rally Legends |
| 0x70E30020 | IDS_DisplayName_39 | Sport Truck |
| 0x70E2C4A0 | IDS_DisplayName_40 | Sport Truck 1 |
| 0x70E2C420 | IDS_DisplayName_41 | Supermini |
| 0x70E2C5A0 | IDS_DisplayName_42 | Timeless Supercars |
| 0x70E2C520 | IDS_DisplayName_43 | Track Toys |
| 0x70E2C6A0 | IDS_DisplayName_44 | Ultimate Grand Touring |
| 0x70E2C620 | IDS_DisplayName_45 | Ultimate Track Toys |
| 0x70E2C7A0 | IDS_DisplayName_46 | V8 Supercar |

### List_FamilyModel — 36 model families

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C739 | IDS_DisplayName_1 | None |
| 0x40E1C6B9 | IDS_DisplayName_2 | Integra |
| 0x40E1C639 | IDS_DisplayName_3 | Civic |
| 0x40E1C539 | IDS_DisplayName_5 | NSX |
| 0x40E1C4B9 | IDS_DisplayName_6 | Corvette |
| 0x40E1C439 | IDS_DisplayName_7 | Viper |
| 0x40E1C339 | IDS_DisplayName_9 | Celica |
| 0x70E384A0 | IDS_DisplayName_10 | Eclipse |
| 0x70E38420 | IDS_DisplayName_11 | IS |
| 0x70E385A0 | IDS_DisplayName_12 | Silvia |
| 0x70E38520 | IDS_DisplayName_13 | Charger |
| 0x70E386A0 | IDS_DisplayName_14 | GTO, Monaro |
| 0x70E38620 | IDS_DisplayName_15 | Fairlady |
| 0x70E387A0 | IDS_DisplayName_16 | RX-7, RX-8 |
| 0x70E38720 | IDS_DisplayName_17 | Camaro |
| 0x70E380A0 | IDS_DisplayName_18 | Mustang |
| 0x70E38020 | IDS_DisplayName_19 | Impreza |
| 0x70E344A0 | IDS_DisplayName_20 | Evo |
| 0x70E34420 | IDS_DisplayName_21 | Skyline |
| 0x70E345A0 | IDS_DisplayName_22 | Supra |
| 0x70E34520 | IDS_DisplayName_23 | Focus |
| 0x70E346A0 | IDS_DisplayName_24 | Miata, MX-5 |
| 0x70E347A0 | IDS_DisplayName_26 | M3 |
| 0x70E34720 | IDS_DisplayName_27 | S4 |
| 0x70E340A0 | IDS_DisplayName_28 | Ferrari V8 |
| 0x70E34020 | IDS_DisplayName_29 | Firebird |
| 0x70E304A0 | IDS_DisplayName_30 | Challenger |
| 0x70E30420 | IDS_DisplayName_31 | CTS |
| 0x70E305A0 | IDS_DisplayName_32 | TT |
| 0x70E30520 | IDS_DisplayName_33 | AMG |
| 0x70E306A0 | IDS_DisplayName_34 | Gallardo |
| 0x70E30620 | IDS_DisplayName_35 | Murciélago |
| 0x70E307A0 | IDS_DisplayName_36 | Mazda 3 |
| 0x70E30720 | IDS_DisplayName_37 | M5 |
| 0x70E300A0 | IDS_DisplayName_38 | Hybrid |
| 0x70E30020 | IDS_DisplayName_39 | 911 |

### List_FamilySpecial — 5 door counts

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C739 | IDS_DisplayName_1 | None |
| 0x40E1C6B9 | IDS_DisplayName_2 | 2 Doors |
| 0x40E1C639 | IDS_DisplayName_3 | 3 Doors |
| 0x40E1C5B9 | IDS_DisplayName_4 | 4 Doors |
| 0x40E1C539 | IDS_DisplayName_5 | 5 Doors |

### CarClasses — 8 performance classes (16 entries: 8 names + 8 descriptions)

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C7B9 | IDS_DisplayName_0 | D |
| 0x40E1C739 | IDS_DisplayName_1 | C |
| 0x40E1C6B9 | IDS_DisplayName_2 | B |
| 0x40E1C639 | IDS_DisplayName_3 | A |
| 0x40E1C5B9 | IDS_DisplayName_4 | S1 |
| 0x40E1C539 | IDS_DisplayName_5 | S2 |
| 0x40E1C4B9 | IDS_DisplayName_6 | R |
| 0x40E1C439 | IDS_DisplayName_7 | X |

### List_LiveryMaterials — paint finishes (62 display names, 168 total entries)

Display names (IDS_DisplayName_*), keyed by finish-code numeric suffix:

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C739 | IDS_DisplayName_1 | Gloss |
| 0x40E1C6B9 | IDS_DisplayName_2 | Semigloss |
| 0x40E1C639 | IDS_DisplayName_3 | Matte |
| 0x40E1C5B9 | IDS_DisplayName_4 | Metallic |
| 0x40E1C4B9 | IDS_DisplayName_6 | Carbon Fiber Matte |
| 0x40E1C439 | IDS_DisplayName_7 | Carbon Fiber Polished |
| 0x40E1C3B9 | IDS_DisplayName_8 | Carbon Kevlar Matte |
| 0x40E1C339 | IDS_DisplayName_9 | Carbon Kevlar Polished |
| 0x70E385A0 | IDS_DisplayName_12 | Chrome |
| 0x70E38520 | IDS_DisplayName_13 | Gold |
| 0x70E386A0 | IDS_DisplayName_14 | Aluminum Brushed |
| 0x70E387A0 | IDS_DisplayName_16 | Aluminum Polished |
| 0x70E38720 | IDS_DisplayName_17 | Aluminum Semigloss |
| 0x70E344A0 | IDS_DisplayName_20 | Blob Camo Desert Matte |
| 0x70E34420 | IDS_DisplayName_21 | Blob Camo Desert Polished |
| 0x70E345A0 | IDS_DisplayName_22 | Blob Camo Snow Matte |
| 0x70E34520 | IDS_DisplayName_23 | Blob Camo Snow Polished |
| 0x70E346A0 | IDS_DisplayName_24 | Blob Camo Woodland Matte |
| 0x70E34620 | IDS_DisplayName_25 | Blob Camo Woodland Polished |
| 0x70E347A0 | IDS_DisplayName_26 | Brass Brushed |
| 0x70E34720 | IDS_DisplayName_27 | Brass Polished |
| 0x70E340A0 | IDS_DisplayName_28 | Brass Semigloss |
| 0x70E30420 | IDS_DisplayName_31 | Copper Brushed |
| 0x70E305A0 | IDS_DisplayName_32 | Copper Polished |
| 0x70E30520 | IDS_DisplayName_33 | Copper Semigloss |
| 0x70E307A0 | IDS_DisplayName_36 | Digital Camo Desert Matte |
| 0x70E30720 | IDS_DisplayName_37 | Digital Camo Desert Polished |
| 0x70E300A0 | IDS_DisplayName_38 | Digital Camo Snow Matte |
| 0x70E30020 | IDS_DisplayName_39 | Digital Camo Snow Polished |
| 0x70E2C4A0 | IDS_DisplayName_40 | Digital Camo Woodland Matte |
| 0x70E2C420 | IDS_DisplayName_41 | Digital Camo Woodland Polished |
| 0x70E2C5A0 | IDS_DisplayName_42 | Spyshot Swirls |
| 0x70E2C520 | IDS_DisplayName_43 | Spyshot Triangles |
| 0x70E2C6A0 | IDS_DisplayName_44 | Steel Brushed |
| 0x70E2C620 | IDS_DisplayName_45 | Diamond Plate |
| 0x70E2C7A0 | IDS_DisplayName_46 | Steel Polished |
| 0x70E2C720 | IDS_DisplayName_47 | Steel Semigloss |
| 0x70E284A0 | IDS_DisplayName_50 | Two-Tone Matte |
| 0x70E28420 | IDS_DisplayName_51 | Two-Tone Polished |
| 0x70E285A0 | IDS_DisplayName_52 | Two-Tone Semigloss |
| 0x70E28520 | IDS_DisplayName_53 | Wood Dark |
| 0x70E287A0 | IDS_DisplayName_56 | Wood Light |
| 0x70E28020 | IDS_DisplayName_59 | Wood Medium |
| 0x70E244A0 | IDS_DisplayName_60 | Realistic Camo Woodland Matte |
| 0x70E24420 | IDS_DisplayName_61 | Realistic Camo Woodland Polished |
| 0x70E245A0 | IDS_DisplayName_62 | Realistic Camo Snow Polished |
| 0x70E24520 | IDS_DisplayName_63 | Realistic Camo Snow Matte |
| 0x70E246A0 | IDS_DisplayName_64 | Zinc |
| 0x70E24620 | IDS_DisplayName_65 | Prismacolor White |
| 0x70E247A0 | IDS_DisplayName_66 | Steel Damascus |
| 0x70E24720 | IDS_DisplayName_67 | Prismacolor Black |
| 0x70E240A0 | IDS_DisplayName_68 | Steel Galvanized |
| 0x70E24020 | IDS_DisplayName_69 | Candy Paint |
| 0x70E204A0 | IDS_DisplayName_70 | Metallic Low Flake |
| 0x70E20420 | IDS_DisplayName_71 | Metallic High Flake |
| 0x70E205A0 | IDS_DisplayName_72 | Metallic Glitter |

**Primary colour label** (`IDS_PrimaryColorDisplayName_*`): tells the user which
colour swatch applies (e.g. "Paint Color", "Tint", "Base Color", "Flake Color",
"Coat Color"). Empty string = no primary colour picker for that finish.

**Secondary colour label** (`IDS_SecondaryColorDisplayName_*`): same idea
(e.g. "Flake Color", "Highlight"). Empty string = no secondary picker.

### WheelCategories — 5 rim style categories

| Hash | Key | Value |
|------|-----|-------|
| 0x70F70E13 | IDS_Name_1 | Stock Rim Style |
| 0x70F70F93 | IDS_Name_2 | Sport Rim Style |
| 0x70F70F13 | IDS_Name_3 | Multi Piece Rim Style |
| 0x70F70C93 | IDS_Name_4 | Specialized Rim Style |
| 0x70F70C13 | IDS_Name_5 | All Rim Styles |

### CarBuckets — 49 entries (Car type bucket names)

| Hash | Key | Value |
|------|-----|-------|
| 0x70F70E13 | IDS_Name_1 | Cult Cars |
| 0x7B8711B8 | IDS_Name_10 | World Classics |
| 0x7B871138 | IDS_Name_11 | Modern Supercars |
| 0x7B8710B8 | IDS_Name_12 | Retro Supercars |
| 0x7B871038 | IDS_Name_13 | Hypercars |
| 0x7B8713B8 | IDS_Name_14 | Retro Super Saloons |
| 0x7B8712B8 | IDS_Name_16 | Utility Heroes |
| 0x7B871238 | IDS_Name_17 | Retro Sports Cars |
| 0x7B8715B8 | IDS_Name_18 | Modern Sports Cars |
| 0x7B871538 | IDS_Name_19 | Modern Super Saloons |
| 0x70F70F93 | IDS_Name_2 | GT Cars |
| 0x7B87D1B8 | IDS_Name_20 | Classic Racers |
| 0x7B87D138 | IDS_Name_21 | Cult Cars |
| 0x7B87D0B8 | IDS_Name_22 | Rare Classics |
| 0x7B87D038 | IDS_Name_23 | Hot Hatch |
| 0x7B87D3B8 | IDS_Name_24 | Retro Hot Hatch |
| 0x7B87D338 | IDS_Name_25 | Super Hot Hatch |
| 0x7B87D2B8 | IDS_Name_26 | Extreme Track Toys |
| 0x7B87D5B8 | IDS_Name_28 | Classic Muscle |
| 0x7B87D538 | IDS_Name_29 | Rods and Customs |
| 0x70F70F13 | IDS_Name_3 | Hot Hatch |
| 0x7B8791B8 | IDS_Name_30 | Retro Muscle |
| 0x7B879138 | IDS_Name_31 | Modern Muscle |
| 0x7B8790B8 | IDS_Name_32 | Retro Rally |
| 0x7B879038 | IDS_Name_33 | Classic Rally |
| 0x7B8793B8 | IDS_Name_34 | Rally Monsters |
| 0x7B879338 | IDS_Name_35 | Modern Rally |
| 0x7B8792B8 | IDS_Name_36 | GT Cars |
| 0x7B879238 | IDS_Name_37 | Super GT |
| 0x7B8795B8 | IDS_Name_38 | Unlimited Offroad |
| 0x7B879538 | IDS_Name_39 | Sports Utility Heroes |
| 0x70F70C93 | IDS_Name_4 | Iconic Rally |
| 0x7B8651B8 | IDS_Name_40 | Offroad |
| 0x7B865138 | IDS_Name_41 | Unlimited Buggies |
| 0x7B8650B8 | IDS_Name_42 | Classic Sports Cars |
| 0x7B865038 | IDS_Name_43 | Track Toys |
| 0x7B8653B8 | IDS_Name_44 | Vintage Racers |
| 0x7B865338 | IDS_Name_45 | Trucks |
| 0x7B8652B8 | IDS_Name_46 | Buggies |
| 0x7B865238 | IDS_Name_47 | Drift Cars |
| 0x7B8655B8 | IDS_Name_48 | Pickups & 4x4's |
| 0x7B865538 | IDS_Name_49 | UTV's |
| 0x70F70C13 | IDS_Name_5 | Muscle |
| 0x7B8611B8 | IDS_Name_50 | Eclectic Domestics |
| 0x7B861138 | IDS_Name_51 | Retro Racers |
| 0x70F70D93 | IDS_Name_6 | Offroad |
| 0x70F70D13 | IDS_Name_7 | Saloon Cars |
| 0x70F70A93 | IDS_Name_8 | Sports Cars |
| 0x70F70A13 | IDS_Name_9 | Supercars |

### CarDetails — 66 entries (Car detail field labels + help strings)

| Hash | Key | Value |
|------|-----|-------|
| 0x40E1C739 | IDS_DisplayName_1 | YEAR |
| 0x70E384A0 | IDS_DisplayName_10 | LAUNCH |
| 0x70E38420 | IDS_DisplayName_11 | COUNTRY OF ORIGIN |
| 0x70E38520 | IDS_DisplayName_13 | PARTS VALUE |
| 0x70E38720 | IDS_DisplayName_17 | ENGINE POSITION |
| 0x70E380A0 | IDS_DisplayName_18 | ENGINE ASPIRATION |
| 0x40E1C6B9 | IDS_DisplayName_2 | MAKE |
| 0x70E344A0 | IDS_DisplayName_20 | POWER |
| 0x70E34420 | IDS_DisplayName_21 | POWER RPM |
| 0x70E345A0 | IDS_DisplayName_22 | TORQUE |
| 0x70E34520 | IDS_DisplayName_23 | TORQUE RPM |
| 0x70E346A0 | IDS_DisplayName_24 | REDLINE RPM |
| 0x70E34620 | IDS_DisplayName_25 | NUMBER OF GEARS |
| 0x70E347A0 | IDS_DisplayName_26 | CURB WEIGHT |
| 0x70E34720 | IDS_DisplayName_27 | FRONT WEIGHT |
| 0x70E340A0 | IDS_DisplayName_28 | FRONT TIRES |
| 0x70E34020 | IDS_DisplayName_29 | REAR TIRES |
| 0x40E1C639 | IDS_DisplayName_3 | MODEL |
| 0x70E304A0 | IDS_DisplayName_30 | CAR CLASS |
| 0x70E30420 | IDS_DisplayName_31 | ENGINE TYPE |
| 0x70E305A0 | IDS_DisplayName_32 | CAR TYPE |
| 0x70E30520 | IDS_DisplayName_33 | BODY STYLE |
| 0x70E306A0 | IDS_DisplayName_34 | MODEL FAMILY |
| 0x70E30620 | IDS_DisplayName_35 | CAR VALUE |
| 0x70E307A0 | IDS_DisplayName_36 | SHARED OWNER |
| 0x70E30720 | IDS_DisplayName_37 | NUMBER OF RACES |
| 0x70E300A0 | IDS_DisplayName_38 | SHARED DATE |
| 0x40E1C5B9 | IDS_DisplayName_4 | PERFORMANCE INDEX |
| 0x40E1C539 | IDS_DisplayName_5 | DRIVE TYPE |
| 0x40E1C4B9 | IDS_DisplayName_6 | ACCELERATION |
| 0x40E1C439 | IDS_DisplayName_7 | TOP SPEED |
| 0x40E1C3B9 | IDS_DisplayName_8 | BRAKING |
| 0x40E1C339 | IDS_DisplayName_9 | HANDLING |
| 0xBC24DE47 | IDS_HelpString_1 | Model year of this car. |
| 0x126F3BDE | IDS_HelpString_10 | Launch rating help text. |
| 0x126F3B5E | IDS_HelpString_11 | The location of the manufacturer. |
| 0x126F3A5E | IDS_HelpString_13 | The value of the parts you've added to this vehicle. |
| 0x126F385E | IDS_HelpString_17 | Location of the engine in this car. This can affect the car's weight distribution. |
| 0x126F3FDE | IDS_HelpString_18 | In normally aspirated engines, air flows through an air filter and then into the cylinders. In turbocharged or supercharged cars, air entering the engine is first pressurized to increase performance. |
| 0xBC24DFC7 | IDS_HelpString_2 | Manufacturer of this car. |
| 0x126FFBDE | IDS_HelpString_20 | The maximum power of this car's engine. |
| 0x126FFB5E | IDS_HelpString_21 | Engine RPM at maximum power. |
| 0x126FFADE | IDS_HelpString_22 | The maximum torque of this car's engine. |
| 0x126FFA5E | IDS_HelpString_23 | Engine RPM at maximum torque. |
| 0x126FF9DE | IDS_HelpString_24 | Engine RPM at redline. |
| 0x126FF95E | IDS_HelpString_25 | The number of gears in this car's transmission. |
| 0x126FF8DE | IDS_HelpString_26 | The total weight of the car on the track, including a driver. |
| 0x126FF85E | IDS_HelpString_27 | The percentage of the car's total weight carried by the front tires. |
| 0x126FFFDE | IDS_HelpString_28 | Tire Width/Aspect Ratio/Rim Diameter. Tire width is measured in millimeters (mm). Aspect Ratio indicates the height of the tire as a percentage of its width. Rim Diameter is measured in inches. |
| 0x126FFF5E | IDS_HelpString_29 | Tire Width/Aspect Ratio/Rim Diameter. Tire width is measured in millimeters (mm). Aspect Ratio indicates the height of the tire as a percentage of its width. Rim Diameter is measured in inches. |
| 0xBC24DF47 | IDS_HelpString_3 | Model name of this car. |
| 0x126FBBDE | IDS_HelpString_30 | Car class help string goes here. |
| 0x126FBB5E | IDS_HelpString_31 | Help string goes here. |
| 0x126FBADE | IDS_HelpString_32 | Help string goes here. |
| 0x126FBA5E | IDS_HelpString_33 | Help string goes here. |
| 0x126FB9DE | IDS_HelpString_34 | Help string goes here. |
| 0x126FB95E | IDS_HelpString_35 | How much this car and its parts are worth. |
| 0x126FB8DE | IDS_HelpString_36 | Club member who shared this car. |
| 0x126FB85E | IDS_HelpString_37 | Number of times this car was raced in the club. |
| 0x126FBFDE | IDS_HelpString_38 | The date this car was originally shared to the club. |
| 0xBC24DCC7 | IDS_HelpString_4 | _(empty)_ |
| 0xBC24DC47 | IDS_HelpString_5 | FWD is Front-Wheel Drive, RWD is Rear-Wheel Drive, and AWD is All-Wheel Drive. |
| 0xBC24DDC7 | IDS_HelpString_6 | How quickly this car gains speed on a scale of 0 - 10 (10 being quickest). |
| 0xBC24DD47 | IDS_HelpString_7 | The top speed of this car on a scale of 0 - 10 (10 being fastest). |
| 0xBC24DAC7 | IDS_HelpString_8 | The stopping power of this car on a scale of 0 - 10 (10 being highest). |
| 0xBC24DA47 | IDS_HelpString_9 | How well this car corners on a scale of 0 - 10 (10 being best). |

### CarFlow — 262 entries (Car selection UI strings)

| Hash | Key | Value |
|------|-----|-------|
| 0x12039F0C | IDS_Add_To_Favorites | Add To Favorites |
| 0xFFCC96BB | IDS_Aftermarket_Title_Current | Current |
| 0xA08E74DC | IDS_Aftermarket_Title_Stats | Stats |
| 0xA14BB8DC | IDS_Aftermarket_Title_Stock | Stock |
| 0xAEF2D2EC | IDS_AllCars_Desc | Select a car to paint. Buying cars is not required, and all cars are available. |
| 0x6EAC97E9 | IDS_Auction_Car | Auction Car |
| 0x4DEC38B0 | IDS_Auction_Car_Successful | Your {0} was successfully listed in the Auction House! Watch the progress of this sale in My Auctions.   Please note, a small Sales Fee will be deducted from the final sale price. |
| 0x995533D9 | IDS_Badge_Hider | Hider |
| 0x326AE048 | IDS_Badge_Recommended | Recommended |
| 0xBB5C79AC | IDS_Badge_Seeker | Seeker |
| 0xC6DBA4D1 | IDS_BigShot_Upload_Failure | Your Big Shot photo was NOT successfully uploaded to the ForzaMotorsport.net server. Make sure you have a connection to Xbox network and try again later. |
| 0x34F1BF81 | IDS_BigShot_Upload_Success | Your Big Shot photo was successfully uploaded to the ForzaMotorsport.net server. You can retrieve it from there later. |
| 0x290FD6BC | IDS_BorrowLeaderCar_Desc | Borrow the leader’s car, including its Tuning Setup, for this Event. You will be returned to your own car upon finishing the Event. |
| 0x09EBD3D9 | IDS_BuyCarSuccessfulDescription | Car has been added to your garage. |
| 0x0B31D833 | IDS_Buy_Car_Garage_Full | Your Garage is full. You have no room for additional cars. |
| 0xFBE28546 | IDS_Buy_Car_QuickAdd_Free_Message | The following FREE cars will be added to your garage: |
| 0xF3BA80B3 | IDS_Buy_Car_QuickAdd_Free_Message_Singular | The following FREE car will be added to your garage: |
| 0x1FFC2D25 | IDS_Buy_Car_QuickAdd_Free_Title | Free Cars |
| 0x01B5D482 | IDS_Buy_Car_QuickAdd_Free_Title_Singular | Free Car |
| 0x5CD95DAB | IDS_Buy_Car_QuickAdd_Free_Wait_Message | Adding cars to garage. Please wait… |
| 0x5F27B23A | IDS_Buy_Car_QuickAdd_Message | The following cars will be added to your garage: |
| 0xA1D81B0D | IDS_Buy_Car_QuickAdd_Message_Singular | The following car will be added to your garage: |
| 0xC20EBE31 | IDS_Buy_Car_QuickAdd_Title | Purchase Cars |
| 0x6F4C9D08 | IDS_Buy_Car_QuickAdd_Title_Singular | Purchase Car |
| 0x4BE0F060 | IDS_Buy_Confirm_Msg_Free | Do you want to choose this car? |
| 0x0B82F466 | IDS_Buy_Confirm_Msg_IE | Do you want to select this car? |
| 0x1D87EB5F | IDS_Buy_Confirm_Title_Free | Confirm Choice |
| 0x677FADFA | IDS_Buy_Confirm_Title_IE | Select Car |
| 0x78C80126 | IDS_Cancel_Title | Cancel |
| 0x64C76869 | IDS_CarCollection | Car Collection |
| 0xBF9D09E5 | IDS_CarCollection_Accolade | [BOLD:{0}] accolade |
| 0x4E939CE4 | IDS_CarCollection_Autoshow | [BOLD:Autoshow] |
| 0x95B72793 | IDS_CarCollection_Autoshow_Navigate | Would you like to go to the Autoshow now? |
| 0x1D9BCD6F | IDS_CarCollection_BarnFind | There are rumors that this car might be found abandoned in a barn… |
| 0xC43DB84C | IDS_CarCollection_Campaign_Format | In the Collection Journal's [BOLD:'{0}'] category |
| 0xFA7D359C | IDS_CarCollection_CarCollectionReward | [BOLD:Car Collection] |
| 0x6EDF5703 | IDS_CarCollection_DLC | [BOLD:Store add-ons] |
| 0xF4200C94 | IDS_CarCollection_Exclusive | This is a hard to find car. |
| 0x660D913E | IDS_CarCollection_Format_UnlockedPlural | YOU'VE COLLECTED [HIGHLIGHT:{0}] MORE CARS! |
| 0x275D8C2A | IDS_CarCollection_Format_UnlockedSingular | YOU'VE COLLECTED [HIGHLIGHT:1] MORE CAR! |
| 0x55BD7FD4 | IDS_CarCollection_JumpToNextRewardPendingUnlock | Next New Reward |
| 0x7A43FC77 | IDS_CarCollection_Mastery | [BOLD:Car Mastery tree] |
| 0x0BC497F7 | IDS_CarCollection_Mission | [BOLD:{0}] mission |
| 0x6F950D03 | IDS_CarCollection_New | New |
| 0x054A04DF | IDS_CarCollection_NewCarCollected | New Car Collected! |
| 0x2809F491 | IDS_CarCollection_NewReward | New Manufacturer Bonus |
| 0x3B4BF3F0 | IDS_CarCollection_ObtainCar | This car is available from: |
| 0x841B6F80 | IDS_CarCollection_PlayerHouse | [BOLD:Property purchase] |
| 0xDA04F828 | IDS_CarCollection_PossiblePrize | It may appear as a reward in [BOLD:Season Events], or the [BOLD:Festival Playlist]. |
| 0x2DFCFF3A | IDS_CarCollection_PossiblePrize_Only | This car may appear as a reward in [BOLD:Season Events], or the [BOLD:Festival Playlist]. |
| 0xB45FF234 | IDS_CarCollection_PrizeUnlock | Prize Unlock |
| 0x2FED7B5C | IDS_CarCollection_RedeemReward | Redeem Reward! |
| 0x2F4EC098 | IDS_CarCollection_RewardObjectiveFormat | Collect [HIGHLIGHT:{0}] More {1} |
| 0xAC35FA55 | IDS_CarCollection_SeperatorFormat | {0}, {1} |
| 0xE338AD35 | IDS_CarCollection_Total | Total |
| 0x3AF37E52 | IDS_CarCollection_TreasureCar | Explore to find a clue to this abandoned car's whereabouts… |
| 0xCB5FDCC6 | IDS_CarCollection_ViewNewUnlocks | View New Unlocks |
| 0x55E96725 | IDS_CarCollection_View_Message | Press to view |
| 0xE70BEDD5 | IDS_CarCollection_Wheelspin | [BOLD:Wheelspin] |
| 0xAFC2ED36 | IDS_CarCollection_XOfY | {0}/{1} |
| 0x50D2E656 | IDS_CarDisplayMakeAndYear_Format | {0} {1} |
| 0x47C55A4C | IDS_CarFilter_Affordable | Affordable |
| 0xE963483D | IDS_CarFilter_Bucket | Car Type |
| 0x419BC698 | IDS_CarFilter_CarClass | Performance Class |
| 0xF0136385 | IDS_CarFilter_Deselected | Not Selected |
| 0x0F956AE8 | IDS_CarFilter_DriveType | Drive Type |
| 0x7BA6028B | IDS_CarFilter_Duplicate | Duplicates |
| 0xA59A1CF9 | IDS_CarFilter_Favorite | Favorites |
| 0xEC6B66D5 | IDS_CarFilter_HasNoSnowTires | Snow Tires Not Fitted |
| 0x0DE4BAD5 | IDS_CarFilter_HasPreset | Bodykits & Presets Available |
| 0x18ECD49F | IDS_CarFilter_HasSnowTires | Snow Tires Fitted |
| 0x38B5A380 | IDS_CarFilter_Invalid_Msg | No cars are available for the filter settings you have selected. |
| 0xCA9B3C64 | IDS_CarFilter_Invalid_Title | No Cars Available |
| 0x3B73A1E8 | IDS_CarFilter_Misc | Misc. |
| 0xFD625579 | IDS_CarFilter_NotOwned | Not Owned |
| 0x5A71460C | IDS_CarFilter_Owned | Owned |
| 0x6830E4A4 | IDS_CarFilter_PhotoCaptured | Captured |
| 0xBDFDDF17 | IDS_CarFilter_PhotoNotCaptured | Not Captured |
| 0xF9274E1C | IDS_CarFilter_Rarity | Rarity |
| 0xA3363428 | IDS_CarFilter_Selected | Selected |
| 0xD7FA20A4 | IDS_CarInfoLabel_InstructionText_RightOnly | Select |
| 0x3F630DB0 | IDS_CarInfo_Hider | Hider Vehicle |
| 0xA0434D7F | IDS_CarInfo_Seeker | Seeker Vehicle |
| 0x2A63107C | IDS_CarMeets_PurchaseQuery | Would you like to purchase this car from the [BOLD:Autoshow]? |
| 0xF63600C5 | IDS_CarNotSuitable_Blueprint_Description | Your car might not be suitable for this Event, would you like to choose another Event? |
| 0xD04D0814 | IDS_CarNotSuitable_Blueprint_Title | Choose Different Event? |
| 0xEF5B0BCD | IDS_CarNotSuitable_LongRoute_Description | This is a very long event, would you like to choose a more powerful car? |
| 0x124B5356 | IDS_CarNotSuitable_LongRoute_Title | Choose a More Powerful Car? |
| 0x68B20253 | IDS_CarNotSuitable_Offroad_Description | Your car might not be suitable for this offroad event, would you like to change car? |
| 0x75EAA914 | IDS_CarNotSuitable_Offroad_Title | Choose a More Suitable Offroad Car? |
| 0xFC469C8C | IDS_CarPass_ViewCar_Message | Do you want to view this car in the Autoshow? |
| 0x78D433B5 | IDS_CarPass_ViewCar_Title | View Car |
| 0x8BC9563D | IDS_CarSelected_UnreleasedCarFormat_Msg | This car will be released on {0}. |
| 0x5A47C300 | IDS_CarSelected_UnreleasedCarPassCarFormat_Msg | This car will be released on {0}. |
| 0xACF82283 | IDS_CarSelected_UnreleasedCarPassCarPurchaseFormat_Msg | Car Pass owners will receive this car on {0}.  The Forza Horizon 6 Car Pass is available now! |
| 0x5D28BBAC | IDS_CarSelected_UnreleasedCarPassCar_Msg | This car is part of the Car Pass and is not yet released. |
| 0x8C902503 | IDS_CarSelected_UnreleasedCarPassCar_Title | Unreleased Car |
| 0x125CD809 | IDS_CarSelected_UnreleasedCar_Msg | This car is not yet released. |
| 0x947976DE | IDS_CarSelected_UnreleasedCar_Title | Unreleased Car |
| 0x172BD3BD | IDS_Car_Filter_Title | Filter |
| 0x991CA58B | IDS_ChangeBlueprint | Change Event |
| 0x030E4A22 | IDS_ChangeCar | Change Car |
| 0xDD8C8FB7 | IDS_Class_Format | {0} Class |
| 0xF7A12826 | IDS_Class_Unknown | ? |
| 0xA565A236 | IDS_Continue | Continue |
| 0x0BAEB336 | IDS_Continue_Dont_Show | Continue, Don't Show This Message Again |
| 0xCE8034A1 | IDS_CreditsBand_Format | {0} Cr+ |
| 0xF13D63C4 | IDS_CreditsSuffix_Billions | b |
| 0xFE3D63C4 | IDS_CreditsSuffix_Millions | m |
| 0x0CAD20A6 | IDS_CreditsSuffix_None | _(empty)_ |
| 0x7FB27F23 | IDS_CreditsSuffix_Thousands | k |
| 0x9400EE30 | IDS_Deliver_Car_To_Player | Get In Car |
| 0x871A2724 | IDS_DetailedView | Detailed View |
| 0x5F2F6F38 | IDS_DragTyres | Drag Tires |
| 0x7487C15C | IDS_DragTyresShort | Drag |
| 0xC354DA6B | IDS_DriftSpringShort | Drift |
| 0x2D50A4C7 | IDS_DriftTyres | Drift Tires |
| 0xE7799EA7 | IDS_DriftTyresShort | Drift |
| 0x21199554 | IDS_EligibleAutoshowCars | All Eligible Cars |
| 0x77809CF4 | IDS_EligibleGarageCars | Eligible Garage Cars |
| 0x524BED93 | IDS_Garage_Full_Title | Garage Full |
| 0x92717877 | IDS_GetTrending_Desc | Analyzing trends. Please wait… |
| 0xF85FC9C9 | IDS_GetTrending_Title | Trending Cars |
| 0xD603515E | IDS_Get_In_Car | Get In Car |
| 0xD72AE00F | IDS_GoToMarketplace | Go to Marketplace |
| 0x25B1FB9C | IDS_HorizonSemiSlickTyres | Horizon Semi-Slick Tires |
| 0xC27DAA7C | IDS_Level_Format | Level {0} |
| 0x4CB2EB52 | IDS_Livery_Upload_Confirm | Would you like to share this with others by uploading it to the Forza Horizon Storefront? |
| 0xEF38D76A | IDS_Livery_Upload_Failure | Your Design was NOT successfully uploaded to the Forza Horizon Storefront. {0} |
| 0x1D12CC3A | IDS_Livery_Upload_Success | Your Design was successfully uploaded to the Forza Horizon Storefront. |
| 0x140BB401 | IDS_Livery_Upload_Title | Upload Design |
| 0x62DA5DA8 | IDS_LoyaltyRewards_Desc | The other reward cars will be added to your Garage. |
| 0xADCD2631 | IDS_LoyaltyRewards_Title | Loyalty Rewards |
| 0x52746D86 | IDS_ModelSelect_AllCars | ALL CARS |
| 0x2294CFE7 | IDS_ModelSelect_BorrowCars | Borrow Car |
| 0x5272092E | IDS_ModelSelect_BuyCars | Buy Car |
| 0x71D77AB2 | IDS_ModelSelect_IsCaptured | Captured |
| 0x06E4F601 | IDS_ModelSelect_IsCurrent | Current Car |
| 0xD6477D0B | IDS_ModelSelect_IsDiscover | Discover |
| 0x0C5E99F0 | IDS_ModelSelect_IsLoyaltyReward | Rewards |
| 0x7F3EE3E4 | IDS_ModelSelect_IsNotCaptured | Rumored |
| 0x61E515E6 | IDS_ModelSelect_IsRecommended | Recommended |
| 0x7593EFE3 | IDS_ModelSelect_IsTrending | Trending |
| 0x3E98F33A | IDS_ModelSelect_RentalCars | Rental Car |
| 0x78EEA2C4 | IDS_NotYourDestination | You can only take part in a Championship at the Horizon Open's current Destination. |
| 0x16B8B04B | IDS_NumericString_Format | {0}{1} |
| 0x9DC6E004 | IDS_Offer_Expires_Message | This offer expires when you leave the Autoshow! |
| 0x8AF5B5B4 | IDS_Offer_Expires_Title | Offer Expires |
| 0xF2D0442F | IDS_OffroadTyres | Offroad Tires |
| 0x1B7E99E1 | IDS_OffroadTyresShort | Offroad |
| 0x18871A63 | IDS_PIRange_Format | {0} - {1} |
| 0xE89246A3 | IDS_PI_Unknown | ? |
| 0xB6A5B6DD | IDS_Photo | Photo |
| 0x4A083962 | IDS_Photo_ConfirmExport_Msg | You are about to create a large photo and upload it to ForzaMotorsport.net. This process can take a bit of time. Are you sure you want to continue? |
| 0x0D5BE67C | IDS_Photo_ConfirmExport_Msg_WithHorizonPromo | You are about to create a large photo and upload it to ForzaMotorsport.net. This process can take a bit of time. Are you sure you want to continue?  Big Shot photos cannot be used for Horizon Promo. |
| 0x1D0B8167 | IDS_Photo_Upload_Confirm_Overwrite | You have already uploaded a Big Shot photo to ForzaMotorsport.net. Do you want to overwrite it? |
| 0x6FB97AA5 | IDS_Photo_Upload_Title | Upload Photo |
| 0x59AC8F0A | IDS_PurchaseCar | Purchase a Car |
| 0xE9263974 | IDS_PurchaseCarDesc | Purchase a new car from the Autoshow. |
| 0x6536764C | IDS_PurchaseStockCar | Purchase Stock Car |
| 0xA08B1D23 | IDS_Purchase_XPAccelerator | Purchase the XP Accelerator to level up faster! |
| 0xB2EEC2A4 | IDS_Purchase_XPAccelerator_Title | Purchase Accelerator |
| 0x3134BFD2 | IDS_Purchase_XPAccelerator_Waiting | Purchasing XP Accelerator. Please wait. |
| 0x2C5B0BEE | IDS_RaceSpringShort | Race |
| 0x9392FC7F | IDS_RallySpringShort | Rally |
| 0xA844954E | IDS_RallyTyres | Rally Tires |
| 0xCFD812EB | IDS_RallyTyresShort | Rally |
| 0xEC692F48 | IDS_Reclaim | Reclaim |
| 0x13A1A80D | IDS_Remove_From_Favorites | Remove From Favorites |
| 0xBC3DE3AC | IDS_RentalCar_Desc | Hire a car for free, but earn reduced XP and Credits. |
| 0xD7DFFA71 | IDS_Report_And_Remove_Livery | Report And Remove Livery |
| 0xE1EC5244 | IDS_SIDI_Treasure_StoreOrDrive_Drive | Start Driving |
| 0x8233C0C7 | IDS_SIDI_Treasure_StoreOrDrive_Msg | Do you want to drive this car? |
| 0x812D52FC | IDS_SIDI_Treasure_StoreOrDrive_Store | Store Car |
| 0x524AD2C5 | IDS_SIDI_Treasure_StoreOrDrive_Title | Collected! |
| 0x2A0275E4 | IDS_Sale_Format | {0}% OFF |
| 0x9D92CB27 | IDS_Select_An_Action | Select An Action |
| 0x0F6F51B6 | IDS_Sell_Car_Action | Remove Car From Garage |
| 0x476BDD57 | IDS_Sell_Car_Confirm | Are you sure you want to remove the selected car? |
| 0x44EFA373 | IDS_SemiSlickTyres | Semi-Slick Tires |
| 0xAA81A304 | IDS_SemiSlickTyresShort | Semi-Slick |
| 0x56F06C16 | IDS_ShortHand_Sale_Format | -{0}% |
| 0x5A1CC280 | IDS_SimpleView | Detailed View |
| 0x2D5ADD08 | IDS_SlickTyres | Slick Tires |
| 0xE72A50DF | IDS_SlickTyresShort | Slick |
| 0xC3332F33 | IDS_SnowTyres | Snow Tires |
| 0x9465C100 | IDS_SnowTyresShort | Snow |
| 0x618C0D03 | IDS_SortBy_BackstageDateAdded | Date Added |
| 0x0103740E | IDS_SortBy_Bucket | Car Type |
| 0xDCF2BEB9 | IDS_SortBy_Captured | Captured |
| 0x7E32B370 | IDS_SortBy_CarModel | Model |
| 0x8C440D95 | IDS_SortBy_Class | Performance Class |
| 0xD399D330 | IDS_SortBy_Country | Country |
| 0x1EA1826D | IDS_SortBy_Level | Level |
| 0x50504DEC | IDS_SortBy_Manufacturer | Manufacturer |
| 0xA55C958D | IDS_SortBy_RecentlyAdded | Recently Added |
| 0xCA2C89DB | IDS_SortBy_ReleaseDate | Release Date |
| 0xF7BE75DA | IDS_SortBy_Upper_BackstageDateAdded | Date Added |
| 0x268EED6D | IDS_SortBy_Upper_Bucket | Car Type |
| 0xBAAA775A | IDS_SortBy_Upper_Captured | Captured |
| 0x4A0B16A7 | IDS_SortBy_Upper_Class | Performance Class |
| 0x155562A3 | IDS_SortBy_Upper_Country | Country |
| 0xD8EE995F | IDS_SortBy_Upper_Level | Level |
| 0x6635C172 | IDS_SortBy_Upper_Manufacturer | Manufacturer |
| 0x979ADA96 | IDS_SortBy_Upper_RecentlyAdded | Recently Added |
| 0xF64042C2 | IDS_SortBy_Upper_ReleaseDate | Release Date |
| 0x9BAA9D0F | IDS_SortBy_Upper_Value | Value |
| 0xEFB416DF | IDS_SortBy_Upper_Year | Year |
| 0x5DE5863D | IDS_SortBy_Value | Value |
| 0x8A3888E9 | IDS_SortBy_Year | Year |
| 0xB6B64608 | IDS_Sort_Title | Sort Selection |
| 0x6343DE73 | IDS_SportSpringShort | Sport |
| 0x2B78A106 | IDS_SportTyres | Sport Tires |
| 0xD639B0AF | IDS_SportTyresShort | Sport |
| 0x9BC0A05E | IDS_Stat_100kmh | 0-100 kph |
| 0xB6914390 | IDS_Stat_60mph | 0-60 mph |
| 0x8754022D | IDS_Stat_Accel | Acceleration |
| 0xC78DC273 | IDS_Stat_AeroBalance | Aero Balance |
| 0xC9B038AB | IDS_Stat_AeroEfficiency | Aero Efficiency |
| 0xE0369645 | IDS_Stat_Braking | Braking |
| 0x58727F8C | IDS_Stat_Displacement | Displacement |
| 0xC7E636CE | IDS_Stat_Displacement_Upper | Displacement |
| 0x9CBC4083 | IDS_Stat_Drivetrain | Drivetrain |
| 0xFF1A09DD | IDS_Stat_Drivetrain_Upper | Drivetrain |
| 0xEB81B42B | IDS_Stat_Engine | Engine |
| 0x96D6CE14 | IDS_Stat_Front | Front |
| 0x552055F4 | IDS_Stat_Front_Upper | Front |
| 0x418F6D08 | IDS_Stat_Handling | Handling |
| 0x47CAB952 | IDS_Stat_LateralGs | Lateral G's |
| 0x24FD4EB0 | IDS_Stat_LateralGs_Upper | Lateral G's |
| 0xCB62D6D2 | IDS_Stat_Launch | Launch |
| 0x70EF07FC | IDS_Stat_Offroad | Offroad |
| 0x45D40DA5 | IDS_Stat_Power | Power |
| 0x6D60B123 | IDS_Stat_PowerToWeight | PWR |
| 0x5E2E92B8 | IDS_Stat_Power_Upper | Power |
| 0x4C8FA0FA | IDS_Stat_RoadBalance | Mechanical Balance |
| 0xB79406BC | IDS_Stat_Speed | Speed |
| 0x17B1C590 | IDS_Stat_Suspension | Suspension |
| 0xC90E47F1 | IDS_Stat_Suspension_Upper | Suspension |
| 0x9C11D531 | IDS_Stat_TopSpeed | Top Speed |
| 0x494CC1DF | IDS_Stat_TopSpeed_Upper | Top Speed |
| 0xB8873022 | IDS_Stat_Torque | Torque |
| 0x12D88D4D | IDS_Stat_Torque_Upper | Torque |
| 0xD46C4F6E | IDS_Stat_TyreCompound | Tire Compound |
| 0xBF25BCFE | IDS_Stat_TyreCompound_Upper | Tire Compound |
| 0x0A4074F3 | IDS_Stat_Weight | Weight |
| 0x0FCBCB86 | IDS_Stat_Weight_Upper | Weight |
| 0xEAB3D673 | IDS_StockSpringShort | Stock |
| 0xD72A50BF | IDS_StockTyresShort | Standard |
| 0xDF2C08B8 | IDS_StreetSpringShort | Street |
| 0x9997BAF3 | IDS_StreetTyres | Street Tires |
| 0x41416F02 | IDS_StreetTyresShort | Street |
| 0xA20AEA6D | IDS_ToggleDetail | Toggle Detail |
| 0xEAB0E994 | IDS_UnableToRegister | Unable to Register |
| 0xC5011073 | IDS_UnentitledDLCCarInGarage_Msg | This is a DLC car that you have not purchased from the Marketplace. |
| 0xD5CC85AD | IDS_View_Car | View Car |
| 0x90B654D5 | IDS_View_History | View History |
| 0x49AFA29E | IDS_VintageRaceTyres | Vintage Race Tires |
| 0xC081AC6C | IDS_VintageRaceTyresShort | Vintage Race |
| 0xFD4DA22C | IDS_VintageTyres | Vintage Tires |
| 0x6791A9F9 | IDS_VintageTyresShort | Vintage |
| 0x88943F96 | IDS_VintageWhiteWallTyres | Vintage White Wall Tires |

### CarHornCategory — 3 entries (Horn categories)

| Hash | Key | Value |
|------|-----|-------|
| 0xFFE9A144 | IDS_DisplayName_52789e2f87fe421ea58bb6236c2a0a2d | MUSICAL HORNS |
| 0xB093AA02 | IDS_DisplayName_8835eb77fd15414b895a87506d2636db | SOUND EFFECTS |
| 0xDD1EEFE0 | IDS_DisplayName_e7ddb50f2f4e4d1f91c77d25a40991e5 | STANDARD HORNS |

### Data_Car — 1302 entries (Car data labels)

| Hash | Key | Value |
|------|-----|-------|
| 0xE1240738 | IDS_DisplayName_1006 | FXX |
| 0xE12407B8 | IDS_DisplayName_1007 | CCGT |
| 0xE12400B8 | IDS_DisplayName_1009 | Lancer Evolution X GSR |
| 0xE12444B8 | IDS_DisplayName_1011 | M3 |
| 0xE1248438 | IDS_DisplayName_1020 | F50 GT |
| 0xE1248538 | IDS_DisplayName_1022 | 430 Scuderia |
| 0xE12485B8 | IDS_DisplayName_1023 | F40 Competizione |
| 0xE124C538 | IDS_DisplayName_1032 | 8C Competizione |
| 0xE124C638 | IDS_DisplayName_1034 | Celica GT-Four ST205 |
| 0xE1250438 | IDS_DisplayName_1040 | M1 |
| 0xE12504B8 | IDS_DisplayName_1041 | Mustang SVT Cobra R |
| 0xE1250538 | IDS_DisplayName_1042 | Skyline 2000GT-R |
| 0xE12506B8 | IDS_DisplayName_1045 | Firebird Trans Am GTA |
| 0xE1250738 | IDS_DisplayName_1046 | Viper SRT-10 ACR |
| 0xE1254538 | IDS_DisplayName_1052 | Ram SRT-10 |
| 0xE12540B8 | IDS_DisplayName_1059 | Z4 M Coupé |
| 0xE1258438 | IDS_DisplayName_1060 | IMPREZA WRX STI |
| 0xE12585B8 | IDS_DisplayName_1063 | Charger Daytona HEMI |
| 0xE1258638 | IDS_DisplayName_1064 | Camaro Z28 |
| 0xE12580B8 | IDS_DisplayName_1069 | Corvette ZR1 |
| 0xE1260738 | IDS_DisplayName_1086 | Focus RS |
| 0xE1264438 | IDS_DisplayName_1090 | SL 65 AMG Black Series |
| 0xE12645B8 | IDS_DisplayName_1093 | Corvette |
| 0xE10405B8 | IDS_DisplayName_1103 | 370Z |
| 0xE1040638 | IDS_DisplayName_1104 | 510 |
| 0xE10406B8 | IDS_DisplayName_1105 | DB5 |
| 0xE1040038 | IDS_DisplayName_1108 | RS200 Evolution |
| 0xE1044438 | IDS_DisplayName_1110 | MX-5 Miata |
| 0xE1048638 | IDS_DisplayName_1124 | Fiat 131 |
| 0xE1048738 | IDS_DisplayName_1126 | M5 |
| 0xE104C438 | IDS_DisplayName_1130 | 12C Coupé |
| 0xE104C4B8 | IDS_DisplayName_1131 | 458 Italia |
| 0xE1054438 | IDS_DisplayName_1150 | Giulia Sprint GTA Stradale |
| 0xE10546B8 | IDS_DisplayName_1155 | Cobra Daytona Coupe |
| 0xE105C4B8 | IDS_DisplayName_1171 | 599XX |
| 0xE105C5B8 | IDS_DisplayName_1173 | Murciélago LP 670-4 SV |
| 0xE105C6B8 | IDS_DisplayName_1175 | Zonda R |
| 0xE1060638 | IDS_DisplayName_1184 | RS 6 |
| 0xE1640438 | IDS_DisplayName_1200 | R8 LMS |
| 0xE1640638 | IDS_DisplayName_1204 | Megane RS 250 |
| 0xE16446B8 | IDS_DisplayName_1215 | NULL CAR |
| 0xE1644738 | IDS_DisplayName_1216 | RS 3 Sportback |
| 0xE1648438 | IDS_DisplayName_1220 | TT RS Coupé |
| 0xE16484B8 | IDS_DisplayName_1221 | Mazdaspeed 3 |
| 0xE16480B8 | IDS_DisplayName_1229 | Furai |
| 0xE164C4B8 | IDS_DisplayName_1231 | Golf R |
| 0xE16545B8 | IDS_DisplayName_1253 | M600 |
| 0xE1658438 | IDS_DisplayName_1260 | LFA |
| 0xE16580B8 | IDS_DisplayName_1269 | 2002 Turbo |
| 0xE165C438 | IDS_DisplayName_1270 | DMC-12 |
| 0xE165C538 | IDS_DisplayName_1272 | Escort RS Cosworth |
| 0xE165C5B8 | IDS_DisplayName_1273 | Civic Type R |
| 0xE165C738 | IDS_DisplayName_1276 | Firebird Trans Am |
| 0xE165C7B8 | IDS_DisplayName_1277 | Cuda 426 HEMI |
| 0xE165C038 | IDS_DisplayName_1278 | XB Falcon GT |
| 0xE1660538 | IDS_DisplayName_1282 | 240SX |
| 0xE16644B8 | IDS_DisplayName_1291 | El Camino Super Sport 454 |
| 0xE16645B8 | IDS_DisplayName_1293 | Sierra Cosworth RS500 |
| 0xE1664638 | IDS_DisplayName_1294 | Syclone |
| 0xE16646B8 | IDS_DisplayName_1295 | 037 Stradale |
| 0xE1664738 | IDS_DisplayName_1296 | 190 E 2.5-16 Evolution II |
| 0xE16640B8 | IDS_DisplayName_1299 | 242 Turbo Evolution |
| 0xE1440438 | IDS_DisplayName_1300 | Impala Super Sport 409 |
| 0xE14404B8 | IDS_DisplayName_1301 | D-Type |
| 0xE1444638 | IDS_DisplayName_1314 | F1 |
| 0xE14485B8 | IDS_DisplayName_1323 | WRX STI |
| 0xE144C438 | IDS_DisplayName_1330 | Camaro Z28 |
| 0xE144C538 | IDS_DisplayName_1332 | Dart HEMI Super Stock |
| 0xE144C6B8 | IDS_DisplayName_1335 | #55 Mazda 787B |
| 0xE1454438 | IDS_DisplayName_1350 | X5 M |
| 0xE1454538 | IDS_DisplayName_1352 | Coronet Super Bee |
| 0xE14546B8 | IDS_DisplayName_1355 | Mustang GT Coupe |
| 0xE14587B8 | IDS_DisplayName_1367 | M5 |
| 0xE1458038 | IDS_DisplayName_1368 | M5 |
| 0xE14580B8 | IDS_DisplayName_1369 | Zonda Cinque Roadster |
| 0xE145C738 | IDS_DisplayName_1376 | Elise Series 1 Sport 190 |
| 0xE145C0B8 | IDS_DisplayName_1379 | Impala Super Sport |
| 0xE14604B8 | IDS_DisplayName_1381 | Galant VR-4 |
| 0xE1460538 | IDS_DisplayName_1382 | LEGACY RS |
| 0xE1460038 | IDS_DisplayName_1388 | M5 |
| 0xE1464538 | IDS_DisplayName_1392 | Sesto Elemento |
| 0xE14645B8 | IDS_DisplayName_1393 | 155 Q4 |
| 0xE1464638 | IDS_DisplayName_1394 | Typhoon |
| 0xE14646B8 | IDS_DisplayName_1395 | MR2 SC |
| 0xE14647B8 | IDS_DisplayName_1397 | Agera |
| 0xE1464038 | IDS_DisplayName_1398 | Aventador LP700-4 |
| 0xE1A447B8 | IDS_DisplayName_1417 | RS 5 Coupé |
| 0xE1A44038 | IDS_DisplayName_1418 | M5 |
| 0xE1A48738 | IDS_DisplayName_1426 | RX-8 R3 |
| 0xE1A48038 | IDS_DisplayName_1428 | Scirocco R |
| 0xE1A480B8 | IDS_DisplayName_1429 | Nova Super Sport 396 |
| 0xE1A4C6B8 | IDS_DisplayName_1435 | Beetle |
| 0xE1A540B8 | IDS_DisplayName_1459 | Bel Air |
| 0xE1A5C7B8 | IDS_DisplayName_1477 | Transit SuperSportVan |
| 0xE1A5C038 | IDS_DisplayName_1478 | #2 Audi Sport quattro S1 |
| 0xE1A60438 | IDS_DisplayName_1480 | RX-7 GSL-SE |
| 0xE1A604B8 | IDS_DisplayName_1481 | 3000 MKIII |
| 0xE1A645B8 | IDS_DisplayName_1493 | 850CSi |
| 0xE1840438 | IDS_DisplayName_1500 | C 63 AMG Coupé Black Series |
| 0xE18445B8 | IDS_DisplayName_1513 | Ghibli Cup |
| 0xE1844638 | IDS_DisplayName_1514 | RX-3 |
| 0xE18447B8 | IDS_DisplayName_1517 | Celica GT-Four RC ST185 |
| 0xE1848538 | IDS_DisplayName_1522 | Wrangler Rubicon |
| 0xE18480B8 | IDS_DisplayName_1529 | Capri RS3100 |
| 0xE184C538 | IDS_DisplayName_1532 | Venom GT |
| 0xE184C5B8 | IDS_DisplayName_1533 | Torana A9X |
| 0xE184C7B8 | IDS_DisplayName_1537 | Corolla SR5 |
| 0xE18500B8 | IDS_DisplayName_1549 | 33 Stradale |
| 0xE18540B8 | IDS_DisplayName_1559 | 300 SLR |
| 0xE1858538 | IDS_DisplayName_1562 | Viper GTS |
| 0xE1858638 | IDS_DisplayName_1564 | Corvette |
| 0xE1858038 | IDS_DisplayName_1568 | Civic RS |
| 0xE185C6B8 | IDS_DisplayName_1575 | Monte Carlo Super Sport |
| 0xE185C038 | IDS_DisplayName_1578 | 250 GT Berlinetta Lusso |
| 0xE1860738 | IDS_DisplayName_1586 | Continental |
| 0xE18607B8 | IDS_DisplayName_1587 | Cosmo 110S Series II |
| 0xE18644B8 | IDS_DisplayName_1591 | 205 Turbo 16 |
| 0xE1864038 | IDS_DisplayName_1598 | M3 GTS |
| 0xE18640B8 | IDS_DisplayName_1599 | 599XX Evolution |
| 0xE1E404B8 | IDS_DisplayName_1601 | Gallardo LP570-4 Spyder Performante |
| 0xE1E407B8 | IDS_DisplayName_1607 | RS 4 Avant |
| 0xE1E487B8 | IDS_DisplayName_1627 | G 65 AMG |
| 0xE1E54438 | IDS_DisplayName_1650 | Civic Si |
| 0xE1E544B8 | IDS_DisplayName_1651 | Atom 500 V8 |
| 0xE1E54638 | IDS_DisplayName_1654 | Mustang Shelby GT500 |
| 0xE1E546B8 | IDS_DisplayName_1655 | BRZ |
| 0xE1E54038 | IDS_DisplayName_1658 | A 45 AMG |
| 0xE1E584B8 | IDS_DisplayName_1661 | Delta S4 |
| 0xE1E58538 | IDS_DisplayName_1662 | Cooper S |
| 0xE1E587B8 | IDS_DisplayName_1667 | P1 |
| 0xE1E58038 | IDS_DisplayName_1668 | Mustang Boss 302 |
| 0xD1240538 | IDS_DisplayName_2002 | GT-R Black Edition (R35) |
| 0xD12405B8 | IDS_DisplayName_2003 | John Cooper Works GP |
| 0xD1240638 | IDS_DisplayName_2004 | MX-5 |
| 0xD1240738 | IDS_DisplayName_2006 | Corvette ZR-1 |
| 0xD12407B8 | IDS_DisplayName_2007 | 86 |
| 0xD12400B8 | IDS_DisplayName_2009 | RS 7 Sportback |
| 0xD1244438 | IDS_DisplayName_2010 | R8 Coupé V10 plus 5.2 FSI quattro |
| 0xD12447B8 | IDS_DisplayName_2017 | 595 esseesse |
| 0xD12440B8 | IDS_DisplayName_2019 | Focus RS |
| 0xD124C638 | IDS_DisplayName_2034 | LaFerrari |
| 0xD124C038 | IDS_DisplayName_2038 | 4C |
| 0xD1250438 | IDS_DisplayName_2040 | Mono |
| 0xD1250538 | IDS_DisplayName_2042 | Veneno |
| 0xD10440B8 | IDS_DisplayName_2119 | Civic CRX Mugen |
| 0xD10484B8 | IDS_DisplayName_2121 | Prelude Si |
| 0xD1048038 | IDS_DisplayName_2128 | XTS Limousine |
| 0xD104C4B8 | IDS_DisplayName_2131 | GEN-F GTS |
| 0xD104C5B8 | IDS_DisplayName_2133 | i8 |
| 0xD1050438 | IDS_DisplayName_2140 | BRAT GL |
| 0xD1050538 | IDS_DisplayName_2142 | Golf R |
| 0xD10506B8 | IDS_DisplayName_2145 | Ranger T6 Rally Raid |
| 0xD10507B8 | IDS_DisplayName_2147 | Metro 6R4 |
| 0xD1050038 | IDS_DisplayName_2148 | X-Raid All4 Racing Countryman |
| 0xD10500B8 | IDS_DisplayName_2149 | Clio Williams |
| 0xD10544B8 | IDS_DisplayName_2151 | Type 2 De Luxe |
| 0xD1054638 | IDS_DisplayName_2154 | M4 Coupé |
| 0xD10584B8 | IDS_DisplayName_2161 | Giulia TZ2 |
| 0xD10585B8 | IDS_DisplayName_2163 | Civic Type R |
| 0xD1058638 | IDS_DisplayName_2164 | Huracán LP 610-4 |
| 0xD1058038 | IDS_DisplayName_2168 | WRX STI |
| 0xD105C4B8 | IDS_DisplayName_2171 | Mazdaspeed MX-5 |
| 0xD105C6B8 | IDS_DisplayName_2175 | RC F |
| 0xD105C7B8 | IDS_DisplayName_2177 | Corvette Z06 |
| 0xD105C038 | IDS_DisplayName_2178 | RS 4 Avant |
| 0xD105C0B8 | IDS_DisplayName_2179 | S1 |
| 0xD1060438 | IDS_DisplayName_2180 | RS 6 Avant |
| 0xD10605B8 | IDS_DisplayName_2183 | Camaro Z/28 |
| 0xD1060638 | IDS_DisplayName_2184 | 458 Speciale |
| 0xD1060038 | IDS_DisplayName_2188 | One:1 |
| 0xD16406B8 | IDS_DisplayName_2205 | S800 |
| 0xD1644738 | IDS_DisplayName_2216 | Fury |
| 0xD16447B8 | IDS_DisplayName_2217 | SVX |
| 0xD1650538 | IDS_DisplayName_2242 | GT S |
| 0xD1658538 | IDS_DisplayName_2262 | ATS-V |
| 0xD16585B8 | IDS_DisplayName_2263 | Challenger SRT Hellcat |
| 0xD16587B8 | IDS_DisplayName_2267 | MX-5 |
| 0xD165C438 | IDS_DisplayName_2270 | Skyline H/T 2000GT-R |
| 0xD165C538 | IDS_DisplayName_2272 | 2000 Roadster |
| 0xD1664438 | IDS_DisplayName_2290 | 918 Spyder |
| 0xD16647B8 | IDS_DisplayName_2297 | 911 GT3 RS 4.0 |
| 0xD14547B8 | IDS_DisplayName_2357 | Focus RS |
| 0xD14585B8 | IDS_DisplayName_2363 | GT |
| 0xD145C4B8 | IDS_DisplayName_2371 | FXX K |
| 0xD145C538 | IDS_DisplayName_2372 | De Luxe Five-Window Coupe |
| 0xD1A40438 | IDS_DisplayName_2400 | Mustang Shelby GT350R |
| 0xD1A44538 | IDS_DisplayName_2412 | Isetta 300 Export |
| 0xD1A44738 | IDS_DisplayName_2416 | Manx |
| 0xD1A48438 | IDS_DisplayName_2420 | Manta 400 |
| 0xD1A484B8 | IDS_DisplayName_2421 | CTS-V Sedan |
| 0xD1A48538 | IDS_DisplayName_2422 | Limited Edition Gen-F GTS Maloo |
| 0xD1A4C438 | IDS_DisplayName_2430 | Nomad |
| 0xD1A587B8 | IDS_DisplayName_2467 | 488 GTB |
| 0xD1A58038 | IDS_DisplayName_2468 | Charger SRT Hellcat |
| 0xD1A580B8 | IDS_DisplayName_2469 | Sports 800 |
| 0x71A34BB8 | IDS_DisplayName_247 | 2000GT |
| 0xD1A5C438 | IDS_DisplayName_2470 | Vulcan |
| 0xD1A5C4B8 | IDS_DisplayName_2471 | C 63 S Coupé |
| 0xD1A5C538 | IDS_DisplayName_2472 | 570S Coupé |
| 0xD1A5C5B8 | IDS_DisplayName_2473 | R8 V10 plus |
| 0xD1A60738 | IDS_DisplayName_2486 | RXC Turbo |
| 0xD1A600B8 | IDS_DisplayName_2489 | 695 Biposto |
| 0x71A34CB8 | IDS_DisplayName_249 | 250 GTO |
| 0xD1A64638 | IDS_DisplayName_2494 | Range Rover Sport SVR |
| 0xD18407B8 | IDS_DisplayName_2507 | 150 Utility Sedan |
| 0x71A308B8 | IDS_DisplayName_251 | 300 SL Coupé |
| 0xD1844538 | IDS_DisplayName_2512 | Skyline GTS-R |
| 0xD18446B8 | IDS_DisplayName_2515 | The Cholla |
| 0xD18447B8 | IDS_DisplayName_2517 | #11 Rockstar F-150 Trophy Truck |
| 0xD1848738 | IDS_DisplayName_2526 | Regera |
| 0xD18487B8 | IDS_DisplayName_2527 | DB11 |
| 0x71A309B8 | IDS_DisplayName_253 | F355 Berlinetta |
| 0xD184C638 | IDS_DisplayName_2534 | 968 Turbo S |
| 0xD184C6B8 | IDS_DisplayName_2535 | 928 GTS |
| 0xD1850538 | IDS_DisplayName_2542 | Giulia Quadrifoglio |
| 0xD1850638 | IDS_DisplayName_2544 | Viper ACR |
| 0xD18500B8 | IDS_DisplayName_2549 | #3 917 LH |
| 0x71A30AB8 | IDS_DisplayName_255 | 512 TR |
| 0xD18544B8 | IDS_DisplayName_2551 | FPV Limited Edition Pursuit Ute |
| 0xD1854538 | IDS_DisplayName_2552 | Class 10 Race Car |
| 0xD18585B8 | IDS_DisplayName_2563 | Supervan III |
| 0xD1858738 | IDS_DisplayName_2566 | FJ40 |
| 0xD1858038 | IDS_DisplayName_2568 | Class 5/1600 Baja Bug |
| 0xD18580B8 | IDS_DisplayName_2569 | Evolution Coupe 1020 |
| 0xD185C638 | IDS_DisplayName_2574 | M12S Warthog CST |
| 0xD185C7B8 | IDS_DisplayName_2577 | F12tdf |
| 0x71A3C838 | IDS_DisplayName_260 | 911 Carrera RS |
| 0x71A3C8B8 | IDS_DisplayName_261 | 911 GT2 |
| 0xD1E445B8 | IDS_DisplayName_2613 | Jimmy |
| 0xD1E44638 | IDS_DisplayName_2614 | Mustang GT 2+2 Fastback |
| 0xD1E44738 | IDS_DisplayName_2616 | Centenario LP 770-4 |
| 0xD1E44038 | IDS_DisplayName_2618 | GT-R (R35) |
| 0x71A3C938 | IDS_DisplayName_262 | 911 GT3 |
| 0xD1E486B8 | IDS_DisplayName_2625 | Bentayga |
| 0xD1E48038 | IDS_DisplayName_2628 | M4 GTS |
| 0xD1E4C738 | IDS_DisplayName_2636 | #1 T100 Baja Truck |
| 0xD1E507B8 | IDS_DisplayName_2647 | Huayra BC Coupe |
| 0xD1E500B8 | IDS_DisplayName_2649 | Crown Victoria Police Interceptor |
| 0x71A3CAB8 | IDS_DisplayName_265 | 911 Turbo 3.3 |
| 0xD1E54538 | IDS_DisplayName_2652 | Montero Evolution |
| 0xD1E54638 | IDS_DisplayName_2654 | GT R |
| 0xD1E540B8 | IDS_DisplayName_2659 | Silvia K's Aero |
| 0xD1E585B8 | IDS_DisplayName_2663 | #37 Polaris RZR Pro 2 Truck |
| 0x71A3CC38 | IDS_DisplayName_268 | 944 Turbo |
| 0x71A3CCB8 | IDS_DisplayName_269 | 959 |
| 0xD1C444B8 | IDS_DisplayName_2711 | 3 Traffic |
| 0xD1C44538 | IDS_DisplayName_2712 | Galant Traffic |
| 0xD1C445B8 | IDS_DisplayName_2713 | Box Truck |
| 0xD1C44638 | IDS_DisplayName_2714 | Bus |
| 0xD1C4C038 | IDS_DisplayName_2738 | NISMO GT-R LM |
| 0xD1C4C0B8 | IDS_DisplayName_2739 | Camaro ZL1 |
| 0xD1C50438 | IDS_DisplayName_2740 | 124 Spider |
| 0xD1C50538 | IDS_DisplayName_2742 | Trailcat |
| 0xD1C505B8 | IDS_DisplayName_2743 | Land Cruiser Arctic Trucks AT37 |
| 0xD1C506B8 | IDS_DisplayName_2745 | Ridgeline Baja Trophy Truck |
| 0xD1C546B8 | IDS_DisplayName_2755 | 911 GT2 RS |
| 0xD1C5C5B8 | IDS_DisplayName_2773 | Cayenne Turbo |
| 0xD1C64538 | IDS_DisplayName_2792 | #2 GT40 Mk II |
| 0xD1C645B8 | IDS_DisplayName_2793 | #24 Ferrari Spa 330 P4 |
| 0xD1C64638 | IDS_DisplayName_2794 | 911 Turbo S Leichtbau |
| 0xD02404B8 | IDS_DisplayName_2801 | #11 Tomica Skyline Turbo Super Silhouette |
| 0x71A048B8 | IDS_DisplayName_281 | Barracuda Formula S |
| 0xD0248538 | IDS_DisplayName_2822 | Safari Turbo |
| 0xD02486B8 | IDS_DisplayName_2825 | Elise GT1 |
| 0xD02504B8 | IDS_DisplayName_2841 | Grand Cherokee Trackhawk |
| 0xD025C438 | IDS_DisplayName_2870 | Civic Type R |
| 0xD025C4B8 | IDS_DisplayName_2871 | Maverick X RS Turbo R |
| 0xD025C538 | IDS_DisplayName_2872 | Veloster N |
| 0x71A04CB8 | IDS_DisplayName_289 | Camaro Super Sport Coupe |
| 0xD0040538 | IDS_DisplayName_2902 | Flatbed |
| 0xD00405B8 | IDS_DisplayName_2903 | LEGACY B4 2.0 GT Traffic |
| 0xD00400B8 | IDS_DisplayName_2909 | Challenger SRT Demon |
| 0xD0044438 | IDS_DisplayName_2910 | Agera RS |
| 0x71A00938 | IDS_DisplayName_292 | Carrera GT |
| 0xD004C6B8 | IDS_DisplayName_2935 | F9 |
| 0xD004C7B8 | IDS_DisplayName_2937 | #14 Rahal Letterman Lanigan Racing Fiesta |
| 0x71A00AB8 | IDS_DisplayName_295 | Celica Sport Specialty II |
| 0x71A00B38 | IDS_DisplayName_296 | Cerbera Speed 12 |
| 0xD0058038 | IDS_DisplayName_2968 | Valkyrie |
| 0xD005C638 | IDS_DisplayName_2974 | 812 Superfast |
| 0xD0060738 | IDS_DisplayName_2986 | Unimog U5023 |
| 0xD00607B8 | IDS_DisplayName_2987 | P50 |
| 0x71A00CB8 | IDS_DisplayName_299 | Chevelle Super Sport 454 |
| 0xD0064538 | IDS_DisplayName_2992 | Lightweight E-Type |
| 0xD00645B8 | IDS_DisplayName_2993 | Griffith |
| 0xD00646B8 | IDS_DisplayName_2995 | Golf GTI |
| 0xD0064738 | IDS_DisplayName_2996 | #13 Ford Mustang |
| 0xD00647B8 | IDS_DisplayName_2997 | #530 HSV Maloo GEN-F |
| 0xC1240438 | IDS_DisplayName_3000 | #777 Nissan 240SX |
| 0xC12405B8 | IDS_DisplayName_3003 | #43 Dodge Viper SRT-10 ACR |
| 0xC12407B8 | IDS_DisplayName_3007 | #34 Andretti Rally Cross Beetle |
| 0x71824938 | IDS_DisplayName_302 | Civic Type R |
| 0xC124C4B8 | IDS_DisplayName_3031 | #185 959 Prodrive Rally Raid |
| 0xC124C6B8 | IDS_DisplayName_3035 | X-Bow GT4 |
| 0xC124C7B8 | IDS_DisplayName_3037 | #98 BMW 325i |
| 0xC12544B8 | IDS_DisplayName_3051 | M-Sport Fiesta RS |
| 0xC12546B8 | IDS_DisplayName_3055 | C-X75 |
| 0x71824B38 | IDS_DisplayName_306 | Cobra 427 S/C |
| 0xC1258538 | IDS_DisplayName_3062 | 512 S |
| 0xC12585B8 | IDS_DisplayName_3063 | X-Class |
| 0xC1258638 | IDS_DisplayName_3064 | GT 4-Door Coupé |
| 0xC125C538 | IDS_DisplayName_3072 | 911 GT3 RS |
| 0xC1260538 | IDS_DisplayName_3082 | MC12 Versione Corsa |
| 0xC12607B8 | IDS_DisplayName_3087 | 650S Spider |
| 0xC1260038 | IDS_DisplayName_3088 | Chevrolet Silverado 1500 Drift Truck |
| 0x71824CB8 | IDS_DisplayName_309 | Corrado VR6 |
| 0xC12644B8 | IDS_DisplayName_3091 | Vantage |
| 0xC10407B8 | IDS_DisplayName_3107 | G 63 AMG 6x6 |
| 0xC1040038 | IDS_DisplayName_3108 | Mustang RTR Spec 5 |
| 0xC1044438 | IDS_DisplayName_3110 | Jeep Wrangler Unlimited |
| 0xC10447B8 | IDS_DisplayName_3117 | 718 Cayman GTS |
| 0xC1044038 | IDS_DisplayName_3118 | Corvette ZR1 |
| 0x71820938 | IDS_DisplayName_312 | Corvette Stingray 427 |
| 0xC1048438 | IDS_DisplayName_3120 | Urus |
| 0xC10485B8 | IDS_DisplayName_3123 | 911 Carrera S |
| 0xC1048038 | IDS_DisplayName_3128 | #25 'Brocky' Ultra4 Bronco RTR |
| 0xC10480B8 | IDS_DisplayName_3129 | Mégane R26.R |
| 0xC104C538 | IDS_DisplayName_3132 | X-Raid John Cooper Works Buggy |
| 0xC104C638 | IDS_DisplayName_3134 | Megane R.S. |
| 0x71820A38 | IDS_DisplayName_314 | Corvette Z06 |
| 0xC10504B8 | IDS_DisplayName_3141 | Intensa Emozione |
| 0xC10500B8 | IDS_DisplayName_3149 | Camaro ZL1 1LE |
| 0x71820AB8 | IDS_DisplayName_315 | Corvette ZR-1 |
| 0xC10545B8 | IDS_DisplayName_3153 | 600LT Coupé |
| 0xC1054738 | IDS_DisplayName_3156 | Speedtail |
| 0x71820B38 | IDS_DisplayName_316 | Countach LP5000 QV |
| 0xC105C438 | IDS_DisplayName_3170 | Supervan 3 |
| 0xC105C5B8 | IDS_DisplayName_3173 | Z4 Roadster |
| 0xC105C738 | IDS_DisplayName_3176 | AMG Hammer Coupe |
| 0xC1060438 | IDS_DisplayName_3180 | 205 Rallye |
| 0xC1060638 | IDS_DisplayName_3184 | #5 Escort RS1800 MkII |
| 0xC10606B8 | IDS_DisplayName_3185 | DBS Superleggera |
| 0xC10607B8 | IDS_DisplayName_3187 | Macan LPR Rally Raid |
| 0xC10600B8 | IDS_DisplayName_3189 | Ford F-150 VelociRaptor 6X6 |
| 0xC1064438 | IDS_DisplayName_3190 | F-150 SVT Lightning |
| 0xC1064038 | IDS_DisplayName_3198 | Pantera GT5 |
| 0x7182C838 | IDS_DisplayName_320 | CR-X SiR |
| 0xC16444B8 | IDS_DisplayName_3211 | Vulcan AMR Pro |
| 0xC1644538 | IDS_DisplayName_3212 | TSR-S |
| 0xC1644638 | IDS_DisplayName_3214 | #70 Porsche Motorsport 935 |
| 0xC16486B8 | IDS_DisplayName_3225 | Portofino |
| 0xC1648738 | IDS_DisplayName_3226 | J50 |
| 0xC16487B8 | IDS_DisplayName_3227 | 488 Pista |
| 0xC1648038 | IDS_DisplayName_3228 | Racing Puma |
| 0x7182C9B8 | IDS_DisplayName_323 | Delta HF Integrale EVO |
| 0xC164C538 | IDS_DisplayName_3232 | #777 Chevrolet Corvette |
| 0xC164C6B8 | IDS_DisplayName_3235 | Pickup LX |
| 0x7182CA38 | IDS_DisplayName_324 | Diablo GTR |
| 0xC16504B8 | IDS_DisplayName_3241 | WRX STI ARX Supercar |
| 0xC16500B8 | IDS_DisplayName_3249 | #117 599 GTB Fiorano |
| 0x7182CAB8 | IDS_DisplayName_325 | Diablo SV |
| 0xC1654438 | IDS_DisplayName_3250 | E 63 S |
| 0xC16546B8 | IDS_DisplayName_3255 | JT |
| 0xC16547B8 | IDS_DisplayName_3257 | Pulsar GTI-R |
| 0x7182CB38 | IDS_DisplayName_326 | Dino 246 GT |
| 0x7182CBB8 | IDS_DisplayName_327 | Eclipse GSX |
| 0xC165C7B8 | IDS_DisplayName_3277 | Mustang Shelby GT500 |
| 0xC16607B8 | IDS_DisplayName_3287 | Sport XJR-15 |
| 0xC1660038 | IDS_DisplayName_3288 | 962CR |
| 0xC16600B8 | IDS_DisplayName_3289 | Aventador SVJ |
| 0xC16645B8 | IDS_DisplayName_3293 | XJ220S TWR |
| 0xC1440638 | IDS_DisplayName_3304 | SE 048SP |
| 0xC14407B8 | IDS_DisplayName_3307 | 370Z Nismo |
| 0xC14444B8 | IDS_DisplayName_3311 | FXX-K Evo |
| 0xC1444538 | IDS_DisplayName_3312 | Monza SP2 |
| 0xC14446B8 | IDS_DisplayName_3315 | Jesko |
| 0xC1444038 | IDS_DisplayName_3318 | RS 4 Avant |
| 0xC14486B8 | IDS_DisplayName_3325 | DB7 GT |
| 0x718289B8 | IDS_DisplayName_333 | Enzo Ferrari |
| 0xC14540B8 | IDS_DisplayName_3359 | RS e-tron GT |
| 0x71828B38 | IDS_DisplayName_336 | E-type |
| 0xC14585B8 | IDS_DisplayName_3363 | #23 Pennzoil NISMO Skyline GT-R |
| 0xC1458638 | IDS_DisplayName_3364 | Valhalla Concept Car |
| 0xC14587B8 | IDS_DisplayName_3367 | F8 Tributo |
| 0xC14580B8 | IDS_DisplayName_3369 | Corvette Stingray Coupe |
| 0xC145C4B8 | IDS_DisplayName_3371 | Huracán EVO |
| 0xC145C5B8 | IDS_DisplayName_3373 | 4Runner TRD Pro |
| 0xC145C638 | IDS_DisplayName_3374 | Tacoma TRD Pro |
| 0x71828C38 | IDS_DisplayName_338 | F1 GT |
| 0xC14646B8 | IDS_DisplayName_3395 | 8 Gordini |
| 0x71834838 | IDS_DisplayName_340 | F40 |
| 0xC1A40438 | IDS_DisplayName_3400 | #99 Mazda RX-8 |
| 0xC1A40538 | IDS_DisplayName_3402 | GR Supra |
| 0xC1A40638 | IDS_DisplayName_3404 | #2069 Ford Performance Bronco R |
| 0xC1A444B8 | IDS_DisplayName_3411 | #34 Toyota Supra MkIV |
| 0xC1A44538 | IDS_DisplayName_3412 | STI S209 |
| 0xC1A445B8 | IDS_DisplayName_3413 | Golf R |
| 0xC1A44638 | IDS_DisplayName_3414 | Defender 110 X |
| 0x71834938 | IDS_DisplayName_342 | F50 |
| 0x718349B8 | IDS_DisplayName_343 | Fairlady Z 432 |
| 0xC1A4C638 | IDS_DisplayName_3434 | M2 Competition Coupé |
| 0xC1A4C0B8 | IDS_DisplayName_3439 | Ford Super Duty F-250 Lariat 'Transformer' |
| 0x71834A38 | IDS_DisplayName_344 | Fairlady Z |
| 0xC1A504B8 | IDS_DisplayName_3441 | Toyota Tacoma TRD 'The Performance Truck' |
| 0xC1A506B8 | IDS_DisplayName_3445 | Taycan Turbo S |
| 0xC1A500B8 | IDS_DisplayName_3449 | Evija |
| 0x71834AB8 | IDS_DisplayName_345 | Fairlady Z Version S Twin Turbo |
| 0xC1A54638 | IDS_DisplayName_3454 | RS 3 Sedan |
| 0xC1A5C738 | IDS_DisplayName_3476 | Super Duty F-450 DRW PLATINUM |
| 0xC1A5C7B8 | IDS_DisplayName_3477 | Silverado LT Trail Boss |
| 0x71834C38 | IDS_DisplayName_348 | GT |
| 0xC1A60538 | IDS_DisplayName_3482 | 765LT Coupé |
| 0xC1A60738 | IDS_DisplayName_3486 | Wrangler Rubicon Traffic |
| 0xC1A64038 | IDS_DisplayName_3498 | S7 LM |
| 0xC1844038 | IDS_DisplayName_3518 | M8 Competition Coupé |
| 0xC1848438 | IDS_DisplayName_3520 | LC 500 |
| 0xC18485B8 | IDS_DisplayName_3523 | #151 Toyota GR Supra |
| 0xC1848638 | IDS_DisplayName_3524 | #411 Toyota Corolla Hatchback |
| 0x718309B8 | IDS_DisplayName_353 | Golf GTi 16v Mk2 |
| 0xC184C5B8 | IDS_DisplayName_3533 | Golf R |
| 0xC184C638 | IDS_DisplayName_3534 | John Cooper Works GP |
| 0xC184C0B8 | IDS_DisplayName_3539 | #23 Yokohama ALPHA |
| 0xC1850438 | IDS_DisplayName_3540 | RX3 |
| 0xC18505B8 | IDS_DisplayName_3543 | Huayra R |
| 0xC1850038 | IDS_DisplayName_3548 | Sunshine S |
| 0xC18500B8 | IDS_DisplayName_3549 | #122 Class 1 Buggy |
| 0xC18544B8 | IDS_DisplayName_3551 | #91 BMW M2 |
| 0xC1854638 | IDS_DisplayName_3554 | #1 Sierra Sierra Enterprises Lancer Evolution Time Attack |
| 0x71830BB8 | IDS_DisplayName_357 | GTO |
| 0x71830C38 | IDS_DisplayName_358 | 288 GTO |
| 0xC1860438 | IDS_DisplayName_3580 | DBX |
| 0xC18605B8 | IDS_DisplayName_3583 | RS 6 Avant |
| 0xC1860638 | IDS_DisplayName_3584 | RS 7 Sportback |
| 0xC1864438 | IDS_DisplayName_3590 | K-10 Custom |
| 0xC1864638 | IDS_DisplayName_3594 | Roma |
| 0xC18646B8 | IDS_DisplayName_3595 | SF90 Stradale |
| 0xC18647B8 | IDS_DisplayName_3597 | F-150 XLT Lariat |
| 0xC18640B8 | IDS_DisplayName_3599 | T.50 |
| 0xC1E40438 | IDS_DisplayName_3600 | Venom F5 |
| 0xC1E405B8 | IDS_DisplayName_3603 | #4402 Ultra 4 'Trophy Jeep' |
| 0xC1E40638 | IDS_DisplayName_3604 | #179 Hammerhead Class 1 |
| 0xC1E406B8 | IDS_DisplayName_3605 | #240 Fastball Racing Class 6100 Spec Trophy Truck |
| 0xC1E40738 | IDS_DisplayName_3606 | Essenza SCV12 |
| 0xC1E40038 | IDS_DisplayName_3608 | Sián Roadster |
| 0xC1E444B8 | IDS_DisplayName_3611 | MC20 |
| 0xC1E44738 | IDS_DisplayName_3616 | GT Black Series |
| 0xC1E447B8 | IDS_DisplayName_3617 | SL 63 |
| 0xC1E48538 | IDS_DisplayName_3622 | GT-R NISMO (R35) |
| 0xC1E486B8 | IDS_DisplayName_3625 | Nevera |
| 0xC1E480B8 | IDS_DisplayName_3629 | GR Yaris |
| 0x7183C9B8 | IDS_DisplayName_363 | Impreza 22B-STi Version |
| 0xC1E4C4B8 | IDS_DisplayName_3631 | Valkyrie AMR Pro |
| 0x7183CA38 | IDS_DisplayName_364 | IMPREZA WRX STI |
| 0xC1E506B8 | IDS_DisplayName_3645 | M4 Competition Coupé |
| 0x7183CAB8 | IDS_DisplayName_365 | IMPREZA WRX STI |
| 0xC1E54438 | IDS_DisplayName_3650 | Mercedes-AMG ONE |
| 0xC1E546B8 | IDS_DisplayName_3655 | 620R |
| 0xC1E547B8 | IDS_DisplayName_3657 | R1T |
| 0xC1E584B8 | IDS_DisplayName_3661 | Exige Cup 430 |
| 0xC1E58538 | IDS_DisplayName_3662 | #37 Polaris RZR Pro 4 Truck |
| 0xC1E586B8 | IDS_DisplayName_3665 | 700R |
| 0xC1E587B8 | IDS_DisplayName_3667 | 911 GT3 |
| 0xC1E58038 | IDS_DisplayName_3668 | Artura |
| 0xC1E5C438 | IDS_DisplayName_3670 | #4 Ford Focus RS |
| 0xC1E5C538 | IDS_DisplayName_3672 | Huracán STO |
| 0xC1E5C038 | IDS_DisplayName_3678 | i30 N |
| 0x7183CC38 | IDS_DisplayName_368 | Integra Type R |
| 0xC1E60738 | IDS_DisplayName_3686 | RZR Pro XP Factory Racing Limited Edition |
| 0xC1E607B8 | IDS_DisplayName_3687 | RZR Pro XP Ultimate |
| 0xC1E64538 | IDS_DisplayName_3692 | F-150 Lightning |
| 0xC1E645B8 | IDS_DisplayName_3693 | #6165 Trick Truck |
| 0xC1E64038 | IDS_DisplayName_3698 | Mission R |
| 0xC1C40438 | IDS_DisplayName_3700 | Sabre |
| 0xC1C44738 | IDS_DisplayName_3716 | Emira |
| 0xC1C440B8 | IDS_DisplayName_3719 | CT4-V Blackwing |
| 0xC1C48438 | IDS_DisplayName_3720 | CT5-V Blackwing |
| 0xC1C48538 | IDS_DisplayName_3722 | HUMMER EV Pickup |
| 0xC1C48638 | IDS_DisplayName_3724 | 296 GTB |
| 0xC1C48738 | IDS_DisplayName_3726 | Integra A-Spec |
| 0xC1C48038 | IDS_DisplayName_3728 | Continental GT Convertible |
| 0xC1C4C6B8 | IDS_DisplayName_3735 | BRZ |
| 0xC1C4C738 | IDS_DisplayName_3736 | Bronco Raptor |
| 0xC1C4C7B8 | IDS_DisplayName_3737 | iX xDrive50 |
| 0x71838A38 | IDS_DisplayName_374 | Lancer Evolution IX MR |
| 0xC1C50638 | IDS_DisplayName_3744 | #64 Forsberg Racing Nissan Z |
| 0xC1C506B8 | IDS_DisplayName_3745 | R8 V10 performance |
| 0xC1C54438 | IDS_DisplayName_3750 | Montero Exceed 2800 TD |
| 0xC1C545B8 | IDS_DisplayName_3753 | Huracán Tecnica |
| 0xC1C546B8 | IDS_DisplayName_3755 | Supervan 4 |
| 0xC1C540B8 | IDS_DisplayName_3759 | Huracán EVO Spyder |
| 0xC1C58438 | IDS_DisplayName_3760 | 718 Cayman GT4 RS |
| 0xC1C584B8 | IDS_DisplayName_3761 | GR86 |
| 0xC1C585B8 | IDS_DisplayName_3763 | M2 |
| 0xC1C58638 | IDS_DisplayName_3764 | M5 CS |
| 0xC1C58738 | IDS_DisplayName_3766 | Corvette Z06 |
| 0xC1C587B8 | IDS_DisplayName_3767 | NSX Type S |
| 0xC1C5C4B8 | IDS_DisplayName_3771 | Corvette E-Ray |
| 0xC1C5C5B8 | IDS_DisplayName_3773 | Civic Type R |
| 0xC1C5C638 | IDS_DisplayName_3774 | Countach LPI 800-4 |
| 0xC1C5C6B8 | IDS_DisplayName_3775 | Aventador LP 780-4 Ultimae |
| 0x71838C38 | IDS_DisplayName_378 | Lancer Evolution VIII MR |
| 0xC1C604B8 | IDS_DisplayName_3781 | 911 GT3 RS |
| 0xC1C605B8 | IDS_DisplayName_3783 | Rallye Golf |
| 0xC1C606B8 | IDS_DisplayName_3785 | Soarer 2.5 GT-T |
| 0xC1C600B8 | IDS_DisplayName_3789 | Hongguang Mini EV |
| 0x71838CB8 | IDS_DisplayName_379 | LEGACY B4 2.0 GT |
| 0xC1C646B8 | IDS_DisplayName_3795 | Manx 2.0 |
| 0xC1C64038 | IDS_DisplayName_3798 | MX-5 Cup |
| 0xC02444B8 | IDS_DisplayName_3811 | Air Sapphire |
| 0xC02440B8 | IDS_DisplayName_3819 | WRX |
| 0x71804938 | IDS_DisplayName_382 | M3 |
| 0xC02485B8 | IDS_DisplayName_3823 | MX-5 Miata RF |
| 0xC02487B8 | IDS_DisplayName_3827 | IONIQ 5 N |
| 0xC02480B8 | IDS_DisplayName_3829 | N Vision 74 |
| 0x718049B8 | IDS_DisplayName_383 | M3 |
| 0xC0250438 | IDS_DisplayName_3840 | Huracán Sterrato |
| 0xC0250738 | IDS_DisplayName_3846 | Mustang GT |
| 0xC02507B8 | IDS_DisplayName_3847 | Mustang Dark Horse |
| 0xC0250038 | IDS_DisplayName_3848 | Camry TRD |
| 0xC02500B8 | IDS_DisplayName_3849 | F-150 Raptor R |
| 0xC0254438 | IDS_DisplayName_3850 | Durango SRT Hellcat |
| 0xC02544B8 | IDS_DisplayName_3851 | Sera |
| 0xC0254538 | IDS_DisplayName_3852 | Beat |
| 0xC0254638 | IDS_DisplayName_3854 | AZ-1 |
| 0xC02546B8 | IDS_DisplayName_3855 | Figaro |
| 0xC0254738 | IDS_DisplayName_3856 | Be-1 |
| 0xC0254038 | IDS_DisplayName_3858 | Stagea RS FOUR V |
| 0xC02540B8 | IDS_DisplayName_3859 | City E II |
| 0xC0258438 | IDS_DisplayName_3860 | S-Cargo |
| 0xC02586B8 | IDS_DisplayName_3865 | Acty |
| 0xC0260438 | IDS_DisplayName_3880 | GR Corolla |
| 0xC0260738 | IDS_DisplayName_3886 | Lancer Evolution III GSR |
| 0xC02644B8 | IDS_DisplayName_3891 | Revuelto |
| 0xC02646B8 | IDS_DisplayName_3895 | SLC 43 Final Edition |
| 0xC00405B8 | IDS_DisplayName_3903 | Fiesta ST |
| 0xC0040638 | IDS_DisplayName_3904 | Focus ST |
| 0xC0040038 | IDS_DisplayName_3908 | e |
| 0x718008B8 | IDS_DisplayName_391 | MC12 |
| 0xC0044438 | IDS_DisplayName_3910 | 911 Rallye |
| 0xC0044638 | IDS_DisplayName_3914 | Chaser GT Twin Turbo |
| 0xC00447B8 | IDS_DisplayName_3917 | R8 Coupé V10 GT RWD |
| 0xC0044038 | IDS_DisplayName_3918 | Gloria Gran Turismo |
| 0xC00484B8 | IDS_DisplayName_3921 | Z NISMO |
| 0xC0048038 | IDS_DisplayName_3928 | i20 N |
| 0xC00480B8 | IDS_DisplayName_3929 | PAO |
| 0xC004C5B8 | IDS_DisplayName_3933 | Chaser 2.5 Tourer V |
| 0xC0054438 | IDS_DisplayName_3950 | 275 GTB4 Spider |
| 0xC00545B8 | IDS_DisplayName_3953 | 911 Turbo S |
| 0xC00546B8 | IDS_DisplayName_3955 | 1500 TRX |
| 0xC00540B8 | IDS_DisplayName_3959 | Challenger SRT Super Stock |
| 0xC0058438 | IDS_DisplayName_3960 | Vivio RX-R |
| 0xC0058638 | IDS_DisplayName_3964 | Prius Prime XSE Premium |
| 0x71800C38 | IDS_DisplayName_398 | MR2 GT |
| 0xC00605B8 | IDS_DisplayName_3983 | X6 M Competition |
| 0xC00640B8 | IDS_DisplayName_3999 | Silvia Spec-R |
| 0xB1240538 | IDS_DisplayName_4002 | Temerario |
| 0xB124C638 | IDS_DisplayName_4034 | Z GT |
| 0xB124C038 | IDS_DisplayName_4038 | Altezza RS200 Z EDITION |
| 0x71624AB8 | IDS_DisplayName_405 | Mustang SVT Cobra R |
| 0xB12546B8 | IDS_DisplayName_4055 | Starlet Glanza V |
| 0xB12547B8 | IDS_DisplayName_4057 | Skyline GT-R V-Spec |
| 0xB12580B8 | IDS_DisplayName_4069 | M3 |
| 0xB12604B8 | IDS_DisplayName_4081 | Gemera |
| 0xB1260638 | IDS_DisplayName_4084 | #269 Attacking the Clock Racing 240Z 'All Carbon Hill Climb Beast' |
| 0xB12606B8 | IDS_DisplayName_4085 | #269 Attacking the Clock Racing Minicab Time Attack |
| 0xB1264438 | IDS_DisplayName_4090 | Lancer Evolution VI GSR TM Edition |
| 0xB1264638 | IDS_DisplayName_4094 | GT-R NISMO |
| 0x716208B8 | IDS_DisplayName_411 | NSX-R |
| 0xB1044638 | IDS_DisplayName_4114 | Skyline GT-R |
| 0xB10440B8 | IDS_DisplayName_4119 | Skyline GT-R 40th Anniversary |
| 0x71620938 | IDS_DisplayName_412 | NSX-R |
| 0xB1048638 | IDS_DisplayName_4124 | G 65 Traffic |
| 0xB10486B8 | IDS_DisplayName_4125 | Acty Traffic |
| 0xB1048738 | IDS_DisplayName_4126 | e Traffic |
| 0xB10487B8 | IDS_DisplayName_4127 | Montero Traffic |
| 0xB1048038 | IDS_DisplayName_4128 | WRX Traffic |
| 0xB10480B8 | IDS_DisplayName_4129 | Stagea Traffic |
| 0xB1050638 | IDS_DisplayName_4144 | RX-7 Type R |
| 0xB10506B8 | IDS_DisplayName_4145 | #123 Mad Mike 808 Wagon 'FURSTY' |
| 0xB10507B8 | IDS_DisplayName_4147 | Giulia GTAm |
| 0xB1054738 | IDS_DisplayName_4156 | F80 |
| 0xB1054038 | IDS_DisplayName_4158 | LFA Forza Edition |
| 0xB1058438 | IDS_DisplayName_4160 | S-Cargo Forza Edition |
| 0xB1058538 | IDS_DisplayName_4162 | Sprinter Trueno GT-APEX Forza Edition |
| 0xB10585B8 | IDS_DisplayName_4163 | Sunshine S Forza Edition |
| 0xB1058638 | IDS_DisplayName_4164 | BRZ Forza Edition |
| 0xB10586B8 | IDS_DisplayName_4165 | RX-3 Forza Edition |
| 0xB1058738 | IDS_DisplayName_4166 | M2 Forza Edition |
| 0xB10587B8 | IDS_DisplayName_4167 | GT-R Black Edition (R35) Forza Edition |
| 0xB1058038 | IDS_DisplayName_4168 | Mustang GT 2+2 Fastback Forza Edition |
| 0xB10580B8 | IDS_DisplayName_4169 | 190 E 2.5-16 Evolution II Forza Edition |
| 0x71620BB8 | IDS_DisplayName_417 | Regal GNX |
| 0xB105C4B8 | IDS_DisplayName_4171 | F-150 XLT Lariat Forza Edition |
| 0xB105C6B8 | IDS_DisplayName_4175 | Super Duty F-450 DRW PLATINUM Forza Edition |
| 0xB105C0B8 | IDS_DisplayName_4179 | #12 Skyline GT-R (BNR32 Gr.A) JTC |
| 0x71620CB8 | IDS_DisplayName_419 | RS 4 |
| 0xB10647B8 | IDS_DisplayName_4197 | MX-5 Miata Forza Edition |
| 0xB1064038 | IDS_DisplayName_4198 | Viper GTS ACR Forza Edition |
| 0xB10640B8 | IDS_DisplayName_4199 | Tacoma TRD Pro Forza Edition |
| 0x7162C838 | IDS_DisplayName_420 | RS 6 |
| 0xB1640438 | IDS_DisplayName_4200 | Evija Forza Edition |
| 0xB16406B8 | IDS_DisplayName_4205 | Patrol |
| 0xB1644438 | IDS_DisplayName_4210 | Scura Motorsports Exige WTAC |
| 0xB16444B8 | IDS_DisplayName_4211 | #19 101 Motorsport CRX WTAC |
| 0xB1644538 | IDS_DisplayName_4212 | #32 Skyline WTAC 'Xtreme GTR' |
| 0xB16445B8 | IDS_DisplayName_4213 | #36 Dream Project S15 Silvia WTAC |
| 0xB1644638 | IDS_DisplayName_4214 | J&J Motorsport Supra WTAC |
| 0xB1644738 | IDS_DisplayName_4216 | Acty 'RakuRaku Express' |
| 0x7162C938 | IDS_DisplayName_422 | RSX Type S |
| 0xB16484B8 | IDS_DisplayName_4221 | GR GT (Prototype) |
| 0xB1648538 | IDS_DisplayName_4222 | Silvia K's |
| 0xB16485B8 | IDS_DisplayName_4223 | Skyline GT-R V·spec II |
| 0x7162C9B8 | IDS_DisplayName_423 | Savanna RX-7 |
| 0xB164C4B8 | IDS_DisplayName_4231 | #52 Evasive Motorsports S2000 WTAC |
| 0xB164C538 | IDS_DisplayName_4232 | Cayman GT3 WTAC |
| 0xB164C638 | IDS_DisplayName_4234 | Civic Type R |
| 0xB1654438 | IDS_DisplayName_4250 | Sprinter Trueno GT Apex 'Touge Edition' |
| 0xB16544B8 | IDS_DisplayName_4251 | S2000 'Touge Edition' |
| 0xB1654538 | IDS_DisplayName_4252 | Impreza 22B-STi Version 'Touge Edition' |
| 0xB1654638 | IDS_DisplayName_4254 | #33 BYP Racing Integra WTAC |
| 0xB16546B8 | IDS_DisplayName_4255 | Crown Super Deluxe Taxi |
| 0xB16547B8 | IDS_DisplayName_4257 | JPN Taxi |
| 0xB16540B8 | IDS_DisplayName_4259 | 86 'Stories' |
| 0xB1658438 | IDS_DisplayName_4260 | GT-R NISMO 'Initial Drive' |
| 0xB16584B8 | IDS_DisplayName_4261 | 911 GT2 'Initial Drive' |
| 0xB16585B8 | IDS_DisplayName_4263 | GR GT (Prototype) |
| 0xB1658638 | IDS_DisplayName_4264 | FXX-K Evo 'Welcome Pack' |
| 0xB16586B8 | IDS_DisplayName_4265 | GT Black Series 'Welcome Pack' |
| 0xB1658738 | IDS_DisplayName_4266 | M4 Competition Coupé 'Welcome Pack' |
| 0xB16587B8 | IDS_DisplayName_4267 | Lancer Evolution VIII MR 'Welcome Pack' |
| 0xB1658038 | IDS_DisplayName_4268 | F-150 Raptor R 'Welcome Pack' |
| 0x7162CBB8 | IDS_DisplayName_427 | S2000 |
| 0xB165C7B8 | IDS_DisplayName_4277 | #21 Hardrace/JDMYard Civic WTAC |
| 0xB165C038 | IDS_DisplayName_4278 | Land Cruiser |
| 0xB16607B8 | IDS_DisplayName_4287 | Vivio RX-R Forza Edition |
| 0xB14405B8 | IDS_DisplayName_4303 | GT-R Black Edition (R35) 'Touge Edition' |
| 0xB14445B8 | IDS_DisplayName_4313 | #3 917 LH Forza Edition |
| 0xB14446B8 | IDS_DisplayName_4315 | P50 Trolli Edition |
| 0x716289B8 | IDS_DisplayName_433 | Sagaris |
| 0xB144C538 | IDS_DisplayName_4332 | Crown Super Deluxe Taxi Traffic |
| 0xB144C5B8 | IDS_DisplayName_4333 | JPN Taxi Traffic |
| 0xB14504B8 | IDS_DisplayName_4341 | J50 Preorder Car |
| 0xB1450538 | IDS_DisplayName_4342 | Sports 800 Fanta Edition |
| 0x71634838 | IDS_DisplayName_440 | Silvia K's |
| 0x71630AB8 | IDS_DisplayName_455 | Sprinter Trueno GT Apex |
| 0x71630C38 | IDS_DisplayName_458 | Stratos HF Stradale |
| 0x7163C838 | IDS_DisplayName_460 | Supra 2.0 GT |
| 0x7163C8B8 | IDS_DisplayName_461 | Supra RZ |
| 0x716049B8 | IDS_DisplayName_483 | Viper GTS ACR |
| 0x71604CB8 | IDS_DisplayName_489 | XJ220 |
| 0x714209B8 | IDS_DisplayName_513 | Charger R/T |
| 0x7143CBB8 | IDS_DisplayName_567 | R390 (GT1) |
| 0x7143CC38 | IDS_DisplayName_568 | AMG CLK GTR |
| 0x7143CCB8 | IDS_DisplayName_569 | NSX-R GT |
| 0x71220AB8 | IDS_DisplayName_615 | 207 Super 2000 |
| 0x7122CAB8 | IDS_DisplayName_625 | Civic Type R |
| 0x712289B8 | IDS_DisplayName_633 | Sport quattro |
| 0x71228A38 | IDS_DisplayName_634 | 5 Turbo |
| 0x71228BB8 | IDS_DisplayName_637 | Miura P400 |
| 0x71228CB8 | IDS_DisplayName_639 | Challenger R/T |
| 0x712348B8 | IDS_DisplayName_641 | 911 GT1 Strassenversion |
| 0x8994307E | IDS_ModelShort_1006 | Ferrari FXX |
| 0x899430FE | IDS_ModelShort_1007 | Koenigsegg CCGT |
| 0x899437FE | IDS_ModelShort_1009 | Lancer GSR '08 |
| 0x899473FE | IDS_ModelShort_1011 | BMW M3 '08 |
| 0x8994B37E | IDS_ModelShort_1020 | Ferrari F50 GT |
| 0x8994B27E | IDS_ModelShort_1022 | Ferrari 430 S |
| 0x8994B2FE | IDS_ModelShort_1023 | Ferrari F40 C |
| 0x8994F27E | IDS_ModelShort_1032 | Alfa Romeo 8C |
| 0x8994F17E | IDS_ModelShort_1034 | Toyota Celica'94 |
| 0x8995337E | IDS_ModelShort_1040 | BMW M1 |
| 0x899533FE | IDS_ModelShort_1041 | Ford Mustang '93 |
| 0x8995327E | IDS_ModelShort_1042 | Nissan GT-R '71 |
| 0x899531FE | IDS_ModelShort_1045 | Pontiac T/A '87 |
| 0x8995307E | IDS_ModelShort_1046 | Dodge Viper '08 |
| 0x8995727E | IDS_ModelShort_1052 | Dodge Ram SRT-10 |
| 0x899577FE | IDS_ModelShort_1059 | BMW Z4 '08 |
| 0x8995B37E | IDS_ModelShort_1060 | SUBARU WRX '08 |
| 0x8995B2FE | IDS_ModelShort_1063 | Charger Daytona |
| 0x8995B17E | IDS_ModelShort_1064 | Chevy Camaro '79 |
| 0x8995B7FE | IDS_ModelShort_1069 | Corvette '09 |
| 0x8996307E | IDS_ModelShort_1086 | Ford Focus '09 |
| 0x8996737E | IDS_ModelShort_1090 | M-B SL65 |
| 0x899672FE | IDS_ModelShort_1093 | Corvette '60 |
| 0x89B432FE | IDS_ModelShort_1103 | Nissan 370Z |
| 0x89B4317E | IDS_ModelShort_1104 | Datsun 510 |
| 0x89B431FE | IDS_ModelShort_1105 | AM DB5 |
| 0x89B4377E | IDS_ModelShort_1108 | Ford RS200 |
| 0x89B4737E | IDS_ModelShort_1110 | Mazda MX-5 '94 |
| 0x89B4B17E | IDS_ModelShort_1124 | Abarth 131 |
| 0x89B4B07E | IDS_ModelShort_1126 | BMW M5 '09 |
| 0x89B4F37E | IDS_ModelShort_1130 | McLaren 12C |
| 0x89B4F3FE | IDS_ModelShort_1131 | Ferrari 458 |
| 0x89B5737E | IDS_ModelShort_1150 | Alfa Romeo GTA |
| 0x89B571FE | IDS_ModelShort_1155 | Shelby Daytona |
| 0x89B5F3FE | IDS_ModelShort_1171 | Ferrari 599XX |
| 0x89B5F2FE | IDS_ModelShort_1173 | Lambo Murciélago |
| 0x89B5F1FE | IDS_ModelShort_1175 | Pagani Zonda R |
| 0x89B6317E | IDS_ModelShort_1184 | Audi RS 6 '09 |
| 0x89D4337E | IDS_ModelShort_1200 | Audi R8 LMS |
| 0x89D4317E | IDS_ModelShort_1204 | Megane RS 250 |
| 0x89D471FE | IDS_ModelShort_1215 | NULL CAR |
| 0x89D4707E | IDS_ModelShort_1216 | Audi RS 3 |
| 0x89D4B37E | IDS_ModelShort_1220 | Audi TT RS |
| 0x89D4B3FE | IDS_ModelShort_1221 | Mazda 3 |
| 0x89D4B7FE | IDS_ModelShort_1229 | Mazda Furai |
| 0x89D4F3FE | IDS_ModelShort_1231 | VW Golf R '10 |
| 0x89D572FE | IDS_ModelShort_1253 | Noble M600 |
| 0x89D5B37E | IDS_ModelShort_1260 | Lexus LFA |
| 0x89D5B7FE | IDS_ModelShort_1269 | BMW 2002 Turbo |
| 0x89D5F37E | IDS_ModelShort_1270 | DeLorean DMC-12 |
| 0x89D5F27E | IDS_ModelShort_1272 | Ford Escort '92 |
| 0x89D5F2FE | IDS_ModelShort_1273 | Honda Civic '97 |
| 0x89D5F07E | IDS_ModelShort_1276 | Pontiac T/A '77 |
| 0x89D5F0FE | IDS_ModelShort_1277 | Plymouth Cuda |
| 0x89D5F77E | IDS_ModelShort_1278 | Ford XB Falcon |
| 0x89D6327E | IDS_ModelShort_1282 | Nissan 240SX '93 |
| 0x89D673FE | IDS_ModelShort_1291 | Chevy El Camino |
| 0x89D672FE | IDS_ModelShort_1293 | Ford RS500 |
| 0x89D6717E | IDS_ModelShort_1294 | GMC Syclone |
| 0x89D671FE | IDS_ModelShort_1295 | Lancia 037 |
| 0x89D6707E | IDS_ModelShort_1296 | MB 190 E '90 |
| 0x89D677FE | IDS_ModelShort_1299 | Volvo 242 Turbo |
| 0x89F4337E | IDS_ModelShort_1300 | Chevy Impala |
| 0x89F433FE | IDS_ModelShort_1301 | Jaguar D-Type |
| 0x89F4717E | IDS_ModelShort_1314 | McLaren F1 |
| 0x89F4B2FE | IDS_ModelShort_1323 | SUBARU WRX '11 |
| 0x89F4F37E | IDS_ModelShort_1330 | Chevy Camaro '70 |
| 0x89F4F27E | IDS_ModelShort_1332 | Dodge Dart '68 |
| 0x89F4F1FE | IDS_ModelShort_1335 | #55 Mazda 787B |
| 0x89F5737E | IDS_ModelShort_1350 | BMW X5 M |
| 0x89F5727E | IDS_ModelShort_1352 | Dodge Super Bee |
| 0x89F571FE | IDS_ModelShort_1355 | Ford Mustang '65 |
| 0x89F5B0FE | IDS_ModelShort_1367 | BMW M5 '03 |
| 0x89F5B77E | IDS_ModelShort_1368 | BMW M5 '88 |
| 0x89F5B7FE | IDS_ModelShort_1369 | Pagani Zonda C |
| 0x89F5F07E | IDS_ModelShort_1376 | Lotus Elise '99 |
| 0x89F5F7FE | IDS_ModelShort_1379 | Chevy Impala '96 |
| 0x89F633FE | IDS_ModelShort_1381 | Galant VR-4 |
| 0x89F6327E | IDS_ModelShort_1382 | SUBARU LEGACY RS |
| 0x89F6377E | IDS_ModelShort_1388 | BMW M5 '12 |
| 0x89F6727E | IDS_ModelShort_1392 | Lambo Sesto |
| 0x89F672FE | IDS_ModelShort_1393 | Alfa Romeo 155 |
| 0x89F6717E | IDS_ModelShort_1394 | GMC Typhoon |
| 0x89F671FE | IDS_ModelShort_1395 | Toyota MR2 '89 |
| 0x89F670FE | IDS_ModelShort_1397 | Koenigsegg Agera |
| 0x89F6777E | IDS_ModelShort_1398 | Aventador '12 |
| 0x891470FE | IDS_ModelShort_1417 | Audi RS 5 |
| 0x8914777E | IDS_ModelShort_1418 | BMW M5 '95 |
| 0x8914B07E | IDS_ModelShort_1426 | Mazda RX-8 |
| 0x8914B77E | IDS_ModelShort_1428 | VW Scirocco '11 |
| 0x8914B7FE | IDS_ModelShort_1429 | Chevy Nova '69 |
| 0x8914F1FE | IDS_ModelShort_1435 | VW Beetle |
| 0x891577FE | IDS_ModelShort_1459 | Chevy Bel Air |
| 0x8915F0FE | IDS_ModelShort_1477 | Ford Transit SSV |
| 0x8915F77E | IDS_ModelShort_1478 | #2 Audi S1 |
| 0x8916337E | IDS_ModelShort_1480 | Mazda RX-7 '85 |
| 0x891633FE | IDS_ModelShort_1481 | 3000 MKIII |
| 0x891672FE | IDS_ModelShort_1493 | BMW 850CSi |
| 0x8934337E | IDS_ModelShort_1500 | M-B C63 |
| 0x893472FE | IDS_ModelShort_1513 | Mas. Ghibli '97 |
| 0x8934717E | IDS_ModelShort_1514 | Mazda RX-3 |
| 0x893470FE | IDS_ModelShort_1517 | Toyota Celica'92 |
| 0x8934B27E | IDS_ModelShort_1522 | Jeep Wrangler |
| 0x8934B7FE | IDS_ModelShort_1529 | Ford Capri MkI |
| 0x8934F27E | IDS_ModelShort_1532 | Venom GT |
| 0x8934F2FE | IDS_ModelShort_1533 | Holden Torana |
| 0x8934F0FE | IDS_ModelShort_1537 | Toyota Corolla |
| 0x893537FE | IDS_ModelShort_1549 | Alfa Romeo 33S |
| 0x893577FE | IDS_ModelShort_1559 | M-B 300 SLR |
| 0x8935B27E | IDS_ModelShort_1562 | SRT Viper '13 |
| 0x8935B17E | IDS_ModelShort_1564 | Corvette '53 |
| 0x8935B77E | IDS_ModelShort_1568 | Honda Civic '74 |
| 0x8935F1FE | IDS_ModelShort_1575 | Chevy MonteCarlo |
| 0x8935F77E | IDS_ModelShort_1578 | Ferrari 250 GT |
| 0x8936307E | IDS_ModelShort_1586 | Continental '62 |
| 0x893630FE | IDS_ModelShort_1587 | Mazda Cosmo |
| 0x893673FE | IDS_ModelShort_1591 | Peugeot 205 T16 |
| 0x8936777E | IDS_ModelShort_1598 | BMW E92 M3 GTS |
| 0x893677FE | IDS_ModelShort_1599 | Ferrari 599XX E |
| 0x895433FE | IDS_ModelShort_1601 | Gallardo Spyder |
| 0x895430FE | IDS_ModelShort_1607 | Audi RS 4 '13 |
| 0x8954B0FE | IDS_ModelShort_1627 | M-B G 65 |
| 0x8955737E | IDS_ModelShort_1650 | Honda Civic '86 |
| 0x895573FE | IDS_ModelShort_1651 | Ariel Atom |
| 0x8955717E | IDS_ModelShort_1654 | Ford Mustang '13 |
| 0x895571FE | IDS_ModelShort_1655 | SUBARU BRZ |
| 0x8955777E | IDS_ModelShort_1658 | M-B A45 |
| 0x8955B3FE | IDS_ModelShort_1661 | Lancia Delta S4 |
| 0x8955B27E | IDS_ModelShort_1662 | MINI '65 |
| 0x8955B0FE | IDS_ModelShort_1667 | McLaren P1 |
| 0x8955B77E | IDS_ModelShort_1668 | Ford Mustang '69 |
| 0xB994327E | IDS_ModelShort_2002 | Nissan GT-R '12 |
| 0xB99432FE | IDS_ModelShort_2003 | MINI JCW '12 |
| 0xB994317E | IDS_ModelShort_2004 | Mazda MX-5 '13 |
| 0xB994307E | IDS_ModelShort_2006 | Corvette '95 |
| 0xB99430FE | IDS_ModelShort_2007 | Toyota 86 |
| 0xB99437FE | IDS_ModelShort_2009 | Audi RS 7 |
| 0xB994737E | IDS_ModelShort_2010 | Audi R8 '13 |
| 0xB99470FE | IDS_ModelShort_2017 | Abarth 595 '68 |
| 0xB99477FE | IDS_ModelShort_2019 | Ford Focus '03 |
| 0xB994F17E | IDS_ModelShort_2034 | LaFerrari |
| 0xB994F77E | IDS_ModelShort_2038 | Alfa Romeo 4C |
| 0xB995337E | IDS_ModelShort_2040 | BAC Mono |
| 0xB995327E | IDS_ModelShort_2042 | Lambo Veneno |
| 0xB9B477FE | IDS_ModelShort_2119 | Honda Civic '84 |
| 0xB9B4B3FE | IDS_ModelShort_2121 | Honda Prelude 94 |
| 0xB9B4B77E | IDS_ModelShort_2128 | Caddy Limo |
| 0xB9B4F3FE | IDS_ModelShort_2131 | HSV GEN-F GTS |
| 0xB9B4F2FE | IDS_ModelShort_2133 | BMW i8 |
| 0xB9B5337E | IDS_ModelShort_2140 | SUBARU BRAT |
| 0xB9B5327E | IDS_ModelShort_2142 | VW Golf R '14 |
| 0xB9B531FE | IDS_ModelShort_2145 | Ford Ranger T6 |
| 0xB9B530FE | IDS_ModelShort_2147 | MG Metro 6R4 |
| 0xB9B5377E | IDS_ModelShort_2148 | MINI X-Raid |
| 0xB9B537FE | IDS_ModelShort_2149 | Renault Clio '93 |
| 0xB9B573FE | IDS_ModelShort_2151 | VW Type 2 |
| 0xB9B5717E | IDS_ModelShort_2154 | BMW M4 '14 |
| 0xB9B5B3FE | IDS_ModelShort_2161 | Alfa Romeo TZ2 |
| 0xB9B5B2FE | IDS_ModelShort_2163 | Honda Civic '16 |
| 0xB9B5B17E | IDS_ModelShort_2164 | Lambo Huracán |
| 0xB9B5B77E | IDS_ModelShort_2168 | SUBARU WRX '15 |
| 0xB9B5F3FE | IDS_ModelShort_2171 | Mazda MX-5 '05 |
| 0xB9B5F1FE | IDS_ModelShort_2175 | Lexus RC F |
| 0xB9B5F0FE | IDS_ModelShort_2177 | Corvette '15 |
| 0xB9B5F77E | IDS_ModelShort_2178 | Audi RS 4 '01 |
| 0xB9B5F7FE | IDS_ModelShort_2179 | Audi S1 |
| 0xB9B6337E | IDS_ModelShort_2180 | Audi RS 6 '15 |
| 0xB9B632FE | IDS_ModelShort_2183 | Chevy Camaro '15 |
| 0xB9B6317E | IDS_ModelShort_2184 | Ferrari 458 S |
| 0xB9B6377E | IDS_ModelShort_2188 | Koenigsegg One |
| 0xB9D431FE | IDS_ModelShort_2205 | Honda S800 |
| 0xB9D4707E | IDS_ModelShort_2216 | Plymouth Fury |
| 0xB9D470FE | IDS_ModelShort_2217 | SUBARU SVX |
| 0xB9D5327E | IDS_ModelShort_2242 | M-B AMG GT |
| 0xB9D5B27E | IDS_ModelShort_2262 | Caddy ATS-V |
| 0xB9D5B2FE | IDS_ModelShort_2263 | Challenger '15 |
| 0xB9D5B0FE | IDS_ModelShort_2267 | Mazda MX-5 '16 |
| 0xB9D5F37E | IDS_ModelShort_2270 | Nissan GT-R '73 |
| 0xB9D5F27E | IDS_ModelShort_2272 | Datsun 2000 |
| 0xB9D6737E | IDS_ModelShort_2290 | Porsche 918 |
| 0xB9D670FE | IDS_ModelShort_2297 | 911 GT3 RS4 '12 |
| 0xB9F570FE | IDS_ModelShort_2357 | Ford Focus '17 |
| 0xB9F5B2FE | IDS_ModelShort_2363 | Ford GT '17 |
| 0xB9F5F3FE | IDS_ModelShort_2371 | Ferrari FXX K |
| 0xB9F5F27E | IDS_ModelShort_2372 | Ford Coupe '32 |
| 0xB914337E | IDS_ModelShort_2400 | Ford GT350R '16 |
| 0xB914727E | IDS_ModelShort_2412 | BMW Isetta |
| 0xB914707E | IDS_ModelShort_2416 | Meyers Manx |
| 0xB914B37E | IDS_ModelShort_2420 | Opel Manta 400 |
| 0xB914B3FE | IDS_ModelShort_2421 | Caddy CTS-V '16 |
| 0xB914B27E | IDS_ModelShort_2422 | HSV Maloo '14 |
| 0xB914F37E | IDS_ModelShort_2430 | Ariel Nomad |
| 0xB915B0FE | IDS_ModelShort_2467 | Ferrari 488 GTB |
| 0xB915B77E | IDS_ModelShort_2468 | Dodge Charger 15 |
| 0xB915B7FE | IDS_ModelShort_2469 | Sports 800 '65 |
| 0xFD722BD6 | IDS_ModelShort_247 | Toyota 2000GT |
| 0xB915F37E | IDS_ModelShort_2470 | AM Vulcan |
| 0xB915F3FE | IDS_ModelShort_2471 | M-B AMG C 63 '16 |
| 0xB915F27E | IDS_ModelShort_2472 | McLaren 570S |
| 0xB915F2FE | IDS_ModelShort_2473 | Audi R8 V10 plus |
| 0xB916307E | IDS_ModelShort_2486 | Radical RXC |
| 0xB91637FE | IDS_ModelShort_2489 | Abarth 695 '16 |
| 0xFD722CD6 | IDS_ModelShort_249 | Ferrari 250 GTO |
| 0xB916717E | IDS_ModelShort_2494 | Range Rover '15 |
| 0xB93430FE | IDS_ModelShort_2507 | Chevy 150 Sedan |
| 0xFD7268D6 | IDS_ModelShort_251 | M-B 300SL |
| 0xB934727E | IDS_ModelShort_2512 | Nissan GT-R '87 |
| 0xB93471FE | IDS_ModelShort_2515 | Penhall Cholla |
| 0xB93470FE | IDS_ModelShort_2517 | #11 Ford F-150 |
| 0xB934B07E | IDS_ModelShort_2526 | Regera |
| 0xB934B0FE | IDS_ModelShort_2527 | AM DB11 |
| 0xFD7269D6 | IDS_ModelShort_253 | Ferrari F355 |
| 0xB934F17E | IDS_ModelShort_2534 | Porsche 968 |
| 0xB934F1FE | IDS_ModelShort_2535 | Porsche 928 GTS |
| 0xB935327E | IDS_ModelShort_2542 | Alfa Giulia '17 |
| 0xB935317E | IDS_ModelShort_2544 | Dodge Viper '16 |
| 0xB93537FE | IDS_ModelShort_2549 | Porsche 917 LH |
| 0xFD726AD6 | IDS_ModelShort_255 | Ferrari 512TR |
| 0xB93573FE | IDS_ModelShort_2551 | Ford Ute '14 |
| 0xB935727E | IDS_ModelShort_2552 | Alumi Craft C.10 |
| 0xB935B2FE | IDS_ModelShort_2563 | Reliant Supervan |
| 0xB935B07E | IDS_ModelShort_2566 | Toyota FJ40 |
| 0xB935B77E | IDS_ModelShort_2568 | VW Class 5 Bug |
| 0xB935B7FE | IDS_ModelShort_2569 | Ultima 1020 |
| 0xB935F17E | IDS_ModelShort_2574 | M12S Warthog CST |
| 0xB935F0FE | IDS_ModelShort_2577 | Ferrari F12tdf |
| 0xFD72A856 | IDS_ModelShort_260 | 911 Carrera '73 |
| 0xFD72A8D6 | IDS_ModelShort_261 | 911 GT2 '95 |
| 0xB95472FE | IDS_ModelShort_2613 | GMC K5 Jimmy |
| 0xB954717E | IDS_ModelShort_2614 | Ford Mustang '68 |
| 0xB954707E | IDS_ModelShort_2616 | Lambo Centenario |
| 0xB954777E | IDS_ModelShort_2618 | Nissan GT-R '17 |
| 0xFD72A956 | IDS_ModelShort_262 | 911 GT3 '04 |
| 0xB954B1FE | IDS_ModelShort_2625 | Bentley Bentayga |
| 0xB954B77E | IDS_ModelShort_2628 | BMW M4 '16 |
| 0xB954F07E | IDS_ModelShort_2636 | #1 Toyota T100 |
| 0xB95530FE | IDS_ModelShort_2647 | Pagani Huayra BC |
| 0xB95537FE | IDS_ModelShort_2649 | Crown Victoria |
| 0xFD72AAD6 | IDS_ModelShort_265 | 911 Turbo '82 |
| 0xB955727E | IDS_ModelShort_2652 | Montero Evo |
| 0xB955717E | IDS_ModelShort_2654 | M-B AMG GTR |
| 0xB95577FE | IDS_ModelShort_2659 | Silvia '98 |
| 0xB955B2FE | IDS_ModelShort_2663 | #37 Pro 2 Truck |
| 0xFD72AC56 | IDS_ModelShort_268 | Porsche 944 |
| 0xFD72ACD6 | IDS_ModelShort_269 | Porsche 959 |
| 0xB97473FE | IDS_ModelShort_2711 | Mazda Traffic |
| 0xB974727E | IDS_ModelShort_2712 | Galant Traffic |
| 0xB97472FE | IDS_ModelShort_2713 | Box Traffic |
| 0xB974717E | IDS_ModelShort_2714 | Bus Traffic |
| 0xB974F77E | IDS_ModelShort_2738 | Nissan GT-R '95 |
| 0xB974F7FE | IDS_ModelShort_2739 | Chevy Camaro '17 |
| 0xB975337E | IDS_ModelShort_2740 | Abarth 124 '17 |
| 0xB975327E | IDS_ModelShort_2742 | Jeep Trailcat |
| 0xB97532FE | IDS_ModelShort_2743 | Toyota AT37 |
| 0xB97531FE | IDS_ModelShort_2745 | Honda Trophy '15 |
| 0xB97571FE | IDS_ModelShort_2755 | 911 GT2 RS '18 |
| 0xB975F2FE | IDS_ModelShort_2773 | Porsche Cayenne |
| 0xB976727E | IDS_ModelShort_2792 | #2 Ford GT40 |
| 0xB97672FE | IDS_ModelShort_2793 | #24 Ferrari P4 |
| 0xB976717E | IDS_ModelShort_2794 | P. 911 Turbo '93 |
| 0xB89433FE | IDS_ModelShort_2801 | Nis. #11 Skyline |
| 0xFD7128D6 | IDS_ModelShort_281 | Barracuda |
| 0xB894B27E | IDS_ModelShort_2822 | Nissan Safari |
| 0xB894B1FE | IDS_ModelShort_2825 | Lotus Elise GT1 |
| 0xB89533FE | IDS_ModelShort_2841 | Jeep Trackhawk |
| 0xB895F37E | IDS_ModelShort_2870 | Honda Civic '18 |
| 0xB895F3FE | IDS_ModelShort_2871 | Can-Am Maverick |
| 0xB895F27E | IDS_ModelShort_2872 | Veloster N |
| 0xFD712CD6 | IDS_ModelShort_289 | Chevy Camaro '69 |
| 0xB8B4327E | IDS_ModelShort_2902 | Flatbed Traffic |
| 0xB8B432FE | IDS_ModelShort_2903 | Legacy Traffic |
| 0xB8B437FE | IDS_ModelShort_2909 | Dodge SRT Demon |
| 0xB8B4737E | IDS_ModelShort_2910 | Agera RS |
| 0xFD716956 | IDS_ModelShort_292 | Porsche Carrera |
| 0xB8B4F1FE | IDS_ModelShort_2935 | Funco F9 |
| 0xB8B4F0FE | IDS_ModelShort_2937 | #14 Ford Fiesta |
| 0xFD716AD6 | IDS_ModelShort_295 | Toyota Celica'03 |
| 0xFD716B56 | IDS_ModelShort_296 | TVR Speed 12 |
| 0xB8B5B77E | IDS_ModelShort_2968 | AM Valkyrie |
| 0xB8B5F17E | IDS_ModelShort_2974 | Ferrari 812 |
| 0xB8B6307E | IDS_ModelShort_2986 | M-B Unimog |
| 0xB8B630FE | IDS_ModelShort_2987 | Peel P50 |
| 0xFD716CD6 | IDS_ModelShort_299 | Chevelle '70 |
| 0xB8B6727E | IDS_ModelShort_2992 | Jaguar LW E-Type |
| 0xB8B672FE | IDS_ModelShort_2993 | TVR Griffith |
| 0xB8B671FE | IDS_ModelShort_2995 | VW Golf GTI '83 |
| 0xB8B6707E | IDS_ModelShort_2996 | #13 Ford Mustang |
| 0xB8B670FE | IDS_ModelShort_2997 | #530 HSV Maloo |
| 0xA994337E | IDS_ModelShort_3000 | #777 Nissan 240 |
| 0xA99432FE | IDS_ModelShort_3003 | #43 Dodge Viper |
| 0xA99430FE | IDS_ModelShort_3007 | #34 VW Beetle |
| 0xFD532956 | IDS_ModelShort_302 | Honda Civic '04 |
| 0xA994F3FE | IDS_ModelShort_3031 | #185 Porsche 959 |
| 0xA994F1FE | IDS_ModelShort_3035 | KTM X-Bow GT4 |
| 0xA994F0FE | IDS_ModelShort_3037 | #98 BMW 325i |
| 0xA99573FE | IDS_ModelShort_3051 | Ford Fiesta RS |
| 0xA99571FE | IDS_ModelShort_3055 | Jaguar C-X75 |
| 0xFD532B56 | IDS_ModelShort_306 | Shelby Cobra 427 |
| 0xA995B27E | IDS_ModelShort_3062 | Ferrari 512 S |
| 0xA995B2FE | IDS_ModelShort_3063 | M-B X-Class |
| 0xA995B17E | IDS_ModelShort_3064 | M-B GT 4 '18 |
| 0xA995F27E | IDS_ModelShort_3072 | 911 GT3 RS '19 |
| 0xA996327E | IDS_ModelShort_3082 | MC12 Corsa '08 |
| 0xA99630FE | IDS_ModelShort_3087 | 650S Spider |
| 0xA996377E | IDS_ModelShort_3088 | Silverado DD |
| 0xFD532CD6 | IDS_ModelShort_309 | VW Corrado |
| 0xA99673FE | IDS_ModelShort_3091 | AM Vantage '19 |
| 0xA9B430FE | IDS_ModelShort_3107 | M-B G 63 6x6 |
| 0xA9B4377E | IDS_ModelShort_3108 | Ford Mustang S5 |
| 0xA9B4737E | IDS_ModelShort_3110 | Jeep Wrangler DD |
| 0xA9B470FE | IDS_ModelShort_3117 | Porsche 718 GTS |
| 0xA9B4777E | IDS_ModelShort_3118 | Corvette '19 |
| 0xFD536956 | IDS_ModelShort_312 | Corvette '67 |
| 0xA9B4B37E | IDS_ModelShort_3120 | Urus '19 |
| 0xA9B4B2FE | IDS_ModelShort_3123 | Porsche 911 '19 |
| 0xA9B4B77E | IDS_ModelShort_3128 | #25 Ford Bronco |
| 0xA9B4B7FE | IDS_ModelShort_3129 | Mégane R26.R |
| 0xA9B4F27E | IDS_ModelShort_3132 | MINI JCW Buggy |
| 0xA9B4F17E | IDS_ModelShort_3134 | Megane R.S. '18 |
| 0xFD536A56 | IDS_ModelShort_314 | Corvette '02 |
| 0xA9B533FE | IDS_ModelShort_3141 | Apollo IE '19 |
| 0xA9B537FE | IDS_ModelShort_3149 | Chevy Camaro '18 |
| 0xFD536AD6 | IDS_ModelShort_315 | Corvette '70 |
| 0xA9B572FE | IDS_ModelShort_3153 | McLaren 600LT |
| 0xA9B5707E | IDS_ModelShort_3156 | Speedtail '19 |
| 0xFD536B56 | IDS_ModelShort_316 | Lambo Countach |
| 0xA9B5F37E | IDS_ModelShort_3170 | Ford Supervan 3 |
| 0xA9B5F2FE | IDS_ModelShort_3173 | BMW Z4 '19 |
| 0xA9B5F07E | IDS_ModelShort_3176 | AMG Hammer Coupe |
| 0xA9B6337E | IDS_ModelShort_3180 | Peugeot 205 R |
| 0xA9B6317E | IDS_ModelShort_3184 | #5 Escort '77 |
| 0xA9B631FE | IDS_ModelShort_3185 | AM DBS SL '19 |
| 0xA9B630FE | IDS_ModelShort_3187 | Porsche Macan RR |
| 0xA9B637FE | IDS_ModelShort_3189 | VelociRaptor '19 |
| 0xA9B6737E | IDS_ModelShort_3190 | Ford Lightning |
| 0xA9B6777E | IDS_ModelShort_3198 | Pantera GT5 |
| 0xFD53A856 | IDS_ModelShort_320 | Honda CR-X |
| 0xA9D473FE | IDS_ModelShort_3211 | AM Vulcan AMR |
| 0xA9D4727E | IDS_ModelShort_3212 | Zenvo TSR-S |
| 0xA9D4717E | IDS_ModelShort_3214 | #70 Porsche 935 |
| 0xA9D4B1FE | IDS_ModelShort_3225 | Portofino '18 |
| 0xA9D4B07E | IDS_ModelShort_3226 | Ferrari J50 '17 |
| 0xA9D4B0FE | IDS_ModelShort_3227 | 488 Pista '19 |
| 0xA9D4B77E | IDS_ModelShort_3228 | Ford Racing Puma |
| 0xFD53A9D6 | IDS_ModelShort_323 | Lancia Delta |
| 0xA9D4F27E | IDS_ModelShort_3232 | #777 Corvette |
| 0xA9D4F1FE | IDS_ModelShort_3235 | VW Pickup '82 |
| 0xFD53AA56 | IDS_ModelShort_324 | Diablo GTR |
| 0xA9D533FE | IDS_ModelShort_3241 | SUBARU STI ARX |
| 0xA9D537FE | IDS_ModelShort_3249 | Formula D 599 |
| 0xFD53AAD6 | IDS_ModelShort_325 | Lambo Diablo SV |
| 0xA9D5737E | IDS_ModelShort_3250 | Mercedes-AMG E63 |
| 0xA9D571FE | IDS_ModelShort_3255 | Jeep JT |
| 0xA9D570FE | IDS_ModelShort_3257 | Nissan Pulsar |
| 0xFD53AB56 | IDS_ModelShort_326 | Ferrari Dino |
| 0xFD53ABD6 | IDS_ModelShort_327 | Eclipse GSX |
| 0xA9D5F0FE | IDS_ModelShort_3277 | Ford GT500 '20 |
| 0xA9D630FE | IDS_ModelShort_3287 | Jaguar XJR-15 |
| 0xA9D6377E | IDS_ModelShort_3288 | Schuppan 962CR |
| 0xA9D637FE | IDS_ModelShort_3289 | Aventador SVJ |
| 0xA9D672FE | IDS_ModelShort_3293 | Jaguar XJ220S |
| 0xA9F4317E | IDS_ModelShort_3304 | Alfa SE 048SP |
| 0xA9F430FE | IDS_ModelShort_3307 | Nissan 370Z '19 |
| 0xA9F473FE | IDS_ModelShort_3311 | Ferrari FXX-K E |
| 0xA9F4727E | IDS_ModelShort_3312 | Ferrari Monza |
| 0xA9F471FE | IDS_ModelShort_3315 | Koenigsegg Jesko |
| 0xA9F4777E | IDS_ModelShort_3318 | RS 4 Avant '18 |
| 0xA9F4B1FE | IDS_ModelShort_3325 | Aston DB7 GT |
| 0xFD53E9D6 | IDS_ModelShort_333 | Ferrari Enzo |
| 0xA9F577FE | IDS_ModelShort_3359 | Audi RS e-tron |
| 0xFD53EB56 | IDS_ModelShort_336 | Jaguar E-type |
| 0xA9F5B2FE | IDS_ModelShort_3363 | Nis. #23 GT-R |
| 0xA9F5B17E | IDS_ModelShort_3364 | Valhalla Concept |
| 0xA9F5B0FE | IDS_ModelShort_3367 | F8 Tributo '19 |
| 0xA9F5B7FE | IDS_ModelShort_3369 | Corvette C8 '20 |
| 0xA9F5F3FE | IDS_ModelShort_3371 | Huracán EVO |
| 0xA9F5F2FE | IDS_ModelShort_3373 | Toyota 4Runner |
| 0xA9F5F17E | IDS_ModelShort_3374 | Toyota Tacoma |
| 0xFD53EC56 | IDS_ModelShort_338 | McLaren F1 GT |
| 0xA9F671FE | IDS_ModelShort_3395 | Renault 8 |
| 0xFD522856 | IDS_ModelShort_340 | Ferrari F40 |
| 0xA914337E | IDS_ModelShort_3400 | #99 Mazda RX-8 |
| 0xA914327E | IDS_ModelShort_3402 | Toyota Supra '20 |
| 0xA914317E | IDS_ModelShort_3404 | #2069 Bronco R |
| 0xA91473FE | IDS_ModelShort_3411 | #34 Toyota Supra |
| 0xA914727E | IDS_ModelShort_3412 | SUBARU STI S209 |
| 0xA91472FE | IDS_ModelShort_3413 | VW Golf R '21 |
| 0xA914717E | IDS_ModelShort_3414 | LR Defender '20 |
| 0xFD522956 | IDS_ModelShort_342 | Ferrari F50 |
| 0xFD5229D6 | IDS_ModelShort_343 | Fairlady Z '69 |
| 0xA914F17E | IDS_ModelShort_3434 | BMW M2 Comp |
| 0xA914F7FE | IDS_ModelShort_3439 | DeBerti F-250 |
| 0xFD522A56 | IDS_ModelShort_344 | Fairlady Z '03 |
| 0xA91533FE | IDS_ModelShort_3441 | DD Tacoma TRD |
| 0xA91531FE | IDS_ModelShort_3445 | Porsche Taycan S |
| 0xA91537FE | IDS_ModelShort_3449 | Lotus Evija '20 |
| 0xFD522AD6 | IDS_ModelShort_345 | Fairlady Z '94 |
| 0xA915717E | IDS_ModelShort_3454 | Audi RS 3 '20 |
| 0xA915F07E | IDS_ModelShort_3476 | Ford SD F-450 |
| 0xA915F0FE | IDS_ModelShort_3477 | Chevy Trail Boss |
| 0xFD522C56 | IDS_ModelShort_348 | Ford GT '05 |
| 0xA916327E | IDS_ModelShort_3482 | McL. 765 '21 |
| 0xA916307E | IDS_ModelShort_3486 | Wrangler T |
| 0xA916777E | IDS_ModelShort_3498 | Saleen S7 LM |
| 0xA934777E | IDS_ModelShort_3518 | BMW M8 Comp |
| 0xA934B37E | IDS_ModelShort_3520 | Lexus LC 500 '21 |
| 0xA934B2FE | IDS_ModelShort_3523 | #151 FD Supra |
| 0xA934B17E | IDS_ModelShort_3524 | #411 FD Corolla |
| 0xFD5269D6 | IDS_ModelShort_353 | VW GTi Mk2 '92 |
| 0xA934F2FE | IDS_ModelShort_3533 | VW Golf R '22 |
| 0xA934F17E | IDS_ModelShort_3534 | MINI JCW GP '21 |
| 0xA934F7FE | IDS_ModelShort_3539 | #23 SIERRA ALPHA |
| 0xA935337E | IDS_ModelShort_3540 | SIERRA RX3 |
| 0xA93532FE | IDS_ModelShort_3543 | Huayra R '22 |
| 0xA935377E | IDS_ModelShort_3548 | Wuling Sunshine |
| 0xA93537FE | IDS_ModelShort_3549 | #122 AC Class 1 |
| 0xA93573FE | IDS_ModelShort_3551 | #91 BMW M2 |
| 0xA935717E | IDS_ModelShort_3554 | #1 Evolution TA |
| 0xFD526BD6 | IDS_ModelShort_357 | Mitsubishi GTO |
| 0xFD526C56 | IDS_ModelShort_358 | Ferrari 288 GTO |
| 0xA936337E | IDS_ModelShort_3580 | AM DBX '21 |
| 0xA93632FE | IDS_ModelShort_3583 | Audi RS 6 '21 |
| 0xA936317E | IDS_ModelShort_3584 | Audi RS 7 '21 |
| 0xA936737E | IDS_ModelShort_3590 | Chevy K10 '72 |
| 0xA936717E | IDS_ModelShort_3594 | Ferrari Roma |
| 0xA93671FE | IDS_ModelShort_3595 | SF90 Stradale |
| 0xA93670FE | IDS_ModelShort_3597 | Ford F-150 '86 |
| 0xA93677FE | IDS_ModelShort_3599 | GMA T.50 |
| 0xA954337E | IDS_ModelShort_3600 | Venom F5 |
| 0xA95432FE | IDS_ModelShort_3603 | #4402 Ultra 4 |
| 0xA954317E | IDS_ModelShort_3604 | #179 Hammerhead |
| 0xA95431FE | IDS_ModelShort_3605 | #240 Jimco TT |
| 0xA954307E | IDS_ModelShort_3606 | Lambo SCV12 |
| 0xA954377E | IDS_ModelShort_3608 | Lambo Sián R |
| 0xA95473FE | IDS_ModelShort_3611 | Maserati MC20 |
| 0xA954707E | IDS_ModelShort_3616 | AMG GT Black |
| 0xA95470FE | IDS_ModelShort_3617 | M-AMG SL 63 '21 |
| 0xA954B27E | IDS_ModelShort_3622 | Nissan GTR '20 |
| 0xA954B1FE | IDS_ModelShort_3625 | Rimac Nevera |
| 0xA954B7FE | IDS_ModelShort_3629 | Toyota GR Yaris |
| 0xFD52A9D6 | IDS_ModelShort_363 | SUBARU 22B '98 |
| 0xA954F3FE | IDS_ModelShort_3631 | AM Valkyrie AMR |
| 0xFD52AA56 | IDS_ModelShort_364 | SUBARU WRX '04 |
| 0xA95531FE | IDS_ModelShort_3645 | BMW M4 '21 |
| 0xFD52AAD6 | IDS_ModelShort_365 | SUBARU WRX '05 |
| 0xA955737E | IDS_ModelShort_3650 | Mercedes-AMG ONE |
| 0xA95571FE | IDS_ModelShort_3655 | McLaren 620R |
| 0xA95570FE | IDS_ModelShort_3657 | RIVIAN R1T |
| 0xA955B3FE | IDS_ModelShort_3661 | Lotus Exige '18 |
| 0xA955B27E | IDS_ModelShort_3662 | #37 Pro 4 Truck |
| 0xA955B1FE | IDS_ModelShort_3665 | SIERRA 700R |
| 0xA955B0FE | IDS_ModelShort_3667 | 911 GT3 '21 |
| 0xA955B77E | IDS_ModelShort_3668 | McL. Artura '23 |
| 0xA955F37E | IDS_ModelShort_3670 | #4 Ford Focus |
| 0xA955F27E | IDS_ModelShort_3672 | Huracán STO |
| 0xA955F77E | IDS_ModelShort_3678 | Hyundai i30 N |
| 0xFD52AC56 | IDS_ModelShort_368 | Acura Integra |
| 0xA956307E | IDS_ModelShort_3686 | RZR Racing |
| 0xA95630FE | IDS_ModelShort_3687 | Polaris RZR Pro |
| 0xA956727E | IDS_ModelShort_3692 | F-150 Lightning |
| 0xA95672FE | IDS_ModelShort_3693 | #6165 Truck |
| 0xA956777E | IDS_ModelShort_3698 | Porsche MissionR |
| 0xA974337E | IDS_ModelShort_3700 | McL. Sabre '21 |
| 0xA974707E | IDS_ModelShort_3716 | Lotus Emira |
| 0xA97477FE | IDS_ModelShort_3719 | Cadillac CT4-V |
| 0xA974B37E | IDS_ModelShort_3720 | Cadillac CT5-V |
| 0xA974B27E | IDS_ModelShort_3722 | HUMMER EV Truck |
| 0xA974B17E | IDS_ModelShort_3724 | Ferrari 296 GTB |
| 0xA974B07E | IDS_ModelShort_3726 | Integra '23 |
| 0xA974B77E | IDS_ModelShort_3728 | Bentley Cont GTC |
| 0xA974F1FE | IDS_ModelShort_3735 | SUBARU BRZ '22 |
| 0xA974F07E | IDS_ModelShort_3736 | Bronco Raptor |
| 0xA974F0FE | IDS_ModelShort_3737 | BMW iX '22 |
| 0xFD52EA56 | IDS_ModelShort_374 | Lancer MR '06 |
| 0xA975317E | IDS_ModelShort_3744 | #64 Nissan Z |
| 0xA97531FE | IDS_ModelShort_3745 | Audi R8 '20 |
| 0xA975737E | IDS_ModelShort_3750 | Montero '95 |
| 0xA97572FE | IDS_ModelShort_3753 | Huracán Tecnica |
| 0xA97571FE | IDS_ModelShort_3755 | Ford SuperVan 4 |
| 0xA97577FE | IDS_ModelShort_3759 | Huracán Spyder |
| 0xA975B37E | IDS_ModelShort_3760 | Cayman GT4 RS |
| 0xA975B3FE | IDS_ModelShort_3761 | Toyota GR86 '22 |
| 0xA975B2FE | IDS_ModelShort_3763 | BMW M2 '23 |
| 0xA975B17E | IDS_ModelShort_3764 | BMW M5 '22 |
| 0xA975B07E | IDS_ModelShort_3766 | Corvette Z06 '23 |
| 0xA975B0FE | IDS_ModelShort_3767 | Acura NSX '22 |
| 0xA975F3FE | IDS_ModelShort_3771 | Corvette E-Ray |
| 0xA975F2FE | IDS_ModelShort_3773 | Honda Civic '23 |
| 0xA975F17E | IDS_ModelShort_3774 | L. Countach '21 |
| 0xA975F1FE | IDS_ModelShort_3775 | L. Aventador '21 |
| 0xFD52EC56 | IDS_ModelShort_378 | Lancer MR '04 |
| 0xA97633FE | IDS_ModelShort_3781 | P. 911 GT3 RS 23 |
| 0xA97632FE | IDS_ModelShort_3783 | VW Rallye Golf |
| 0xA97631FE | IDS_ModelShort_3785 | Soarer '97 |
| 0xA97637FE | IDS_ModelShort_3789 | Wuling Mini EV |
| 0xFD52ECD6 | IDS_ModelShort_379 | SUBARU LEGACY |
| 0xA97671FE | IDS_ModelShort_3795 | Meyers Manx 2.0 |
| 0xA976777E | IDS_ModelShort_3798 | MX-5 Cup '17 |
| 0xA89473FE | IDS_ModelShort_3811 | Lucid Air |
| 0xA89477FE | IDS_ModelShort_3819 | SUBARU WRX '22 |
| 0xFD512956 | IDS_ModelShort_382 | BMW M3 '97 |
| 0xA894B2FE | IDS_ModelShort_3823 | Mazda MX-5 RF |
| 0xA894B0FE | IDS_ModelShort_3827 | IONIQ 5 N '23 |
| 0xA894B7FE | IDS_ModelShort_3829 | N Vision 74 '22 |
| 0xFD5129D6 | IDS_ModelShort_383 | BMW M3 '05 |
| 0xA895337E | IDS_ModelShort_3840 | Huracán Sterrato |
| 0xA895307E | IDS_ModelShort_3846 | Ford Mustang '24 |
| 0xA89530FE | IDS_ModelShort_3847 | Ford Dark Horse |
| 0xA895377E | IDS_ModelShort_3848 | Toyota Camry '23 |
| 0xA89537FE | IDS_ModelShort_3849 | Ford Raptor R |
| 0xA895737E | IDS_ModelShort_3850 | SRT Durango '21 |
| 0xA89573FE | IDS_ModelShort_3851 | Toyota Sera '91 |
| 0xA895727E | IDS_ModelShort_3852 | Honda Beat '91 |
| 0xA895717E | IDS_ModelShort_3854 | Autozam AZ-1 '93 |
| 0xA89571FE | IDS_ModelShort_3855 | Figaro '91 |
| 0xA895707E | IDS_ModelShort_3856 | Nissan Be-1 '87 |
| 0xA895777E | IDS_ModelShort_3858 | N. Stagea '97 |
| 0xA89577FE | IDS_ModelShort_3859 | City E II '84 |
| 0xA895B37E | IDS_ModelShort_3860 | S-Cargo '89 |
| 0xA895B1FE | IDS_ModelShort_3865 | Honda Acty '94 |
| 0xA896337E | IDS_ModelShort_3880 | GR Corolla '23 |
| 0xA896307E | IDS_ModelShort_3886 | Mit. Evo III '95 |
| 0xA89673FE | IDS_ModelShort_3891 | Revuelto '24 |
| 0xA89671FE | IDS_ModelShort_3895 | M-AMG SLC 43 '20 |
| 0xA8B432FE | IDS_ModelShort_3903 | Ford Fiesta '23 |
| 0xA8B4317E | IDS_ModelShort_3904 | Ford Focus '22 |
| 0xA8B4377E | IDS_ModelShort_3908 | Honda e '22 |
| 0xFD5168D6 | IDS_ModelShort_391 | Maserati MC12 |
| 0xA8B4737E | IDS_ModelShort_3910 | P. 911 Rallye |
| 0xA8B4717E | IDS_ModelShort_3914 | Chaser GT TT '91 |
| 0xA8B470FE | IDS_ModelShort_3917 | Audi R8 GT '23 |
| 0xA8B4777E | IDS_ModelShort_3918 | Gloria GT '95 |
| 0xA8B4B3FE | IDS_ModelShort_3921 | Z NISMO '24 |
| 0xA8B4B77E | IDS_ModelShort_3928 | Hyundai i20 N |
| 0xA8B4B7FE | IDS_ModelShort_3929 | Nissan PAO '89 |
| 0xA8B4F2FE | IDS_ModelShort_3933 | Toyota Chaser V |
| 0xA8B5737E | IDS_ModelShort_3950 | 275 GTB4 '67 |
| 0xA8B572FE | IDS_ModelShort_3953 | 911 Turbo S '23 |
| 0xA8B571FE | IDS_ModelShort_3955 | RAM TRX '24 |
| 0xA8B577FE | IDS_ModelShort_3959 | Challenger '22 |
| 0xA8B5B37E | IDS_ModelShort_3960 | SUBARU Vivio '94 |
| 0xA8B5B17E | IDS_ModelShort_3964 | Toyota Prius '24 |
| 0xFD516C56 | IDS_ModelShort_398 | Toyota MR2 '95 |
| 0xA8B632FE | IDS_ModelShort_3983 | BMW X6 M '24 |
| 0xA8B677FE | IDS_ModelShort_3999 | Nissan Silvia 02 |
| 0xD994327E | IDS_ModelShort_4002 | L. Temerario |
| 0xD994F17E | IDS_ModelShort_4034 | Honda Z '72 |
| 0xD994F77E | IDS_ModelShort_4038 | Toy. Altezza '99 |
| 0xFDB32AD6 | IDS_ModelShort_405 | Ford Mustang '00 |
| 0xD99571FE | IDS_ModelShort_4055 | Toy. Starlet '96 |
| 0xD99570FE | IDS_ModelShort_4057 | Nis. Skyline '97 |
| 0xD995B7FE | IDS_ModelShort_4069 | BMW M3 '88 |
| 0xD99633FE | IDS_ModelShort_4081 | Gemera |
| 0xD996317E | IDS_ModelShort_4084 | Datsun #269 240Z |
| 0xD99631FE | IDS_ModelShort_4085 | Mit. Minicab TA |
| 0xD996737E | IDS_ModelShort_4090 | Mit. Evo. TME |
| 0xD996717E | IDS_ModelShort_4094 | NISMO GT-R '24 |
| 0xFDB368D6 | IDS_ModelShort_411 | Honda NSX-R '05 |
| 0xD9B4717E | IDS_ModelShort_4114 | Skyline GT-R '92 |
| 0xD9B477FE | IDS_ModelShort_4119 | GT-R 40AE |
| 0xFDB36956 | IDS_ModelShort_412 | Honda NSX-R '92 |
| 0xD9B4B17E | IDS_ModelShort_4124 | G 65 Traffic |
| 0xD9B4B1FE | IDS_ModelShort_4125 | Acty Traffic |
| 0xD9B4B07E | IDS_ModelShort_4126 | e Traffic |
| 0xD9B4B0FE | IDS_ModelShort_4127 | Montero Traffic |
| 0xD9B4B77E | IDS_ModelShort_4128 | WRX Traffic |
| 0xD9B4B7FE | IDS_ModelShort_4129 | Stagea Traffic |
| 0xD9B5317E | IDS_ModelShort_4144 | Mazda RX-7 '92 |
| 0xD9B531FE | IDS_ModelShort_4145 | MM 808 Wagon |
| 0xD9B530FE | IDS_ModelShort_4147 | AR Giulia GTAm |
| 0xD9B5707E | IDS_ModelShort_4156 | Ferrari F80 '24 |
| 0xD9B5777E | IDS_ModelShort_4158 | Lexus LFA FE |
| 0xD9B5B37E | IDS_ModelShort_4160 | S-Cargo FE |
| 0xD9B5B27E | IDS_ModelShort_4162 | Toyota AE86 FE |
| 0xD9B5B2FE | IDS_ModelShort_4163 | Sunshine FE |
| 0xD9B5B17E | IDS_ModelShort_4164 | SUBARU BRZ FE |
| 0xD9B5B1FE | IDS_ModelShort_4165 | Mazda RX-3 FE |
| 0xD9B5B07E | IDS_ModelShort_4166 | BMW M2 FE '23 |
| 0xD9B5B0FE | IDS_ModelShort_4167 | GT-R FE '12 |
| 0xD9B5B77E | IDS_ModelShort_4168 | Mustang FE '68 |
| 0xD9B5B7FE | IDS_ModelShort_4169 | MB 190 E FE |
| 0xFDB36BD6 | IDS_ModelShort_417 | Buick Regal GNX |
| 0xD9B5F3FE | IDS_ModelShort_4171 | Ford F-150 FE |
| 0xD9B5F1FE | IDS_ModelShort_4175 | Ford F-450 FE |
| 0xD9B5F7FE | IDS_ModelShort_4179 | #12 N. Skyline |
| 0xFDB36CD6 | IDS_ModelShort_419 | Audi RS 4 '06 |
| 0xD9B670FE | IDS_ModelShort_4197 | MX-5 FE '94 |
| 0xD9B6777E | IDS_ModelShort_4198 | Viper FE '99 |
| 0xD9B677FE | IDS_ModelShort_4199 | Tacoma FE |
| 0xFDB3A856 | IDS_ModelShort_420 | Audi RS 6 '03 |
| 0xD9D4337E | IDS_ModelShort_4200 | Lotus Evija FE |
| 0xD9D431FE | IDS_ModelShort_4205 | Nis. Patrol '72 |
| 0xD9D4737E | IDS_ModelShort_4210 | Exige WTAC |
| 0xD9D473FE | IDS_ModelShort_4211 | #19 CRX WTAC |
| 0xD9D4727E | IDS_ModelShort_4212 | #32 Skyline WTAC |
| 0xD9D472FE | IDS_ModelShort_4213 | #36 Silvia WTAC |
| 0xD9D4717E | IDS_ModelShort_4214 | JJM Supra WTAC |
| 0xD9D4707E | IDS_ModelShort_4216 | Acty 'RakuRaku' |
| 0xFDB3A956 | IDS_ModelShort_422 | Acura RSX |
| 0xD9D4B3FE | IDS_ModelShort_4221 | GR GT Prototype |
| 0xD9D4B27E | IDS_ModelShort_4222 | Nissan Silvia 89 |
| 0xD9D4B2FE | IDS_ModelShort_4223 | Skyline GT-R '00 |
| 0xFDB3A9D6 | IDS_ModelShort_423 | Mazda RX-7 '90 |
| 0xD9D4F3FE | IDS_ModelShort_4231 | #52 S2000 WTAC |
| 0xD9D4F27E | IDS_ModelShort_4232 | Cayman WTAC |
| 0xD9D4F17E | IDS_ModelShort_4234 | Honda Civic '08 |
| 0xD9D5737E | IDS_ModelShort_4250 | Toyota AE86 TG |
| 0xD9D573FE | IDS_ModelShort_4251 | Honda S2000 TG |
| 0xD9D5727E | IDS_ModelShort_4252 | SUBARU 22B TG |
| 0xD9D5717E | IDS_ModelShort_4254 | #33 H. Integra |
| 0xD9D571FE | IDS_ModelShort_4255 | T. Crown Taxi |
| 0xD9D570FE | IDS_ModelShort_4257 | Toyota JPN Taxi |
| 0xD9D577FE | IDS_ModelShort_4259 | 86 Stories |
| 0xD9D5B37E | IDS_ModelShort_4260 | GT-R ID |
| 0xD9D5B3FE | IDS_ModelShort_4261 | P. 911 GT2 ID |
| 0xD9D5B2FE | IDS_ModelShort_4263 | GR GT Prototype |
| 0xD9D5B17E | IDS_ModelShort_4264 | Ferrari FXX-K WP |
| 0xD9D5B1FE | IDS_ModelShort_4265 | M-AMG GT WP |
| 0xD9D5B07E | IDS_ModelShort_4266 | BMW M4 '21 WP |
| 0xD9D5B0FE | IDS_ModelShort_4267 | Mit. Evo VIII WP |
| 0xD9D5B77E | IDS_ModelShort_4268 | Ford Raptor R WP |
| 0xFDB3ABD6 | IDS_ModelShort_427 | Honda S2000 |
| 0xD9D5F0FE | IDS_ModelShort_4277 | #21 Civic WTAC |
| 0xD9D5F77E | IDS_ModelShort_4278 | Toyota LC '25 |
| 0xD9D630FE | IDS_ModelShort_4287 | SUBARU Vivio FE |
| 0xD9F432FE | IDS_ModelShort_4303 | Nissan GT-R TG |
| 0xD9F472FE | IDS_ModelShort_4313 | Porsche 917 FE |
| 0xD9F471FE | IDS_ModelShort_4315 | Peel P50 Trolli |
| 0xFDB3E9D6 | IDS_ModelShort_433 | TVR Sagaris |
| 0xD9F4F27E | IDS_ModelShort_4332 | T. Crown Traffic |
| 0xD9F4F2FE | IDS_ModelShort_4333 | T. Taxi Traffic |
| 0xD9F533FE | IDS_ModelShort_4341 | Ferrari J50 PO |
| 0xD9F5327E | IDS_ModelShort_4342 | Sports 800 FE |
| 0xFDB22856 | IDS_ModelShort_440 | Silvia '94 |
| 0xFDB26AD6 | IDS_ModelShort_455 | Toyota Trueno |
| 0xFDB26C56 | IDS_ModelShort_458 | Lancia Stratos |
| 0xFDB2A856 | IDS_ModelShort_460 | Toyota Supra '92 |
| 0xFDB2A8D6 | IDS_ModelShort_461 | Toyota Supra '98 |
| 0xFDB129D6 | IDS_ModelShort_483 | Dodge Viper '99 |
| 0xFDB12CD6 | IDS_ModelShort_489 | Jaguar XJ220 |
| 0xFD9369D6 | IDS_ModelShort_513 | Dodge Charger 69 |
| 0xFD92ABD6 | IDS_ModelShort_567 | Nissan R390 |
| 0xFD92AC56 | IDS_ModelShort_568 | M-B CLK-GTR |
| 0xFD92ACD6 | IDS_ModelShort_569 | Honda NSX-R GT |
| 0xFDF36AD6 | IDS_ModelShort_615 | Peugeot 207 S |
| 0xFDF3AAD6 | IDS_ModelShort_625 | Honda Civic '07 |
| 0xFDF3E9D6 | IDS_ModelShort_633 | Audi quattro |
| 0xFDF3EA56 | IDS_ModelShort_634 | Renault 5 Turbo |
| 0xFDF3EBD6 | IDS_ModelShort_637 | Lambo Miura |
| 0xFDF3ECD6 | IDS_ModelShort_639 | Challenger '70 |
| 0xFDF228D6 | IDS_ModelShort_641 | Porsche 911 GT1 |

### DefaultGarageLayout — 16 entries (Default garage layout labels)

| Hash | Key | Value |
|------|-----|-------|
| 0x41D30921 | IDS_Description_10d98c1bff744bd886dabb9e977ae1be | The default Garage Layout for Hakusan Mountain Lodge. |
| 0xF3C3BCC7 | IDS_Description_117949c3382a4b0291b678e284379ab3 | The default Garage Layout for Soko 78. |
| 0x284085BA | IDS_Description_19b87d640a4540b6a14a29e77a484e88 | The default Garage Layout for Yashiki House. |
| 0x26838C4E | IDS_Description_1c575ba3171041c7baf4530253e25d58 | The default Garage Layout for Minka House. |
| 0xF275A73B | IDS_Description_40c2baacbd6545dd8ea9ca9e46c854e6 | The default Garage Layout for Tokyo House. |
| 0xAE3E728C | IDS_Description_813083c469f34c2eba49672b446e0d30 | The default Garage Layout for Mei's House. |
| 0x9544524D | IDS_Description_9eb7062719924bda9a7b9b76d5375bca | The default Garage Layout for Fuji Unkai House. |
| 0x46EAF1BE | IDS_Description_ad3e376be1b74e45a7d7721fa3ad407c | The default Garage Layout for Vision House. |
| 0xE50177F7 | IDS_Name_1ef5a74c6aa84616a20f53b448ae3f59 | Hakusan Mountain Lodge |
| 0x2DE4E0F1 | IDS_Name_4f8c3e5092d3432bbdbc0ac07caaf409 | Mei's House |
| 0xAF18FB75 | IDS_Name_7598eb6324dc4f92a8d18b879eb3e5b7 | Minka House |
| 0x57F81C28 | IDS_Name_7e1c38f2791e433bb123753b903430b7 | Vision House |
| 0xF3ECC1E7 | IDS_Name_9a7f5b0039ed4e0f86d17ee4b0a7160d | Tokyo House |
| 0x68CF4565 | IDS_Name_cc83e9ffc7264329be3332609a3546e5 | Fuji Unkai House |
| 0xBD93F72B | IDS_Name_dd9971ed102a4a938c34c98b2f6dfe69 | Yashiki House |
| 0xED716A20 | IDS_Name_eb3603be533c400aa943a2560781b344 | Soko 78 |

### Livery_Decals — 708 entries (Brand/logo display names for decals)

| Hash | Key | Value |
|------|-----|-------|
| 0x70E384A0 | IDS_DisplayName_10 | Addco |
| 0x71C24838 | IDS_DisplayName_100 | Flowmaster |
| 0x92020470 | IDS_DisplayName_10000 | James Bond Edition |
| 0x920204F0 | IDS_DisplayName_10001 | DeLorean |
| 0x92020570 | IDS_DisplayName_10002 | Xbox |
| 0x920205F0 | IDS_DisplayName_10003 | Xbox One X |
| 0x92020670 | IDS_DisplayName_10004 | Xbox Series X¦S |
| 0x920206F0 | IDS_DisplayName_10005 | Xbox Game Pass |
| 0x92020770 | IDS_DisplayName_10006 | Xbox Game Studios |
| 0x920207F0 | IDS_DisplayName_10007 | Pegasus (ExxonMobil) |
| 0x92020070 | IDS_DisplayName_10008 | McLaren |
| 0x920200F0 | IDS_DisplayName_10009 | Ford Racing Oval |
| 0xE12404B8 | IDS_DisplayName_1001 | Forza Motorsport 6 (Vertical) |
| 0x92024470 | IDS_DisplayName_10010 | Ford Racing Wordmark |
| 0x920244F0 | IDS_DisplayName_10011 | Ford Racing Wordmark White |
| 0xE1240538 | IDS_DisplayName_1002 | Forza Motorsport 6 (Horizontal) |
| 0xE12405B8 | IDS_DisplayName_1003 | Spania GTA |
| 0xE1240638 | IDS_DisplayName_1004 | BAC |
| 0xE12406B8 | IDS_DisplayName_1005 | Formula E |
| 0xE1240738 | IDS_DisplayName_1006 | Lola |
| 0xE12407B8 | IDS_DisplayName_1007 | Mercedes-AMG |
| 0xE1240038 | IDS_DisplayName_1008 | Terradyne |
| 0xE12400B8 | IDS_DisplayName_1009 | Datsun |
| 0x71C248B8 | IDS_DisplayName_101 | Focus Tuning |
| 0xE1244438 | IDS_DisplayName_1010 | Watson |
| 0xE12444B8 | IDS_DisplayName_1011 | W Motors |
| 0xE1244538 | IDS_DisplayName_1012 | Sunbeam |
| 0xE12445B8 | IDS_DisplayName_1013 | Zenvo |
| 0xE1244638 | IDS_DisplayName_1014 | Chryslus |
| 0xE12446B8 | IDS_DisplayName_1015 | Meyers |
| 0x71C24938 | IDS_DisplayName_102 | Ford Racing Parts |
| 0x71C249B8 | IDS_DisplayName_103 | Forge |
| 0x71C24AB8 | IDS_DisplayName_105 | Fram Filters |
| 0x71C24B38 | IDS_DisplayName_106 | Garage Vary |
| 0x71C24BB8 | IDS_DisplayName_107 | Garrett |
| 0x71C24C38 | IDS_DisplayName_108 | Gemballa |
| 0x71C24CB8 | IDS_DisplayName_109 | GEMS |
| 0x71C20838 | IDS_DisplayName_110 | Gialla |
| 0x82020470 | IDS_DisplayName_11000 | Forza Motorsport |
| 0x820204F0 | IDS_DisplayName_11001 | Forza Horizon 6 |
| 0x82020570 | IDS_DisplayName_11002 | FH6 Festival |
| 0x820205F0 | IDS_DisplayName_11003 | Gacha City Radio |
| 0x82020670 | IDS_DisplayName_11004 | Horizon Wave |
| 0x820206F0 | IDS_DisplayName_11005 | Horizon XS |
| 0x82020770 | IDS_DisplayName_11006 | Hospital Records |
| 0x820207F0 | IDS_DisplayName_11007 | Opus |
| 0x82020070 | IDS_DisplayName_11008 | Subpop Records |
| 0x820200F0 | IDS_DisplayName_11009 | Yellow Wristband |
| 0x82024470 | IDS_DisplayName_11010 | Green Wristband |
| 0x820244F0 | IDS_DisplayName_11011 | Blue Wristband |
| 0x82024570 | IDS_DisplayName_11012 | Pink Wristband |
| 0x820245F0 | IDS_DisplayName_11013 | Orange Wristband |
| 0x82024670 | IDS_DisplayName_11014 | Purple Wristband |
| 0x820246F0 | IDS_DisplayName_11015 | Gold Wristband |
| 0x82024770 | IDS_DisplayName_11016 | D Class |
| 0x820247F0 | IDS_DisplayName_11017 | C Class |
| 0x82024070 | IDS_DisplayName_11018 | B Class |
| 0x820240F0 | IDS_DisplayName_11019 | A Class |
| 0x82028470 | IDS_DisplayName_11020 | S1 Class |
| 0x820284F0 | IDS_DisplayName_11021 | S2 Class |
| 0x82028570 | IDS_DisplayName_11022 | R Class |
| 0x820285F0 | IDS_DisplayName_11023 | X Class |
| 0x82028670 | IDS_DisplayName_11024 | Cross Country Icon |
| 0x820286F0 | IDS_DisplayName_11025 | Dirt Racing Icon |
| 0x82028770 | IDS_DisplayName_11026 | Road Racing Icon |
| 0x820287F0 | IDS_DisplayName_11027 | Hide & Seek |
| 0x82028070 | IDS_DisplayName_11028 | Play Drift |
| 0x820280F0 | IDS_DisplayName_11029 | Play Racing |
| 0x8202C470 | IDS_DisplayName_11030 | Spec Racing |
| 0x8202C4F0 | IDS_DisplayName_11031 | The Eliminator |
| 0x8202C570 | IDS_DisplayName_11032 | Touge Showdown |
| 0x8202C5F0 | IDS_DisplayName_11033 | Discover Japan |
| 0x8202C670 | IDS_DisplayName_11034 | Yellow Stamp |
| 0x8202C6F0 | IDS_DisplayName_11035 | Green Stamp |
| 0x8202C770 | IDS_DisplayName_11036 | Blue Stamp |
| 0x8202C7F0 | IDS_DisplayName_11037 | Pink Stamp |
| 0x8202C070 | IDS_DisplayName_11038 | Orange Stamp |
| 0x8202C0F0 | IDS_DisplayName_11039 | Purple Stamp |
| 0x82030470 | IDS_DisplayName_11040 | Gold Stamp |
| 0x820304F0 | IDS_DisplayName_11041 | Drift Club Japan |
| 0x82030570 | IDS_DisplayName_11042 | Day Trips |
| 0x820305F0 | IDS_DisplayName_11043 | Moto Auto |
| 0x82030670 | IDS_DisplayName_11044 | Yuji's Autos |
| 0x820306F0 | IDS_DisplayName_11045 | Jobs Icon |
| 0x82030770 | IDS_DisplayName_11046 | Jobs Acty |
| 0x820307F0 | IDS_DisplayName_11047 | RakuRaku |
| 0x82030070 | IDS_DisplayName_11048 | Curry Rice |
| 0x820300F0 | IDS_DisplayName_11049 | Dango |
| 0x82034470 | IDS_DisplayName_11050 | Edamame |
| 0x820344F0 | IDS_DisplayName_11051 | Kakigori |
| 0x82034570 | IDS_DisplayName_11052 | Matcha Tea |
| 0x820345F0 | IDS_DisplayName_11053 | Omurice |
| 0x82034670 | IDS_DisplayName_11054 | Onigiri |
| 0x820346F0 | IDS_DisplayName_11055 | Ramen |
| 0x82034770 | IDS_DisplayName_11056 | Tempura |
| 0x820347F0 | IDS_DisplayName_11057 | Akira |
| 0x82034070 | IDS_DisplayName_11058 | Chika Pin |
| 0x820340F0 | IDS_DisplayName_11059 | Jun |
| 0x82038470 | IDS_DisplayName_11060 | Mr Hot Bun |
| 0x820384F0 | IDS_DisplayName_11061 | Norio-kun |
| 0x82038570 | IDS_DisplayName_11062 | Nozomi |
| 0x820385F0 | IDS_DisplayName_11063 | Prof Hondo |
| 0x82038670 | IDS_DisplayName_11064 | Ryuko |
| 0x820386F0 | IDS_DisplayName_11065 | Sakura |
| 0x82038770 | IDS_DisplayName_11066 | Yazzy |
| 0x820387F0 | IDS_DisplayName_11067 | 365 |
| 0x82038070 | IDS_DisplayName_11068 | Café Asatsuyu |
| 0x820380F0 | IDS_DisplayName_11069 | GAME |
| 0x8203C470 | IDS_DisplayName_11070 | Grand Kaneko |
| 0x8203C4F0 | IDS_DisplayName_11071 | Irokawa Space Center |
| 0x8203C570 | IDS_DisplayName_11072 | Moriya |
| 0x8203C5F0 | IDS_DisplayName_11073 | Nakaya |
| 0x8203C670 | IDS_DisplayName_11074 | Regent |
| 0x8203C6F0 | IDS_DisplayName_11075 | Tatsu |
| 0x8203C770 | IDS_DisplayName_11076 | GridLife |
| 0x8203C7F0 | IDS_DisplayName_11077 | Larry Chen Sticker |
| 0x71C208B8 | IDS_DisplayName_111 | GM Performance |
| 0x71C20938 | IDS_DisplayName_112 | Guldstrand Motorsports |
| 0x71C209B8 | IDS_DisplayName_113 | Goodridge |
| 0x71C20A38 | IDS_DisplayName_114 | Goodyear |
| 0x71C20AB8 | IDS_DisplayName_115 | Gracer |
| 0x71C20B38 | IDS_DisplayName_116 | Gram Lights |
| 0x71C20BB8 | IDS_DisplayName_117 | Gravana Tuning |
| 0x71C20C38 | IDS_DisplayName_118 | Greddy Performance Products, Inc |
| 0x71C20CB8 | IDS_DisplayName_119 | Green Filter |
| 0x70E385A0 | IDS_DisplayName_12 | Alcon |
| 0x71C2C838 | IDS_DisplayName_120 | Grex |
| 0x71C2C938 | IDS_DisplayName_122 | Gude |
| 0x71C2CA38 | IDS_DisplayName_124 | Hahn Racecraft |
| 0x71C2CAB8 | IDS_DisplayName_125 | Hamann |
| 0x71C2CB38 | IDS_DisplayName_126 | Hays |
| 0x71C2CBB8 | IDS_DisplayName_127 | Havoline |
| 0x71C2CCB8 | IDS_DisplayName_129 | Hennessey |
| 0x70E38520 | IDS_DisplayName_13 | American Racing |
| 0x71C288B8 | IDS_DisplayName_131 | HKS |
| 0x71C28938 | IDS_DisplayName_132 | Holley |
| 0x71C289B8 | IDS_DisplayName_133 | Hooker |
| 0x71C28A38 | IDS_DisplayName_134 | Hoosier |
| 0x71C28AB8 | IDS_DisplayName_135 | Hotchkis |
| 0x71C28B38 | IDS_DisplayName_136 | HRE |
| 0x71C28BB8 | IDS_DisplayName_137 | H & R Springs |
| 0x71C28C38 | IDS_DisplayName_138 | Ibherdesign |
| 0x71C28CB8 | IDS_DisplayName_139 | Ichibahn |
| 0x70E386A0 | IDS_DisplayName_14 | Amsoil |
| 0x71C34838 | IDS_DisplayName_140 | INGS |
| 0x71C348B8 | IDS_DisplayName_141 | Injen |
| 0x71C349B8 | IDS_DisplayName_143 | Invidia |
| 0x71C34A38 | IDS_DisplayName_144 | IPD USA |
| 0x71C34AB8 | IDS_DisplayName_145 | ITG |
| 0x71C34B38 | IDS_DisplayName_146 | J's Racing |
| 0x71C34BB8 | IDS_DisplayName_147 | Jackson Racing |
| 0x71C34C38 | IDS_DisplayName_148 | JE Design |
| 0x71C34CB8 | IDS_DisplayName_149 | JSP Motorsport |
| 0x70E38620 | IDS_DisplayName_15 | Anceltion |
| 0x71C30838 | IDS_DisplayName_150 | JUN |
| 0x71C309B8 | IDS_DisplayName_153 | Kaminari USA |
| 0x71C30A38 | IDS_DisplayName_154 | Ken Style |
| 0x71C30AB8 | IDS_DisplayName_155 | Kleeman |
| 0x71C30B38 | IDS_DisplayName_156 | K & N |
| 0x71C30C38 | IDS_DisplayName_158 | Koenigseder |
| 0x71C30CB8 | IDS_DisplayName_159 | Koni |
| 0x70E387A0 | IDS_DisplayName_16 | Ansa |
| 0x71C3C838 | IDS_DisplayName_160 | König |
| 0x71C3C9B8 | IDS_DisplayName_163 | JIC Magic |
| 0x71C3CA38 | IDS_DisplayName_164 | Lingenfelter |
| 0x71C3CB38 | IDS_DisplayName_166 | Lotus Sport |
| 0x70E38720 | IDS_DisplayName_17 | APC - American Products Company |
| 0x71C38838 | IDS_DisplayName_170 | Magnaflow Performance |
| 0x71C388B8 | IDS_DisplayName_171 | Magneti Marelli |
| 0x71C38938 | IDS_DisplayName_172 | Marga Hills |
| 0x71C389B8 | IDS_DisplayName_173 | Mattig |
| 0x71C38AB8 | IDS_DisplayName_175 | MazdaSpeed |
| 0x71C38B38 | IDS_DisplayName_176 | MHT |
| 0x71C38C38 | IDS_DisplayName_178 | Mine's |
| 0x71C04838 | IDS_DisplayName_180 | Mobil 1 |
| 0x71C048B8 | IDS_DisplayName_181 | Momo Auto Accessories |
| 0x71C04938 | IDS_DisplayName_182 | Mopar |
| 0x71C04AB8 | IDS_DisplayName_185 | Motec |
| 0x71C04B38 | IDS_DisplayName_186 | Motegi Racing |
| 0x71C04BB8 | IDS_DisplayName_187 | Moton |
| 0x71C04C38 | IDS_DisplayName_188 | Motul |
| 0x71C04CB8 | IDS_DisplayName_189 | Mugen |
| 0x70E38020 | IDS_DisplayName_19 | A'PEXi |
| 0x71C008B8 | IDS_DisplayName_191 | NGK |
| 0x71C00938 | IDS_DisplayName_192 | Nismo |
| 0x71C009B8 | IDS_DisplayName_193 | Nitto |
| 0x71C00A38 | IDS_DisplayName_194 | NOS |
| 0x71C00AB8 | IDS_DisplayName_195 | Novitec |
| 0x71C00B38 | IDS_DisplayName_196 | NX - Nitrous Express |
| 0x71C00BB8 | IDS_DisplayName_197 | Oettinger |
| 0x71C00C38 | IDS_DisplayName_198 | Ogura Clutch |
| 0x71C00CB8 | IDS_DisplayName_199 | Öhlins |
| 0x40E1C6B9 | IDS_DisplayName_2 | 5Zigen USA, Inc. |
| 0x70E344A0 | IDS_DisplayName_20 | AP Racing |
| 0x71A24838 | IDS_DisplayName_200 | OMP America |
| 0xD1240438 | IDS_DisplayName_2000 | Terradyne |
| 0xD12404B8 | IDS_DisplayName_2001 | Penhall |
| 0xD1240538 | IDS_DisplayName_2002 | Baldwin Motorsports |
| 0xD12405B8 | IDS_DisplayName_2003 | AlumiCraft |
| 0xD1240638 | IDS_DisplayName_2004 | Rebellion Automotive |
| 0xD12406B8 | IDS_DisplayName_2005 | Polaris |
| 0xD1240738 | IDS_DisplayName_2006 | HSV |
| 0xD1240038 | IDS_DisplayName_2008 | International |
| 0xD12400B8 | IDS_DisplayName_2009 | Talbot |
| 0x71A248B8 | IDS_DisplayName_201 | OPC |
| 0xD1244438 | IDS_DisplayName_2010 | HDT |
| 0xD12444B8 | IDS_DisplayName_2011 | RJ Anderson |
| 0xD1244538 | IDS_DisplayName_2012 | Tata |
| 0xD12445B8 | IDS_DisplayName_2013 | RWB |
| 0xD1244638 | IDS_DisplayName_2014 | Hoonigan |
| 0xD12446B8 | IDS_DisplayName_2015 | Quartz |
| 0xD1244738 | IDS_DisplayName_2016 | Yamaha |
| 0xD12447B8 | IDS_DisplayName_2017 | Exomotive |
| 0xD1244038 | IDS_DisplayName_2018 | VUHL |
| 0xD12440B8 | IDS_DisplayName_2019 | Can-Am |
| 0xD1248438 | IDS_DisplayName_2020 | Morris |
| 0xD12484B8 | IDS_DisplayName_2021 | Austin |
| 0xD1248538 | IDS_DisplayName_2022 | Genesis |
| 0xD12485B8 | IDS_DisplayName_2023 | Cooper |
| 0xD1248638 | IDS_DisplayName_2024 | Funco Motorsports |
| 0xD12486B8 | IDS_DisplayName_2025 | Camburg Engineering |
| 0xD1248738 | IDS_DisplayName_2026 | Scuderia Cameron Glickenhaus |
| 0xD12487B8 | IDS_DisplayName_2027 | Campbell Enterprises |
| 0xD1248038 | IDS_DisplayName_2028 | DAF Trucks |
| 0xD12480B8 | IDS_DisplayName_2029 | Peel |
| 0x71A249B8 | IDS_DisplayName_203 | Origin Lab |
| 0xD124C438 | IDS_DisplayName_2030 | Formula Drift |
| 0xD124C4B8 | IDS_DisplayName_2031 | Napier |
| 0xD124C538 | IDS_DisplayName_2032 | Matra |
| 0xD124C5B8 | IDS_DisplayName_2033 | Merkur |
| 0xD124C638 | IDS_DisplayName_2034 | Oreca |
| 0xD124C738 | IDS_DisplayName_2036 | Eagle - EAE |
| 0xD124C7B8 | IDS_DisplayName_2037 | DS |
| 0xD124C038 | IDS_DisplayName_2038 | Alpine |
| 0xD124C0B8 | IDS_DisplayName_2039 | Hillman |
| 0x71A24A38 | IDS_DisplayName_204 | OZ Racing |
| 0xD1250438 | IDS_DisplayName_2040 | Willys |
| 0xD12504B8 | IDS_DisplayName_2041 | Delage |
| 0xD1250538 | IDS_DisplayName_2042 | Rimac |
| 0xD12505B8 | IDS_DisplayName_2043 | Apollo |
| 0xD1250638 | IDS_DisplayName_2044 | Italdesign |
| 0xD12506B8 | IDS_DisplayName_2045 | ATS Automobili |
| 0xD1250738 | IDS_DisplayName_2046 | RAESR |
| 0xD12507B8 | IDS_DisplayName_2047 | LEGO Speed Champions |
| 0xD1250038 | IDS_DisplayName_2048 | Rover |
| 0xD12500B8 | IDS_DisplayName_2049 | Polestar |
| 0xD1254438 | IDS_DisplayName_2050 | BRM |
| 0xD12544B8 | IDS_DisplayName_2051 | Chevron |
| 0xD1254538 | IDS_DisplayName_2052 | Matra |
| 0xD12545B8 | IDS_DisplayName_2053 | Venturi |
| 0xD1254638 | IDS_DisplayName_2054 | Isuzu |
| 0xD12546B8 | IDS_DisplayName_2055 | All American Racers |
| 0xD1254738 | IDS_DisplayName_2056 | Elemental |
| 0xD12547B8 | IDS_DisplayName_2057 | Ligier |
| 0xD1254038 | IDS_DisplayName_2058 | Automobili Pininfarina |
| 0xD12540B8 | IDS_DisplayName_2059 | Penske |
| 0x71A24B38 | IDS_DisplayName_206 | Paxton |
| 0xD1258438 | IDS_DisplayName_2060 | NIO |
| 0xD12584B8 | IDS_DisplayName_2061 | Elva |
| 0xD1258538 | IDS_DisplayName_2062 | Ginetta |
| 0xD12585B8 | IDS_DisplayName_2063 | Quadra |
| 0xD12586B8 | IDS_DisplayName_2065 | DeBerti |
| 0x71A24BB8 | IDS_DisplayName_207 | PES |
| 0xD125C438 | IDS_DisplayName_2070 | Hot Wheels Monster Trucks |
| 0xD125C4B8 | IDS_DisplayName_2071 | Forsberg Racing |
| 0xD125C538 | IDS_DisplayName_2072 | SIERRA Cars |
| 0xD125C5B8 | IDS_DisplayName_2073 | Xpeng |
| 0xD125C638 | IDS_DisplayName_2074 | Wuling |
| 0xD125C6B8 | IDS_DisplayName_2075 | Lynk & Co |
| 0xD125C738 | IDS_DisplayName_2076 | Bajaj |
| 0xD125C7B8 | IDS_DisplayName_2077 | Gordon Murray Automotive |
| 0xD125C038 | IDS_DisplayName_2078 | Icona |
| 0xD125C0B8 | IDS_DisplayName_2079 | Jimco |
| 0x71A24C38 | IDS_DisplayName_208 | Peugeot Sport Int'l |
| 0xD1260438 | IDS_DisplayName_2080 | Rivian |
| 0xD12604B8 | IDS_DisplayName_2081 | Spark |
| 0xD1260538 | IDS_DisplayName_2082 | Casey Currie Motorsports |
| 0xD12605B8 | IDS_DisplayName_2083 | Mirage |
| 0xD1260638 | IDS_DisplayName_2084 | Extreme E |
| 0xD12606B8 | IDS_DisplayName_2085 | Schuppan |
| 0xD1260738 | IDS_DisplayName_2086 | CUPRA |
| 0xD12607B8 | IDS_DisplayName_2087 | Czinger |
| 0xD1260038 | IDS_DisplayName_2088 | McMurtry |
| 0xD12600B8 | IDS_DisplayName_2089 | Freightliner |
| 0x71A24CB8 | IDS_DisplayName_209 | P-Factor |
| 0xD1264438 | IDS_DisplayName_2090 | Zeekr |
| 0xD12644B8 | IDS_DisplayName_2091 | DUQUEINE |
| 0xD1264538 | IDS_DisplayName_2092 | Lucid |
| 0xD12645B8 | IDS_DisplayName_2093 | Singer |
| 0xD1264638 | IDS_DisplayName_2094 | Universal Studios |
| 0xD12646B8 | IDS_DisplayName_2095 | Fast and Furious |
| 0xD1264738 | IDS_DisplayName_2096 | Eunos |
| 0xD12647B8 | IDS_DisplayName_2097 | Daihatsu |
| 0xD1264038 | IDS_DisplayName_2098 | Autozam |
| 0xD12640B8 | IDS_DisplayName_2099 | Parnelli |
| 0x70E34420 | IDS_DisplayName_21 | Arrow |
| 0xD1040438 | IDS_DisplayName_2100 | Forza Motorsport (horizontal) |
| 0xD10404B8 | IDS_DisplayName_2101 | Forza Motorsport (stacked) |
| 0x71A208B8 | IDS_DisplayName_211 | Phillips 66 |
| 0xD10446B8 | IDS_DisplayName_2115 | INEOS |
| 0x71A209B8 | IDS_DisplayName_213 | PIAA |
| 0xD104C7B8 | IDS_DisplayName_2137 | GR |
| 0x71A20A38 | IDS_DisplayName_214 | Pirelli |
| 0x71A20AB8 | IDS_DisplayName_215 | Porsche Motorsport |
| 0x71A20BB8 | IDS_DisplayName_217 | Progress |
| 0x71A20C38 | IDS_DisplayName_218 | Pro Turbo Systems |
| 0x71A20CB8 | IDS_DisplayName_219 | Pure |
| 0x70E345A0 | IDS_DisplayName_22 | Arco |
| 0x71A2C838 | IDS_DisplayName_220 | Quaife |
| 0x71A2C8B8 | IDS_DisplayName_221 | Quaker State |
| 0x71A2C938 | IDS_DisplayName_222 | Quartermaster |
| 0x71A2C9B8 | IDS_DisplayName_223 | Racetech NA |
| 0x71A2CA38 | IDS_DisplayName_224 | Racing Beat |
| 0x71A2CAB8 | IDS_DisplayName_225 | Racing Dynamics |
| 0x71A2CB38 | IDS_DisplayName_226 | Racing Hart |
| 0x71A2CBB8 | IDS_DisplayName_227 | Ralliart |
| 0x71A2CC38 | IDS_DisplayName_228 | RAYS Engineering |
| 0x71A2CCB8 | IDS_DisplayName_229 | Razzi Ground Effects |
| 0x70E34520 | IDS_DisplayName_23 | APR Performance |
| 0x71A28838 | IDS_DisplayName_230 | Recaro |
| 0x71A288B8 | IDS_DisplayName_231 | Cut |
| 0x71A28938 | IDS_DisplayName_232 | Red Line Oil |
| 0x71A289B8 | IDS_DisplayName_233 | Reiter Engineering |
| 0x71A28AB8 | IDS_DisplayName_235 | Renault Sport |
| 0x71A28B38 | IDS_DisplayName_236 | Rieger |
| 0x71A28BB8 | IDS_DisplayName_237 | RK Sport |
| 0x71A28C38 | IDS_DisplayName_238 | R Magic |
| 0x71A28CB8 | IDS_DisplayName_239 | RMR Products |
| 0x70E346A0 | IDS_DisplayName_24 | Audi Motorsport |
| 0x71A34838 | IDS_DisplayName_240 | Road Race Engineering |
| 0x71A348B8 | IDS_DisplayName_241 | RO_JA Motorsports |
| 0x71A34938 | IDS_DisplayName_242 | Pennzoil |
| 0x71A34B38 | IDS_DisplayName_246 | Royal Purple |
| 0x71A34BB8 | IDS_DisplayName_247 | Sachs |
| 0x71A34C38 | IDS_DisplayName_248 | Sarona Design |
| 0x71A34CB8 | IDS_DisplayName_249 | Saturn Motorports |
| 0x71A30838 | IDS_DisplayName_250 | Scorpion |
| 0x71A308B8 | IDS_DisplayName_251 | SEAT Sport |
| 0x71A30938 | IDS_DisplayName_252 | Seibon |
| 0x71A30A38 | IDS_DisplayName_254 | Shelby Automobiles |
| 0x71A30AB8 | IDS_DisplayName_255 | Shell |
| 0x71A30B38 | IDS_DisplayName_256 | Shine Street |
| 0x71A30BB8 | IDS_DisplayName_257 | Skunk2 |
| 0x71A30C38 | IDS_DisplayName_258 | Sparco |
| 0x71A30CB8 | IDS_DisplayName_259 | Spearco |
| 0x71A3C838 | IDS_DisplayName_260 | Speed Alliance |
| 0x71A3C8B8 | IDS_DisplayName_261 | Speedline Corse |
| 0x71A3C938 | IDS_DisplayName_262 | Squires |
| 0x71A3C9B8 | IDS_DisplayName_263 | ST Suspension |
| 0x71A3CA38 | IDS_DisplayName_264 | Sti |
| 0x71A3CAB8 | IDS_DisplayName_265 | Stillen |
| 0x71A3CB38 | IDS_DisplayName_266 | StopTech |
| 0x71A3CBB8 | IDS_DisplayName_267 | Street Concept |
| 0x71A3CCB8 | IDS_DisplayName_269 | Sunoco |
| 0x70E34720 | IDS_DisplayName_27 | Auto Exe Inc. |
| 0x71A38838 | IDS_DisplayName_270 | Supersprint |
| 0x71A388B8 | IDS_DisplayName_271 | SYMS |
| 0x71A38938 | IDS_DisplayName_272 | Taitec |
| 0x71A389B8 | IDS_DisplayName_273 | Tanabe |
| 0x71A38A38 | IDS_DisplayName_274 | Tech Art |
| 0x71A38AB8 | IDS_DisplayName_275 | Tein |
| 0x71A38B38 | IDS_DisplayName_276 | Texaco |
| 0x71A38BB8 | IDS_DisplayName_277 | Tilton |
| 0x71A38C38 | IDS_DisplayName_278 | Toda |
| 0x71A38CB8 | IDS_DisplayName_279 | Tom's Co., Ltd. |
| 0x70E340A0 | IDS_DisplayName_28 | Autobacs |
| 0x71A04938 | IDS_DisplayName_282 | Top Secret |
| 0x71A049B8 | IDS_DisplayName_283 | Torsen |
| 0x71A04A38 | IDS_DisplayName_284 | Toyo Tires |
| 0x71A04B38 | IDS_DisplayName_286 | Trial Japan/Trial USA |
| 0x71A04C38 | IDS_DisplayName_288 | TSW |
| 0x71A04CB8 | IDS_DisplayName_289 | Tubi |
| 0x70E34020 | IDS_DisplayName_29 | Autorotor |
| 0x71A00838 | IDS_DisplayName_290 | Turbonetics |
| 0x71A009B8 | IDS_DisplayName_293 | Valvoline |
| 0x71A00A38 | IDS_DisplayName_294 | Varis |
| 0x71A00AB8 | IDS_DisplayName_295 | Vertex |
| 0x71A00B38 | IDS_DisplayName_296 | VF Engineering |
| 0x71A00BB8 | IDS_DisplayName_297 | VIS Racing |
| 0x71A00C38 | IDS_DisplayName_298 | Volk Racing |
| 0x71A00CB8 | IDS_DisplayName_299 | Volvo - Factory Option |
| 0x40E1C639 | IDS_DisplayName_3 | AB Flug |
| 0x71824838 | IDS_DisplayName_300 | Vortech |
| 0xC12404B8 | IDS_DisplayName_3001 | Forza Horizon 2 |
| 0xC1240538 | IDS_DisplayName_3002 | Playground |
| 0xC12405B8 | IDS_DisplayName_3003 | Horizon Festival |
| 0xC1240638 | IDS_DisplayName_3004 | Playground Games |
| 0xC12406B8 | IDS_DisplayName_3005 | Horizon Bass Arena Radio |
| 0xC1240738 | IDS_DisplayName_3006 | Horizon Pulse Radio |
| 0xC12407B8 | IDS_DisplayName_3007 | Horizon XS Radio |
| 0xC1240038 | IDS_DisplayName_3008 | Levante FM Radio |
| 0xC12400B8 | IDS_DisplayName_3009 | Forza Horizon |
| 0x718248B8 | IDS_DisplayName_301 | VST - Volvo Sport Tuning |
| 0xC1244438 | IDS_DisplayName_3010 | Forza Horizon 3 (Vertical) |
| 0xC12444B8 | IDS_DisplayName_3011 | Block Party |
| 0xC1244538 | IDS_DisplayName_3012 | Timeless |
| 0xC12445B8 | IDS_DisplayName_3013 | Rockstar |
| 0xC1244638 | IDS_DisplayName_3014 | Reliant |
| 0xC12446B8 | IDS_DisplayName_3015 | Jensen |
| 0xC1244738 | IDS_DisplayName_3016 | UNSC Emblem |
| 0xC12447B8 | IDS_DisplayName_3017 | UNSC Logo |
| 0xC1244038 | IDS_DisplayName_3018 | Liang Dortmund Logo (Small) |
| 0xC12440B8 | IDS_DisplayName_3019 | Liang Dortmund Logo (Large) |
| 0x71824938 | IDS_DisplayName_302 | VXR |
| 0xC1248438 | IDS_DisplayName_3020 | Liberty Walk |
| 0xC1248538 | IDS_DisplayName_3022 | RWB |
| 0xC12485B8 | IDS_DisplayName_3023 | Forza Horizon 4 |
| 0xC1248638 | IDS_DisplayName_3024 | Forza Motorsport 7 (Vertical) |
| 0x71824A38 | IDS_DisplayName_304 | Weiand |
| 0x71824AB8 | IDS_DisplayName_305 | Whipple |
| 0x71824B38 | IDS_DisplayName_306 | Wilwood |
| 0x71824BB8 | IDS_DisplayName_307 | Wings West |
| 0x71824C38 | IDS_DisplayName_308 | Work |
| 0x70E30420 | IDS_DisplayName_31 | AWE |
| 0x71820838 | IDS_DisplayName_310 | XS Engineering |
| 0x718208B8 | IDS_DisplayName_311 | Yokohama |
| 0x718209B8 | IDS_DisplayName_313 | Zero/sports |
| 0x71820A38 | IDS_DisplayName_314 | ZEX |
| 0x71820AB8 | IDS_DisplayName_315 | ZF |
| 0x71820B38 | IDS_DisplayName_316 | Acura |
| 0x71820BB8 | IDS_DisplayName_317 | Aston Martin |
| 0x71820C38 | IDS_DisplayName_318 | Audi |
| 0x71820CB8 | IDS_DisplayName_319 | Bentley |
| 0x7182C8B8 | IDS_DisplayName_321 | BMW |
| 0x7182C938 | IDS_DisplayName_322 | Tire Rack |
| 0x7182C9B8 | IDS_DisplayName_323 | Buick |
| 0x7182CA38 | IDS_DisplayName_324 | Cadillac |
| 0x7182CAB8 | IDS_DisplayName_325 | Chevrolet |
| 0x7182CB38 | IDS_DisplayName_326 | Chrysler |
| 0x7182CBB8 | IDS_DisplayName_327 | Dodge |
| 0x7182CC38 | IDS_DisplayName_328 | Eagle |
| 0x7182CCB8 | IDS_DisplayName_329 | Ferrari |
| 0x70E30520 | IDS_DisplayName_33 | BBS |
| 0x71828838 | IDS_DisplayName_330 | Ford |
| 0x718288B8 | IDS_DisplayName_331 | Honda |
| 0x71828938 | IDS_DisplayName_332 | Hyundai |
| 0x718289B8 | IDS_DisplayName_333 | Infiniti |
| 0x71828A38 | IDS_DisplayName_334 | Jaguar |
| 0x71828AB8 | IDS_DisplayName_335 | Koenigsegg |
| 0x71828B38 | IDS_DisplayName_336 | Lamborghini |
| 0x71828BB8 | IDS_DisplayName_337 | Lancia |
| 0x71828C38 | IDS_DisplayName_338 | Lexus |
| 0x71828CB8 | IDS_DisplayName_339 | Lotus |
| 0x70E306A0 | IDS_DisplayName_34 | Bell |
| 0x71834838 | IDS_DisplayName_340 | Maserati |
| 0x718348B8 | IDS_DisplayName_341 | Mazda |
| 0x71834938 | IDS_DisplayName_342 | McLaren |
| 0x718349B8 | IDS_DisplayName_343 | Mercedes-Benz |
| 0x71834A38 | IDS_DisplayName_344 | MINI |
| 0x71834AB8 | IDS_DisplayName_345 | Mitsubishi |
| 0x71834B38 | IDS_DisplayName_346 | Nissan |
| 0x71834BB8 | IDS_DisplayName_347 | Opel |
| 0x71834CB8 | IDS_DisplayName_349 | Pagani |
| 0x70E30620 | IDS_DisplayName_35 | BF Goodrich |
| 0x71830838 | IDS_DisplayName_350 | Panoz |
| 0x718308B8 | IDS_DisplayName_351 | Peugeot |
| 0x71830938 | IDS_DisplayName_352 | Plymouth |
| 0x718309B8 | IDS_DisplayName_353 | Pontiac |
| 0x71830A38 | IDS_DisplayName_354 | Porsche |
| 0x71830AB8 | IDS_DisplayName_355 | Proto Motors |
| 0x71830B38 | IDS_DisplayName_356 | Renault |
| 0x71830BB8 | IDS_DisplayName_357 | Saab |
| 0x71830C38 | IDS_DisplayName_358 | Saleen |
| 0x71830CB8 | IDS_DisplayName_359 | Saturn |
| 0x70E307A0 | IDS_DisplayName_36 | Bilstein |
| 0x7183C838 | IDS_DisplayName_360 | Scion |
| 0x7183C8B8 | IDS_DisplayName_361 | SEAT |
| 0x7183C938 | IDS_DisplayName_362 | Shelby |
| 0x7183C9B8 | IDS_DisplayName_363 | Subaru |
| 0x7183CA38 | IDS_DisplayName_364 | Toyota |
| 0x7183CAB8 | IDS_DisplayName_365 | TVR |
| 0x7183CB38 | IDS_DisplayName_366 | Vauxhall |
| 0x7183CBB8 | IDS_DisplayName_367 | Volkswagen |
| 0x7183CC38 | IDS_DisplayName_368 | Volvo |
| 0x7183CCB8 | IDS_DisplayName_369 | Forza Motorsport |
| 0x70E30720 | IDS_DisplayName_37 | Blitz |
| 0x71838838 | IDS_DisplayName_370 | Forza Motorsport 2 |
| 0x718388B8 | IDS_DisplayName_371 | MGS |
| 0x71838938 | IDS_DisplayName_372 | Microsoft |
| 0x718389B8 | IDS_DisplayName_373 | XBOX (Horizontal Format) |
| 0x71838A38 | IDS_DisplayName_374 | XBOX (Vertical Format) |
| 0x71838AB8 | IDS_DisplayName_375 | XBOX 360 (Horizontal Format) |
| 0x71838B38 | IDS_DisplayName_376 | XBOX 360 (Vertical Format) |
| 0x71838BB8 | IDS_DisplayName_377 | XBox LIVE |
| 0x71838C38 | IDS_DisplayName_378 | Turn 10 |
| 0x71838CB8 | IDS_DisplayName_379 | XBOX360 Rings of Light |
| 0x70E300A0 | IDS_DisplayName_38 | BMP Design |
| 0x71804838 | IDS_DisplayName_380 | Procharger |
| 0x718048B8 | IDS_DisplayName_381 | Magnusson |
| 0x71804938 | IDS_DisplayName_382 | Porsche 911 |
| 0x718049B8 | IDS_DisplayName_383 | Porsche T'Equipment |
| 0x71804A38 | IDS_DisplayName_384 | GM |
| 0x71804AB8 | IDS_DisplayName_385 | STaSIS Engineering |
| 0x71804B38 | IDS_DisplayName_386 | TRD |
| 0x71804BB8 | IDS_DisplayName_387 | Abarth |
| 0x71804C38 | IDS_DisplayName_388 | ABT Sportsline |
| 0x71804CB8 | IDS_DisplayName_389 | Advan |
| 0x70E30020 | IDS_DisplayName_39 | Bomex |
| 0x71800838 | IDS_DisplayName_390 | AiM Sport |
| 0x718008B8 | IDS_DisplayName_391 | Aisin |
| 0x71800938 | IDS_DisplayName_392 | Alfa Romeo |
| 0x718009B8 | IDS_DisplayName_393 | Alpinestars |
| 0x71800A38 | IDS_DisplayName_394 | APR Motorsports |
| 0x71800AB8 | IDS_DisplayName_395 | ARE |
| 0x71800B38 | IDS_DisplayName_396 | Arias Pistons |
| 0x71800BB8 | IDS_DisplayName_397 | Asanti |
| 0x71800C38 | IDS_DisplayName_398 | Aston Martin Racing |
| 0x71800CB8 | IDS_DisplayName_399 | ATS |
| 0x40E1C5B9 | IDS_DisplayName_4 | Accel |
| 0x70E2C4A0 | IDS_DisplayName_40 | Borbet |
| 0x71624838 | IDS_DisplayName_400 | ATS Diesel |
| 0x716248B8 | IDS_DisplayName_401 | Beru |
| 0x71624938 | IDS_DisplayName_402 | Borrani |
| 0x716249B8 | IDS_DisplayName_403 | Boyd Coddington |
| 0x71624A38 | IDS_DisplayName_404 | Bugatti |
| 0x71624AB8 | IDS_DisplayName_405 | Cadillac V Series |
| 0x71624B38 | IDS_DisplayName_406 | Callaway |
| 0x71624BB8 | IDS_DisplayName_407 | Citroën |
| 0x71624C38 | IDS_DisplayName_408 | Clutch Masters |
| 0x71624CB8 | IDS_DisplayName_409 | Comp Cams |
| 0x70E2C420 | IDS_DisplayName_41 | Border |
| 0x71620838 | IDS_DisplayName_410 | Compomotive |
| 0x716209B8 | IDS_DisplayName_413 | CP Pistons |
| 0x71620A38 | IDS_DisplayName_414 | Cragar |
| 0x71620AB8 | IDS_DisplayName_415 | Crower |
| 0x71620B38 | IDS_DisplayName_416 | Dellorto |
| 0x71620BB8 | IDS_DisplayName_417 | Dodge SRT |
| 0x71620C38 | IDS_DisplayName_418 | Dropstars |
| 0x71620CB8 | IDS_DisplayName_419 | DUB |
| 0x70E2C5A0 | IDS_DisplayName_42 | Borla Performance Industries |
| 0x7162C838 | IDS_DisplayName_420 | Dymag |
| 0x7162C8B8 | IDS_DisplayName_421 | EBC Brakes |
| 0x7162C938 | IDS_DisplayName_422 | Edo Competition |
| 0x7162C9B8 | IDS_DisplayName_423 | Ferrari Corse Clienti |
| 0x7162CA38 | IDS_DisplayName_424 | FIAT |
| 0x7162CAB8 | IDS_DisplayName_425 | F-Sport |
| 0x7162CB38 | IDS_DisplayName_426 | Getrag |
| 0x7162CBB8 | IDS_DisplayName_427 | Giacuzzo |
| 0x7162CC38 | IDS_DisplayName_428 | Halibrand |
| 0x7162CCB8 | IDS_DisplayName_429 | Hirsch Performance (Saab) |
| 0x70E2C520 | IDS_DisplayName_43 | Bosch |
| 0x71628838 | IDS_DisplayName_430 | Hitachi |
| 0x716288B8 | IDS_DisplayName_431 | Hole Shot Wheels |
| 0x71628938 | IDS_DisplayName_432 | iForged |
| 0x716289B8 | IDS_DisplayName_433 | JE Pistons |
| 0x71628A38 | IDS_DisplayName_434 | Jenvey |
| 0x71628AB8 | IDS_DisplayName_435 | Kosei |
| 0x71628B38 | IDS_DisplayName_436 | KW |
| 0x71628BB8 | IDS_DisplayName_437 | Land Rover |
| 0x71628C38 | IDS_DisplayName_438 | Lexani |
| 0x71628CB8 | IDS_DisplayName_439 | Line Extras |
| 0x70E2C6A0 | IDS_DisplayName_44 | Bozz Speed |
| 0x71634838 | IDS_DisplayName_440 | Lumma Design |
| 0x716348B8 | IDS_DisplayName_441 | Lunati |
| 0x71634938 | IDS_DisplayName_442 | Magnuson |
| 0x716349B8 | IDS_DisplayName_443 | Mahle |
| 0x71634A38 | IDS_DisplayName_444 | Manley |
| 0x71634AB8 | IDS_DisplayName_445 | Maserati Corse |
| 0x71634B38 | IDS_DisplayName_446 | Mickey Thompson |
| 0x71634BB8 | IDS_DisplayName_447 | Modulare |
| 0x71634C38 | IDS_DisplayName_448 | Monster Motorsport |
| 0x71634CB8 | IDS_DisplayName_449 | MSD |
| 0x71630838 | IDS_DisplayName_450 | Piper Cams |
| 0x71630938 | IDS_DisplayName_452 | Ricardo |
| 0x716309B8 | IDS_DisplayName_453 | Rota |
| 0x71630A38 | IDS_DisplayName_454 | RS Watanabe |
| 0x71630AB8 | IDS_DisplayName_455 | Safety Devices |
| 0x71630B38 | IDS_DisplayName_456 | Schrick |
| 0x71630BB8 | IDS_DisplayName_457 | Setrab |
| 0x71630C38 | IDS_DisplayName_458 | Solex |
| 0x71630CB8 | IDS_DisplayName_459 | Steeda |
| 0x70E2C7A0 | IDS_DisplayName_46 | Brabus |
| 0x7163C838 | IDS_DisplayName_460 | Supertech |
| 0x7163C8B8 | IDS_DisplayName_461 | Team Dynamics Racing |
| 0x7163C938 | IDS_DisplayName_462 | Tenzo R |
| 0x7163C9B8 | IDS_DisplayName_463 | Tial |
| 0x7163CA38 | IDS_DisplayName_464 | Tremec |
| 0x7163CB38 | IDS_DisplayName_466 | VFN Fiberglass |
| 0x7163CBB8 | IDS_DisplayName_467 | Vienna |
| 0x7163CC38 | IDS_DisplayName_468 | Weber |
| 0x7163CCB8 | IDS_DisplayName_469 | WedsSport |
| 0x70E2C720 | IDS_DisplayName_47 | Brembo |
| 0x71638838 | IDS_DisplayName_470 | Weichers Sport |
| 0x716388B8 | IDS_DisplayName_471 | Weld Racing |
| 0x71638938 | IDS_DisplayName_472 | Whiteline |
| 0x716389B8 | IDS_DisplayName_473 | Wiseco |
| 0x71638A38 | IDS_DisplayName_474 | Wossner |
| 0x71638AB8 | IDS_DisplayName_475 | Forza Motorsport 3 |
| 0x71638B38 | IDS_DisplayName_476 | FM3 |
| 0x71638BB8 | IDS_DisplayName_477 | Forza Motorsport 3 Script |
| 0x71638C38 | IDS_DisplayName_478 | MSN Autos |
| 0x71638CB8 | IDS_DisplayName_479 | Bertone |
| 0x70E2C0A0 | IDS_DisplayName_48 | Breyton |
| 0x71604838 | IDS_DisplayName_480 | Devon |
| 0x716048B8 | IDS_DisplayName_481 | Gumpert |
| 0x71604938 | IDS_DisplayName_482 | Joss |
| 0x716049B8 | IDS_DisplayName_483 | Morgan |
| 0x71604A38 | IDS_DisplayName_484 | Mosler |
| 0x71604AB8 | IDS_DisplayName_485 | Noble |
| 0x71604B38 | IDS_DisplayName_486 | Radical |
| 0x71604BB8 | IDS_DisplayName_487 | Rossion |
| 0x71604C38 | IDS_DisplayName_488 | Spada Vetture Sport |
| 0x71604CB8 | IDS_DisplayName_489 | Spyker |
| 0x70E2C020 | IDS_DisplayName_49 | Bride |
| 0x71600838 | IDS_DisplayName_490 | SSC |
| 0x716008B8 | IDS_DisplayName_491 | Wiesmann |
| 0x71600938 | IDS_DisplayName_492 | Kia |
| 0x716009B8 | IDS_DisplayName_493 | Suzuki |
| 0x71600A38 | IDS_DisplayName_494 | AMC |
| 0x71600AB8 | IDS_DisplayName_495 | De Tomaso |
| 0x71600BB8 | IDS_DisplayName_497 | Mercury |
| 0x71600C38 | IDS_DisplayName_498 | GMC |
| 0x71600CB8 | IDS_DisplayName_499 | Tesla |
| 0x70E284A0 | IDS_DisplayName_50 | Bridgestone |
| 0x71424838 | IDS_DisplayName_500 | Oldsmobile |
| 0x714248B8 | IDS_DisplayName_501 | HUMMER |
| 0x71424938 | IDS_DisplayName_502 | AMG Transport Dynamics |
| 0x714249B8 | IDS_DisplayName_503 | Eagle |
| 0x71424A38 | IDS_DisplayName_504 | Jeep |
| 0x71424AB8 | IDS_DisplayName_505 | Plymouth |
| 0x71424BB8 | IDS_DisplayName_507 | FM4 |
| 0x71424C38 | IDS_DisplayName_508 | RUF |
| 0x71424CB8 | IDS_DisplayName_509 | Holden |
| 0x70E28420 | IDS_DisplayName_51 | Buddy Club |
| 0x71420838 | IDS_DisplayName_510 | Ascari |
| 0x714208B8 | IDS_DisplayName_511 | Fisker |
| 0x71420938 | IDS_DisplayName_512 | Skoda |
| 0x714209B8 | IDS_DisplayName_513 | smart |
| 0x71420A38 | IDS_DisplayName_514 | Ultima |
| 0x71420AB8 | IDS_DisplayName_515 | Austin-Healey |
| 0x71420B38 | IDS_DisplayName_516 | MG |
| 0x71420BB8 | IDS_DisplayName_517 | Triumph |
| 0x71420C38 | IDS_DisplayName_518 | Bowler |
| 0x71420CB8 | IDS_DisplayName_519 | Hudson |
| 0x70E285A0 | IDS_DisplayName_52 | BurnsStainless |
| 0x7142C838 | IDS_DisplayName_520 | Hennessey |
| 0x7142C8B8 | IDS_DisplayName_521 | Viper |
| 0x7142C938 | IDS_DisplayName_522 | Lincoln |
| 0x7142C9B8 | IDS_DisplayName_523 | Maybach |
| 0x7142CA38 | IDS_DisplayName_524 | Chaparral |
| 0x7142CB38 | IDS_DisplayName_526 | Ariel |
| 0x7142CBB8 | IDS_DisplayName_527 | Caterham |
| 0x7142CC38 | IDS_DisplayName_528 | Infinity |
| 0x7142CCB8 | IDS_DisplayName_529 | KTM |
| 0x70E28520 | IDS_DisplayName_53 | Burn-up |
| 0x71428838 | IDS_DisplayName_530 | Brabham |
| 0x714288B8 | IDS_DisplayName_531 | Forza Motorsport 5 (Logo) |
| 0x71428938 | IDS_DisplayName_532 | Forza Motorsport 5 (Vertical) |
| 0x714289B8 | IDS_DisplayName_533 | Forza Motorsport 5 (Horizontal) |
| 0x71428A38 | IDS_DisplayName_534 | Xbox One (Color) |
| 0x71428AB8 | IDS_DisplayName_535 | Xbox One |
| 0x71428B38 | IDS_DisplayName_536 | Falken |
| 0x71428BB8 | IDS_DisplayName_537 | Rolls-Royce |
| 0x71428C38 | IDS_DisplayName_538 | Donkervoort |
| 0x71428CB8 | IDS_DisplayName_539 | Hot Wheels |
| 0x70E286A0 | IDS_DisplayName_54 | Caractere |
| 0x71434838 | IDS_DisplayName_540 | Hot Wheels (Reverse) |
| 0x714348B8 | IDS_DisplayName_541 | Savage Rivale |
| 0x71434938 | IDS_DisplayName_542 | Local Motors |
| 0x714349B8 | IDS_DisplayName_543 | Ram |
| 0x71434A38 | IDS_DisplayName_544 | Caparo |
| 0x71434AB8 | IDS_DisplayName_545 | SRT |
| 0x71434B38 | IDS_DisplayName_546 | Robby Gordon |
| 0x71434BB8 | IDS_DisplayName_547 | Auto Union |
| 0x71434C38 | IDS_DisplayName_548 | RAM |
| 0x71434CB8 | IDS_DisplayName_549 | Caparo |
| 0x71430838 | IDS_DisplayName_550 | Robby Gordon |
| 0x714308B8 | IDS_DisplayName_551 | Forza Horizon 2 Storm Island |
| 0x70E287A0 | IDS_DisplayName_56 | Castrol |
| 0x70E28720 | IDS_DisplayName_57 | Centerforce |
| 0x70E280A0 | IDS_DisplayName_58 | Center Line |
| 0x70E28020 | IDS_DisplayName_59 | Cervini's Auto Design |
| 0x40E1C4B9 | IDS_DisplayName_6 | ACT - Advanced Clutch Technology, Inc |
| 0x70E244A0 | IDS_DisplayName_60 | Chargespeed |
| 0x70E24420 | IDS_DisplayName_61 | Champion Plugs |
| 0x70E245A0 | IDS_DisplayName_62 | Chevron |
| 0x70E24620 | IDS_DisplayName_65 | Cobra |
| 0x70E247A0 | IDS_DisplayName_66 | Comptech Sport |
| 0x70E24720 | IDS_DisplayName_67 | Conoco |
| 0x70E240A0 | IDS_DisplayName_68 | Cork Sport |
| 0x70E24020 | IDS_DisplayName_69 | Cosworth |
| 0x40E1C439 | IDS_DisplayName_7 | AEM - Advanced Engine Management |
| 0x70E204A0 | IDS_DisplayName_70 | Crane |
| 0x70E20420 | IDS_DisplayName_71 | Cusco |
| 0x70E205A0 | IDS_DisplayName_72 | C-West Inc. |
| 0x70E20520 | IDS_DisplayName_73 | DC Sports |
| 0x70E20620 | IDS_DisplayName_75 | Detroit Locker |
| 0x70E207A0 | IDS_DisplayName_76 | DG Motorsports |
| 0x70E20720 | IDS_DisplayName_77 | Dietrich |
| 0x70E200A0 | IDS_DisplayName_78 | Do-Luck |
| 0x70E20020 | IDS_DisplayName_79 | Earls |
| 0x70E1C4A0 | IDS_DisplayName_80 | Eaton |
| 0x70E1C5A0 | IDS_DisplayName_82 | Eclipse |
| 0x70E1C520 | IDS_DisplayName_83 | Edelbrock |
| 0x70E1C6A0 | IDS_DisplayName_84 | Eibach |
| 0x70E1C620 | IDS_DisplayName_85 | Endless |
| 0x70E1C7A0 | IDS_DisplayName_86 | Enkei |
| 0x70E1C720 | IDS_DisplayName_87 | Erebuni |
| 0x70E1C0A0 | IDS_DisplayName_88 | Euro Sport |
| 0x70E1C020 | IDS_DisplayName_89 | Exedy |
| 0x40E1C339 | IDS_DisplayName_9 | AMG |
| 0x70E184A0 | IDS_DisplayName_90 | Extreme Dimensions |
| 0x70E185A0 | IDS_DisplayName_92 | Evolve |
| 0x70E186A0 | IDS_DisplayName_94 | Firestone |
| 0x70E18620 | IDS_DisplayName_95 | FEED - Fujita Engineering Evolutional Development |
| 0x70E187A0 | IDS_DisplayName_96 | Ferodo |
| 0x70E18720 | IDS_DisplayName_97 | Fidanza |
| 0x70E180A0 | IDS_DisplayName_98 | Fikse USA, Inc. |

### PaintableGroups — 80 entries (Paint region names + error messages)

| Hash | Key | Value |
|------|-----|-------|
| 0x4B0B1036 | IDS_ErrorString_0 | _(empty)_ |
| 0xC4218AC2 | IDS_ErrorString_100 | _(empty)_ |
| 0x10C57962 | IDS_ErrorString_1000 | _(empty)_ |
| 0x10E57962 | IDS_ErrorString_1100 | _(empty)_ |
| 0x10857962 | IDS_ErrorString_1200 | This car does not have a paintable wing. |
| 0x10853962 | IDS_ErrorString_1210 | _(empty)_ |
| 0x1085F962 | IDS_ErrorString_1220 | _(empty)_ |
| 0x1085B962 | IDS_ErrorString_1230 | _(empty)_ |
| 0x10A57962 | IDS_ErrorString_1300 | _(empty)_ |
| 0x10457962 | IDS_ErrorString_1400 | _(empty)_ |
| 0x10657962 | IDS_ErrorString_1500 | _(empty)_ |
| 0x10057962 | IDS_ErrorString_1600 | This car does not have paintable brakes. |
| 0x10257962 | IDS_ErrorString_1700 | This car does not have paintable rims. |
| 0x11C57962 | IDS_ErrorString_1800 | _(empty)_ |
| 0x11E57962 | IDS_ErrorString_1900 | _(empty)_ |
| 0xC4418AC2 | IDS_ErrorString_200 | _(empty)_ |
| 0x20C57962 | IDS_ErrorString_2000 | _(empty)_ |
| 0x20E57962 | IDS_ErrorString_2100 | _(empty)_ |
| 0x20857962 | IDS_ErrorString_2200 | _(empty)_ |
| 0x20857BE2 | IDS_ErrorString_2205 | This car does not have paintable front rims. |
| 0x20853962 | IDS_ErrorString_2210 | _(empty)_ |
| 0x20853BE2 | IDS_ErrorString_2215 | _(empty)_ |
| 0x2085F962 | IDS_ErrorString_2220 | _(empty)_ |
| 0x2085FBE2 | IDS_ErrorString_2225 | _(empty)_ |
| 0x2085B962 | IDS_ErrorString_2230 | _(empty)_ |
| 0x2085BBE2 | IDS_ErrorString_2235 | This car does not have paintable rear rims. |
| 0x20847962 | IDS_ErrorString_2240 | _(empty)_ |
| 0x20847BE2 | IDS_ErrorString_2245 | _(empty)_ |
| 0x20843962 | IDS_ErrorString_2250 | _(empty)_ |
| 0x20843BE2 | IDS_ErrorString_2255 | _(empty)_ |
| 0x2084F962 | IDS_ErrorString_2260 | _(empty)_ |
| 0x20A57962 | IDS_ErrorString_2300 | This car does not have tintable windows. |
| 0xC4618AC2 | IDS_ErrorString_300 | _(empty)_ |
| 0xC461CAC2 | IDS_ErrorString_310 | _(empty)_ |
| 0xC4818AC2 | IDS_ErrorString_400 | This car does not have a paintable hood. |
| 0xC4A18AC2 | IDS_ErrorString_500 | _(empty)_ |
| 0xC4C18AC2 | IDS_ErrorString_600 | _(empty)_ |
| 0xC4E18AC2 | IDS_ErrorString_700 | _(empty)_ |
| 0xC5018AC2 | IDS_ErrorString_800 | This car does not have paintable mirrors. |
| 0xC5218AC2 | IDS_ErrorString_900 | _(empty)_ |
| 0x70F70E93 | IDS_Name_0 | Paint Body |
| 0xC388C43D | IDS_Name_100 | Body 1 |
| 0xC46206E1 | IDS_Name_1000 | Mirror 2 |
| 0xC44206E1 | IDS_Name_1100 | Mirror 3 |
| 0xC42206E1 | IDS_Name_1200 | Wing |
| 0xC42246E1 | IDS_Name_1210 | Wing Endplates |
| 0xC42286E1 | IDS_Name_1220 | Wing Plane |
| 0xC422C6E1 | IDS_Name_1230 | Wing Struts |
| 0xC40206E1 | IDS_Name_1300 | Wing 1 |
| 0xC4E206E1 | IDS_Name_1400 | Wing 2 |
| 0xC4C206E1 | IDS_Name_1500 | Wing 3 |
| 0xC4A206E1 | IDS_Name_1600 | Brakes |
| 0xC48206E1 | IDS_Name_1700 | Rims |
| 0xC56206E1 | IDS_Name_1800 | Rims 1 |
| 0xC54206E1 | IDS_Name_1900 | Rims 2 |
| 0xC3E8C43D | IDS_Name_200 | Body 2 |
| 0xF46206E1 | IDS_Name_2000 | Rims 3 |
| 0xF44206E1 | IDS_Name_2100 | Inner Barrel |
| 0xF42206E1 | IDS_Name_2200 | Outer Lip |
| 0xF4220461 | IDS_Name_2205 | Rims (Front) |
| 0xF42246E1 | IDS_Name_2210 | Rims 1 |
| 0xF4224461 | IDS_Name_2215 | Rims 2 |
| 0xF42286E1 | IDS_Name_2220 | Rims 3 |
| 0xF4228461 | IDS_Name_2225 | Inner Barrel |
| 0xF422C6E1 | IDS_Name_2230 | Outer Lip |
| 0xF422C461 | IDS_Name_2235 | Rims (Rear) |
| 0xF42306E1 | IDS_Name_2240 | Rims 1 |
| 0xF4230461 | IDS_Name_2245 | Rims 2 |
| 0xF42346E1 | IDS_Name_2250 | Rims 3 |
| 0xF4234461 | IDS_Name_2255 | Inner Barrel |
| 0xF42386E1 | IDS_Name_2260 | Outer Lip |
| 0xF40206E1 | IDS_Name_2300 | Window Tint |
| 0xC3C8C43D | IDS_Name_300 | Body 3 |
| 0xC3C8843D | IDS_Name_310 | Tow Hook |
| 0xC328C43D | IDS_Name_400 | Hood |
| 0xC308C43D | IDS_Name_500 | Hood 1 |
| 0xC368C43D | IDS_Name_600 | Hood 2 |
| 0xC348C43D | IDS_Name_700 | Hood 3 |
| 0xC2A8C43D | IDS_Name_800 | Mirrors |
| 0xC288C43D | IDS_Name_900 | Mirror 1 |

### UpgradeTypes — 106 entries (Upgrade type labels + descriptions)

| Hash | Key | Value |
|------|-----|-------|
| 0x69532BB6 | IDS_Description_1 | You can swap in a new engine to get more power and possibly reduced weight, but every engine has its own upgrade path. Any upgrades on your current engine will not apply to the new engine. As a result, your car's power may actually decrease with an engine swap. Even with a new, more powerful engine, you may not win races. Winning performance calls for a balance between power and handling. An engine swap also makes an audible difference. |
| 0xA995C334 | IDS_Description_10 | A supercharger is an air pump driven by a belt connected to the engine's crankshaft. It provides a major power increase by compressing the air-fuel mixture and forcing it into the engine at more than atmospheric pressure. The result is more energy per stroke, which makes more power. These upgrades also make an audible difference.  Positive-displacement superchargers produce low boost across the RPM range and a noticeable improvement in low-end and mid-range torque.  While positive-displacement superchargers produce boost more evenly than centrifugal superchargers, they are less efficient. |
| 0xA995C3B4 | IDS_Description_11 | An intercooler is a small radiator that cools the hot air from a turbocharger or supercharger before it is forced into the engine. This makes the air-fuel mixture cooler, and therefore more dense, packing more energy per stroke. |
| 0xA995C234 | IDS_Description_12 | Brakes are an important part of the total performance picture. To be competitive, your car's brake performance must match its power and handling. Leading the pack at the end of a straight won't help if you can't slow down fast enough to make the next turn. These upgrades increase braking power and decrease brake fade due to excessive heat. |
| 0xA995C2B4 | IDS_Description_13 | Springs and dampers can make a big difference in your car's handling by maintaining optimum ride height and tire contact. |
| 0xA995C134 | IDS_Description_14 | Front anti-roll bars (also called anti-sway bars) provide extra stability when cornering. When you turn left or right, the car body tends to roll in the opposite direction. By tying the left and right sides of the suspension together, anti-sway bars make the car ride more level and keep one side from rolling or swaying more than the other. |
| 0xA995C1B4 | IDS_Description_15 | The transmission transmits you car's power from the engine to the drive wheels. Transmission upgrades can make shifts quicker and more efficient, reduce friction and power loss, and provide better durability. These upgrades also make an audible difference. |
| 0xA995C034 | IDS_Description_16 | The clutch is the vital link between the engine and the transmission. Upgrades increase the clutch's ability to handle the extra torque of a racing engine without damage. |
| 0xA995C0B4 | IDS_Description_17 | For a stock car, the rotating mass of the flywheel smoothes and steadies the rotation of the driveshaft, but it decreases throttle response and acceleration. Upgrading to a lighter-weight flywheel allows the engine to respond to the throttle more quickly and increase RPM faster, providing better acceleration. |
| 0xA995C734 | IDS_Description_18 | You can improve throttle response and acceleration by decreasing the weight and inertia of driveline components, especially the driveshaft itself. |
| 0xA995C7B4 | IDS_Description_19 | The differential allows the tires on each side of the car to turn at different rates because the inside tire travels a shorter distance around a turn than the outside tire. A limited-slip differential locks at a preset point to limit this difference in rotational speed, providing maximum traction under acceleration and/or deceleration. |
| 0x69532A36 | IDS_Description_2 | Upgraded cams let your engine breathe more freely and rev to higher RPM, producing more torque and power. The net result is a higher redline and more power in the high-RPM range. |
| 0xA9950334 | IDS_Description_20 | A lighter car accelerates and handles better than a heavier one. Reducing weight by removing nonessential materials or replacing stock parts with lighter ones pays off on the track. |
| 0xA99503B4 | IDS_Description_21 | Upgrading to tires with a softer, more aggressive compound increases traction and improves the tires ability to maintain traction despite high heat, but also increases wear. The harder compound used in stock tires sacrifices grip to increase wear. These upgrades also make an audible difference. |
| 0xA9950234 | IDS_Description_22 | Choose larger rims and low-profile tires with shorter, more rigid sidewalls. These tires are less prone to deforming as acceleration and cornering forces increase. This improves traction by maintaining tread contact with the pavement. |
| 0xA99502B4 | IDS_Description_23 | In general, more rubber on the road means better traction and performance. Upgrading to larger, wider tires provides more contact area and thus more traction. You can use wider normal-profile tires to improve traction by enlarging the tires contact patch on the pavement. Or you can choose larger rims and low-profile tires with shorter, more rigid sidewalls. These tires are less prone to deforming as acceleration and cornering forces increase. This improves traction by maintaining tread contact with the pavement. |
| 0xA9950134 | IDS_Description_24 | You can upgrade your front bumper to increase the load over the front wheels by increasing downforce. These upgrades allow higher cornering speeds. Note that Race upgrades make downforce adjustable. |
| 0xA99501B4 | IDS_Description_25 | Upgrading the rear wing on your car increases the load over the rear wheels by generating downforce to allow higher cornering speeds. Note that Race upgrades make downforce adjustable. |
| 0xA9950034 | IDS_Description_26 | Rear bumper upgrades improve handling by decreasing lift at high speeds. The Level 3 upgrade also increases the load over the rear wheels by adding adjustable downforce. These changes allow higher cornering speeds. |
| 0xA99500B4 | IDS_Description_27 | Adding modified side skirts reduces weight and drag to improve overall performance. |
| 0xA9950734 | IDS_Description_28 | Upgrading to a lighter-weight hood reduces overall weight and balance to improve performance. |
| 0x69532AB6 | IDS_Description_3 | Displacement upgrades make the engine more durable and less damage-prone. They can also reduce friction/inertia and increase displacement/compression to make the engine more powerful and responsive. |
| 0xA9954334 | IDS_Description_30 | Upgrading rims can improve handling by decreasing the wheels' unsprung weight and rotational inertia. This upgrade can also enhance performance by decreasing the overall weight of the car. |
| 0xA99543B4 | IDS_Description_31 | Upgrading rims can improve handling by decreasing the wheels' unsprung weight and rotational inertia. This upgrade can also enhance performance by decreasing the overall weight of the car. |
| 0xA9954234 | IDS_Description_32 | Upgrading rims can improve handling by decreasing the wheels' unsprung weight and rotational inertia. This upgrade can also enhance performance by decreasing the overall weight of the car. |
| 0xA99540B4 | IDS_Description_37 | You can swap an entirely new drivetrain into your car to get different driving characteristics and possibly reduced weight, but every drivetrain has its own upgrade path. Any upgrades on your current drivetrain will not apply to the new drivetrain. As a result, your car's handling may actually decrease with a drivetrain swap. Even with an upgraded drivetrain, you may not win races. Winning performance calls for a balance between power and handling. |
| 0xA9954734 | IDS_Description_38 | You can significantly alter your car's body work and stance for a bold new look, different driving characteristics, and possibly reduced weight. Some upgrades on your car's current body will not apply to the new body kit. As a result, your car's handling may actually decrease. Even with a body kit, you may not win races. Winning performance calls for a balance between power and handling. |
| 0x69532936 | IDS_Description_4 | Fuel system upgrades can yield big power increases. They provide more efficient fuel flow, more precise timing, the ability to use higher-octane fuel, and they extract more power from the fuel you use. These changes can be as simple as installing a custom Engine Control Unit (ECU) chip or as complex as changing the fuel pump and tank, injectors, and fuel hoses. |
| 0xA9948334 | IDS_Description_40 | Improves torque. Only available on naturally aspirated engines. |
| 0xA9948234 | IDS_Description_42 | Remove the stock restrictor plate. Improves torque.  Only for race cars. |
| 0xA9948134 | IDS_Description_44 | Improves torque. Only for diesel engines. |
| 0xA99481B4 | IDS_Description_45 | Improves torque. Only for carbureted engines. |
| 0xA9948034 | IDS_Description_46 | Valves allow the air and fuel mixture to enter and exit the engine. Upgrading these allows for more air flow increasing power. |
| 0xA99480B4 | IDS_Description_47 | Upgrading pistons allows for high-compression ratios increasing power. |
| 0xA99487B4 | IDS_Description_49 | Improves torque. Only for rotary engines. |
| 0x695329B6 | IDS_Description_5 | Ignition upgrades help the engine burn fuel more efficiently to produce more power. Adding better coils, spark plugs, and ignition wiring can make a significant difference in engine power and car performance. |
| 0xA994C334 | IDS_Description_50 | Adding oil cooling keeps the engine's oil at the correct temperature, aiding efficiency and increasing power. |
| 0xA994C3B4 | IDS_Description_51 | A turbocharger provides a major power increase by using exhaust gases to spin a turbine which compresses the air-fuel mixture and forces it into the engine at more than atmospheric pressure. The result is more energy per stroke, which makes more power. These upgrades also make an audible difference. Two are usually better! |
| 0xA994C2B4 | IDS_Description_53 | Rear anti-roll bars (also called anti-sway bars) provide extra stability when cornering. When you turn left or right the car body tends to roll in the opposite direction. By tying the left and right sides of the suspension together, anti-sway bars make the car ride more level, keeping one side from rolling or swaying more than the other. |
| 0xA994C134 | IDS_Description_54 | Chassis reinforcements stiffen the shell of the car, reducing flex when cornering, which in turn aid the suspension in keeping the maximum amount of tire on the road. |
| 0xA994C1B4 | IDS_Description_55 | In general, more rubber on the road means better traction and performance. Upgrading to larger, wider tires provides more contact area and thus more traction. You can use wider normal-profile tires to improve traction by enlarging the tires contact patch on the pavement. Or you can choose larger rims and low-profile tires with shorter, more rigid sidewalls. These tires are less prone to deforming as acceleration and cornering forces increase. This improves traction by maintaining tread contact with the pavement. |
| 0xA994C034 | IDS_Description_56 | Choose larger rims and low-profile tires with shorter, more rigid sidewalls. These tires are less prone to deforming as acceleration and cornering forces increase. This improves traction by maintaining tread contact with the pavement. |
| 0xA994C0B4 | IDS_Description_57 | Change how air flows into your car's engine. Naturally aspirated engines pull in air unassisted, while a turbo or supercharger can compress the air before it reaches the cylinder to extract more performance from an engine. |
| 0xA994C734 | IDS_Description_58 | High-voltage battery packs and generators transmit power to electric motors and drive wheels. Battery and motor upgrades increase power output and durability. |
| 0xA994C7B4 | IDS_Description_59 | A wider spacing between the front tires can provide additional stability under lateral G force. |
| 0x69532836 | IDS_Description_6 | Exhaust system upgrades such as improved headers, mufflers, bypasses, and large-bore tubing provide extra power for a relatively low cost. They let the engine exhale more freely and create more power by reducing back pressure and extracting exhaust gases more efficiently. These upgrades also make an audible difference. |
| 0xA9940334 | IDS_Description_60 | A wider spacing between the rear tires can provide additional stability under lateral G force. |
| 0xA99403B4 | IDS_Description_61 | Increase or decrease the front wheel size. |
| 0xA9940234 | IDS_Description_62 | Increase or decrease the rear wheel size. |
| 0xA99402B4 | IDS_Description_63 | _(empty)_ |
| 0x695328B6 | IDS_Description_7 | Intake upgrades help the engine inhale more freely and provide a lot of bang for the buck. Less restrictive air filters and a tuned intake manifold allow more air into the engine, making more power. |
| 0x69532F36 | IDS_Description_8 | A turbocharger provides a major power increase by using exhaust gases to spin a turbine, which compresses the air-fuel mixture and forces it into the engine at more than atmospheric pressure. The result is more energy per stroke, which makes more power. These upgrades also make an audible difference. |
| 0x69532FB6 | IDS_Description_9 | A supercharger is an air pump driven by a belt connected to the engine's crankshaft. It provides a major power increase by compressing the air-fuel mixture and forcing it into the engine at more than atmospheric pressure. The result is more energy per stroke, which makes more power. These upgrades also make an audible difference.  A centrifugal supercharger forces induction with an impeller fan, similar to a turbocharger. Centrifugal superchargers build boost in proportion to RPM and noticeably improve top-end power.  While centrifugal superchargers produce boost more efficiently than positive-displacement superchargers, all of that power is concentrated at the top of the RPM band. |
| 0x70F70E13 | IDS_Name_1 | Engine Swap |
| 0x7B8711B8 | IDS_Name_10 | Positive Displacement Supercharger |
| 0x7B871138 | IDS_Name_11 | Intercooler |
| 0x7B8710B8 | IDS_Name_12 | Brakes |
| 0x7B871038 | IDS_Name_13 | Spring and Dampers |
| 0x7B8713B8 | IDS_Name_14 | Front Anti-roll Bars |
| 0x7B871338 | IDS_Name_15 | Transmission |
| 0x7B8712B8 | IDS_Name_16 | Clutch |
| 0x7B871238 | IDS_Name_17 | Flywheel |
| 0x7B8715B8 | IDS_Name_18 | Driveline |
| 0x7B871538 | IDS_Name_19 | Differential |
| 0x70F70F93 | IDS_Name_2 | Camshaft |
| 0x7B87D1B8 | IDS_Name_20 | Weight Reduction |
| 0x7B87D138 | IDS_Name_21 | Tire Compound |
| 0x7B87D0B8 | IDS_Name_22 | Front Rim Size |
| 0x7B87D038 | IDS_Name_23 | Front Tire Width |
| 0x7B87D3B8 | IDS_Name_24 | Front Bumper |
| 0x7B87D338 | IDS_Name_25 | Rear Wing |
| 0x7B87D2B8 | IDS_Name_26 | Rear Bumper |
| 0x7B87D238 | IDS_Name_27 | Side Skirts |
| 0x7B87D5B8 | IDS_Name_28 | Hood |
| 0x70F70F13 | IDS_Name_3 | Displacement |
| 0x7B8791B8 | IDS_Name_30 | Rim Style |
| 0x7B879138 | IDS_Name_31 | Front Rim Style |
| 0x7B8790B8 | IDS_Name_32 | Rear Rim Style |
| 0x7B879238 | IDS_Name_37 | Drivetrain Swap |
| 0x7B8795B8 | IDS_Name_38 | Body Kit |
| 0x70F70C93 | IDS_Name_4 | Fuel System |
| 0x7B8651B8 | IDS_Name_40 | Intake Manifold / Throttle Body |
| 0x7B8650B8 | IDS_Name_42 | Restrictor Plate |
| 0x7B8653B8 | IDS_Name_44 | Fuel System |
| 0x7B865338 | IDS_Name_45 | Carburetor |
| 0x7B8652B8 | IDS_Name_46 | Valves |
| 0x7B865238 | IDS_Name_47 | Pistons / Compression |
| 0x7B865538 | IDS_Name_49 | Rotors / Compression |
| 0x70F70C13 | IDS_Name_5 | Ignition |
| 0x7B8611B8 | IDS_Name_50 | Oil / Cooling |
| 0x7B861138 | IDS_Name_51 | Twin Turbo |
| 0x7B861038 | IDS_Name_53 | Rear Anti-roll Bars |
| 0x7B8613B8 | IDS_Name_54 | Chassis Reinforcement / Roll Cage |
| 0x7B861338 | IDS_Name_55 | Rear Tire Width |
| 0x7B8612B8 | IDS_Name_56 | Rear Rim Size |
| 0x7B861238 | IDS_Name_57 | Aspiration |
| 0x7B8615B8 | IDS_Name_58 | Motor and Battery |
| 0x7B861538 | IDS_Name_59 | Front Track Width |
| 0x70F70D93 | IDS_Name_6 | Exhaust |
| 0x7B86D1B8 | IDS_Name_60 | Rear Track Width |
| 0x7B86D138 | IDS_Name_61 | Front Tire Profile Size |
| 0x7B86D0B8 | IDS_Name_62 | Rear Tire Profile Size |
| 0x7B86D038 | IDS_Name_63 | Motor and Battery Swap |
| 0x70F70D13 | IDS_Name_7 | Intake |
| 0x70F70A93 | IDS_Name_8 | Single Turbo |
| 0x70F70A13 | IDS_Name_9 | Centrifugal Supercharger |

### Upgrades — 496 entries (Upgrade part names)

| Hash | Key | Value |
|------|-----|-------|
| 0x69532BB6 | IDS_Description_1 | _(empty)_ |
| 0xA995C334 | IDS_Description_10 | _(empty)_ |
| 0xCAE18254 | IDS_Description_100 | _(empty)_ |
| 0xCAE182D4 | IDS_Description_101 | _(empty)_ |
| 0xCAE18354 | IDS_Description_102 | _(empty)_ |
| 0xCAE183D4 | IDS_Description_103 | _(empty)_ |
| 0xCAE18054 | IDS_Description_104 | _(empty)_ |
| 0xCAE180D4 | IDS_Description_105 | _(empty)_ |
| 0xA995C3B4 | IDS_Description_11 | _(empty)_ |
| 0xCAE1C354 | IDS_Description_112 | _(empty)_ |
| 0xCAE1C3D4 | IDS_Description_113 | _(empty)_ |
| 0xCAE1C054 | IDS_Description_114 | _(empty)_ |
| 0xCAE1C0D4 | IDS_Description_115 | _(empty)_ |
| 0xCAE1C154 | IDS_Description_116 | _(empty)_ |
| 0xCAE1C1D4 | IDS_Description_117 | _(empty)_ |
| 0xCAE1C654 | IDS_Description_118 | _(empty)_ |
| 0xCAE1C6D4 | IDS_Description_119 | _(empty)_ |
| 0xA995C234 | IDS_Description_12 | _(empty)_ |
| 0xCAE10254 | IDS_Description_120 | _(empty)_ |
| 0xCAE102D4 | IDS_Description_121 | _(empty)_ |
| 0xCAE10354 | IDS_Description_122 | _(empty)_ |
| 0xCAE103D4 | IDS_Description_123 | _(empty)_ |
| 0xCAE10054 | IDS_Description_124 | _(empty)_ |
| 0xCAE100D4 | IDS_Description_125 | _(empty)_ |
| 0xA995C2B4 | IDS_Description_13 | _(empty)_ |
| 0xCAE14254 | IDS_Description_130 | _(empty)_ |
| 0xCAE14354 | IDS_Description_132 | _(empty)_ |
| 0xCAE14054 | IDS_Description_134 | _(empty)_ |
| 0xCAE140D4 | IDS_Description_135 | _(empty)_ |
| 0xCAE141D4 | IDS_Description_137 | _(empty)_ |
| 0xCAE146D4 | IDS_Description_139 | _(empty)_ |
| 0xA995C134 | IDS_Description_14 | _(empty)_ |
| 0xCAE082D4 | IDS_Description_141 | _(empty)_ |
| 0xCAE08354 | IDS_Description_142 | _(empty)_ |
| 0xCAE083D4 | IDS_Description_143 | _(empty)_ |
| 0xCAE08054 | IDS_Description_144 | _(empty)_ |
| 0xCAE080D4 | IDS_Description_145 | _(empty)_ |
| 0xCAE08154 | IDS_Description_146 | _(empty)_ |
| 0xCAE081D4 | IDS_Description_147 | _(empty)_ |
| 0xCAE08654 | IDS_Description_148 | _(empty)_ |
| 0xCAE086D4 | IDS_Description_149 | _(empty)_ |
| 0xA995C1B4 | IDS_Description_15 | _(empty)_ |
| 0xCAE0C254 | IDS_Description_150 | _(empty)_ |
| 0xCAE0C2D4 | IDS_Description_151 | _(empty)_ |
| 0xCAE0C354 | IDS_Description_152 | _(empty)_ |
| 0xCAE0C3D4 | IDS_Description_153 | _(empty)_ |
| 0xCAE0C054 | IDS_Description_154 | _(empty)_ |
| 0xCAE0C0D4 | IDS_Description_155 | _(empty)_ |
| 0xCAE0C154 | IDS_Description_156 | _(empty)_ |
| 0xCAE0C1D4 | IDS_Description_157 | _(empty)_ |
| 0xCAE0C654 | IDS_Description_158 | _(empty)_ |
| 0xCAE0C6D4 | IDS_Description_159 | _(empty)_ |
| 0xA995C034 | IDS_Description_16 | _(empty)_ |
| 0xCAE00254 | IDS_Description_160 | _(empty)_ |
| 0xCAE002D4 | IDS_Description_161 | _(empty)_ |
| 0xCAE00354 | IDS_Description_162 | _(empty)_ |
| 0xA995C0B4 | IDS_Description_17 | _(empty)_ |
| 0xA995C734 | IDS_Description_18 | _(empty)_ |
| 0xA995C7B4 | IDS_Description_19 | _(empty)_ |
| 0x69532A36 | IDS_Description_2 | _(empty)_ |
| 0xA9950334 | IDS_Description_20 | _(empty)_ |
| 0xCA818654 | IDS_Description_208 | _(empty)_ |
| 0xCA8186D4 | IDS_Description_209 | _(empty)_ |
| 0xA99503B4 | IDS_Description_21 | _(empty)_ |
| 0xCA81C254 | IDS_Description_210 | _(empty)_ |
| 0xCA81C2D4 | IDS_Description_211 | _(empty)_ |
| 0xCA81C354 | IDS_Description_212 | _(empty)_ |
| 0xCA81C3D4 | IDS_Description_213 | _(empty)_ |
| 0xCA81C054 | IDS_Description_214 | _(empty)_ |
| 0xCA81C0D4 | IDS_Description_215 | _(empty)_ |
| 0xCA81C154 | IDS_Description_216 | _(empty)_ |
| 0xCA81C1D4 | IDS_Description_217 | _(empty)_ |
| 0xCA81C654 | IDS_Description_218 | _(empty)_ |
| 0xCA81C6D4 | IDS_Description_219 | _(empty)_ |
| 0xA9950234 | IDS_Description_22 | _(empty)_ |
| 0xCA810254 | IDS_Description_220 | _(empty)_ |
| 0xCA8102D4 | IDS_Description_221 | _(empty)_ |
| 0xCA810354 | IDS_Description_222 | _(empty)_ |
| 0xCA8103D4 | IDS_Description_223 | _(empty)_ |
| 0xCA810054 | IDS_Description_224 | _(empty)_ |
| 0xCA8100D4 | IDS_Description_225 | _(empty)_ |
| 0xCA810154 | IDS_Description_226 | _(empty)_ |
| 0xCA8101D4 | IDS_Description_227 | _(empty)_ |
| 0xCA810654 | IDS_Description_228 | _(empty)_ |
| 0xCA8106D4 | IDS_Description_229 | _(empty)_ |
| 0xA99502B4 | IDS_Description_23 | _(empty)_ |
| 0xCA814254 | IDS_Description_230 | _(empty)_ |
| 0xCA8142D4 | IDS_Description_231 | _(empty)_ |
| 0xCA814354 | IDS_Description_232 | _(empty)_ |
| 0xCA8143D4 | IDS_Description_233 | _(empty)_ |
| 0xCA814054 | IDS_Description_234 | _(empty)_ |
| 0xCA8140D4 | IDS_Description_235 | _(empty)_ |
| 0xCA814154 | IDS_Description_236 | _(empty)_ |
| 0xCA8141D4 | IDS_Description_237 | _(empty)_ |
| 0xCA814654 | IDS_Description_238 | _(empty)_ |
| 0xCA8146D4 | IDS_Description_239 | _(empty)_ |
| 0xA9950134 | IDS_Description_24 | _(empty)_ |
| 0xCA808054 | IDS_Description_244 | _(empty)_ |
| 0xCA8080D4 | IDS_Description_245 | _(empty)_ |
| 0xCA808154 | IDS_Description_246 | _(empty)_ |
| 0xCA8081D4 | IDS_Description_247 | _(empty)_ |
| 0xCA808654 | IDS_Description_248 | _(empty)_ |
| 0xCA8086D4 | IDS_Description_249 | _(empty)_ |
| 0xA99501B4 | IDS_Description_25 | _(empty)_ |
| 0xCA80C254 | IDS_Description_250 | _(empty)_ |
| 0xCA80C2D4 | IDS_Description_251 | _(empty)_ |
| 0xCA80C354 | IDS_Description_252 | _(empty)_ |
| 0xCA80C3D4 | IDS_Description_253 | _(empty)_ |
| 0xCA80C054 | IDS_Description_254 | _(empty)_ |
| 0xCA80C0D4 | IDS_Description_255 | _(empty)_ |
| 0xCA80C154 | IDS_Description_256 | _(empty)_ |
| 0xCA80C1D4 | IDS_Description_257 | _(empty)_ |
| 0xCA80C654 | IDS_Description_258 | _(empty)_ |
| 0xCA80C6D4 | IDS_Description_259 | _(empty)_ |
| 0xA9950034 | IDS_Description_26 | _(empty)_ |
| 0xCA800254 | IDS_Description_260 | _(empty)_ |
| 0xCA8002D4 | IDS_Description_261 | _(empty)_ |
| 0xCA800354 | IDS_Description_262 | _(empty)_ |
| 0xCA800054 | IDS_Description_264 | _(empty)_ |
| 0xCA8000D4 | IDS_Description_265 | _(empty)_ |
| 0xCA800154 | IDS_Description_266 | _(empty)_ |
| 0xCA8001D4 | IDS_Description_267 | _(empty)_ |
| 0xA99500B4 | IDS_Description_27 | _(empty)_ |
| 0xCA804254 | IDS_Description_270 | _(empty)_ |
| 0xCA8042D4 | IDS_Description_271 | _(empty)_ |
| 0xCA804354 | IDS_Description_272 | _(empty)_ |
| 0xCA8043D4 | IDS_Description_273 | _(empty)_ |
| 0xCA804054 | IDS_Description_274 | _(empty)_ |
| 0xCA8040D4 | IDS_Description_275 | _(empty)_ |
| 0xCA804154 | IDS_Description_276 | _(empty)_ |
| 0xCA8041D4 | IDS_Description_277 | _(empty)_ |
| 0xCA804654 | IDS_Description_278 | _(empty)_ |
| 0xCA8046D4 | IDS_Description_279 | _(empty)_ |
| 0xA9950734 | IDS_Description_28 | _(empty)_ |
| 0xCA838254 | IDS_Description_280 | _(empty)_ |
| 0xCA8382D4 | IDS_Description_281 | _(empty)_ |
| 0xCA838354 | IDS_Description_282 | _(empty)_ |
| 0xCA8383D4 | IDS_Description_283 | _(empty)_ |
| 0xCA838054 | IDS_Description_284 | _(empty)_ |
| 0xCA8380D4 | IDS_Description_285 | _(empty)_ |
| 0xCA838154 | IDS_Description_286 | _(empty)_ |
| 0xCA8381D4 | IDS_Description_287 | _(empty)_ |
| 0xCA838654 | IDS_Description_288 | _(empty)_ |
| 0xCA8386D4 | IDS_Description_289 | _(empty)_ |
| 0xA99507B4 | IDS_Description_29 | _(empty)_ |
| 0xCA83C254 | IDS_Description_290 | _(empty)_ |
| 0xCA83C2D4 | IDS_Description_291 | _(empty)_ |
| 0xCA83C354 | IDS_Description_292 | _(empty)_ |
| 0xCA83C3D4 | IDS_Description_293 | _(empty)_ |
| 0xCA83C054 | IDS_Description_294 | _(empty)_ |
| 0xCA83C0D4 | IDS_Description_295 | _(empty)_ |
| 0xCA83C154 | IDS_Description_296 | _(empty)_ |
| 0xCA83C1D4 | IDS_Description_297 | _(empty)_ |
| 0xCA83C654 | IDS_Description_298 | _(empty)_ |
| 0xCA83C6D4 | IDS_Description_299 | _(empty)_ |
| 0x69532AB6 | IDS_Description_3 | _(empty)_ |
| 0xA9954334 | IDS_Description_30 | _(empty)_ |
| 0xCAA18254 | IDS_Description_300 | _(empty)_ |
| 0xCAA182D4 | IDS_Description_301 | _(empty)_ |
| 0xCAA18354 | IDS_Description_302 | _(empty)_ |
| 0xCAA183D4 | IDS_Description_303 | _(empty)_ |
| 0xCAA18054 | IDS_Description_304 | _(empty)_ |
| 0xCAA180D4 | IDS_Description_305 | _(empty)_ |
| 0xCAA18154 | IDS_Description_306 | _(empty)_ |
| 0xCAA181D4 | IDS_Description_307 | _(empty)_ |
| 0xCAA18654 | IDS_Description_308 | _(empty)_ |
| 0xCAA186D4 | IDS_Description_309 | _(empty)_ |
| 0xA99543B4 | IDS_Description_31 | _(empty)_ |
| 0xCAA1C254 | IDS_Description_310 | _(empty)_ |
| 0xCAA1C2D4 | IDS_Description_311 | _(empty)_ |
| 0xCAA1C354 | IDS_Description_312 | _(empty)_ |
| 0xCAA1C3D4 | IDS_Description_313 | _(empty)_ |
| 0xCAA1C054 | IDS_Description_314 | _(empty)_ |
| 0xCAA1C0D4 | IDS_Description_315 | _(empty)_ |
| 0xA9954234 | IDS_Description_32 | _(empty)_ |
| 0xA99542B4 | IDS_Description_33 | _(empty)_ |
| 0xA9954134 | IDS_Description_34 | _(empty)_ |
| 0xA99541B4 | IDS_Description_35 | _(empty)_ |
| 0xA9954034 | IDS_Description_36 | _(empty)_ |
| 0xA99540B4 | IDS_Description_37 | _(empty)_ |
| 0xA9954734 | IDS_Description_38 | _(empty)_ |
| 0xA99547B4 | IDS_Description_39 | _(empty)_ |
| 0x69532936 | IDS_Description_4 | _(empty)_ |
| 0xA9948334 | IDS_Description_40 | _(empty)_ |
| 0xA99483B4 | IDS_Description_41 | _(empty)_ |
| 0xA9948234 | IDS_Description_42 | _(empty)_ |
| 0xA99482B4 | IDS_Description_43 | _(empty)_ |
| 0xA9948134 | IDS_Description_44 | _(empty)_ |
| 0xA99481B4 | IDS_Description_45 | _(empty)_ |
| 0xA9948034 | IDS_Description_46 | _(empty)_ |
| 0xA99480B4 | IDS_Description_47 | _(empty)_ |
| 0xA9948734 | IDS_Description_48 | _(empty)_ |
| 0xA99487B4 | IDS_Description_49 | _(empty)_ |
| 0x695329B6 | IDS_Description_5 | _(empty)_ |
| 0xA994C334 | IDS_Description_50 | _(empty)_ |
| 0xA994C3B4 | IDS_Description_51 | _(empty)_ |
| 0xA994C234 | IDS_Description_52 | _(empty)_ |
| 0xA994C2B4 | IDS_Description_53 | _(empty)_ |
| 0xA994C134 | IDS_Description_54 | _(empty)_ |
| 0xA994C1B4 | IDS_Description_55 | _(empty)_ |
| 0xA994C034 | IDS_Description_56 | _(empty)_ |
| 0xA994C0B4 | IDS_Description_57 | _(empty)_ |
| 0xA994C734 | IDS_Description_58 | _(empty)_ |
| 0xA994C7B4 | IDS_Description_59 | _(empty)_ |
| 0x69532836 | IDS_Description_6 | _(empty)_ |
| 0xA9940334 | IDS_Description_60 | _(empty)_ |
| 0xA99403B4 | IDS_Description_61 | _(empty)_ |
| 0xA9940234 | IDS_Description_62 | _(empty)_ |
| 0xA99402B4 | IDS_Description_63 | _(empty)_ |
| 0xA9940134 | IDS_Description_64 | _(empty)_ |
| 0xA99401B4 | IDS_Description_65 | _(empty)_ |
| 0xA9940034 | IDS_Description_66 | _(empty)_ |
| 0xA99400B4 | IDS_Description_67 | _(empty)_ |
| 0xA9940734 | IDS_Description_68 | _(empty)_ |
| 0xA99407B4 | IDS_Description_69 | _(empty)_ |
| 0x695328B6 | IDS_Description_7 | _(empty)_ |
| 0xA9944334 | IDS_Description_70 | _(empty)_ |
| 0xA99443B4 | IDS_Description_71 | _(empty)_ |
| 0xA9944234 | IDS_Description_72 | _(empty)_ |
| 0xA99442B4 | IDS_Description_73 | _(empty)_ |
| 0xA9944134 | IDS_Description_74 | _(empty)_ |
| 0xA99441B4 | IDS_Description_75 | _(empty)_ |
| 0xA9944034 | IDS_Description_76 | _(empty)_ |
| 0xA99440B4 | IDS_Description_77 | _(empty)_ |
| 0xA9944734 | IDS_Description_78 | _(empty)_ |
| 0xA99447B4 | IDS_Description_79 | _(empty)_ |
| 0x69532F36 | IDS_Description_8 | _(empty)_ |
| 0xA9978334 | IDS_Description_80 | _(empty)_ |
| 0xA99783B4 | IDS_Description_81 | _(empty)_ |
| 0xA9978234 | IDS_Description_82 | _(empty)_ |
| 0xA99782B4 | IDS_Description_83 | _(empty)_ |
| 0xA9978134 | IDS_Description_84 | _(empty)_ |
| 0xA99781B4 | IDS_Description_85 | _(empty)_ |
| 0xA9978034 | IDS_Description_86 | _(empty)_ |
| 0xA99780B4 | IDS_Description_87 | _(empty)_ |
| 0xA9978734 | IDS_Description_88 | _(empty)_ |
| 0xA99787B4 | IDS_Description_89 | _(empty)_ |
| 0x69532FB6 | IDS_Description_9 | _(empty)_ |
| 0xA997C334 | IDS_Description_90 | _(empty)_ |
| 0xA997C3B4 | IDS_Description_91 | _(empty)_ |
| 0xA997C234 | IDS_Description_92 | _(empty)_ |
| 0xA997C2B4 | IDS_Description_93 | _(empty)_ |
| 0xA997C134 | IDS_Description_94 | _(empty)_ |
| 0xA997C1B4 | IDS_Description_95 | _(empty)_ |
| 0xA997C034 | IDS_Description_96 | _(empty)_ |
| 0xA997C0B4 | IDS_Description_97 | _(empty)_ |
| 0xA997C734 | IDS_Description_98 | _(empty)_ |
| 0xA997C7B4 | IDS_Description_99 | _(empty)_ |
| 0x70F70E13 | IDS_Name_1 | Stock Powertrain Swap |
| 0x7B8711B8 | IDS_Name_10 | Race Engine Block |
| 0xC388C43D | IDS_Name_100 | Stock Hood |
| 0xC388C4BD | IDS_Name_101 | Street Hood |
| 0xC388C53D | IDS_Name_102 | Stock Engine Block |
| 0xC388C5BD | IDS_Name_103 | Street Engine Block |
| 0xC388C63D | IDS_Name_104 | Sport Engine Block |
| 0xC388C6BD | IDS_Name_105 | Race Engine Block |
| 0x7B871138 | IDS_Name_11 | Stock Fuel System |
| 0xC388853D | IDS_Name_112 | No Intercooler |
| 0xC38885BD | IDS_Name_113 | Race Weight Reduction |
| 0xC388863D | IDS_Name_114 | Stock Rim Style |
| 0xC38886BD | IDS_Name_115 | Alternative Rim Style |
| 0xC388873D | IDS_Name_116 | Upgraded Front Tire Width |
| 0xC38887BD | IDS_Name_117 | Upgraded Front Tire Width |
| 0xC388803D | IDS_Name_118 | Upgraded Front Tire Width |
| 0xC38880BD | IDS_Name_119 | Upgraded Front Tire Width |
| 0x7B8710B8 | IDS_Name_12 | Street Fuel System |
| 0xC388443D | IDS_Name_120 | Upgraded Front Rim Size |
| 0xC38844BD | IDS_Name_121 | Upgraded Front Rim Size |
| 0xC388453D | IDS_Name_122 | Upgraded Front Rim Size |
| 0xC38845BD | IDS_Name_123 | Upgraded Front Rim Size |
| 0xC388463D | IDS_Name_124 | Upgraded Front Rim Size |
| 0xC38846BD | IDS_Name_125 | Upgraded Front Rim Size |
| 0x7B871038 | IDS_Name_13 | Sport Fuel System |
| 0xC388043D | IDS_Name_130 | Stock - Naturally Aspirated |
| 0xC388053D | IDS_Name_132 | Naturally Aspirated |
| 0xC388063D | IDS_Name_134 | Single Turbo |
| 0xC38806BD | IDS_Name_135 | Twin Turbo |
| 0xC38807BD | IDS_Name_137 | Positive-Displacement Supercharger |
| 0xC38800BD | IDS_Name_139 | Centrifugal Supercharger |
| 0x7B8713B8 | IDS_Name_14 | Race Fuel System |
| 0xC389C4BD | IDS_Name_141 | Stock Drivetrain |
| 0xC389C53D | IDS_Name_142 | Alternate Drivetrain |
| 0xC389C5BD | IDS_Name_143 | Stock Body |
| 0xC389C63D | IDS_Name_144 | Widebody Kit |
| 0xC389C6BD | IDS_Name_145 | Upgraded Rear Tire Width |
| 0xC389C73D | IDS_Name_146 | Upgraded Rear Tire Width |
| 0xC389C7BD | IDS_Name_147 | Upgraded Rear Tire Width |
| 0xC389C03D | IDS_Name_148 | Upgraded Rear Tire Width |
| 0xC389C0BD | IDS_Name_149 | Stock Rear Tire Width |
| 0x7B871338 | IDS_Name_15 | Stock Ignition |
| 0xC389843D | IDS_Name_150 | Upgraded Front Tire Width |
| 0xC38984BD | IDS_Name_151 | Upgraded Rear Tire Width |
| 0xC389853D | IDS_Name_152 | Upgraded Front Rim Size |
| 0xC38985BD | IDS_Name_153 | Upgraded Rear Rim Size |
| 0xC389863D | IDS_Name_154 | Upgraded Rear Rim Size |
| 0xC38986BD | IDS_Name_155 | Upgraded Rear Tire Width |
| 0xC389873D | IDS_Name_156 | Upgraded Rear Rim Size |
| 0xC38987BD | IDS_Name_157 | Upgraded Rear Rim Size |
| 0xC389803D | IDS_Name_158 | Upgraded Rear Rim Size |
| 0xC38980BD | IDS_Name_159 | Upgraded Rear Rim Size |
| 0x7B8712B8 | IDS_Name_16 | Street Ignition |
| 0xC389443D | IDS_Name_160 | Upgraded Rear Rim Size |
| 0xC38944BD | IDS_Name_161 | Stock Restrictor Plate |
| 0xC389453D | IDS_Name_162 | No Restrictor Plate |
| 0x7B871238 | IDS_Name_17 | Sport Ignition |
| 0x7B8715B8 | IDS_Name_18 | Race Ignition |
| 0x7B871538 | IDS_Name_19 | Stock Exhaust |
| 0x70F70F93 | IDS_Name_2 | Street Powertrain Swap |
| 0x7B87D1B8 | IDS_Name_20 | Street Exhaust |
| 0xC3E8C03D | IDS_Name_208 | Stock Intake Manifold / Throttle Body |
| 0xC3E8C0BD | IDS_Name_209 | Street Intake Manifold / Throttle Body |
| 0x7B87D138 | IDS_Name_21 | Sport Exhaust |
| 0xC3E8843D | IDS_Name_210 | Sport Intake Manifold / Throttle Body |
| 0xC3E884BD | IDS_Name_211 | Race Intake Manifold / Throttle Body |
| 0xC3E8853D | IDS_Name_212 | Stock Diesel Fuel System |
| 0xC3E885BD | IDS_Name_213 | Street Diesel Fuel System |
| 0xC3E8863D | IDS_Name_214 | Sport Diesel Fuel System |
| 0xC3E886BD | IDS_Name_215 | Race Diesel Ignition |
| 0xC3E8873D | IDS_Name_216 | Stock Carburetor |
| 0xC3E887BD | IDS_Name_217 | Street Carburetor |
| 0xC3E8803D | IDS_Name_218 | Sport Carburetor |
| 0xC3E880BD | IDS_Name_219 | Race Carburetor |
| 0x7B87D0B8 | IDS_Name_22 | Race Exhaust |
| 0xC3E8443D | IDS_Name_220 | Stock Valves |
| 0xC3E844BD | IDS_Name_221 | Street Valves |
| 0xC3E8453D | IDS_Name_222 | Sport Valves |
| 0xC3E845BD | IDS_Name_223 | Race Valves |
| 0xC3E8463D | IDS_Name_224 | Stock Pistons / Compression |
| 0xC3E846BD | IDS_Name_225 | Street Pistons / Compression |
| 0xC3E8473D | IDS_Name_226 | Sport Pistons / Compression |
| 0xC3E847BD | IDS_Name_227 | Race Pistons / Compression |
| 0xC3E8403D | IDS_Name_228 | Stock Rotors / Compression |
| 0xC3E840BD | IDS_Name_229 | Street Rotors / Compression |
| 0x7B87D038 | IDS_Name_23 | Stock Intake |
| 0xC3E8043D | IDS_Name_230 | Sport Rotors / Compression |
| 0xC3E804BD | IDS_Name_231 | Race Rotors / Compression |
| 0xC3E8053D | IDS_Name_232 | Stock Oil / Cooling |
| 0xC3E805BD | IDS_Name_233 | Street Oil / Cooling |
| 0xC3E8063D | IDS_Name_234 | Sport Oil / Cooling |
| 0xC3E806BD | IDS_Name_235 | Race Oil / Cooling |
| 0xC3E8073D | IDS_Name_236 | Stock Twin Turbo |
| 0xC3E807BD | IDS_Name_237 | Street Twin Turbo |
| 0xC3E8003D | IDS_Name_238 | Sport Twin Turbo |
| 0xC3E800BD | IDS_Name_239 | Race Twin Turbo |
| 0x7B87D3B8 | IDS_Name_24 | Street Intake |
| 0xC3E9C63D | IDS_Name_244 | Stock Rear Anti-roll Bars |
| 0xC3E9C6BD | IDS_Name_245 | Street Rear Anti-roll Bars |
| 0xC3E9C73D | IDS_Name_246 | Sport Rear Anti-roll Bars |
| 0xC3E9C7BD | IDS_Name_247 | Race Rear Anti-roll Bars |
| 0xC3E9C03D | IDS_Name_248 | Stock Chassis Reinforcement / Roll Cage |
| 0xC3E9C0BD | IDS_Name_249 | Street Chassis Reinforcement / Roll Cage |
| 0x7B87D338 | IDS_Name_25 | Sport Intake |
| 0xC3E9843D | IDS_Name_250 | Sport Chassis Reinforcement / Roll Cage |
| 0xC3E984BD | IDS_Name_251 | Race Chassis Reinforcement / Roll Cage |
| 0xC3E9853D | IDS_Name_252 | Stock - Single Turbo |
| 0xC3E985BD | IDS_Name_253 | Stock - Twin Turbo |
| 0xC3E9863D | IDS_Name_254 | Stock - Quad Turbo |
| 0xC3E986BD | IDS_Name_255 | Stock - Positive-Displacement Supercharger |
| 0xC3E9873D | IDS_Name_256 | Stock - Centrifugal Supercharger |
| 0xC3E987BD | IDS_Name_257 | Stock Rear Rim Size |
| 0xC3E9803D | IDS_Name_258 | Upgraded Rear Rim Size |
| 0xC3E980BD | IDS_Name_259 | Stock Motor and Battery Parts |
| 0x7B87D2B8 | IDS_Name_26 | Race Intake |
| 0xC3E9443D | IDS_Name_260 | Street Motor and Battery Parts |
| 0xC3E944BD | IDS_Name_261 | Sport Motor and Battery Parts |
| 0xC3E9453D | IDS_Name_262 | Race Motor and Battery Parts |
| 0xC3E9463D | IDS_Name_264 | Remove Front Bumper |
| 0xC3E946BD | IDS_Name_265 | Remove Wing |
| 0xC3E9473D | IDS_Name_266 | Remove Rear Bumper |
| 0xC3E947BD | IDS_Name_267 | Remove Side Skirts |
| 0x7B87D238 | IDS_Name_27 | Stock Turbo |
| 0xC3E9043D | IDS_Name_270 | Remove Restrictors |
| 0xC3E904BD | IDS_Name_271 | Rally Transmission |
| 0xC3E9053D | IDS_Name_272 | Rally Spring and Dampers |
| 0xC3E905BD | IDS_Name_273 | Rally Tire Compound |
| 0xC3E9063D | IDS_Name_274 | Offroad Race Tire Compound |
| 0xC3E906BD | IDS_Name_275 | 'Horizon' Semi-Slick Race Tire Compound |
| 0xC3E9073D | IDS_Name_276 | Roo Bars |
| 0xC3E907BD | IDS_Name_277 | Snow Tire Compound |
| 0xC3E9003D | IDS_Name_278 | Drift Spring and Dampers |
| 0xC3E900BD | IDS_Name_279 | Hot Rod Conversion |
| 0x7B87D5B8 | IDS_Name_28 | Street Turbo |
| 0xC3EAC43D | IDS_Name_280 | Drag Tire Compound |
| 0xC3EAC4BD | IDS_Name_281 | Vintage Race Tire Compound |
| 0xC3EAC53D | IDS_Name_282 | Stock Front Track Width |
| 0xC3EAC5BD | IDS_Name_283 | Upgraded Front Track Width |
| 0xC3EAC63D | IDS_Name_284 | Stock Rear Track Width |
| 0xC3EAC6BD | IDS_Name_285 | Upgraded Rear Track Width |
| 0xC3EAC73D | IDS_Name_286 | Upgraded Front Track Width |
| 0xC3EAC7BD | IDS_Name_287 | Upgraded Front Track Width |
| 0xC3EAC03D | IDS_Name_288 | Upgraded Rear Track Width |
| 0xC3EAC0BD | IDS_Name_289 | Upgraded Rear Track Width |
| 0x7B87D538 | IDS_Name_29 | Sport Turbo |
| 0xC3EA843D | IDS_Name_290 | Submarine Conversion |
| 0xC3EA84BD | IDS_Name_291 | Rally Diff |
| 0xC3EA853D | IDS_Name_292 | Drift Diff |
| 0xC3EA85BD | IDS_Name_293 | Race Transmission: 6 Speed |
| 0xC3EA863D | IDS_Name_294 | Race Transmission: 7 Speed |
| 0xC3EA86BD | IDS_Name_295 | Race Transmission: 8 Speed |
| 0xC3EA873D | IDS_Name_296 | Race Transmission: 9 Speed |
| 0xC3EA87BD | IDS_Name_297 | Race Transmission: 10 Speed |
| 0xC3EA803D | IDS_Name_298 | Drift Tire Compound |
| 0xC3EA80BD | IDS_Name_299 | Slick Race Tire Compound |
| 0x70F70F13 | IDS_Name_3 | Stock Cams and Valves |
| 0x7B8791B8 | IDS_Name_30 | Race Turbo |
| 0xC3C8C43D | IDS_Name_300 | Vintage White Wall Tire Compound |
| 0xC3C8C4BD | IDS_Name_301 | Offroad Diff |
| 0xC3C8C53D | IDS_Name_302 | Drift Transmission: 4 Speed |
| 0xC3C8C5BD | IDS_Name_303 | 'Hot Wheels' Semi-Slick Race Tire Compound |
| 0xC3C8C63D | IDS_Name_304 | Stock Tire Profile Size |
| 0xC3C8C6BD | IDS_Name_305 | Upgraded Front Tire Profile Size |
| 0xC3C8C73D | IDS_Name_306 | Upgraded Front Tire Profile Size |
| 0xC3C8C7BD | IDS_Name_307 | Upgraded Front Tire Profile Size |
| 0xC3C8C03D | IDS_Name_308 | Stock Rear Tire Profile Size |
| 0xC3C8C0BD | IDS_Name_309 | Upgraded Rear Tire Profile Size |
| 0x7B879138 | IDS_Name_31 | Stock Positive Displacement Supercharger |
| 0xC3C8843D | IDS_Name_310 | Upgraded Rear Tire Profile Size |
| 0xC3C884BD | IDS_Name_311 | Upgraded Rear Tire Profile Size |
| 0xC3C8853D | IDS_Name_312 | Race Turbo With Anti-Lag |
| 0xC3C885BD | IDS_Name_313 | Race Twin Turbo With Anti-Lag |
| 0xC3C8863D | IDS_Name_314 | Stock Motor and Battery Swap |
| 0xC3C886BD | IDS_Name_315 | Street Motor Swap |
| 0x7B8790B8 | IDS_Name_32 | Street Positive Displacement Supercharger |
| 0x7B879038 | IDS_Name_33 | Sport Positive Displacement Supercharger |
| 0x7B8793B8 | IDS_Name_34 | Race Positive Displacement Supercharger |
| 0x7B879338 | IDS_Name_35 | Stock Centrifugal Supercharger |
| 0x7B8792B8 | IDS_Name_36 | Street Centrifugal Supercharger |
| 0x7B879238 | IDS_Name_37 | Sport Centrifugal Supercharger |
| 0x7B8795B8 | IDS_Name_38 | Race Centrifugal Supercharger |
| 0x7B879538 | IDS_Name_39 | Stock Intercooler |
| 0x70F70C93 | IDS_Name_4 | Street Cams and Valves |
| 0x7B8651B8 | IDS_Name_40 | Street Intercooler |
| 0x7B865138 | IDS_Name_41 | Sport Intercooler |
| 0x7B8650B8 | IDS_Name_42 | Race Intercooler |
| 0x7B865038 | IDS_Name_43 | Stock Brakes |
| 0x7B8653B8 | IDS_Name_44 | Street Brakes |
| 0x7B865338 | IDS_Name_45 | Sport Brakes |
| 0x7B8652B8 | IDS_Name_46 | Race Brakes |
| 0x7B865238 | IDS_Name_47 | Stock Spring and Dampers |
| 0x7B8655B8 | IDS_Name_48 | Street Spring and Dampers |
| 0x7B865538 | IDS_Name_49 | Sport Spring and Dampers |
| 0x70F70C13 | IDS_Name_5 | Sport Cams and Valves |
| 0x7B8611B8 | IDS_Name_50 | Race Spring and Dampers |
| 0x7B861138 | IDS_Name_51 | Stock Front Anti-roll Bars |
| 0x7B8610B8 | IDS_Name_52 | Street Front Anti-roll Bars |
| 0x7B861038 | IDS_Name_53 | Sport Front Anti-roll Bars |
| 0x7B8613B8 | IDS_Name_54 | Race Front Anti-roll Bars |
| 0x7B861338 | IDS_Name_55 | Stock Transmission |
| 0x7B8612B8 | IDS_Name_56 | Street Transmission |
| 0x7B861238 | IDS_Name_57 | Sport Transmission |
| 0x7B8615B8 | IDS_Name_58 | Race Transmission |
| 0x7B861538 | IDS_Name_59 | Stock Clutch |
| 0x70F70D93 | IDS_Name_6 | Race Cams and Valves |
| 0x7B86D1B8 | IDS_Name_60 | Street Clutch |
| 0x7B86D138 | IDS_Name_61 | Sport Clutch |
| 0x7B86D0B8 | IDS_Name_62 | Race Clutch |
| 0x7B86D038 | IDS_Name_63 | Stock Flywheel |
| 0x7B86D3B8 | IDS_Name_64 | Street Flywheel |
| 0x7B86D338 | IDS_Name_65 | Sport Flywheel |
| 0x7B86D2B8 | IDS_Name_66 | Race Flywheel |
| 0x7B86D238 | IDS_Name_67 | Stock Driveline |
| 0x7B86D5B8 | IDS_Name_68 | Street Driveline |
| 0x7B86D538 | IDS_Name_69 | Sport Driveline |
| 0x70F70D13 | IDS_Name_7 | Stock Engine Block |
| 0x7B8691B8 | IDS_Name_70 | Race Driveline |
| 0x7B869138 | IDS_Name_71 | Stock Diff |
| 0x7B8690B8 | IDS_Name_72 | Street Diff |
| 0x7B869038 | IDS_Name_73 | Sport Diff |
| 0x7B8693B8 | IDS_Name_74 | Race Diff |
| 0x7B869338 | IDS_Name_75 | Stock Weight Reduction |
| 0x7B8692B8 | IDS_Name_76 | Street Weight Reduction |
| 0x7B869238 | IDS_Name_77 | Sport Weight Reduction |
| 0x7B8695B8 | IDS_Name_78 | Stock Tire Compound |
| 0x7B869538 | IDS_Name_79 | Street Tire Compound |
| 0x70F70A93 | IDS_Name_8 | Street Engine Block |
| 0x7B8551B8 | IDS_Name_80 | Sport Tire Compound |
| 0x7B855138 | IDS_Name_81 | Semi-Slick Race Tire Compound |
| 0x7B8550B8 | IDS_Name_82 | Stock Front Rim Size |
| 0x7B855038 | IDS_Name_83 | Upgraded Front Rim Size |
| 0x7B8553B8 | IDS_Name_84 | Stock Front Tire Width |
| 0x7B855338 | IDS_Name_85 | Upgraded Front Tire Width |
| 0x7B8552B8 | IDS_Name_86 | Stock Front Bumper |
| 0x7B855238 | IDS_Name_87 | Street Front Bumper |
| 0x7B8555B8 | IDS_Name_88 | Sport Front Bumper |
| 0x7B855538 | IDS_Name_89 | Race Front Bumper |
| 0x70F70A13 | IDS_Name_9 | Sport Engine Block |
| 0x7B8511B8 | IDS_Name_90 | Stock Rear Wing |
| 0x7B851138 | IDS_Name_91 | Street Rear Wing |
| 0x7B8510B8 | IDS_Name_92 | Sport Rear Wing |
| 0x7B851038 | IDS_Name_93 | Race Rear Wing |
| 0x7B8513B8 | IDS_Name_94 | Stock Rear Bumper |
| 0x7B851338 | IDS_Name_95 | Street Rear Bumper |
| 0x7B8512B8 | IDS_Name_96 | Sport Rear Bumper |
| 0x7B851238 | IDS_Name_97 | Race Rear Bumper |
| 0x7B8515B8 | IDS_Name_98 | Stock Side Skirts |
| 0x7B851538 | IDS_Name_99 | Street Side Skirts |

---

## Related source files

- `src/core/paint_finish_catalog.h` / `.cpp` — hardcoded paint-finish table (reads
  `List_LiveryMaterials` keys, does **not** parse `.str` at runtime)
- `src/core/game_paths.*` — locates `media/Stripped/StringTables/` and other paths
