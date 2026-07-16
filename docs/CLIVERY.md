# C_livery Encoding Reference

Concise reference for the Forza `C_livery` binary payload. It describes the chunk
container and how the embedded artwork relates to the [`C_group`](CGROUP.md)
format: a `C_livery` wraps one embedded group-derived artwork stream (a `gyvl`
chunk) with livery-specific root, section, and transform framing.

## Container

Identical wrapper to `C_group`: an 8-byte header followed by a single zlib stream.

```text
0x00 u32 compressed_length      (= filesize - 8)
0x04 u32 uncompressed_length
0x08 zlib payload (78 9c ...)
```

## Top-Level: a FourCC Chunk Container

Unlike `C_group` (a single `gyvl` blob), the decompressed `C_livery` is a tagged
chunk container. Tags are stored byte-reversed (little-endian FourCC):

```text
bytes "vlrc"   root / file record
bytes "yrvl"   livery-level records
bytes "gyvl"   vinyl group = one embedded C_group (the artwork)
```

Chunk sequence, always in this order:

```text
order  tag @off     size            role
  1    vlrc 0x00    26 B  (fixed)    root / file header (metadata)
  2    yrvl 0x1a    25 B  (fixed)    livery-info record (GUID + flag)
  3    gyvl 0x33    variable, BULK   the artwork (embedded C_group)
  4    yrvl ...     52 B             per-section decal-count stats
  5    yrvl ...     variable         layer/section descriptor table
  6    yrvl ...     8 B              terminator (yrvl 00 00 00 00)
```

There is exactly one `vlrc` and one `gyvl`. `gyvl` is always at 0x33, the first
`yrvl` at 0x1a. The `u32` immediately before the `gyvl` magic is the gyvl body
length. Other 4-letter ASCII runs found by a naive scan are float data, not tags.

## vlrc — Root Header

```text
0x00 4 bytes  tag "vlrc"
0x04 u32      version = 2
0x08 u32      source flag. 0 for a locally-authored livery; 1 on a subset of
              downloaded/shared liveries (all with a foreign, non-null creator id).
              For a from-scratch save: 0.
0x0c u32      0  (constant)
0x10 u32      car id — which car the livery is for (decimal). Matches the u32 in
              the car's .carbin and assets/cars/car_ids.json.
0x14 u32      category/type field. Small enum-or-bitmask, observed {0,1,2,4,6}
              (looks like bits 0x01|0x02|0x04). Use 0 for from-scratch output.
0x18 u16      0  (constant)
```

Every field here except the car id is either a fixed constant or safely 0 for a
newly-authored livery.

## gyvl — Embedded C_group Header

The `gyvl` chunk is a `C_group` (decode its body with the
[`C_group`](CGROUP.md) grammar) but with a shorter header dialect:

```text
rel 0x00  4   magic "gyvl"
rel 0x04  u32 version = 0        (standalone C_group uses 1)
rel 0x08  u32 0
rel 0x0c  f32 1.0               (root scale; livery root is always identity)
rel 0x10  u32 0
rel 0x14  u8  0
rel 0x15  ...                   body starts here
```

There is no root counted-group marker (`20`/`60`) and no root child bitmap at the
standalone offsets. The body at rel 0x15 is a flat positional stream of the 11
section slots (see Sections), not a counted root.

## Group Headers and Transform Framing

Counted and markerless groups own three control bytes followed by a variable-size
child bitmap. The bitmap length is derived from the child count, so every bitmap
byte is consumed before looking for a transform marker:

```text
counted:    20|60  u16 count  u8 bitmap_blocks  control[3]  bitmap[ceil(count/8)]
markerless:        u16 count  u8 bitmap_blocks  control[3]  bitmap[ceil(count/8)]
```

For bitmaps larger than the byte-sized block field, the field stores the wrapped
low byte while the decoder derives the full length from the count. The detailed
meaning of the complete structural bitmap remains open; it is preserved even when
it is not used to choose a decoded child type.

Transform framing begins after the complete header. Standalone `C_group` markers
terminate in `03`; their livery equivalents terminate in `01`. An odd final bitmap
byte immediately followed by `01` is therefore a bitmap byte followed by a
one-byte livery transform marker.

