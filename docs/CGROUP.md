# C_group Encoding Reference

This is the current reference for the Forza vinyl `C_group` binary payload and
the structures used by the editor codec.

## Container

`C_group` is a zlib-compressed record with an 8-byte wrapper:

```text
0x00 u32 compressed_length
0x04 u32 uncompressed_length
0x08 zlib payload
```

The decompressed payload begins with:

```text
0x00 4 bytes  magic "gyvl"
0x04 u32      version = 1
0x08 u32      reserved
```

## Header

The internal header contains a root/global transform followed by the root
counted group record:

```text
0x0C byte     root transform marker
0x0D f32      root px
0x11 f32      root py
0x15 f32      root scale
0x19 f32      root rotation
0x1D byte     root group marker, 20 or 60
0x1E u16      root direct child count
0x20 u16      stored root bitmap byte count
0x22 2 bytes  fixed/control area
0x24 bytes    root direct-child bitmap
```

Root bitmap bits are LSB-first:

```text
bit = 1  direct root child is a group
bit = 0  direct root child is a shape
```

The root group marker may be `00` while retaining the same count, control,
bitmap, and layer-data layout.

Layer data begins after the root bitmap:

```text
layer_start = 0x24 + root_child_blocks
```

The stored root bitmap extent is authoritative. It is distinct from the nested
header's derived bitmap length. Children beyond a shortened stored bitmap have
shape type. New roots store the complete bitmap; an explicitly shortened profile
requires the omitted child-type bits to select shapes.

Flat files can contain zero padding before the first shape record.

An identity-bearing root uses the shared attribute descriptor before its child
header:

```text
0x1D byte     root descriptor, 21
0x1E 8 bytes  opaque identity
0x26 bytes    markerless counted root group
```

The markerless group begins with its `u16` child count and otherwise follows the
markerless child-block layout below. Descriptor bit `01` supplies the identity
field; source policy interprets the ownership state separately.

## Physical framing

The shared `vinyl_framing` reader separates physical byte ownership from
scene interpretation. A child block, group attributes, boundary metadata, and
traversal state are distinct units. The complete child tree validates a candidate
frame; scalar or bitmap bytes are interpreted within their enclosing record.

### Child blocks

Nested counted and markerless blocks use:

```text
counted:    20|60  u16 direct_children  u16 bitmap_bytes  control[2]  bitmap[]
markerless:        u16 direct_children  u16 bitmap_bytes  control[2]  bitmap[]
```

Nested bitmap length is `ceil(direct_children / 8)`. Bits are LSB-first;
a set bit selects a group and a clear bit selects a shape. Every physical shape
or nested group consumes one direct-child slot. Attributes and traversal controls
consume none. A block closes when all its direct children have been consumed.
The standalone root uses its separately stored bitmap extent.

Markerless headers require the surrounding child relation and a structurally
valid successor. A preceding traversal byte can shift a candidate header by one
byte; the decoder checks the unshifted header before accepting that candidate.

### Group attributes

Placement contains four little-endian floats:

```text
f32 px  f32 py  f32 scale_x  f32 rotation
```

A descriptor in attribute context contains:

```text
u8 flags
u64 identity       when flags & 01
f32 signed_scale_y when flags & 10
child block
```

The two presence bits are independent of `20`. Identity bytes remain opaque.
The `20`, `40`, and `80` bits retain contextual meaning; framing alone
does not assign mask state. A separate counted block marker can follow the
attributes. The encoder and decoder share these field definitions.

Placement can precede its block or occur inline after a parent header and bitmap.
The first-child type and successor block determine ownership. Existing dialect
rules determine rotation sign, transform composition, and inherited masks.
Bounded partial attribute spans retain their raw bytes; a missing scalar is not
given an invented value or emitted as a newly authored transform.

### Traversal state

Standalone generation 1 terminates transform leads with `01`, generation 2
with `02`, and generation 3 with `03`. Closing `01` controls describe
completed nesting. Their ownership follows the counted tree and dialect.
The bitmap extent is consumed before any following transform lead is read.

Embedded artwork uses its own lead and trailing-state interpretation over the
same child-block and attribute fields. Its boundary records and logical section
mapping are described in [CLIVERY.md](CLIVERY.md).

## Shapes

Shape records are 31 or 32 bytes:

```text
00 02 + shape payload   normal 32-byte record
01 02 + shape payload   context/control-bearing 32-byte record
02    + shape payload   bare 31-byte record
```

Shape payload:

```text
u16 shape_id
f32 rotation
f32 px
f32 py
f32 scale_x
f32 scale_y
f32 skew
u8  b
u8  g
u8  r
u8  a
```

Shape-record recognition validates Y scale and skew together through the
represented affine basis and its relative shear. Placement values and basis
components must remain finite and within the decoder's geometric bounds.

