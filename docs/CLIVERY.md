# C_livery Encoding Reference

Concise reference for the Forza `C_livery` binary payload. It describes the chunk
container and how the embedded artwork relates to the [`C_group`](CGROUP.md)
format: a `C_livery` wraps one embedded `C_group` (a `gyvl` chunk) whose only
difference from the standalone form is the transform-marker dialect below.

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
0x04 u32      version = 2  (constant across all 46 samples)
0x08 u32      source flag. 0 for a locally-authored livery; 1 on a subset of
              downloaded/shared liveries (all with a foreign, non-null creator id).
              For a from-scratch save: 0.
0x0c u32      0  (constant)
0x10 u32      car id — which car the livery is for (decimal). Matches the u32 in
              the car's .carbin (before its name) and the tests/liveries/Livery_<id>
              folder numbers (e.g. 1294 = gmc_syclone_92). This is how the game knows
              which car to apply the livery to; see assets/cars/car_ids.json.
0x14 u32      category/type field. Small enum-or-bitmask, observed {0,1,2,4,6}
              (looks like bits 0x01|0x02|0x04). 0 in every hand-authored sample
              (Empty / One* / FrontBack / TwoFront), so 0 is safe for from-scratch.
0x18 u16      0  (constant)
```

Every field here except the car id is either a fixed constant or safely 0 for a
newly-authored livery. Deduced from all 46 `tests/liveries` samples.

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

## Transform-Marker Dialect

Shapes, group headers, child bitmaps, and transform payloads are byte-identical to
`C_group`. Only the **transform markers** differ. A standalone `C_group` transform
marker is `<lead> <body> 03`; in the livery:

```text
00-family   00 [01]* 03   ->   00           (collapse to a single byte)
odd-family  <odd> 03       ->   <odd> 01      (terminating 03 becomes 01)
```

`<odd>` is any odd lead byte (01, 03, 05, 07, 0b, 0d, 0f, 1b, 1d, 1f, 33, 3d, 3f,
7f, ff, ...). The livery marker set is the closed grammar `{ 00, <odd> 01, 01 }`.
The 16-float payload (px, py, sx, rot) is unchanged, as is the optional signed-Y
suffix — except its marker byte:

```text
30 + f32 scale_y      normal group
70 + f32 scale_y      mask group   (70 = 30 | 40, the mask bit)
```

Recognize both suffixes with `(byte & ~0x40) == 0x30`.

The bare `00` marker is too common to match by its byte alone. A `00`-family
transform must be recognized **structurally**: a `00`/`01` lead + a valid 16-float
payload (+ optional 30/70 suffix) followed by a group or shape. It is therefore
always a separate (pending) transform applied to the following group/shape; the
distinctive odd-family markers may appear inline (in a group header) or separate.

Everything else — counted groups, markerless groups, inline first-child
transforms, masks, flattening — follows the `C_group` rules unchanged.

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

## yrvl info record (first yrvl, @0x1a)

The livery-level record between `vlrc` and `gyvl`. Fully parsed:

```text
0x00 4 bytes  tag "yrvl"
0x04 u32      size = 19 (constant)
0x08 8 bytes  creator id (author GUID). Always ends in the constant suffix
              ...0900. A locally-authored / anonymous livery uses the null author
              0000000000000900 — a value the game itself writes (seen in several
              samples), so it is the correct choice for from-scratch.
0x10 u32      flag = 1  (0 in two shared-livery outliers; use 1 for from-scratch)
0x14 u8       0
0x15 u32      gyvl body length — MUST equal the byte length of the following gyvl
              chunk body (gyvl tag start .. next yrvl). This is the one computed
              field: write it after emitting gyvl.
```

## Trailing yrvl Chunks

### yrvl stats (52 B) — per-section decal counts

`u32` counters starting right after the 4-byte `yrvl` tag (no size field), 12 in
all. The first 11 are the decal counts per section, in storage order (confirmed
one-by-one from the `One<Panel>` samples, each of which sets exactly one slot to
1); all zero for an empty livery. These counts drive the section walk (a section
is consumed until its decals reach its count). The 12th u32 is a small counter,
0 in every hand-authored and most captured samples (only two outliers show 1 / 6);
write 0.

### yrvl descriptor table (variable)

A panel/graphic registry. For any livery whose artwork uses only the game's
built-in shapes it is a **592-byte global constant** — byte-identical across every
`One*`, `Empty`, `FrontBack`, `TwoFront` sample AND across different cars
(car 1294 vs 383). Its layout: an `00 02` header, 8 fixed block/material GUIDs
(32-byte records), 11 fixed panel GUIDs (`…ffffffff 00000000 <panel-guid>`), then
a tail `0b 00 00 00` (=11) followed by the 11 panel GUIDs concatenated. All of
these GUIDs are global constants (not car- or livery-specific).

The table grows only when the artwork references **custom uploaded graphics**
(shared logos/decals, which carry their own share GUIDs) — the extra size is
dominated by 27-byte-per-reference records (most sample deltas are exact multiples
of 27; a few other record kinds exist). Built-in-shape liveries add nothing, which
is why the whole `One*` set keeps the 592-byte constant even with a decal placed.
So: **a from-scratch livery using only built-in shapes can emit the 592-byte
constant verbatim.** The per-custom-graphic record layout is the one remaining
open item, needed only for uploaded-logo support.

### yrvl terminator (8 B)

Constant across all samples:

```text
"yrvl" 00 00 00 00
```

## Summary Model

```text
C_livery = zlib( vlrc[header] + yrvl[info] + gyvl[ = one C_group ]
                 + yrvl[stats] + yrvl[descriptor table] + yrvl[end] )
```

All artwork and panel assignment live inside `gyvl`: shapes/groups per the
`C_group` grammar with the transform-marker dialect above, panel membership by
positional slot. The trailing `yrvl` table is constant scaffolding.
