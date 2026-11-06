import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import zipfile
import zlib


PALETTE_TAG = 0x4D4E434C
PALETTE_KEY = "manufacturer_palettes"
ZIP_LZX = 21
XMEM_FRAME_SIZE = 32768
LZX_WINDOW_BITS = (16, 17, 15, 18, 19, 20, 21)
CATALOG_METADATA = frozenset({
    "manufacturer_selector_mapping", "model", "archive", "zip_entry",
    "bundle_version", "palette_version", "binary_sha256", "opaque_tail",
    "BaseTexture", "DamageTexture", "PreviewTexture", "CameraPreset",
    "IconPath", "ErrorString", "SortOrder", "releaseOrder",
})


def material_name(path):
    name = path.replace("\\", "/").rsplit("/", 1)[-1]
    for suffix in (".materialbin", ".matpresetbin"):
        if name.lower().endswith(suffix):
            return name[:-len(suffix)]
    return name


def compact_catalog(value):
    if isinstance(value, list):
        return [compact_catalog(item) for item in value]
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key in CATALOG_METADATA:
                continue
            if key in ("MaterialPath", "material"):
                key = "Material" if key == "MaterialPath" else "name"
                item = material_name(item)
            result[key] = compact_catalog(item)
        return result
    return value


class Cursor:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def take(self, size):
        if size < 0 or self.offset + size > len(self.data):
            raise ValueError(f"Truncated palette at byte {self.offset}")
        value = self.data[self.offset:self.offset + size]
        self.offset += size
        return value

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def string(self):
        return self.take(self.u8()).decode("latin1")

    def rgb(self):
        value = list(struct.unpack("<fff", self.take(12)))
        if not all(math.isfinite(channel) for channel in value):
            raise ValueError(f"Non-finite palette color at byte {self.offset - 12}")
        return value


def palette_blob(data):
    if len(data) < 8 or data[:4] != b"burG":
        raise ValueError("Invalid palette bundle header")
    version = tuple(data[4:6])
    if version >= (1, 1):
        if len(data) < 20:
            raise ValueError("Truncated bundle header")
        count = struct.unpack_from("<I", data, 16)[0]
        first = 20
    else:
        count = struct.unpack_from("<H", data, 6)[0]
        first = 16
    if first + count * 24 > len(data):
        raise ValueError("Truncated bundle directory")
    blobs = []
    for index in range(count):
        tag, major, minor, _, _, offset, compressed, expanded = struct.unpack_from(
            "<IBBHIIII", data, first + index * 24)
        if tag != PALETTE_TAG:
            continue
        size = compressed or expanded
        if not size or offset + size > len(data):
            raise ValueError("Invalid palette blob span")
        blob = data[offset:offset + size]
        if compressed and expanded and compressed != expanded:
            blob = zlib.decompress(blob)
        if expanded and len(blob) != expanded:
            raise ValueError("Palette blob size differs from its header")
        blobs.append(((major, minor), blob))
    if len(blobs) != 1:
        raise ValueError(f"Expected one manufacturer palette blob, found {len(blobs)}")
    return version, *blobs[0]


def decode_palette(data):
    bundle_version, version, blob = palette_blob(data)
    if version not in {(1, 0), (1, 1), (2, 0)}:
        raise ValueError(f"Unsupported manufacturer palette version {version}")
    cursor = Cursor(blob)
    colors = []
    for index in range(cursor.u8()):
        entries = []
        for _ in range(cursor.u8()):
            entry = {}
            if version[0] >= 2:
                count = cursor.u32()
                if count > 256:
                    raise ValueError("Invalid material slot count")
                entry["material_slots"] = [cursor.string() for _ in range(count)]
            else:
                entry["material_mask"] = cursor.u32() if version[1] >= 1 else cursor.u16()
            entry["rgb"] = cursor.rgb()
            entry["material"] = cursor.string()
            entries.append(entry)
        color = {"index": index, "enabled": bool(entries), "entries": entries}
        if version[0] >= 2:
            overrides = []
            for _ in range(3):
                present = cursor.u8()
                if present not in (0, 1):
                    raise ValueError("Invalid color override presence flag")
                overrides.append({"enabled": bool(present), "rgb": cursor.rgb() if present else None})
            color["color_overrides"] = overrides
        colors.append(color)
    result = {
        "bundle_version": list(bundle_version),
        "palette_version": list(version),
        "binary_sha256": hashlib.sha256(data).hexdigest(),
        "colors": colors,
    }
    if cursor.offset < len(blob):
        result["opaque_tail"] = blob[cursor.offset:].hex()
    return result