Standalone streams identify their record generation in the root transform
marker. Generations 1 and 2 use `01` for shape records. Generation 1 uses `01`
as its transform terminator, generation 2 uses `02`, and generation 3 uses the
markers shown above. The shared tree walk
selects the record dialect from the root header and decodes the original bytes.
The direct-child bitmap selects whether each following child is a shape or a
group, so marker-like bytes outside that child type are not interpreted as
records. It also resolves ambiguous bytes immediately after a group header:
a shape bit selects a valid shape record, while a group bit permits a
markerless child group without requiring a preceding transform record.

Generations 1 and 2 use odd record and transform leads as traversal framing. Their
parity does not carry trailing mask state. Generation 3 assigns trailing mask
state to the contextual odd leads described below.

PGS export uses generation 1 for FH2, FM5, and FM6, generation 2 for FH3 and FM7,
and generation 3 for FH4, FH5, and FH6. All generations encode group transform
frames and relative shape placements. Both older generations use `01` shape
markers. Generation 1 writes bare shape records; generation 2 can place closing
controls before a shape, producing the framed `01 01` form.
Generation 1 writes a `00` root group marker and uses `01` transform
markers followed by a control byte and markerless group headers. Its standalone
stream has no closing trailer. Generation 2 writes a `20` root marker, uses `02`
transform terminators, and emits `01` closing controls according to group depth.
These are export choices. Import selects the dialect from each stream's root
marker; game saves can retain earlier generations, including generation 1 in FH3.

Built-in vector IDs are canonicalized and validated against the runtime registry
in `assets/vector/shape_names.json`. A complete unsupported livery record can
still contribute structural occupancy to preserve section alignment, but it is
not exposed as an editable shape.

`01 02` is not a universal mask marker. It is context-sensitive and can occur
on ordinary visible nested shapes.

At a shared parent boundary, an odd shape lead carries trailing mask state for the
preceding direct shape sibling. An odd group-transform lead can carry that state
through the immediately preceding terminal-child chain after a group closes.

## Masks

Mask rules:

```text
60 group marker => mask group
mask state inherits through children and remains authoritative
generation 3 odd record state applies to the preceding direct shape
chromatic color data clears ambiguous record-level mask state outside mask groups
```

Chromatic color data means:

```text
B != G or G != R
```

This keeps explicit mask-group ancestry authoritative while rejecting ambiguous
record-level mask state.

## Raster Shape Basis

Raster shape transforms use the actual raster dimensions as model-space size.
The basis is not always normalized to a 128-pixel axis.

```text
model_width  = raster_width
model_height = raster_height
```

Renderers should map sprite pixels around actual half extents:

```text
x_model = x - width / 2
y_model = height / 2 - y
```

## Flattening

Flattening composes:

```text
root/header transform
group transforms
shape local transform
```

Flat export writes one root containing only visible flat shape records. Ordinary
flat shape records use `00 02`; a `01 02` record marks a mask (there is no `60`
group wrapper available in a flat root).

The `01 02` marker is a *trailing* flag: the game masks the shape that
*precedes* an `01 02` record. So a masked layer is encoded by leaving its own
record at `00 02` and stamping `01 02` onto the next record. Flat export emits
`01 02` for a record when the previous visible layer carries the editor mask
flag.

## Forza Motorsport Record Dialect

Forza Motorsport (2023+) standalone groups use an uncompressed `gyvl` data
file with the same scene model and companion-header arrangement. The root
transform starts at `0x0c` and accepts generation markers `01`, `02`, and `03`.
An optional signed Y-scale field follows when descriptor bit `10` is set.
The remaining descriptor state is interpreted by the Motorsport profile.

The root is normally a counted group:

```text
20/60 + u16 child_count + u16 child_blocks + control[2] + bitmap[child_blocks]
```

The Motorsport markerless root replaces the group marker with `00`. For both forms,
the stored root bitmap extent places the layer stream at
`0x24 + child_blocks`. Structurally valid records determine the boundary when
additional transform or control data appears before the first child.

The dialect accepts 32-byte framed shapes and 31-byte bare shapes:

```text
00 02 + shape payload
01 02 + shape payload

01/02/03 + shape payload
```

The payload fields match the shape layout above. Shape identifiers are
canonicalized and checked against the registry before a candidate is accepted.

Separate group transforms use a generation marker followed by position,
uniform scale, and rotation. The optional signed Y-scale extension follows the
same independent attribute fields as the root. The successor child block
validates attribute framing.

Nested counted and markerless groups use the same child-count and bitmap model.
Flat mask state is trailing: `01 02` on a shape marks the preceding shape, and a
final `01` trailer marks the last shape. Group masks continue to inherit through
descendants.

Both dialects enter `VinylTreeDecoder::decodeGroup()`. The Motorsport profile
normalizes its record markers, root header, glyph identifiers, signed scale,
and trailing mask state inside that shared pipeline before the decoded tree is
converted to scene layers.
