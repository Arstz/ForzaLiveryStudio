# Forza Motorsport C_livery Encoding Reference

This document describes the Forza Motorsport (2023+) `C_livery` data stream.
It covers the compressed block sequence, tagged payload, section metadata, and
the embedded artwork dialect. Standalone FM groups are documented in
[`FM_CGROUP.md`](FM_CGROUP.md); the current ForzaTech group format is documented
in [`CGROUP.md`](CGROUP.md).

## Compressed Block Sequence

The `data` file contains one logical byte stream split into consecutive zlib
blocks. Each block has this wrapper:

```text
+0x00  u32  compressed_size
+0x04  u32  decompressed_size
+0x08  ...  zlib payload
```

The next block begins immediately after `8 + compressed_size` bytes. Decoding
inflates every block and concatenates the decompressed bytes in file order.
Block boundaries do not divide semantic containers and may occur inside
artwork or metadata records.

## Inflated Stream

The concatenated stream uses the shared tagged chunk model:

```text
vlrc  root metadata
yrvl  livery information
gyvl  embedded artwork
yrvl  seven section counters
yrvl  section descriptor data
yrvl  stream terminator
```

Chunk offsets are variable. Tags must be located in the complete concatenated
stream, not within individual compressed blocks.

## vlrc Root Header

```text
0x00  4 bytes  "vlrc"
0x04  u32      version
0x08  u32      reserved
0x0c  u32      reserved
0x10  u32      target car id
0x14  u32      category or state field
0x18  u16      reserved
```

The target car id uses the same numeric registry key as the editor's car
registry and livery project metadata.

## gyvl Artwork

The artwork chunk begins with this shortened group header:

```text
0x00  4 bytes  "gyvl"
0x04  u32      version
0x08  u32      reserved
0x0c  f32      root scale
0x10  u32      reserved
0x14  u8       control
0x15  ...      section stream
```

The artwork body ends at the first `yrvl` tag after `gyvl`. It contains seven
logical livery section slots in this order:

```text
0 Front
1 Back
2 Top
3 Left
4 Right
5 Spoiler
6 FrontWindshield
```

Populated sections contain group and shape records followed by a section
transform remnant. Empty section scaffolds are 23 bytes. Terminal scaffolds
may be omitted from the physical stream; the decoder supplies temporary tail
padding while walking sections and does not add it to the preserved source
payload.

## Shape Records

Embedded FM artwork can use current framed records and older framed or bare
records:

```text
00 02 + shape payload
01 02 + shape payload
00 01 + shape payload
01 01 + shape payload
01/02/03 + shape payload
```

Framed records occupy 32 bytes and bare records occupy 31 bytes. The payload
layout is shared with the standalone FM group format:

```text
u16 shape_id
f32 rotation
f32 position_x
f32 position_y
f32 scale_x
f32 scale_y
f32 skew
u8  blue
u8  green
u8  red
u8  alpha
```

The FM decoder validates vector identifiers against the shape registry,
recognizes uploaded-logo identifiers, canonicalizes legacy glyph identifiers,
and normalizes older markers before invoking the shared livery tree walker.

## Group and Transform Records

Groups use the same child-count and child-bitmap model as FM standalone groups.
Embedded transforms use the livery marker dialect:

```text
00-family   00 [01]* 03   -> normalized 00
odd-family  <odd> 03       -> normalized <odd> 01
```

An optional signed Y scale follows a transform:

```text
30 + f32 scale_y
70 + f32 scale_y
```

Section walking uses the seven counters together with structurally valid group,
shape, logo, transform, remnant, and child-bitmap records.

## Section Counters and Descriptors

The first `yrvl` after the artwork contains seven little-endian counters:

```text
"yrvl" + 7 * u32
```

The counters describe logical decal occupancy and may include uploaded-logo
weighting. They are section-walking metadata rather than a byte-length table.
The following `yrvl` contains descriptor data. The stream ends with an eight-byte
`yrvl` terminator.

## Decoder Path

`readFM2023LiveryPayload()` inflates and concatenates the block sequence, locates
the chunks, extracts the car id, artwork body, and section counters, and preserves
the complete inflated source. `decodeFM2023LiverySections()` normalizes the FM
record dialect and builds the seven section trees. `importFM2023Asset()` converts
those trees into the editor scene.

## Validation

The optional comparison helper reports the physical and logical structure of an
FM asset without GUI inspection:

```powershell
.\build\Release\fh6_livery_compare.exe --fm-stats <assetFolder>
```

For liveries it prints section counters, decoded shapes, uploaded logos, skipped
records, groups, and incomplete declared groups.