The 16-byte transform payload stores `px`, `py`, `sx`, and the wire rotation. A
separate transform led by `01` uses the opposite editor rotation sign when its
normal non-mirrored successor begins with a scaled first-child frame. Top-section,
unit-scale container, mask, and mirrored transforms retain the wire sign. A `00`
lead retains the wire sign. The optional signed-Y suffix is:

```text
30 + f32 scale_y      normal group
70 + f32 scale_y      mask group   (70 = 30 | 40, the mask bit)
```

Recognize both suffixes with `(byte & ~0x40) == 0x30`.

A counted `60` group establishes mask state for every descendant shape. Shape
color bytes do not override that inherited group state; chromatic color can only
reject ambiguous mask state attached directly to an otherwise normal record.

Record-level mask state is trailing. When an odd shape lead follows a direct shape
sibling, its low bit marks that preceding shape. An odd group-transform lead can
carry the same state after a group closes; it resolves through the immediately
preceding terminal-child chain. The remainder of either token describes the
current shape or group.

A separate pending transform stands outside a group header. It uses a lead byte
and a valid 16-byte payload, with the optional signed-Y suffix, followed by a group
through a bounded successor form. It is recognized structurally rather than from
the lead value. Requiring the successor group disambiguates it from per-record
flags and shape data.

A separate transform's group need not sit **immediately** after the payload — a
single flag/control byte can intervene: `<lead> <payload> <flag> <group>`. A
separate-transform reader accepts the group at `next` or one flag byte later, then
lets the walker consume the flag while carrying the pending transform forward.

There is an ambiguity between that one-byte-gap form and a flag immediately before
another separate transform. A sequence `<flag> <lead> <payload> <group>` can also
produce numerically plausible floats when decoded one byte early as
`<lead> <payload> <flag> <group>`. A valid transform at the current position takes
precedence when its payload ends directly at a group or framed successor. A
recognized flag is consumed first only when the current candidate requires the
one-byte gap and the shifted candidate has exact structural alignment.

A second framed successor form places a 9-byte trailer between the transform and
the group:

```text
<lead> <payload> [30/70 + scale_y] 21 ?? ?? ?? ?? ?? ?? 09 00 <group>
```

The middle six bytes vary. The complete trailer is opaque framing and is consumed
atomically so its bytes cannot be reinterpreted as transform leads.

The confirmed group-successor offsets are therefore 0 bytes, one flag byte, or
the exact 9-byte frame. Scanning an arbitrary distance for a group is unsafe
because transform payload and shape bytes frequently contain plausible
markerless counts.

While a separate transform is pending, an intervening flag remains part of that
successor form. Reading a second standalone transform at the flag position can
reinterpret group-count bytes as a transform marker and replace the aligned
pending payload.

The same ambiguity exists for **shapes**. A shape record is `00 02`/`01 02`
(32 B) or a bare `02` (31 B), and a single per-shape flag byte can precede it.
Validate the **full** transform of a candidate shape, including `scale_y`, before
accepting a bare record.

The markerless section wrapper may place its first-child transform payload directly
after the bitmap without a marker. This form is accepted only when the bitmap and
the following counted or markerless group agree structurally. The payload may use
the signed-Y suffix and framed successor described above.

Decoded transform values and group ancestry come directly from the parsed records.
The decoder does not normalize signed scale, absorb siblings, or reparent section
spans after the walk.

## Sections

The gyvl body is a fixed ordered list of 11 section slots, present even in an empty
livery. Section membership is positional: a shape carries no section field; its
panel is decided by which slot its group is emitted into. Storage order:

```text
0 Front   1 Back   2 Top   3 Left   4 Right   5 Spoiler
6 FrontWindshield   7 BackWindshield   8 TopWindow
9 LeftWindow   10 RightWindow
```

(This differs from the in-game UI list order.) An empty slot is a constant 23-byte
transform scaffold (orientation only: rotation in {0, 90, -90, 180}, scale 1.0; no
extent/bounds field — per-panel bounds are not stored in the file). A populated
slot is a run of one or more top-level groups whose decal count sums to that
section's stats value, followed by an 18-byte scaffold remnant.

A populated slot's **first** top-level group is wrapped in a markerless group whose
header contains its direct-child count, bitmap-block field, three control bytes,
and child bitmap. The first child placement payload follows that bitmap directly.
Subsequent top-level groups in the slot use their own separate transforms the same
way (an ordinary bare `00` when the placement is simple).

