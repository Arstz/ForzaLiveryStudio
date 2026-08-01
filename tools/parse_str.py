"""
ForzaTech .str (hashed string table) parser for FH6 EN.zip.

Format (from 7akeem0/forzatech-localization-toolkit):
  0x00..0x02  magic 00 08
  0x02..0x80  null-terminated table name + zero padding
  0x80..0x84  FH6-specific (0x00020000), FH5 had constant 0x8C here
  0x84..0x88  values section offset (0x8C)
  0x88..0x8C  keys section offset (= 0x8C + values_section_size)
  0x8C..      VALUES section
  ...         KEYS section

Each section (VALUES, KEYS):
  +0x00  u32  section_size  = 12 + 8*entry_count + blob_size
  +0x04  u32  blob_size
  +0x08  u32  entry_count
  +0x0C  8*N  entries (hash:u32, offset_into_blob:u32)
  +...   var  null-terminated UTF-8 string blob

Keys and values are linked by index: keys[i].hash == values[i].hash.

Usage:
  python parse_str.py [--zip PATH] [--filter PATTERN] [--dump-all]
"""

import struct
import zipfile
import sys
import os
from dataclasses import dataclass, field


@dataclass
class Entry:
    hash: int
    string: str


@dataclass
class StrTable:
    name: str
    keys: list = field(default_factory=list)
    values: list = field(default_factory=list)
    fh6_flags: int = 0


def read_section(data, off):
    """Read a (section_size, blob_size, entry_count, entries) from data at off."""
    section_size, blob_size, count = struct.unpack_from('<III', data, off)
    entries_off = off + 12
    blob_off = entries_off + 8 * count
    entries = []
    for i in range(count):
        h, s_off = struct.unpack_from('<II', data, entries_off + i * 8)
        end = data.index(b'\x00', blob_off + s_off)
        entries.append(Entry(h, data[blob_off + s_off:end].decode('utf-8', errors='replace')))
    return entries


def parse_str(data):
    """Parse a .str file into a StrTable."""
    if len(data) < 0x8C:
        raise ValueError(f"File too small ({len(data)} bytes)")
    if data[:2] != b'\x00\x08':
        raise ValueError(f"Bad magic: {data[:2].hex()}")

    name_end = data.index(b'\x00', 2)
    name = data[2:name_end].decode('ascii', errors='replace')

    fh6_flags = struct.unpack_from('<I', data, 0x80)[0]
    values_off = struct.unpack_from('<I', data, 0x84)[0]
    keys_off = struct.unpack_from('<I', data, 0x88)[0]

    if values_off != 0x8C:
        raise ValueError(f"Unexpected values_off: {values_off:#x}")

    values = read_section(data, values_off)
    keys = read_section(data, keys_off)

    return StrTable(name=name, keys=keys, values=values, fh6_flags=fh6_flags)


CAR_RELATED = [
    'Data_Car.str',
    'CarBuckets.str',
    'CarClasses.str',
    'CarDetails.str',
    'CarFlow.str',
    'CarHistory.str',
    'CarHorn.str',
    'CarHornCategory.str',
    'CarLoadingCutscene.str',
    'CarMeets.str',
    'AftermarketCars.str',
    'List_CarMake.str',
    'List_CarType.str',
    'Dialogue_CarAuto.str',
    'OMCarRestrictions.str',
    'ChallengeCarCollectionObjective.str',
    'PaintableGroups.str',
    'TreasureCarInstances.str',
    'UpgradeCar.str',
    'Upgrades.str',
    'Upgrades_flow.str',
    'UpgradeTypes.str',
    'UpgradeWizard.str',
    'UpgradePresetPackages.str',
    'Livery.str',
    'Livery_Categories.str',
    'Livery_Decals.str',
    'Livery_VinylNames.str',
    'List_LiveryMaterials.str',
    'List_Aspiration.str',
    'List_Country.str',
    'List_Cylinders.str',
    'List_DriveType.str',
    'List_EngineConfig.str',
    'List_EnginePlacement.str',
    'List_FamilyBody.str',
    'List_FamilyModel.str',
    'List_FamilySpecial.str',
    'List_PartManufacturer.str',
    'List_Region.str',
    'List_ShiftSystem.str',
    'WheelCategories.str',
    'DefaultGarageLayout.str',
    'GarageLayout.str',
    'ConvertibleModeCustomStrings.str',
]


def main():
    zip_path = None
    file_filter = ""
    dump_all = False
    specific_files = []

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == '--zip' and i + 1 < len(args):
            zip_path = args[i + 1]
            i += 2
        elif args[i] == '--filter' and i + 1 < len(args):
            file_filter = args[i + 1]
            i += 2
        elif args[i] == '--dump-all':
            dump_all = True
            i += 1
        elif args[i] == '--files':
            i += 1
            while i < len(args) and not args[i].startswith('--'):
                specific_files.append(args[i])
                i += 1
        else:
            i += 1

    if not zip_path:
        zip_path = r'S:\SteamLibrary\steamapps\common\ForzaHorizon6\media\Stripped\StringTables\EN.zip'

    if not os.path.exists(zip_path):
        print(f"ERROR: {zip_path} not found")
        sys.exit(1)

    with zipfile.ZipFile(zip_path, 'r') as zf:
        str_entries = sorted(e for e in zf.namelist() if e.endswith('.str'))

        if specific_files:
            target = specific_files
        elif file_filter:
            target = [f for f in str_entries if file_filter.lower() in f.lower()]
        else:
            target = [f for f in str_entries if f in CAR_RELATED]

        target.sort()

        for entry_name in target:
            if entry_name not in str_entries:
                print(f"WARNING: {entry_name} not found in zip")
                continue

            raw = zf.read(entry_name)
            try:
                t = parse_str(raw)
            except Exception as e:
                print(f"\n{'='*80}")
                print(f"FILE: {entry_name}  ({len(raw)} bytes)")
                print(f"  PARSE ERROR: {e}")
                continue

            print(f"\n{'='*80}")
            print(f"FILE: {entry_name}  ({len(raw)} bytes)")
            print(f"  Table: {t.name}  |  FH6 flags: 0x{t.fh6_flags:08X}")
            print(f"  Values: {len(t.values)} entries  |  Keys: {len(t.keys)} entries")

            if len(t.keys) != len(t.values):
                print(f"  WARNING: key/value count mismatch!")

            n = len(t.values)
            show = dump_all or n <= 100

            if show:
                for i in range(n):
                    k = t.keys[i].string if i < len(t.keys) else "<missing>"
                    v = t.values[i].string if i < len(t.values) else "<missing>"
                    h = t.values[i].hash if i < len(t.values) else 0
                    print(f"    0x{h:08X}  {k:50s} = {v}")
            else:
                for i in range(min(25, n)):
                    k = t.keys[i].string if i < len(t.keys) else "<missing>"
                    v = t.values[i].string if i < len(t.values) else "<missing>"
                    h = t.values[i].hash if i < len(t.values) else 0
                    print(f"    0x{h:08X}  {k:50s} = {v}")
                print(f"    ... ({n - 35} more) ...")
                for i in range(max(0, n - 10), n):
                    k = t.keys[i].string if i < len(t.keys) else "<missing>"
                    v = t.values[i].string if i < len(t.values) else "<missing>"
                    h = t.values[i].hash if i < len(t.values) else 0
                    print(f"    0x{h:08X}  {k:50s} = {v}")


if __name__ == '__main__':
    main()
