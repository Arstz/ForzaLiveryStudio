import argparse
import json
from pathlib import Path
import sqlite3

from append_manufacturer_palettes import compact_catalog


DATABASES = {
    "FH2": "FH2gamedbRC.slt",
    "FH3": "FH3gamedbRC.slt",
    "FH4": "FH4gamedbRC.slt",
    "FH5": "FH5gamedbRC.sqlite",
    "FH6": "FH6gamedbRC.sqlite",
    "FM5": "FM5gamedb.slt",
    "FM6": "FM6GameDB.slt",
    "FM7": "FM7gamedb.slt",
    "FM2023": "FM2023gamedb.sqlite",
}


def rows(database, table):
    if not database.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)).fetchone():
        return []
    cursor = database.execute(f'SELECT * FROM "{table}" ORDER BY 1')
    columns = [column[0] for column in cursor.description]
    return [dict(zip(columns, row)) for row in cursor]


def export_game(path):
    with sqlite3.connect(path.resolve().as_uri() + "?mode=ro", uri=True) as database:
        palette = {}
        for row in rows(database, "Combo_Colors"):
            palette.setdefault(str(row.pop("Ordinal")), []).append(row)
        return {
            "manufacturer_colors": palette,
            "manufacturer_color_exceptions": rows(database, "Combo_Colors_Exceptions"),
            "stripe_colors": rows(database, "StripeColors"),
            "color_classes": rows(database, "List_ColorClass"),
            "finish_classes": rows(database, "List_ColorFinish"),
            "special_colors": rows(database, "List_SpecialColors"),
            "finishes": rows(database, "List_LiveryMaterials"),
            "paintable_groups": rows(database, "PaintableGroups"),
            "paintable_group_names": rows(database, "PaintableGroupNames"),
        }


def format_catalog(value, depth=0):
    compact = lambda item: json.dumps(item, ensure_ascii=False, separators=(",", ":"))
    if isinstance(value, dict) and value and depth < 4:
        entries = [compact(key) + ":" + format_catalog(item, depth + 1)
                   for key, item in value.items()]
        return "{\n" + ",\n".join(entries) + "\n}"
    if isinstance(value, list) and value and depth < 4:
        return "[\n" + ",\n".join(format_catalog(item, depth + 1) for item in value) + "\n]"
    return compact(value)


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--databases", type=Path, default=root / "tests/GameDB")
    parser.add_argument("--output", type=Path, default=root / "assets/cars/paint_colors.json")
    options = parser.parse_args()
    previous = json.loads(options.output.read_text(encoding="utf-8")) if options.output.exists() else {}
    games = {game: export_game(options.databases / name) for game, name in DATABASES.items()}
    for game, entry in games.items():
        native = previous.get("games", {}).get(game, {}).get("manufacturer_palettes")
        if native is not None:
            entry["manufacturer_palettes"] = native
    catalog = {
        **previous,
        "version": 1,
        "games": games,
    }
    catalog = compact_catalog(catalog)
    options.output.write_text(format_catalog(catalog) + "\n", encoding="utf-8", newline="\n")
    for game, data in catalog["games"].items():
        print(game, "cars", len(data["manufacturer_colors"]), "finishes", len(data["finishes"]))
    print(options.output)


if __name__ == "__main__":
    main()