### Built-in shape ID validation

The numeric interval around an ID is not sufficient to identify a built-in vector
shape. The decoder uses the exact ID set in
`assets/vector/shape_names.json`, which is the shipped vector-shape registry.
Values merely falling inside the broader encoded range are not exposed as shapes;
in particular, `256` is not a sentinel and receives no special treatment.

One confirmed wire alias is canonicalized before that lookup: captured `C_group`
and `C_livery` streams encode Arial lowercase `a` as `0x07d0`, while the logical
ID used by `shape_names.json` and `shape_geometry.json.gz` is `0x07d1`. The
decoder maps only this observed wire value to the logical registry ID; it does not
expand the accepted numeric range. The alias occurs in multiple standalone and
livery captures and accounts for otherwise missing leaves in mirrored sections.

A complete, well-framed 32-byte record whose ID is absent from that registry can
still occupy a structural child/decal position. The section walker counts that
occupancy so later groups and slots remain aligned, but it does not create a
`VinylShape` or an editor layer for the unsupported ID. This structural fallback
requires the full record framing, sane finite transform values, and the ordinary
opaque record tail so transform bytes cannot be mistaken for a skipped shape.

### Custom logo / decal records

A placed custom graphic (shared logo/decal) is stored inside the section stream as
an ordinary 32-byte shape record (`00 02`/`01 02` + payload) whose **shape id has
the high bit set**. This marks it as a raster decal rather than an ID from the
built-in vector registry; the raster id is `id & 0x7fff`, indexing the `yrvl`
descriptor table. Its transform is **arbitrary**, exactly like a vector
shape: position, scale, rotation, skew and color are all encoded in the record.
Recognise a logo by `id ≥ 0x8000` with a sane shape transform.

One logo is one physical gyvl record, but the corresponding section's `yrvl` count
can retain the number of vector decals from which that uploaded logo was created.
When that happens, the logo has a logical count greater than one. The decoder
learns this weight only from a constrained residual: the last populated slot must
contain exactly one logo ID, the residual must divide evenly across its placements,
and the same logo ID must occur in an earlier slot. It then reparses all slots with
that learned weight. This prevents an earlier mirrored section from borrowing the
first records of the following section without assigning a per-livery exception.

**All 11 slots are always emitted, in order** — the body never stops after the last
populated slot. The empty scaffolds that follow it are part of the record and act
as the section delimiters. A decoder must therefore **bound each populated slot's
walk** so it cannot run past its own decals + 18-byte remnant into the following
slots: reserve the remaining slots' footprint (23 B for each still-empty slot)
against the body end. Without that bound, a populated slot whose decodable decals
fall short of its stats count keeps walking to the end of the body and swallows the
trailing empty scaffolds; an exporter that byte-preserves those slots would then
omit them, shifting every following slot and corrupting the section container.

There is an additional root-level boundary guard for a small remaining stats
deficit. It is considered only when the walker is back at the current section root,
has no pending transform, and the next slot is populated. If the fixed 18-byte
remnant is followed by a structurally valid markerless root for that next slot, the
current walk stops before the remnant instead of consuming the next section to
satisfy the deficit. Restricting the check to section-root state prevents a nested
group from being closed on a coincidental byte pattern.

## yrvl info record (first yrvl, @0x1a)

The livery-level record between `vlrc` and `gyvl`. Fully parsed:

```text
0x00 4 bytes  tag "yrvl"
0x04 u32      size = 19 (constant)
0x08 8 bytes  creator id (author GUID). Always ends in the constant suffix
              ...0900. Use the null author 0000000000000900 for from-scratch output.
0x10 u32      flag. Use 1 for from-scratch output.
0x14 u8       0
0x15 u32      gyvl body length — MUST equal the byte length of the following gyvl
              chunk body (gyvl tag start .. next yrvl). This is the one computed
              field: write it after emitting gyvl.
```

## Trailing yrvl Chunks

### yrvl stats (52 B) — per-section decal counts

`u32` counters starting right after the 4-byte `yrvl` tag (no size field), 12 in
all. The first 11 are the decal counts per section in storage order. These counts
drive the section walk. The 12th u32 is a small counter; write 0.

### yrvl descriptor table (variable)

