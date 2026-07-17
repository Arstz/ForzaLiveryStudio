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
by a UTF-16LE description and the published metadata fields.

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
byte[28]  section prefix
byte[2]   section marker
byte[7]   section padding
u32       type value
u32       target car identifier
byte[16]  asset GUID
byte[]    trailing data
```

The metadata field block, creator identity tag, section prefix, type value, GUID,
and trailing data are preserved by the editor. Livery export synchronizes the
target car identifier with the embedded container metadata.

## Published Layout

Published headers use the same metadata fields after a non-empty description.
Their section prefix can contain state and repeated identity data and is retained
unchanged.

## Codec Behavior

`parseHeader()` validates string and fixed-field bounds before returning
`HeaderMetadata`. `buildHeader()` writes the corresponding draft or published
layout. A new draft receives format version 7, the current date fields, a new
GUID, and initialized opaque blocks.

Imported header bytes remain attached to the project. Group export converts
published metadata to a draft header and clears the description before writing
the output folder. Livery export writes edited names and creators while retaining
the remaining decoded header fields.