def xmem_frames(data, expanded_size):
    frames = []
    offset = 0
    produced = 0
    view = memoryview(data)
    while produced < expanded_size:
        if offset >= len(data):
            raise ValueError("Truncated XMem frame header")
        header_size = 5 if data[offset] == 255 else 2
        if offset + header_size > len(data):
            raise ValueError("Truncated XMem frame header")
        if header_size == 5:
            expanded, compressed = struct.unpack_from(">HH", data, offset + 1)
        else:
            expanded = min(XMEM_FRAME_SIZE, expanded_size - produced)
            compressed = struct.unpack_from(">H", data, offset)[0]
        offset += header_size
        if not 0 < expanded <= min(XMEM_FRAME_SIZE, expanded_size - produced):
            raise ValueError("Invalid XMem frame output size")
        if not compressed or offset + compressed > len(data):
            raise ValueError("Invalid XMem frame compressed size")
        frames.append((view[offset:offset + compressed], expanded))
        offset += compressed
        produced += expanded
    if data[offset:] not in (b"", b"\x00\x00", b"\x00" * 5, b"\xff\x00\x00\x00\x00"):
        raise ValueError("Unexpected data after XMem frames")
    return frames


def decompress_xmem(data, expanded_size, expected_crc):
    frames = xmem_frames(data, expanded_size)
    if not frames:
        if expected_crc:
            raise ValueError("Invalid CRC for empty XMem entry")
        return b""
    try:
        from refinery.lib.seven.lzx import LzxDecoder
    except ImportError as error:
        raise RuntimeError("ZIP method 21 requires binary-refinery; install it with: "
                           "python -m pip install binary-refinery") from error
    failure = ""
    for window_bits in LZX_WINDOW_BITS:
        decoder = LzxDecoder()
        decoder.set_params_and_alloc(window_bits)
        output = bytearray()
        try:
            for frame, expanded in frames:
                block = decoder.decompress(frame, expanded)
                if len(block) != expanded:
                    raise ValueError("LZX frame size differs from its header")
                output.extend(block)
                decoder.keep_history = True
            if len(output) != expanded_size:
                raise ValueError("LZX entry size differs from its ZIP header")
            if zlib.crc32(output) != expected_crc:
                raise ValueError("Invalid CRC for LZX entry")
            return bytes(output)
        except (EOFError, IndexError, ValueError, RuntimeError, ArithmeticError) as error:
            failure = str(error)
    raise ValueError(f"ZIP method 21 LZX decoding failed: {failure}")


def read_zip_entry(archive, name):
    entry = archive.getinfo(name)
    if entry.compress_type != ZIP_LZX:
        return archive.read(entry)
    if entry.flag_bits & 65:
        raise ValueError("Encrypted ZIP entries are unsupported")
    stream = archive.fp
    stream.seek(entry.header_offset)
    header = stream.read(30)
    if len(header) != 30 or header[:4] != b"PK\x03\x04":
        raise ValueError("Invalid ZIP local header")
    _, _, flags, method, _, _, _, _, _, name_size, extra_size = struct.unpack("<4s5H3I2H", header)
    if method != ZIP_LZX or flags & 65:
        raise ValueError("Invalid LZX ZIP local header")
    local_name = stream.read(name_size)
    encoding = "utf-8" if flags & 2048 else "cp437"
    if local_name.decode(encoding) != entry.orig_filename:
        raise ValueError("ZIP entry name differs from its local header")
    offset = entry.header_offset + 30 + name_size + extra_size
    if offset + entry.compress_size > archive.start_dir:
        raise ValueError("ZIP entry overlaps the central directory")
    stream.seek(offset)
    data = stream.read(entry.compress_size)
    if len(data) != entry.compress_size:
        raise ValueError("Truncated LZX ZIP entry")
    return decompress_xmem(data, entry.file_size, entry.CRC)


def read_archive(path):
    try:
        with zipfile.ZipFile(path) as archive:
            candidates = [name for name in archive.namelist()
                          if name.replace("\\", "/").rsplit("/", 1)[-1].lower() == "manufacturercolors.bin"]
            root_entries = [name for name in candidates if name.lower() == "manufacturercolors.bin"]
            candidates = root_entries or candidates
            if not candidates:
                return {"archive": path.name, "status": "no_palette"}
            if len(candidates) != 1:
                raise ValueError("Multiple manufacturer palette files")
            name = candidates[0]
            palette = decode_palette(read_zip_entry(archive, name))
        return {"archive": path.name, "status": "ok", "palette": {
            "model": path.stem, "archive": path.name, "zip_entry": name, **palette,
        }}
    except (OSError, ValueError, RuntimeError, struct.error, zipfile.BadZipFile, zlib.error) as error:
        return {"archive": path.name, "status": "error", "error": str(error)}


def model_index(registry, game):
    models = {}
    for car, entry in registry.items():
        if game not in {version.strip() for version in entry.get("version", "").split(",")}:
            continue
        model = entry.get("games", {}).get(game, {}).get("model", entry.get("model", ""))
        if model:
            models.setdefault(model.casefold(), []).append(car)
    return models


