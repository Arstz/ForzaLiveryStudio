import argparse
from collections import defaultdict
import json
from pathlib import Path
import sqlite3
import struct
import sys
import zipfile


DATABASES = {
    "FH6": "FH6gamedbRC.sqlite",
    "FH5": "FH5gamedbRC.sqlite",
    "FH4": "FH4gamedbRC.slt",
    "FH3": "FH3gamedbRC.slt",
    "FH2": "FH2gamedbRC.slt",
    "FH1": "FH1gamedb.slt",
    "FM2023": "FM2023gamedb.sqlite",
    "FM7": "FM7gamedb.slt",
    "FM6": "FM6GameDB.slt",
    "FM5": "FM5gamedb.slt",
    "FM4": "FM4gamedb.slt",
    "FM3": "FM3gamedb.slt",
    "FM2": "FM2gamedb.slt",
}
TYRE_PART_TYPE = 8
LEVEL_NAMES = {
    0: "Stock Tire Compound",
    1: "Street Tire Compound",
    2: "Sport Tire Compound",
    3: "Race Tire Compound",
    4: "'Horizon' Race Tire Compound",
    5: "Rally Tire Compound",
    6: "Slick Race Tire Compound",
    7: "Offroad Race Tire Compound",
    8: "Snow Tire Compound",
    9: "Drag Tire Compound",
    11: "Vintage Race Tire Compound",
    12: "Vintage White Wall Tire Compound",
    13: "'Hot Wheels' Race Tire Compound",
    15: "Drift Tire Compound",
}


def read_string_table(data, offset):
    if offset < 0 or offset + 12 > len(data):
        raise ValueError("Invalid string-table offset")
    size, text_size, count = struct.unpack_from("<III", data, offset)
    text_start = offset + 12 + count * 8
    text_end = text_start + text_size
    if size != count * 8 + text_size or text_end > len(data):
        raise ValueError("Invalid string-table size")
    strings = {}
    for index in range(count):
        key, position = struct.unpack_from("<II", data, offset + 12 + index * 8)
        if position >= text_size:
            raise ValueError("Invalid string position")
        end = data.find(b"\0", text_start + position, text_end)
        if end < 0 or key in strings:
            raise ValueError("Invalid string-table entry")
        strings[key] = data[text_start + position:end].decode("utf-8")
    return strings


def fh6_strings(folder):
    media = folder if folder.name.casefold() == "media" else folder / "media"
    with zipfile.ZipFile(media / "Stripped/StringTables/EN.zip") as archive:
        data = archive.read("Upgrades.str")
    if len(data) < 140 or data[:2] != b"\0\x08" or struct.unpack_from("<H", data, 130)[0] != 2:
        raise ValueError("Unsupported FH6 string-table format")
    values_offset, names_offset = struct.unpack_from("<II", data, 132)
    values = read_string_table(data, values_offset)
    names = read_string_table(data, names_offset)
    if values.keys() != names.keys():
        raise ValueError("String-table names and values differ")
    return {names[key]: value for key, value in values.items()}


def level_names(database, game, strings):
    names = dict(LEVEL_NAMES)
    if game.startswith("FM"):
        names[4] = "Drag Tire Compound"
    if game == "FH3":
        names[9] = "Redline Tire Compound"
    if game == "FH5":
        for level in (3, 4, 13):
            names[level] = names[level].replace("Race", "Semi-Slick Race")
    if game == "FH6":
        for upgrade_id, level in database.execute(
            "SELECT u.id,u.Level FROM Upgrades u "
            "JOIN UpgradeTypes t ON t.id=u.TypeId WHERE t.PartName='TireCompound'"
        ):
            names[level] = strings[f"IDS_Name_{upgrade_id}"]
    return names


