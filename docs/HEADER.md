# Forza Horizon Header Format

The `header` sidecar stores project metadata separately from `C_group` and
`C_livery`. The editor decodes the fields required by the project UI and retains
opaque bytes for round-trip output.

## Common Preamble

```text
u32       format version
u32       name length in UTF-16 code units
byte[]    name in UTF-16LE
u32       description length or zero
```

A zero description length selects the draft layout. A nonzero length is followed
by a UTF-16LE description and an opaque published tail.

## Draft Layout

The draft fields following the zero description length are:

```text
u16       year
u8        month
u8        day
byte[16]  metadata field block
byte[8]   creator identity tag
u32       creator-name length in UTF-16 code units
byte[]    creator name in UTF-16LE
byte[28]  reserved
byte[2]   section marker
byte[7]   section padding
u32       type value
byte[16]  asset GUID
byte[]    trailing data
```

The metadata field block, creator identity tag, type value, GUID, and trailing
data are preserved by the editor. Their external semantics are not required for
project editing.

## Published Layout

For a published header, the editor decodes the version, name, and description,
then retains the remainder as `publishedTail`. Rebuilding the header writes the
decoded preamble and description followed by that tail unchanged.

## Codec Behavior

`parseHeader()` validates string and fixed-field bounds before returning
`HeaderMetadata`. `buildHeader()` writes the corresponding draft or published
layout. A new draft receives format version 7, the current date fields, a new
GUID, and initialized opaque blocks.

Imported header bytes remain attached to the project. Group export converts
published metadata to a draft header and clears the description before writing
the output folder.
