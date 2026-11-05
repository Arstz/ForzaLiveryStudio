import argparse
from pathlib import Path
import struct
import sys
import zlib


SLOT_NAMES = {
    0: "Body",
    1: "Unknown",
    2: "Hood",
    3: "Mirrors",
    4: "Spoiler",
    5: "Brakes",
    6: "Rims: group 1",
    7: "Window tint",
    8: "Body (variant 2)",
    9: "Hood (variant 2)",
    10: "Mirrors (variant 2)",
    11: "Spoiler (variant 2)",
    12: "Body (variant 3)",
    13: "Hood (variant 3)",
    14: "Mirrors (variant 3)",
    15: "Spoiler (variant 3)",
    16: "Unknown",
    17: "Unknown",
    18: "Unknown",
    19: "Unknown",
    20: "Unknown",
    21: "Unknown",
    22: "Unknown",
    23: "Unknown",
    24: "Unknown",
    25: "Unknown",
    26: "Unknown",
    27: "Unknown",
    28: "Unknown",
    29: "Unknown",
    30: "Rims: group 2",
    31: "Unknown rim area",
    32: "Rims: barrel",
    33: "Unknown rim area",
}

RECORD_COUNTS = (20, 30, 34)
RECORD_SIZE = 40
BLOCK_OVERHEAD = 3
PREFIX_SIZES = (0, 8, 16)
NO_MANUFACTURER_COLOR = 0xffffffff


def read_u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise ValueError("Truncated integer field")
    return struct.unpack_from("<I", data, offset)[0]


def resolve_livery(path):
    if path.is_dir():
        candidates = sorted(file for file in path.iterdir() if file.is_file()
                            and (file.name.lower() in {"c_livery", "data"}
                                 or file.name.lower().endswith(".c_livery")))
        if len(candidates) != 1:
            raise ValueError(f"Found {len(candidates)} livery candidates; pass the exact file path")
        return candidates[0]
    return path


def inflate_payload(data):
    if data.startswith(b"vlrc"):
        return data
    chunks = []
    offset = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("Truncated compression header")
        compressed, expanded = struct.unpack_from("<II", data, offset)
        end = offset + 8 + compressed
        if not compressed or not expanded or end > len(data):
            raise ValueError("Invalid compressed block lengths")
        chunk = zlib.decompress(data[offset + 8:end])
        if len(chunk) != expanded:
            raise ValueError("Uncompressed block length does not match its header")
        chunks.append(chunk)
        offset = end
    payload = b"".join(chunks)
    if not payload.startswith(b"vlrc"):
        raise ValueError("The file does not contain a livery payload")
    return payload


def paint_table(payload):
    if len(payload) < 20 or not payload.startswith(b"vlrc"):
        raise ValueError("Invalid livery header")
    if read_u32(payload, 4) == 2 and payload[26:30] == b"yrvl":
        raise ValueError("Hashed paint tables are not supported; expected positional paint")
    artwork = payload.find(b"gyvl")
    if artwork < 4:
        raise ValueError("Artwork chunk was not found")
    artwork_size = read_u32(payload, artwork - 4)
    stats = artwork + artwork_size
    if artwork_size < 21 or stats + 28 > len(payload):
        raise ValueError("Invalid artwork length or truncated section counters")
    tagged = payload[stats:stats + 4] == b"yrvl"
    start = stats + (36 if tagged else 28)
    if tagged and payload[start - 4:start] != b"yrvl":
        raise ValueError("Paint tag was not found after the section counters")
    end = payload.rfind(b"yrvl", start) if tagged else len(payload)
    for count in RECORD_COUNTS:
        candidate = end - (BLOCK_OVERHEAD + count * RECORD_SIZE)
        if candidate - start not in PREFIX_SIZES:
            continue
        block = payload[candidate:end]
        if len(block) != BLOCK_OVERHEAD + count * RECORD_SIZE:
            continue
        if block[0] > 1 or block[-2] > 1 or block[-1] > 1:
            continue
        records = [block[offset:offset + RECORD_SIZE]
                   for offset in range(1, len(block) - 2, RECORD_SIZE)]
        if all(record[:2] == b"\x01\x01"
               and all(record[offset] in (0, 1) for offset in (2, 7, 12, 17, 22))
               for record in records):
            return block, payload[start:candidate]
    raise ValueError("No validated 20-, 30-, or 34-slot positional paint table was found")


def color_value(data):
    blue, green, red, alpha = data
    value = f"#{red:02X}{green:02X}{blue:02X}"
    return value if alpha == 255 else value + f"{alpha:02X}"


def record_values(record):
    values = []
    manufacturer = read_u32(record, 27)
    finish = read_u32(record, 31)
    enabled = bool(record[2] or record[7] or finish or manufacturer != NO_MANUFACTURER_COLOR)
    if manufacturer != NO_MANUFACTURER_COLOR:
        values.append(f"M={manufacturer}")
    for label, flag, offset in (("P", 2, 3), ("S", 7, 8)):
        color = record[offset:offset + 4]
        if record[flag] or any(color):
            suffix = "" if record[flag] else "(off)"
            values.append(f"{label}={color_value(color)}{suffix}")
    if finish:
        values.append(f"F={finish}")
    scalars = []
    for index, flag in enumerate((12, 17, 22), 1):
        scalar = struct.unpack_from("<f", record, flag + 1)[0]
        if record[flag] or scalar != 0:
            suffix = "" if record[flag] else "(off)"
            scalars.append(f"U{index}={scalar:.6g}{suffix}")
        enabled = enabled or bool(record[flag])
    values.extend(scalars)
    if any(record[35:40]):
        values.append("tail=" + record[35:40].hex())
    return enabled, "; ".join(values) if values else "-"


def print_table(path, payload, block, prefix, active_only):
    rows = []
    for slot, offset in enumerate(range(1, len(block) - 2, RECORD_SIZE)):
        enabled, value = record_values(block[offset:offset + RECORD_SIZE])
        if not active_only or enabled:
            rows.append((str(slot), SLOT_NAMES.get(slot, "Unknown"), "yes" if enabled else "no", value))
    headers = ("Slot", "Name", "Enabled", "Value")
    widths = [max(len(row[column]) for row in [headers, *rows]) for column in range(3)]

    def line(row):
        return "  ".join(row[column].ljust(widths[column]) for column in range(3)) + "  " + row[3]

    print(f"{path.name} | Car {read_u32(payload, 16)} | {(len(block) - BLOCK_OVERHEAD) // RECORD_SIZE} slots")
    print("P/S: primary/secondary; F: finish ID; M: manufacturer selector (overrides P/S).")
    print("U1-U3: scalar overrides; (off): disabled stored value; colors displayed as: #RRGGBB[AA].")
    if prefix or block[-2:] != b"\x00\x01":
        print(f"Paint prefix: {len(prefix)} bytes | Trailer: {block[-2:].hex()}")
    print()
    print(line(headers))
    print(line(tuple("-" * width for width in [*widths, 5])))
    for row in rows:
        print(line(row))


def main():
    parser = argparse.ArgumentParser(description="Outputs livery slots with values")
    parser.add_argument("livery", type=Path, help="Path to livery file or folder with exactly one livery")
    parser.add_argument("--active-only", action="store_true", help="Hide slots with no enabled paint settings")
    options = parser.parse_args()
    try:
        path = resolve_livery(options.livery)
        payload = inflate_payload(path.read_bytes())
        block, prefix = paint_table(payload)
        print_table(path, payload, block, prefix, options.active_only)
        return 0
    except (OSError, ValueError, struct.error, zlib.error) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
