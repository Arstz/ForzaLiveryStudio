# Forza Horizon Header File Format

This document describes the binary structure of the `header` file in Forza
Horizon vinyl groups. Unknown fields are documented by layout and observed
behavior only; their semantic meaning is still inferred.

## File Overview

The `header` file stores metadata for a vinyl group: name, creator, timestamp,
description when published, and a unique GUID.

Two structural variants exist:

| Variant | Size Range | Description |
|---------|------------|-------------|
| **Draft** | ~145 bytes | Simple structure with null padding and trailing metadata |
| **Published** | ~133 bytes | Includes description field, published flag, duplicate GUID |

## Section 1: File Preamble

Fixed header that identifies the file format and contains the vinyl group name.

```text
Offset  Size  Field              Example
------  ----  -----------------  ---------------------------
0x00    4     Format Version     07 00 00 00  (always 7)
0x04    4     Name Length        UTF-16LE character count
0x08    N*2   Vinyl Group Name   UTF-16LE string
```

After the name string, the next field depends on whether the vinyl is published.

### Draft

```text
+0x00  4  Null Padding  00 00 00 00
```

### Published

```text
+0x00  4    Description Length  UTF-16LE character count
+0x04  N*2  Description Text    UTF-16LE string
```

## Section 2: Metadata Block

This section follows immediately after the vinyl name preamble. Its absolute
offset varies based on name length and description presence.

### Timestamp Group

```text
Offset  Size  Field     Notes
------  ----  ------    ---------------------------
+0x00   2     Year      Little-endian uint16
+0x02   1     Month
+0x03   1     Day       Usually zero in observed files
+0x04   2     Field A   Usually 2
+0x06   2     Field B   Usually 2
```

### Variable Fields

```text
Offset  Size  Field       Notes
------  ----  ---------   ---------------------------
+0x08   2     field_x     Varies per vinyl group
+0x0A   2     field_y     Varies per vinyl group
+0x0C   2     field_z     Varies per vinyl group
+0x0E   2     field_w_lo  Lower uint16, varies
+0x10   2     field_w_hi  Usually 2
+0x12   2     pad         Usually zeros
```

The exact meaning of these values is unknown. They may encode group category,
layer or shape counts, or validation/hash data.

### Creator Identity Block

```text
Offset  Size  Field            Notes
------  ----  ---------------  ---------------------------
+0x14   4     creator_tag1     Opaque creator/account-derived bytes
+0x18   2     creator_tag2     Opaque creator/account-derived bytes
+0x1A   2     sep_0900         Constant separator
+0x1C   4     creator_len      Name length in UTF-16LE characters
+0x20   N*2   creator_name     UTF-16LE string
```

The creator tags are stable across files from the same creator, suggesting they
are derived from account identity rather than vinyl content.

## Section 3: After Creator Name

### Draft

```text
Offset          Content                           Size
------          -------                           ----
after creator   00 00 00 00 ...                   28 bytes zero padding
                01 02 00 00 00 00 00 00 00 XX     13 bytes section header
                XX 00 00 00                       type value bytes
                16-byte GUID                      Unique identifier
                24-byte trailing data             Metadata (6 x uint32)
```

### Published

```text
Offset          Content                           Size
------          -------                           ----
after creator   01 00 00 00                       4 bytes published flag
                00 00 00 00 00 00 00 00           8 bytes zero padding
                16-byte GUID                      First copy, no section header
                01 02 00 00 00 00 00 00 00 02     13 bytes section header
                00 00 00                          section header tail
                16-byte GUID                      Second copy, same GUID
```

### Published Flag

`01 00 00 00` (`uint32 = 1`) appears immediately after the creator name in
published files. Draft files have 28 zero bytes in this location instead.

### Section 3 Header

```text
Offset  Size  Content
------  ----  -------------------
0x00    2     01 02  (marker)
0x02    7     00 00 00 00 00 00 00
0x09    1     Type byte
0x0A    3     00 00 00
```

The type byte varies per vinyl group. Its meaning is unknown.

### GUID

A unique 128-bit identifier formatted as a standard UUID. In published files the
GUID appears twice: once without a section header, and once inside the section 3
block. Both copies are identical.

### Trailing Data

Draft files contain 24 bytes after the GUID, typically interpreted as six
little-endian `uint32` values. This trailing data is absent in published files,
where it is replaced by the duplicate GUID structure.

## Keywords

Keywords such as category tags are not stored in local files (`header`,
`C_group`, or `thumb.webp`). They appear to exist only in external metadata
associated with the vinyl group's GUID.

## Summary Byte Layout

### Draft

```text
0x00  [FormatVer=7][NameLen][VinylName UTF-16LE...]
      [00 00 00 00]
      [year][month][day]
      [02 00][02 00]
      [field_x][field_y][field_z]
      [field_w_lo][02 00]
      [00 00]
      [creator_tag1 4B][creator_tag2 2B]
      [09 00]
      [creator_len][creator_name UTF-16LE...]
      [00 * 28]
      [01 02][00*7][type][00*3]
      [GUID 16B]
      [trailing 24B]
```

### Published

```text
0x00  [FormatVer=7][NameLen][VinylName UTF-16LE...]
      [desc_len][desc_text UTF-16LE...]
      [year][month][day]
      [02 00][02 00]
      [field_x][field_y][field_z]
      [field_w_lo][02 00]
      [00 00]
      [creator_tag1 4B][creator_tag2 2B]
      [09 00]
      [creator_len][creator_name UTF-16LE...]
      [01 00 00 00]
      [00 * 8]
      [GUID 16B]
      [01 02][00*7][02][00*3]
      [GUID 16B]
```
