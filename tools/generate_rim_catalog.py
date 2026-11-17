import argparse
from collections import defaultdict
import json
from pathlib import Path
import sqlite3
import struct
import sys
import zipfile

from append_tyre_names import DATABASES, read_string_table


GAME_ORDER = (
    "FH1", "FH2", "FH3", "FH4", "FH5", "FH6",
    "FM2", "FM3", "FM4", "FM5", "FM6", "FM7", "FM2023",
)


def decode_text(value):
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        return value.decode("cp1252")


def rows(database, table):
    if not database.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)
    ).fetchone():
        return None
    return [dict(row) for row in database.execute(f'SELECT * FROM "{table}" ORDER BY 1')]


def index_rows(records, key):
    if records is None:
        raise ValueError(f"Missing required table with key {key}")
    result = {}
    for record in records:
        identity = record[key]
        if identity in result:
            raise ValueError(f"Duplicate {key}: {identity}")
        result[identity] = record
    return result


def localized_strings(folder):
    media = folder if folder.name.casefold() == "media" else folder / "media"
    result = {}
    with zipfile.ZipFile(media / "Stripped/StringTables/EN.zip") as archive:
        for table in ("List_PartManufacturer", "WheelCategories"):
            data = archive.read(table + ".str")
            if len(data) < 140 or data[:2] != b"\0\x08" or struct.unpack_from("<H", data, 130)[0] != 2:
                raise ValueError(f"Unsupported string-table format: {table}")
            values_offset, names_offset = struct.unpack_from("<II", data, 132)
            values = read_string_table(data, values_offset)
            names = read_string_table(data, names_offset)
            if values.keys() != names.keys():
                raise ValueError(f"String-table names and values differ: {table}")
            result[table] = {names[key]: value for key, value in values.items()}
    return result


def clean_name(value):
    if value is None or value.strip().casefold() in ("", "null"):
        return None
    return value.strip()


def named_entry(value, translation=None, fallback=None):
    value = clean_name(value)
    if value and not value.startswith("_&"):
        return {"name": value}
    name = clean_name(translation) or clean_name(fallback)
    result = {"name": name}
    if name is None and value:
        result["name_key"] = value
    return result


def load_game(path):
    with sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True) as database:
        database.row_factory = sqlite3.Row
        database.text_factory = decode_text
        return {
            "rims": index_rows(rows(database, "List_Wheels"), "ID"),
            "manufacturers": index_rows(rows(database, "List_PartManufacturer"), "Id"),
            "cars": index_rows(rows(database, "Data_Car"), "Id"),
            "car_restrictions": rows(database, "CarUpgradeExceptions"),
            "wheel_exceptions": rows(database, "WheelExceptions"),
            "categories": rows(database, "WheelCategories"),
            "annotations": rows(database, "WheelAnnotations"),
        }


def consolidate(entries):
    default = next(iter(entries.values()))
    result = dict(default)
    result["version"] = ", ".join(game for game in GAME_ORDER if game in entries)
    overrides = {}
    for game in GAME_ORDER:
        if game not in entries:
            continue
        changes = {key: value for key, value in entries[game].items() if value != default.get(key)}
        changes.update({key: None for key in default if key not in entries[game]})
        if changes:
            overrides[game] = changes
    if overrides:
        result["games"] = overrides
    return result


def manufacturer_catalog(games, strings):
    literal_names = {}
    for data in games.values():
        for identity, record in data["manufacturers"].items():
            name = clean_name(record["PartManufacturer"])
            if name and not name.startswith("_&"):
                literal_names.setdefault(identity, name)
    entries = defaultdict(dict)
    for game, data in games.items():
        used = {rim["PartManufacturerID"] for rim in data["rims"].values()}
        for identity in sorted(used):
            record = data["manufacturers"].get(identity)
            entry = named_entry(
                record["PartManufacturer"] if record else None,
                strings.get(f"IDS_PartManufacturer_{identity}"), literal_names.get(identity))
            entry["defined"] = record is not None
            entries[identity][game] = entry
    return {str(identity): consolidate(data) for identity, data in sorted(entries.items())}


def rim_catalog(games):
    entries = defaultdict(dict)
    for game, data in games.items():
        for identity, rim in data["rims"].items():
            if not rim["MediaName"] or rim["IsStock"] not in (0, 1):
                raise ValueError(f"{game}: invalid rim {identity}")
            entries[identity][game] = {
                "model": rim["MediaName"],
                "name": clean_name(rim["DisplayName"]),
                "manufacturer": rim["PartManufacturerID"],
                "stock": bool(rim["IsStock"]),
                "type": rim["Type"],
                "release_order": rim.get("releaseOrder", 0),
            }
    return {str(identity): consolidate(data) for identity, data in sorted(entries.items())}


