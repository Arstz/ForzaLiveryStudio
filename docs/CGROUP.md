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
0x20 u8       root child block count = ceil(child_count / 8)
0x21 3 bytes  fixed/control area
0x24 bytes    root direct-child bitmap
```

Root bitmap bits are LSB-first:

```text
bit = 1  direct root child is a group
bit = 0  direct root child is a shape
```

Layer data begins after the root bitmap:

```text
layer_start = 0x24 + root_child_blocks
```

Flat files can contain zero padding before the first shape record.

## Transforms

Transform payload:

```text
marker + f32 px + f32 py + f32 scale_x + f32 rotation
```

Optional signed Y scale extension:

```text
30 + f32 scale_y
```

If the `30` extension is absent, `scale_y` is equal to `scale_x`.

Known transform marker forms:

```text
03
00 03
01 03
00 01 03
00 01 01 03
00 01 01 01 03
00 + repeated 01 bytes + 03
03 03
df 03 03
```

An `odd_byte 03` sequence at the end of a group header is parsed as:

```text
final bitmap byte
03 transform marker
```

Header lengths determine that ownership before transform parsing begins.

## Groups

Counted group record:

```text
20/60 + u16 child_count + u16 child_blocks + reserved[2] + bitmap[ceil(child_count / 8)]
```

`child_blocks` is:

```text
ceil(child_count / 8)
```

The child bitmap contains one type bit per declared direct child. Its stored block
count must match the bitmap length derived from `child_count`.

`20` opens a normal group. `60` opens a mask group.

Groups and shapes consume one child slot of the current group. Transform and
control records do not consume child slots.

The decoder closes a group as soon as its declared direct child count is
satisfied.

## Markerless Groups

After a pending transform, a counted group can omit the `20/60` marker:

```text
u16 child_count + u16 child_blocks + reserved[2] + bitmap[ceil(child_count / 8)]
```

This markerless form is accepted only after a pending transform and only when
the following bytes continue with a valid child record or an inline transform
for the first child.

Because the markerless form is ambiguous inside shape data, it is accepted only
when the declared bitmap and following child record are structurally valid.

## Inline First-Child Transforms

Group records can carry a transform after their reserved bytes and child bitmap.
If the next token is another group, the transform belongs to the first child rather
than the current group.

First-child inline transform markers include:

```text
03
00 03
01 03
03 03
df 03 03
```

This rule applies to both counted and markerless groups. Marker recognition starts
after the complete group header and child bitmap.

## Embedded Livery Section Walk

`C_livery` embeds a version-0 `gyvl` stream whose body is split into the fixed
section slots by the trailing `yrvl` stats counts. Its transform dialect differs
from standalone `C_group`: a separate group transform is usually a single lead
byte plus the 16-byte transform payload, and the `00` family can retain a
`00 01` prefix before that payload.

The following group can start immediately, after one control byte, or after the
framed 9-byte livery trailer `21 ?? ?? ?? ?? ?? ?? 09 00`. The trailer is consumed
as one opaque unit. It is not a free-form scan allowance: only successor offsets
0, 1, and the exact 9-byte frame are accepted, each requiring a structurally valid
group at the resulting position. See [`CLIVERY.md`](CLIVERY.md) for the full
transform-dialect and section-boundary rules.

The section walker retains the ancestry produced by counted records and bounded
successor framing. It does not move sibling records into inferred containers after
decoding.

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
An optional signed Y-scale extension follows as `30 + f32` or `70 + f32`, with
the high form carrying mask state.

The root is normally a counted group:

```text
20/60 + u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

The Motorsport markerless root replaces the group marker with `00`. For both forms,
`child_blocks = ceil(child_count / 8)` and the layer stream starts at
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
same layout as the root. Marker recognition also requires a structurally valid
group after the transform so shape markers remain unambiguous.

Nested counted and markerless groups use the same child-count and bitmap model.
Flat mask state is trailing: `01 02` on a shape marks the preceding shape, and a
final `01` trailer marks the last shape. Group masks continue to inherit through
descendants.

Both dialects enter `VinylTreeDecoder::decodeGroup()`. The Motorsport profile
normalizes its record markers, root header, glyph identifiers, signed scale,
and trailing mask state inside that shared pipeline before the decoded tree is
converted to scene layers.
