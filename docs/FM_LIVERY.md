# Forza Motorsport C_livery Encoding Reference

Concise reference for the Forza Motorsport (2023+) `C_livery` binary payload.
It describes the chunk container, how the embedded artwork relates to the
[`C_group`](CGROUP.md) format, and the multi-container file structure used by
FM2023.

**Relationship to FH6:** FM2023 and FH6 share the same chunk-tag system
(`vlrc`/`yrvl`/`gyvl`), the same `C_group` shape/group grammar, and the same
transform-marker livery dialect. They differ in section count (7 vs 11),
container packaging (multi-container vs single blob), and header version.

---

## File Structure: Multi-Container Format

Unlike FH6 (a single compressed blob), an FM2023 livery data file packs
**three independent containers** concatenated:

```text
+0x0000  Container 1: C_livery       (vlrc + yrvl + gyvl chunks)
+0x1DF2  Container 2: C_group body   (raw section stream, no header)
+0x49D7  Container 3: yrvl metadata  (stats + descriptor + terminator)
```

Each container uses the standard wrapper:

```text
0x00 u32 compressed_length
0x04 u32 uncompressed_length
0x08 zlib payload (78 9c ...)
```

Container sizes are representative of the observed livery and will vary.

### Container 1 — C_livery Chunks

The first container is a tagged chunk stream:

```text
order  tag   offset  size            role
  1   vlrc   0x00    26 B  (fixed)    root / file header (metadata)
  2   yrvl   0x29    variable         livery-info record
  3   gyvl   0x168   variable, BULK   the artwork (embedded C_group)
```

> Note: offset 0x1A-0x28 (15 bytes after vlrc) is padding/unknown, not a yrvl.
> The first yrvl is at 0x29, and gyvl is at 0x168 (not 0x33 as in FH6).

### Container 2 — C_group Working Copy

The second container is a raw `C_group` body with no `vlrc`/`gyvl`/`yrvl`
wrapper. It starts directly with the section stream (flat shapes, then group
hierarchy). Shape IDs and distribution match Container 1, confirming this is
the same artwork — likely the edited/working copy.

### Container 3 — yrvl Metadata

The third container holds the section metadata that FH6 stores inline after
`gyvl`:

```text
order  tag   offset   size            role
  1   yrvl   +0x00    32 B            per-section decal-count stats (7 sections)
  2   yrvl   +0x20    variable        layer/section descriptor table
  3   yrvl   +0xCF7   8 B             terminator (yrvl 00 00 00 3f)
```

---

## vlrc — Root Header

```text
0x00 4 bytes  tag "vlrc"
0x04 u32      version = 2
0x08 u32      0
0x0c u32      0
0x10 u32      livery share id (decimal; matches folder name)
0x14 u32      small flag/category (0..4; meaning unknown)
0x18 u16      0
```

Identical to FH6. Version is always 2.

---

## yrvl — Livery-Info Record

```text
0x00 4 bytes  tag "yrvl"
0x04 ...      variable payload (GUID + flags)
```

Observed size: 25 bytes, same as FH6.

---

## gyvl — Embedded C_group Header

The `gyvl` chunk is a `C_group` but with a shorter header dialect, identical
to FH6:

```text
rel 0x00  4   magic "gyvl"
rel 0x04  u32 version = 0        (standalone C_group uses 1)
rel 0x08  u32 0
rel 0x0c  f32 1.0               (root scale; livery root is always identity)
rel 0x10  u32 0
rel 0x14  u8  0
rel 0x15  ...                   body starts here
```

No root counted-group marker (`20`/`60`). The body is a flat positional stream
of section slots.

---

## Transform-Marker Dialect

Shapes, group headers, child bitmaps, and transform payloads are byte-identical
to FH6 `C_group`. The transform-marker livery dialect is the same:

```text
00-family   00 [01]* 03   ->   00           (collapse to a single byte)
odd-family  <odd> 03       ->   <odd> 01      (terminating 03 becomes 01)
```

Scale-Y suffixes:

```text
30 + f32 scale_y      normal group
70 + f32 scale_y      mask group   (70 = 30 | 40, the mask bit)
```

---

## Sections

FM2023 uses **7 section slots** (FH6 uses 11). The window-specific sections
(BackWindshield, TopWindow, LeftWindow, RightWindow) are absent.

```text
0 Front    1 Back    2 Top    3 Left    4 Right    5 Spoiler
6 FrontWindshield
```

An empty slot is a constant 23-byte transform scaffold (rotation in
{0, 90, -90, 180}, scale 1.0). A populated slot is a run of one or more
top-level groups whose decal count sums to that section's stats value,
followed by an 18-byte scaffold remnant.

---

## yrvl Stats — Per-Section Decal Counts

```text
yrvl + 7 * u32   (28 bytes, not 11 * u32 as in FH6)
```

`u32` counters starting right after the 4-byte `yrvl` tag. The first 7 values
are the decal counts per section in storage order. All zero for an empty
livery.

---

## yrvl Descriptor Table (variable)

Records carry an `00 01` sub-marker, a length, and RGBA. Structure differs
from FH6's `00 02`-based records. Fields are not confirmed.

---

## yrvl Terminator (8 B)

```text
"yrvl" 00 00 00 3f
```

Note the non-zero `3f` suffix, unlike FH6 where the suffix is `00 00 00 00`.

---

## Summary Model

```text
FM2023 data file = Container1 + Container2 + Container3

Container1 = zlib( vlrc[header] + yrvl[info] + gyvl[ = one C_group ] )
Container2 = zlib( gyvl body stream only, no header )
Container3 = zlib( yrvl[stats 7x u32] + yrvl[descriptor table] + yrvl[terminator] )
```

All artwork and panel assignment live inside the two `gyvl` bodies (Container 1
= original, Container 2 = working copy), using the `C_group` grammar with the
livery transform-marker dialect. Panel membership is by positional slot across
7 sections. The `yrvl` metadata in Container 3 drives the section walker.