def import_game(catalog, registry, game, directory, workers):
    if not directory.is_dir():
        raise ValueError(f"Cars directory was not found: {directory}")
    models = model_index(registry, game)
    archives = sorted((path for path in directory.iterdir()
                       if path.is_file() and path.suffix.lower() == ".zip"), key=lambda path: path.name.casefold())
    matched = [path for path in archives if path.stem.casefold() in models]
    if not matched:
        raise ValueError(f"No registered {game} car archives were found in {directory}")
    palettes = dict(catalog["games"][game].get(PALETTE_KEY, {}))
    failures = []
    missing = []
    imported = 0
    color_count = 0
    entry_count = 0
    versions = {}
    with ThreadPoolExecutor(max_workers=workers) as executor:
        for number, result in enumerate(executor.map(read_archive, matched), 1):
            if result["status"] == "error":
                failures.append(result)
            elif result["status"] == "no_palette":
                missing.append(result["archive"])
            else:
                palette = result["palette"]
                cars = models[Path(result["archive"]).stem.casefold()]
                for car in cars:
                    palettes[car] = palette
                imported += len(cars)
                color_count += len(palette["colors"]) * len(cars)
                entry_count += sum(len(color["entries"]) for color in palette["colors"]) * len(cars)
                version = ".".join(map(str, palette["palette_version"]))
                versions[version] = versions.get(version, 0) + 1
            if number % 100 == 0 or number == len(matched):
                print(f"{game}: inspected {number}/{len(matched)} archives", flush=True)
    catalog["games"][game][PALETTE_KEY] = dict(sorted(palettes.items(), key=lambda item: int(item[0])))
    available = {path.stem.casefold() for path in archives}
    return {
        "game": game, "archives": len(archives), "matched_archives": len(matched),
        "imported_cars": imported, "palette_indices": color_count, "material_entries": entry_count,
        "palette_versions": versions, "no_palette": missing, "errors": failures,
        "unregistered_archives": [path.name for path in archives if path.stem.casefold() not in models],
        "missing_models": sorted(model for model in models if model not in available),
    }


def format_catalog(value, depth=0):
    compact = lambda item: json.dumps(item, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    if isinstance(value, dict) and value and depth < 4:
        entries = [compact(key) + ":" + format_catalog(item, depth + 1)
                   for key, item in value.items()]
        return "{\n" + ",\n".join(entries) + "\n}"
    if isinstance(value, list) and value and depth < 4:
        return "[\n" + ",\n".join(format_catalog(item, depth + 1) for item in value) + "\n]"
    return compact(value)


def main():
    script_directory = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Append manufacturer colors into paint json",
        epilog="ZIP method 21 (XMem LZX) requires: python -m pip install binary-refinery")
    parser.add_argument("--game", nargs=2, action="append", required=True, metavar=("GAME", "CARS_DIRECTORY"))
    parser.add_argument("--catalog", type=Path, default=script_directory / "paint_colors.json",
                        help="Paint catalog JSON (default: paint_colors.json beside this script)")
    parser.add_argument("--registry", type=Path, default=script_directory / "car_ids.json",
                        help="Car registry JSON (default: car_ids.json beside this script)")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--report", type=Path)
    options = parser.parse_args()
    if options.workers < 1:
        parser.error("--workers must be positive")
    try:
        catalog = json.loads(options.catalog.read_text(encoding="utf-8"))
        registry = json.loads(options.registry.read_text(encoding="utf-8"))
        if catalog.get("version") != 1 or not isinstance(catalog.get("games"), dict):
            raise ValueError("Unsupported paint catalog schema")
        summaries = []
        for game, directory in options.game:
            game = game.upper()
            if game not in catalog["games"]:
                raise ValueError(f"Game is not present in the catalog: {game}")
            summaries.append(import_game(catalog, registry, game, Path(directory), options.workers))
        if options.report:
            options.report.write_text(json.dumps({"games": summaries}, indent=2) + "\n", encoding="utf-8")
        failures = [f"{summary['game']}/{entry['archive']}: {entry['error']}"
                    for summary in summaries for entry in summary["errors"]]
        if failures:
            raise ValueError("Palette import failed; catalog was not changed:\n" + "\n".join(failures))
        options.catalog.write_text(format_catalog(compact_catalog(catalog)) + "\n", encoding="utf-8", newline="\n")
        for summary in summaries:
            print(f"{summary['game']}: appended {summary['imported_cars']} car palettes, "
                  f"{summary['palette_indices']} color indices, {summary['material_entries']} material entries; "
                  f"{len(summary['no_palette'])} archives have no palette")
        print(options.catalog)
        return 0
    except (OSError, ValueError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