def game_restrictions(game, data, strings):
    result = {"stock_rims": {}}
    for identity, car in data["cars"].items():
        rim_id = car["StockWheelID"]
        if rim_id not in data["rims"]:
            raise ValueError(f"{game}: stock rim {rim_id} for car {identity} is missing")
        result["stock_rims"][str(identity)] = rim_id
    if data["car_restrictions"] is not None:
        result["car_restrictions"] = {
            str(row["CarID"]): {"no_rim_styles": row["NoRimStyles"]}
            for row in data["car_restrictions"]
        }
    if data["wheel_exceptions"] is not None:
        result["wheel_exceptions"] = [row["WheelID"] for row in data["wheel_exceptions"]]
    if data["categories"] is not None:
        result["categories"] = {}
        for row in data["categories"]:
            result["categories"][str(row["ID"])] = {
                **named_entry(row["Name"], strings.get(f"IDS_Name_{row['ID']}")),
                "stock": bool(row["IsStock"]),
                "order": row["DisplayOrder"],
                "release_order": row.get("ReleaseOrder", 0),
            }
    if data["annotations"] is not None:
        categories = result.get("categories", {})
        annotations = defaultdict(list)
        for row in data["annotations"]:
            if row["WheelID"] not in data["rims"] or str(row["CategoryID"]) not in categories:
                raise ValueError(f"{game}: invalid rim annotation {row['ID']}")
            annotations[str(row["WheelID"])].append({
                "category": row["CategoryID"],
                "order": row["DisplayOrder"],
                "release_order": row.get("ReleaseOrder", 0),
            })
        result["annotations"] = dict(sorted(annotations.items(), key=lambda item: int(item[0])))
    return result


def format_catalog(catalog):
    compact = lambda value: json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    sections = []
    for key, value in catalog.items():
        if isinstance(value, dict):
            text = "{\n" + ",\n".join(compact(identity) + ":" + compact(entry)
                                      for identity, entry in value.items()) + "\n}"
        else:
            text = compact(value)
        sections.append(compact(key) + ":" + text)
    return "{\n" + ",\n".join(sections) + "\n}\n"


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(
        description="Export rim IDs, manufacturers, title availability, and database restrictions.",
        epilog="Canonical fields prefer newer Horizon titles, then Motorsport; games entries override "
               "only differing fields. Version lists describe database membership. Restriction flags "
               "retain native values. Absent restriction tables are omitted. Unresolved names use null "
               "and retain their localization key when present. Manufacturer defined records whether "
               "the title supplies a manufacturer row; manufacturer 0 remains unspecified.")
    parser.add_argument("--databases", type=Path, default=root / "tests/GameDB")
    parser.add_argument("--output", type=Path, default=root / "assets/cars/rim_ids.json")
    parser.add_argument("--fh6-folder", type=Path, required=True,
                        help="FH6 installation or media folder supplying English manufacturer and category labels")
    options = parser.parse_args()
    try:
        strings = localized_strings(options.fh6_folder)
        games = {game: load_game(options.databases / filename) for game, filename in DATABASES.items()}
        catalog = {
            "version": 1,
            "manufacturers": manufacturer_catalog(games, strings["List_PartManufacturer"]),
            "rims": rim_catalog(games),
            "games": {game: game_restrictions(game, games[game], strings["WheelCategories"])
                      for game in GAME_ORDER},
        }
        output = format_catalog(catalog)
        if not options.output.exists() or options.output.read_text(encoding="utf-8") != output:
            options.output.write_text(output, encoding="utf-8", newline="\n")
        for game in GAME_ORDER:
            rims = games[game]["rims"]
            print(f"{game}: {len(rims)} rims, {sum(not rim['IsStock'] for rim in rims.values())} aftermarket")
        print(f"{len(catalog['rims'])} rim IDs, {len(catalog['manufacturers'])} manufacturer IDs: {options.output}")
        unresolved = [identity for identity, entry in catalog["manufacturers"].items()
                      if identity != "0" and entry["name"] is None]
        if unresolved:
            print("Unresolved manufacturer names (IDs retained): " + ", ".join(unresolved))
        return 0
    except (OSError, ValueError, KeyError, sqlite3.Error, zipfile.BadZipFile) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
