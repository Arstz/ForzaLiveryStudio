# C_group Encoding Reference

This is the current concise reference for the Forza vinyl `C_group` binary
payload. It describes the active decoder model and separates confirmed
structures from areas that are still treated conservatively.

## Container

`C_group` is a zlib-compressed record with an 8-byte wrapper:

```text
0x00 u32 compressed_length
0x04 u32 uncompressed_length
0x08 zlib payload
```

The decompressed payload begins with:

```text
0x00 4 bytes  magic, usually "gyvl"
0x04 u32      version, observed as 1
0x08 u32      unknown
```

## Header

The internal header contains a root/global transform followed by the root
counted group record:

```text
0x0C byte     root transform marker, usually 03
0x0D f32      root px
0x11 f32      root py
0x15 f32      root scale
0x19 f32      root rotation
0x1D byte     root group marker, usually 20 or 60
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

If the `30` extension is absent, `scale_y` is usually equal to `scale_x`.
Parser-created containers introduced by bare `01 03` can use `abs(scale_x)` for
`scale_y`.

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
odd_byte 03
```

The `odd_byte 03` form is best understood as:

```text
control/bitmap byte
03 transform marker
```

The decoder currently recognizes it as one marker so the transform is attached
to the correct child. Observed odd bytes include values such as `07`, `0b`,
`0d`, `15`, `1f`, `2f`, and `3f`.

## Groups

Counted group record:

```text
20/60 + u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

`child_blocks` is:

```text
ceil(child_count / 8)
```

In embedded livery `gyvl` payloads, records whose bitmap is longer than one
byte can store only the low byte of that value in the `child_blocks` field. The
decoder expands this to the full `ceil(child_count / 8)` length only when the
surrounding bytes are structurally plausible and the recovered bitmap is sparse
enough to distinguish it from shape payload bytes.

`20` opens a normal group. `60` opens a mask group.

Groups and shapes consume one child slot of the current group. Transform and
control records do not consume child slots.

The decoder closes a group as soon as its declared direct child count is
satisfied. Root bitmap constraints can force a standalone root shape into the
previous root group when the root bitmap says the next root item must be a
group.

## Markerless Groups

After a pending transform, a counted group can omit the `20/60` marker:

```text
u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

This markerless form is accepted only after a pending transform and only when
the following bytes continue with a valid child record or an inline transform
for the first child.

Markerless groups can also carry a single extra control byte before their first
child.

The same wrapped-bitmap rule used for counted groups applies to markerless
groups in embedded livery payloads. Because the markerless form is ambiguous
inside shape data, wrapped markerless groups are accepted only with the sparse
bitmap and following-child guards.

## Inline First-Child Transforms

Some group records carry a transform after their control bytes. If the next
token is another group, the transform belongs to the first child rather than the
current group.

First-child inline transform markers include:

```text
03
00 03
01 03
03 03
df 03 03
odd_byte 03
```

This rule applies to both counted and markerless groups.

## Embedded Livery Section Walk

`C_livery` embeds a version-0 `gyvl` stream whose body is split into the fixed
section slots by the trailing `yrvl` stats counts. Its transform dialect differs
from standalone `C_group`: a separate group transform is usually a single lead
byte plus the 16-byte transform payload, and the `00` family can retain a
`00 01` prefix before that payload.

Large section-start identity containers can be followed by more transform-led
records that still belong to the same decoded section container. When a large
section reaches its stats count and the first at-section-start identity group
captured only a minority of the section leaves, the decoder marks that group as
`livery_section_span` and moves the remaining section-top records under it. The
importer preserves that marked wrapper instead of flattening it into the section
folder.

Some embedded livery sections contain a second-level span with a tiny
`ff`/`3f` markerless group at the start of a much longer sibling run. After the
section-span wrapper is recovered, the decoder can mark that child as
`livery_nested_span` and absorb following siblings until the next structurally
plausible sibling run begins. Current boundary guards require the anchor to be
small, the parent run to be large, and the stop point to be either a counted
group or a markerless group followed by a loose run of direct shapes.

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

`01 02` is not a universal mask marker. It is context-sensitive and can occur
on ordinary visible nested shapes.

## Masks

Confirmed mask rules:

```text
60 group marker => mask group
mask state inherits through children
chromatic color data clears decoded mask state
```

Chromatic color data means:

```text
B != G or G != R
```

This override handles corrupted mask flags from extracted grouped files.

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

Some decoded root children can be marked root-transform-exempt when their local
bytes are already root-space.

Flat export writes one root containing only visible flat shape records. Ordinary
flat shape records use `00 02`; a `01 02` record marks a mask (there is no `60`
group wrapper available in a flat root).

The `01 02` marker is a *trailing* flag: the game masks the shape that
*precedes* an `01 02` record. So a masked layer is encoded by leaving its own
record at `00 02` and stamping `01 02` onto the next record. Flat export emits
`01 02` for a record when the previous visible layer carries the editor mask
flag.

## Export Status

Flat export is the stable export path.

Grouped/nested export is experimental. The decoder can recover many nested
trees, but canonical nested serialization still depends on control/bitmap byte
ownership that is not fully formalized for every file.
