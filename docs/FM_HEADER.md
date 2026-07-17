# Forza Motorsport Header Format

Forza Motorsport asset folders use a `header` sidecar with the same variable-size
name, date, and creator prefix as the Horizon header. Format version 9 identifies
this dialect.

## Shared Prefix

```text
u32       format version
u32       name length in UTF-16 code units
byte[]    name in UTF-16LE
u32       reserved
u16       year
u8        month
u8        day
byte[16]  metadata field block
byte[8]   creator identity tag
u32       creator-name length in UTF-16 code units
byte[]    creator name in UTF-16LE
```

The import browser uses the shared metadata fields when they can be decoded and
retains the original header bytes with the project.

## Trailing Layout

The draft trailing structure is:

```text
byte[12]  reserved
byte[16]  asset GUID
byte[2]   marker
byte[8]   padding
u32       logical decal count
u32       target car identifier
byte[16]  repeated asset GUID
```

The Motorsport importer treats this tail as source metadata rather than editor
project state. Livery target information is decoded from the livery data stream,
and the retained header is not regenerated during import.
