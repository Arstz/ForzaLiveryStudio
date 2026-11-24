# Forza header sidecar format

The `header` sidecar stores metadata separately from `C_group` and `C_livery`.
Horizon and Motorsport use the shared prefix below with version-specific tails.
The editor decodes UI fields and retains opaque source bytes for round trips.

## Shared prefix

All numeric fields are little-endian. Text lengths count UTF-16 code units.

```text
u32       format version
u32       name length
byte[]    name in UTF-16LE
u32       description length
byte[]    description in UTF-16LE
u16       year
u8        month
u8        day
byte[16]  metadata field block
byte[8]   creator identity tag
u32       creator-name length
byte[]    creator name in UTF-16LE
```

A zero description length selects the draft layout and contributes no description
bytes. Published headers carry a non-empty description; their date and creator
fields follow the same layout. The metadata field block supplies the asset type
used to determine optional livery car fields.

## Versioned tails

The tail begins immediately after the creator name. Versions 2 and later use:

```text
byte[28]  section prefix
byte[]    section marker and padding
u32?      category
u32       type value / logical decal count
u32?      target car identifier
byte[16]  asset GUID
byte[]    opaque trailing metadata
```

| Version | Games | Prefix bytes | Marker + padding bytes | Asset ID bytes | Car identifier |
| --- | --- | --- | --- | --- | --- |
| 0 | FH2, FM5, FM6 | 16 | 0 | 8 | Livery asset type. |
| 1 | FH3 | 16 | 1 | 8 | Livery asset type. |
| 2 | FM7 | 28 | 1 | 16 | Livery asset type. |
| 4 | FH4; accepted FH5 imports | 28 | 5 | 16 | Livery asset type. |
| 6 | FH5 export | 28 | 5 | 16 | Livery asset type. |
| 7 | FH6 | 28 | 9 | 16 | Present. |
| 9 | Motorsport 2023 | 28 | 9 | 16 | Present. |

Versions 0 and 1 replace the generic section prefix with sixteen bytes and use
a padding field whose size equals the version. The decal total follows that
padding, then the optional livery car identifier and eight-byte asset identifier.
Version 2 uses a single `01` marker. Versions 4 and 6 use a two-byte marker plus
three padding bytes; versions 7 and 9 use a two-byte marker plus seven bytes.
Version 9 inserts the category before the decal count.

Opaque blocks, creator identity, marker padding, GUIDs, and trailing metadata
remain attached to the project. Published section prefixes can contain state and
repeated identity fields. Trailing bytes can contain repeated identity and count
fields; the core layout is selected by version and asset type.

## Motorsport 2023 tail

Version 9 uses the shared prefix above. Its livery tail expands to:

```text
byte[12]  state and reserved prefix
byte[16]  asset GUID
byte[2]   marker
byte[7]   padding and state
u32       category
u32       logical decal count
u32       target car identifier
byte[16]  repeated asset GUID
byte[]    opaque trailing metadata
```

The fixed tail through the repeated GUID is 65 bytes. The first twenty-eight
bytes form the generic section prefix. Both prefix and marker padding carry
state and are preserved as opaque metadata.

The import browser reads shared metadata when decodable and retains the original
header bytes. Livery target information comes from the livery stream. Import
preserves this tail as source metadata without regenerating the header.

## Codec behavior

`parseHeader()` validates string and fixed-field bounds into `HeaderMetadata`.
`buildHeader()` writes the matching draft or published layout.
`defaultDraftHeader()` initializes version 7, the current date, a new GUID, and
opaque blocks.

Group export normalizes publication metadata to a draft header, clears the
description, synchronizes decal totals, and applies the destination save-profile
identity. Livery export synchronizes the target car and decal count with the
container. Native target export selects the destination layout and retains
compatible identity fields; opaque data without a destination representation
receives target defaults. Projects without source headers receive generated
draft metadata.

See [group encoding](CGROUP.md), [livery export profiles](CLIVERY.md#native-export-targets),
and [WGS export](WGS.md) for the enclosing asset formats.
