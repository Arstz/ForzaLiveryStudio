# Forza Motorsport Header File Format

This document describes the binary structure of the `header` file in Forza
Motorsport (2023+). It covers the draft variant observed on an unshared livery.
Unknown fields are documented by layout and observed behavior only.

**Relationship to FH6:** FM2023 header format version is 9 (FH6 uses 7). The
general structure (preamble, metadata, creator block, GUID) is shared, but
field positions, sizes, and the trailing section differ.

---

## File Overview

| Variant | Size | Description |
|---------|------|-------------|
| **Draft** | ~233 bytes | Observed: no description, year/month, creator block, GUID |

Only the draft variant has been observed. Published format may differ.

---

## Section 1: File Preamble

```text
Offset  Size  Field              Example
------  ----  -----------------  ---------------------------
0x00    4     Format Version     09 00 00 00  (v9; FH6 uses v7)
0x04    4     Name Length        UTF-16LE character count
0x08    N*2   Vinyl Group Name   UTF-16LE string
```

After the name string:

### Draft (observed)

```text
+0x00   4   Null Padding          00 00 00 00
```

Unlike FH6 where the padding is immediately after the name, in FM2023 there is
a 4-byte gap (`00 00 00 00`) before the metadata block.

---

## Section 2: Metadata Block

Absolute offset varies by name length. For a 55-character name, offset is
0x7A.

### Timestamp Group

```text
Offset  Size  Field     Notes
------  ----  ------    ---------------------------
+0x00   2     Year      Little-endian uint16  (observed: 2025)
+0x02   1     Month     (observed: 3)
+0x03   1     Day       Usually zero in observed files
+0x04   2     field_A   (observed: 0)
+0x06   2     field_B   (observed: 21)
```

### Variable Fields

```text
Offset  Size  Field       Notes
------  ----  ---------   ---------------------------
+0x08   2     field_x     (observed: 12)
+0x0A   2     field_y     (observed: 33)
+0x0C   2     field_z     (observed: 4)
+0x0E   2     field_w_lo  Lower uint16 (observed: 955)
+0x10   2     field_w_hi  (observed: 3)
+0x12   2     pad         Usually zeros

+0x14  total: 20 bytes in this block
```

The exact meaning of these values is unknown. They may encode layer or shape
counts, or validation/hash data.

---

## Section 3: Creator Identity Block

```text
Offset  Size  Field            Notes
------  ----  ---------------  ---------------------------
+0x00   4     creator_tag1     Opaque creator/account-derived bytes
+0x04   2     creator_tag2     Opaque creator/account-derived bytes
+0x06   2     sep_0900         Constant separator 09 00
+0x08   4     creator_len      Name length in UTF-16LE characters
+0x0C   N*2   creator_name     UTF-16LE string
```

## Section 4: Trailing Data (Draft)

After the creator name, the draft format contains:

```text
+0x00   12   Zero padding          00 * 12
+0x0C   16   GUID (UUID v4/v5)     Unique identifier
+0x1C   2    01 02                 Marker
+0x1E   8    ff ff ff ff ff ff ff ff   / 0xFF padding
+0x26   4    eb 08 00 00           u32 = 2283 (total decal count)
+0x2A   4    bb 0e 00 00           u32 = 3771 (livery share id)
+0x2E   16   GUID (duplicate)      Same GUID as first copy
```

The GUID appears twice — once after zero padding, and once after the marker
block. The two `u32` values following the first GUID copy repeat the total
decal count and share ID found in the `C_livery` payload.

---

## Summary Byte Layout

### Draft (observed)

```text
0x00  [FormatVer=9][NameLen][VinylName UTF-16LE...]
      [00 00 00 00]
      [year][month][day]
      [field_A][field_B]
      [field_x][field_y][field_z]
      [field_w_lo][field_w_hi]
      [pad 00 00]
      [creator_tag1 4B][creator_tag2 2B]
      [09 00]
      [creator_len][creator_name UTF-16LE...]
      [00 * 12]
      [GUID 16B]
      [01 02][ff * 8]
      [total_decal_count u32]
      [share_id u32]
      [GUID 16B]
```

Total for the observed livery with 55-char name: **233 bytes**.

---

## Keywords

Keywords such as category tags are not stored in local files (`header`,
`C_group`, or `C_livery`). They appear to exist only in external metadata
associated with the vinyl group's GUID.
