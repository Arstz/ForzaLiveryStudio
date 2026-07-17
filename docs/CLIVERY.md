# C_livery Encoding Reference

`C_livery` stores livery metadata, grouped artwork, section counters, material
paint state, and graphic descriptors in one compressed container. The artwork
uses the group grammar documented in [CGROUP.md](CGROUP.md) with livery framing.

## Container

The Horizon container uses the same compressed wrapper as `C_group`:

```text
u32       compressed length
u32       uncompressed length
byte[]    zlib payload
```

The decompressed stream is a tagged sequence:

```text
vlrc      root metadata
yrvl      livery information
gyvl      artwork
yrvl      section counters
yrvl      descriptor table
yrvl      terminator
```

Tags are stored as byte strings in the payload. The `gyvl` body length is stored
in the preceding livery-information record.

## Root Metadata

The `vlrc` record begins with:

```text
0x00  byte[4]  "vlrc"
0x04  u32      version
0x08  u32      source state
0x0c  u32      reserved
0x10  u32      target car identifier
0x14  u32      category state
0x18  u16      reserved
```

The editor reads the target car identifier and preserves the remaining source
metadata during source-backed encoding.

## Artwork Header

The embedded `gyvl` chunk uses this header:

```text
0x00  byte[4]  "gyvl"
0x04  u32      version
0x08  u32      reserved
0x0c  f32      root scale
0x10  u32      reserved
0x14  u8       control
0x15  byte[]   section stream
```

Unlike standalone `C_group`, the artwork body is a positional section stream
rather than a counted root group.

## Groups and Transforms

Counted and markerless groups store the direct-child count and child-bitmap block
count as little-endian fields, followed by two reserved bytes and the bitmap:

```text
counted:    20|60  u16 count  u16 bitmap_blocks  reserved[2]  bitmap[]
markerless:        u16 count  u16 bitmap_blocks  reserved[2]  bitmap[]
```

The bitmap contains one child-type bit per declared direct child. A `60` group
supplies inherited mask state to its descendants.

Livery transform markers terminate in `01`. The transform payload contains
position, uniform scale, and rotation, followed by an optional signed Y-scale:

```text
lead + f32 px + f32 py + f32 scale_x + f32 rotation
30 + f32 scale_y
70 + f32 scale_y
```

The high suffix form carries group mask state. Separate transforms are accepted
when a structurally valid successor group follows directly, through one control
byte, or through the framed livery trailer. Markerless section wrappers can also
carry the first-child transform directly after their bitmap.

Record-level mask state is trailing. It resolves against the preceding direct
shape or terminal group child, while inherited mask-group state remains
authoritative.

## Sections

The Horizon artwork stream contains eleven ordered section slots:

```text
Front
Back
Top
Left
Right
Spoiler
FrontWindshield
BackWindshield
TopWindow
LeftWindow
RightWindow
```

Section membership is positional and is represented in the editor by a top-level
group carrying `isLiverySection` and `liverySectionSlot`.

Export composes group transforms into direct world-space shape records for ordinary
sections. A section containing masked shapes retains structured records so group
mask ancestry and trailing shape-mask state remain expressible.

An empty slot uses a 23-byte scaffold. A populated slot contains its section root,
grouped artwork, and an 18-byte remnant. Section counters describe logical decal
occupancy rather than byte length. The decoder reserves the remaining section
footprint while walking each slot so record parsing stays within its boundary.

## Shape Records

Artwork shapes use the shape payload defined in [CGROUP.md](CGROUP.md). Built-in
vector identifiers are canonicalized and validated against the runtime registry.
Unsupported records can retain structural occupancy without becoming editable
scene layers.

A shape identifier with its high bit set represents a raster logo. The remaining
bits identify the raster descriptor. The section counter can assign a logical
weight greater than one to a physical logo placement; the decoder derives that
weight from the section metadata before building the final scene.

## Livery Information

The `yrvl` record preceding the artwork stores creator metadata, state, and the
`gyvl` body length. The length must match the emitted artwork chunk.

The section-counter record following the artwork stores eleven section counts and
one trailing counter. The decoder uses the section counts to bound and validate
the artwork walk. The encoder rebuilds these counts from the exported section
contents.

If trailing mask state belongs to the final shape in a section, a standalone `01`
marker follows the completed section walk. The decoder applies it to that terminal
shape before advancing to the section remnant.

## Descriptor Table

The descriptor chunk contains paint bindings, panel records, and raster graphic
references. Its structurally decoded record table has this layout:

```text
u8        table mode
u8        record type
u16       record count
byte[6]   reserved
byte[27]  records[record count]
u32       panel count
byte[8]   panel identifiers[panel count]
```

A paint record uses the 27-byte envelope:

```text
byte[8]   material identifier
u8        value type
u8        primary color enabled
byte[4]   primary BGRA color
u8        secondary color enabled
byte[4]   secondary BGRA color
u32       manufacturer color selector
u32       finish code
```

The editor matches paint records to decoded car materials by identifier. Enabled
colors and supported finish codes affect the 3D preview. Manufacturer selectors
and descriptor records without an editor representation remain opaque source
metadata.

The stream ends with:

```text
"yrvl" 00 00 00 00
```

## Forza Motorsport Dialect

Forza Motorsport stores the tagged stream in consecutive compressed blocks.
Decoding inflates every block and concatenates the results before locating tags.

Its artwork uses seven positional section slots and accepts both framed and bare
shape records. The shared `VinylTreeDecoder` applies the Motorsport record profile
for marker normalization, identifier canonicalization, root framing, section
padding, and post-decode mask state while retaining the same group and section
walkers.

`readFM2023LiveryPayload()` reconstructs the tagged stream and extracts its
artwork and counters. `decodeFM2023LiverySections()` builds the section trees, and
`importFM2023Asset()` converts them to the editor scene.