A panel, material, paint and graphic registry. A minimal table with default paint
is 592 bytes, but paint bindings and graphic references make the table variable.
The `yrvl` tag may be followed by an opaque graphic-reference prefix before the
record table.

The record table can be located structurally. From its header through the end of
the descriptor chunk its size is `102 + record_count * 27` bytes:

```text
u8       table mode, observed 00 or 01
u8       02
u16      record_count
6 bytes  initial zero padding
27 bytes records[record_count]
u32      panel_count = 11
8 bytes  panel_ids[11]
```

The final 92 bytes are fixed: `0b 00 00 00` followed by the 11 panel identifiers.
The record count includes material paint bindings and the 11 panel records.
Cars can add material aliases, so paint records must be selected by identifier
rather than by their table position.

#### Paint material record

Paint values use the same 27-byte record envelope as the panel records:

```text
0x00  8    material identifier
0x08  u8   value type = 02
0x09  u8   primary color enabled
0x0a  4    primary color, BGRA8
0x0e  u8   secondary color enabled
0x0f  4    secondary color, BGRA8
0x13  u32  manufacturer-color selector
0x17  u32  finish code
```

The three editor color channels are converted to RGB and stored in byte order
blue, green, red. The alpha byte is normally `ff`. A disabled color field can
retain stale channel bytes and must be ignored.

A manual editor color uses manufacturer selector `ffffffff` and a hexadecimal
finish code from the following table:

```text
00  none
01  gloss
02  semigloss
03  matte
04  metallic
32  two-tone matte
33  two-tone polished
34  two-tone semigloss
45  candy
46  metallic low flake
47  metallic high flake
48  metallic glitter
```

Metallic and two-tone finishes enable the secondary BGRA color. Candy uses only
the primary color. Other finish codes can exist in the unobserved gaps.

A manufacturer color stores its selector in the four bytes at `0x13` and uses an
all-zero finish. The selector is an opaque palette value and is not assumed to be
the zero-based record position.

The palette data is stored with the matching car in `ManufacturerColors.bin`.
Each palette record includes its affected material-channel names, a primary RGB
float triple, a calibrated material path, and a second RGB float triple. The
material path supplies the manufacturer finish definition. Palette resolution
therefore requires the car assets in addition to the livery descriptor.

The observed palette block begins with its `u8` record count at raw offset `0x2c`.
Its variable-length entries use this layout:

```text
u8        primary color enabled
u32       material-channel count
repeat    u8 string length + ASCII material-channel name
f32[3]    primary RGB
u8        material-path length
bytes     ASCII material path
u8        secondary color enabled
f32[3]    secondary RGB
u16       entry tail
```

The color floats are normalized record values and are not the livery table's
BGRA8 encoding. Additional data can follow the counted palette entries.

The common material identifiers are:

```text
Body       75a639c8a7e8dbf7  172b921176b2e548  57aa6a5afa926f49
Hood       53d9e57fd8e9c16a  111c9af2b55fd94f  e1af6b67b934d478
Mirrors    2211740cf5f05f1e  e2fa31058d3b5f11  e0f816ef8fede623
Wing       9a31ee53021148cd  6569a28ac213eabc  83b84e1a39c263b9
           a0f1a88cb3f07f4a  babbd810ff6b5006  98376f6b633d889a
Brakes     b955df430a5e49a5
Window     a4f9ffa21bfd8295
```

Rims use five paired material channels:

```text
channel  front             rear
1        78de6407455e92b8  43e77afae8b11366
2        227ffc9e86b6ed15  11e717cfa8bd3c3a
3        3de891e13bd30724  50d97f4721ea38c3
4        18d320f30bf84d56  6390ea14e780150f
5        114f706cefb18b81  64efb6ea85954e87
```

The per-custom-graphic record layout remains open and is needed for uploaded-logo
output.

### yrvl terminator (8 B)

Constant terminator:

```text
"yrvl" 00 00 00 00
```

## Summary Model

```text
C_livery = zlib( vlrc[header] + yrvl[info] + gyvl[ = one C_group ]
                 + yrvl[stats] + yrvl[descriptor table] + yrvl[end] )
```

Artwork and panel assignment live inside `gyvl`: shapes/groups per the `C_group`
grammar with the transform-marker dialect above, and panel membership by
positional slot. The trailing `yrvl` table carries the material paint state,
panel registry and graphic references.
