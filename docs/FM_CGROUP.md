# Forza Motorsport C_group Encoding Reference

This document describes the standalone Forza Motorsport (2023+) `C_group`
payload accepted by the editor. The format shares the `gyvl` container and
scene model with the current [`C_group`](CGROUP.md) format, but uses an older
record dialect for headers, transforms, shapes, and masks.

## Container

The `data` file is an uncompressed `gyvl` payload:

```text
0x00  4 bytes  magic "gyvl"
0x04  u32      version = 1
0x08  u32      reserved
```

The companion `header` file is described in [`FM_HEADER.md`](FM_HEADER.md).

## Root Transform

The root transform begins at `0x0c`:

```text
0x0c  u8   generation marker
0x0d  f32  position_x
0x11  f32  position_y
0x15  f32  scale_x
0x19  f32  rotation
```

Generation markers `01`, `02`, and `03` are accepted. An optional signed
Y-scale extension can follow:

```text
30 + f32 scale_y
70 + f32 scale_y
```

The low form is a normal extension. The high form carries the mask bit.

## Root Group Header

The common root header uses the counted-group layout:

```text
20/60 + u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

An older markerless form replaces the `20` marker with `00` while retaining
the remaining field positions:

```text
00 + u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

For both forms:

```text
child_blocks = ceil(child_count / 8)
layer_start  = 0x24 + child_blocks
```

Extended headers can place transform or control records before the first
child. Their layer boundary is established from structurally valid records
rather than a fixed fallback offset.

## Shapes

The format contains both framed and bare shape records.

Framed record:

```text
00 02 + shape payload
01 02 + shape payload
```

Bare record:

```text
01 + shape payload
02 + shape payload
03 + shape payload
```

Framed records are 32 bytes and bare records are 31 bytes. The shared payload
is:

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

Record recognition requires a registry-backed shape ID and finite,
structurally plausible transform values. Legacy glyph identifiers are
canonicalized before registry validation.

## Group Transforms

Separate transforms use a generation marker followed by four floats:

```text
01/02/03
f32 position_x
f32 position_y
f32 scale_x
f32 rotation
```

The optional `30`/`70` Y-scale extension follows the same rules as the root
transform. A generation marker is treated as a transform only when its payload
is valid and a counted or markerless group follows directly or after one
control byte. This guard keeps shape markers and transform markers distinct.

The decoder normalizes the older `01` and `02` forms to the shared internal
transform dialect before building the scene tree.

## Groups

Nested counted groups use:

```text
20/60 + u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

Markerless nested groups can follow a separate transform:

```text
u16 child_count + u8 child_blocks + control[child_blocks + 2]
```

`20` is a normal group and `60` is a mask group. Groups close when their
declared direct-child count is satisfied. Child counts and bitmap lengths are
validated before a candidate header is accepted.

## Masks

Flat mask state is trailing:

```text
01 02 on a shape record  -> mask the preceding shape
01 in the final trailer  -> mask the final shape
```

The final trailer is one or two bytes. Explicit trailing mask state is retained
when the decoded scene layer is created. Group mask state continues to inherit
through descendants.

## Decoder Path

Standalone FM assets use `decodeFM2023RawGroup()` and
`importFM2023Asset()`. The decoder performs FM-specific record normalization,
then passes the normalized layer stream to the shared vinyl tree builder.
The FM normalization is isolated from the FH6 `C_group` and embedded-livery
paths.

## Validation

The supported dialect has been confirmed through complete child-count checks
and editor rendering across the available FM group corpus.

The optional comparison helper prints structural statistics for an asset:

```powershell
.\build\Release\fh6_livery_compare.exe --fm-stats <assetFolder>
```

It reports decoded shapes, masks, groups, skipped records, and incomplete
declared groups without requiring GUI inspection.