def export_game(path, game, strings):
    result = defaultdict(dict)
    with sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True) as database:
        database.row_factory = sqlite3.Row
        if game == "FH6":
            part_type = database.execute(
                "SELECT Id FROM Data_UpgradePart WHERE TableName='List_UpgradeTireCompound'"
            ).fetchone()
            if part_type is None or part_type[0] != TYRE_PART_TYPE:
                raise ValueError("Unexpected FH6 tyre compound part type")
        names = level_names(database, game, strings)
        columns = {row[1] for row in database.execute("PRAGMA table_info(List_UpgradeTireCompound)")}
        orphan_count = database.execute(
            "SELECT COUNT(*) FROM List_UpgradeTireCompound t "
            "LEFT JOIN Data_Car c ON c.Id=t.Ordinal WHERE c.Id IS NULL"
        ).fetchone()[0]
        if orphan_count:
            raise ValueError(f"{game}: {orphan_count} tyre records have no car")
        for row in database.execute(
            "SELECT c.MediaName,t.* FROM List_UpgradeTireCompound t "
            "JOIN Data_Car c ON c.Id=t.Ordinal ORDER BY c.MediaName,t.Id"
        ):
            name = names[0] if row["IsStock"] else names[row["Level"]]
            model_name = row["TireModelName"] if "TireModelName" in columns else None
            if row["IsStock"] and model_name:
                name += f" ({model_name})"
            key = f"{TYRE_PART_TYPE}:{row['Id']}"
            model = row["MediaName"]
            if not model:
                raise ValueError(f"{game}: tyre {key} has no model name")
            previous = result[model].get(key)
            if previous is not None and previous != name:
                raise ValueError(f"{game}: conflicting names for {model}/{key}")
            result[model][key] = name
    return result


def append_names(catalog, games):
    models = {model.casefold(): model for model in catalog}
    collected = defaultdict(lambda: defaultdict(dict))
    for game, cars in games.items():
        for model, names in cars.items():
            model = models.setdefault(model.casefold(), model)
            for key, name in names.items():
                if game in collected[model][key] and collected[model][key][game] != name:
                    raise ValueError(f"{game}: conflicting model aliases for {model}/{key}")
                collected[model][key][game] = name
    for model, parts in sorted(collected.items()):
        entry = catalog.setdefault(model, {})
        for key, names in sorted(parts.items(), key=lambda item: int(item[0].split(":")[1])):
            default = next(iter(names.values()))
            entry[key] = default
            for game, name in names.items():
                overrides = entry.setdefault("games", {}).setdefault(game, {})
                overrides.pop(key, None)
                if name != default:
                    overrides[key] = name
        if "games" in entry:
            overrides = {game: names for game, names in entry.pop("games").items() if names}
            if overrides:
                entry["games"] = overrides
    return sum(len(parts) for parts in collected.values())


def format_catalog(catalog):
    compact = lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    return "{\n" + ",\n".join(compact(model) + ":" + compact(names)
                               for model, names in catalog.items()) + "\n}\n"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Append tyre compound names to the part catalog, preserving existing part names.",
        epilog="Top-level names prefer newer Horizon titles, then Motorsport. "
               "Per-model games entries override names that differ in other titles. "
               "Stock names retain the database's tyre model identifier in parentheses.")
    parser.add_argument("--databases", type=Path, default=root / "tests/GameDB")
    parser.add_argument("--output", type=Path, default=root / "assets/cars/part_names.json")
    parser.add_argument("--fh6-folder", type=Path, required=True,
                        help="FH6 installation or media folder supplying English upgrade labels")
    options = parser.parse_args()
    try:
        catalog = json.loads(options.output.read_text(encoding="utf-8"))
        strings = fh6_strings(options.fh6_folder)
        games = {game: export_game(options.databases / filename, game, strings)
                 for game, filename in DATABASES.items()}
        count = append_names(catalog, games)
        output = format_catalog(catalog)
        if output != options.output.read_text(encoding="utf-8"):
            options.output.write_text(output, encoding="utf-8", newline="\n")
        for game, cars in games.items():
            print(f"{game}: {sum(len(names) for names in cars.values())} tyre names, {len(cars)} cars")
        print(f"{count} distinct model/part keys written to {options.output}")
        return 0
    except (OSError, ValueError, KeyError, sqlite3.Error, zipfile.BadZipFile) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
