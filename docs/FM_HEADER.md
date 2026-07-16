# Forza Motorsport Header File Format

This document describes the draft Forza Motorsport (2023+) `header` file. The
general preamble, creator block, and GUID model are shared with the current
ForzaTech header, while field positions and trailing data differ. Published
headers may use another trailing layout.

## Preamble

```text
0x00  u32      format version
0x04  u32      name length in UTF-16 code units
0x08  ...      name in UTF-16LE
       4 bytes reserved
```

FM headers use format version 9. Offsets after the name are variable.

## Metadata Block

The metadata block follows the reserved field:

```text
+0x00  u16  year
+0x02  u8   month
+0x03  u8   day
+0x04  u16  field_a
+0x06  u16  field_b
+0x08  u16  field_x
+0x0a  u16  field_y
+0x0c  u16  field_z
+0x0e  u16  field_w_low
+0x10  u16  field_w_high
+0x12  u16  reserved
```

The opaque fields are preserved by the editor.

## Creator Identity

```text
+0x00  4 bytes  creator tag part 1
+0x04  2 bytes  creator tag part 2
+0x06  2 bytes  separator
+0x08  u32      creator-name length in UTF-16 code units
+0x0c  ...      creator name in UTF-16LE
```

## Draft Trailing Data

The draft trailing block follows the creator name:

```text
+0x00  12 bytes  reserved
+0x0c  16 bytes  asset GUID
+0x1c  2 bytes   marker
+0x1e  8 bytes   padding
+0x26  u32       logical decal count
+0x2a  u32       target car id
+0x2e  16 bytes  repeated asset GUID
```

The target car id is the numeric registry key stored in the livery `vlrc`
header. Both GUID copies identify the same asset.

## Layout Summary

```text
[version]
[name length][name UTF-16LE]
[reserved]
[date and opaque metadata]
[creator tags][separator]
[creator-name length][creator name UTF-16LE]
[reserved]
[GUID]
[marker][padding]
[logical decal count]
[target car id]
[GUID]
```

Keywords and category tags are not part of the local header stream.
