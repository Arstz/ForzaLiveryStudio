# C_livery Encoding Reference

`C_livery` stores livery metadata, grouped artwork, section counters, material
paint state, and graphic descriptors in one compressed container. The artwork
uses the group grammar documented in [CGROUP.md](CGROUP.md) with livery framing.

## Container

The Horizon container uses the same compressed wrapper as `C_group`:

```text
u32       compressed length
u32       uncompressed length
byte[]    zlib payload
```

The decompressed FH6 stream is a tagged sequence:

```text
vlrc      root metadata
yrvl      livery information
gyvl      artwork
yrvl      section counters
yrvl      descriptor table
yrvl      terminator
```

Tags are stored as byte strings in the payload. The `gyvl` body length is stored
in the preceding livery-information record.

## Root Metadata

The shared `vlrc` fields begin with:

```text
0x00  byte[4]  "vlrc"
0x04  u32      version
0x08  u32      source state
0x0c  u32      reserved
0x10  u32      target car identifier
0x14  u32      category state
```

Root-version-1 streams have one state byte at `0x18`, followed by `yrvl` at
`0x19`. FH6 root-version-2 streams have two bytes at `0x18`, followed by `yrvl`
at `0x1a`. Motorsport 2023 also uses root version 2, with an extended prefix
placing `yrvl` at `0x29`. The root version alone does not select the layout.

The editor reads the target car identifier and preserves the remaining source
metadata during source-backed encoding.

## Artwork Header

The embedded `gyvl` chunk uses this header:

```text
0x00  byte[4]  "gyvl"
0x04  u32      version
0x08  u32      reserved
0x0c  f32      root scale
0x10  u32      reserved
0x14  u8       control
0x15  byte[]   section stream
```

Unlike standalone `C_group`, the artwork body is a positional section stream
rather than a counted root group.

## Groups and Transforms

Counted and markerless groups use the shared child-block grammar in
[CGROUP.md](CGROUP.md#physical-framing). Child counts determine physical tree
boundaries. Group descriptors independently select opaque identity and signed
Y-scale fields through bits `01` and `10`.

Placement leads, inline ownership, rotation sign, and mask state remain dialect
interpretation. A descriptor's high bits do not define a universal mask rule.
The physical reader retains complete and bounded partial attribute spans before
the existing semantic decoder interprets the artwork.

The FH6 encoder uses markerless section roots. Artwork group header selection
follows the destination profile and relative signed basis, with counted `20` or
`60` headers for canonical group framing. The first shape in a counted group uses the explicit
`01 02` lead.

Record-level mask state is trailing. It resolves against the preceding direct
shape or terminal group child, while inherited mask-group state remains
authoritative. Every shape inheriting a `60` group carries an explicit `01 02`
lead, including the group's first shape.

Native group-transform state carries the preceding direct shape's mask bit.
The encoder derives this bit from sibling mask state when selecting the
destination record framing.

For FH4, FH5, and FH6, a shape following a group begins with one state byte
(`00` or `01`) followed by `02`. Counted group boundaries close the preceding
nesting. The next shape record starts directly after the terminal child payload.
Shape leads reflect traversal state across group boundaries. A markerless root
can begin with the bare `02` form.

Bare `02` framing belongs to the first child position, not to an imported
shape's identity. Moving a first child behind another child requires a state
byte before `02`, including when the preceding child is a raster logo.
Unchanged source slots are also checked for noninitial bare shapes: missing
state bytes are inserted backwards by source offset so the remaining artwork
bytes and transforms stay intact. Import remains tolerant of these records.

## Sections

The FH6 artwork stream contains eleven ordered section slots:

```text
Front
Back
Top
Left
Right
Spoiler
FrontWindshield
BackWindshield
TopWindow
LeftWindow
RightWindow
```

Section membership is positional and is represented in the editor by a top-level
group carrying `isLiverySection` and `liverySectionSlot`.

Export retains visible section group hierarchy and composes scene transforms into
encoded group origins and relative shape records. When source metadata is
available, retained groups preserve their transform frames and header dialect
independently of changes to their child list. Artwork groups are normalized to
the encodable cardinality before their child bitmap is written. Artwork record
framing is derived from the current sibling structure and mask state. Mask
ancestry and trailing shape-mask state remain expressible through the same
structured representation.

The physical body is an ordered stream of child blocks, boundary frames, and
trailing state. The shared reader frames this stream independently of the section
counters and retains unsupported and degenerate artwork in physical occupancy.

A boundary frame occupies seventeen bytes:

```text
byte[8] opaque
f32     unit_scalar
f32     orientation
u8      context_flags
```

The opaque bytes are retained without interpreting them as a position transform.
Boundary flags remain contextual metadata. An empty child block occupies six
bytes. A boundary frame can precede or follow it; those independent units explain
the twenty-three-byte empty layouts. Boundary frames are not permanently assigned
as section footers or preambles. A populated block can end with only its trailing
state byte, and an empty block can terminate the body directly.

Section counters describe logical occupancy, not byte lengths. Physical record
count, logical occupancy, and editable geometry are tracked separately. A complete
physical stream whose ordered blocks match the section layout supplies exact
artwork boundaries. The decoder retains its metadata-guided recovery walk when
the stream cannot be mapped unambiguously. Extra blocks or missing slots remain
a mapping disagreement; the framing reader does not select a historical snapshot
or fabricate missing artwork.

Source preservation uses validated physical artwork spans and boundary records.
Unchanged, fully mapped artwork retains its original body, opaque metadata, and
logical counters. Missing noninitial shape-state leads are repaired by source
offset. An edited slot is rebuilt from its scene hierarchy; compatible untouched
slots retain their physical artwork. Boundary records and empty child blocks are
assembled explicitly, without synthetic terminal padding or a final byte trim.
Unmapped source artwork uses the recovery decoder. Validated child blocks at
recovered offsets can still be reused; edited or incomplete blocks are rebuilt
from the scene. The raw imported payload remains project source data.

New artwork uses complete attributes and destination-specific traversal state.
Export does not reproduce partial attribute fragments as newly authored content.

## Shape Records

Artwork shapes use the shape payload defined in [CGROUP.md](CGROUP.md). Built-in
vector identifiers are canonicalized and validated against the runtime registry.
Unsupported records can retain structural occupancy without becoming editable
scene layers.

A shape identifier with its high bit set represents a raster logo. The remaining
bits identify the raster descriptor. The section counter can assign a logical
weight greater than one to a physical logo placement; the decoder derives that
weight from the section metadata before building the final scene.

## Livery Information

The `yrvl` record preceding the artwork stores creator metadata, state, and the
`gyvl` body length. The length must match the emitted artwork chunk.

The FH6 section-counter record following the artwork stores eleven section counts and
one trailing counter. The decoder uses the section counts to interpret and validate
section occupancy after physical framing. The encoder rebuilds these counts from the exported section
contents.

Older formats use seven counters, including the trailing counter. Section slots
without declared artwork remain empty in the decoded scene.

If trailing mask state belongs to the final shape in a section, a standalone `01`
marker follows the completed section walk. The decoder applies it to that terminal
shape before advancing past the completed child block.

## Descriptor Table

The FH6 descriptor chunk contains paint bindings, panel records, and raster graphic
references. Its structurally decoded record table has this layout:

```text
u8        table mode
u8        record type
u16       record count
byte[6]   reserved
byte[27]  records[record count]
u32       panel count
byte[8]   panel identifiers[panel count]
```

A paint record uses the 27-byte envelope:

```text
byte[8]   material identifier
u8        value type
u8        primary color enabled
byte[4]   primary BGRA color
u8        secondary color enabled
byte[4]   secondary BGRA color
u32       manufacturer color selector
u32       finish code
```

The editor matches paint records to decoded car materials by identifier. Enabled
colors and supported finish codes affect the 3D preview. Manufacturer selectors
and descriptor records without an editor representation remain opaque source
metadata.

Positional paint layouts use fixed 40-byte records. Their material binding is
selected by slot, while each record stores primary and secondary BGRA values,
their enable flags, three optional scalar fields, a manufacturer selector, a
finish identifier, and an opaque trailing field. An active manufacturer selector
takes precedence over retained custom color bytes. Custom paint uses selector
`0xffffffff`.

The editable positional bindings cover body, hood, mirrors, spoiler, brakes,
rims, and glass, including the corresponding body-panel color groups. Native
paint blocks remain project metadata so scalar fields and unmapped records
survive project persistence and native export. Whole-wheel edits update the
extended wheel records together; independent axle paint requires a destination
with separate axle bindings.

`assets/cars/paint_colors.json` stores per-title GameDB manufacturer palettes,
color classifications, secondary and stripe exceptions, special-color values,
and finish definitions. Manufacturer palette sequence values are retained as
database identifiers rather than interpreted as native save selectors. Finish
translation between titles uses material identity and destination availability.

The optional per-car `manufacturer_palettes` map stores native selector order,
normalized RGB floats, material references, and material masks or slot names.
Palette entries retain all material assignments and versioned color overrides.
`tools/append_manufacturer_palettes.py` populates these records from car archives;
repeat its `--game` argument to import multiple titles. Database regeneration
preserves the appended native palettes.

The stream ends with:

```text
"yrvl" 00 00 00 00
```

## Forza Motorsport Dialect

Forza Motorsport stores the tagged stream in consecutive compressed blocks.
Decoding inflates every block and concatenates the results before locating tags.

The shared livery codec reads Motorsport `data` files and Horizon `C_livery`
files. Physical record coverage selects the artwork dialect and section layout;
counter storage and artwork section count are independent. Shape markers,
child bitmaps, transforms, and boundary metadata retain their original bytes.
Both current and legacy shape records are supported, including Motorsport logo
identifiers and trailing mask state.

`readLiveryPayload()` reconstructs the tagged stream and extracts its artwork
and counters. `decodeLiverySections()` builds the section trees, and
`importCLivery()` converts them to the editor scene. Raw Motorsport groups use
`readCGroupPayload()` and `importCGroupNested()`.

## Native Export Targets

The PGS exporter selects the metadata envelope, artwork section count, counters,
paint layout, header version, and compression framing for the destination game.
FH6 retains its eleven artwork sections and hashed paint descriptors.

| Target | Header version | Livery information version | Artwork sections | Paint records |
| --- | --- | --- | --- | --- |
| FH2 | 0 | 5 | 6 | 20 |
| FH3 | 1 | 6 | 6 | 30 |
| FM5 | 0 | 2 | 6 | 20 |
| FM6 | 0 | 5 | 6 | 30 |
| FM7 | 2 | 7 | 6 | 30 |
| FH4 | 4 | 17 | 6 | 34 |
| FH5 | 6 | 18 | 6 | 34 |
| FH6 | 7 | 19 | 11 | Variable (hashed) |
| FM2023 | 9 | 19 | 6 | 34 |

FH6 stores a counted table of 27-byte hashed paint records. Its record count
depends on the material bindings carried by the livery.

The table specifies the selected export profiles. Import also accepts older
layouts present in game saves, including FH5 header version 4 with livery
information version 17 and Motorsport 2023 livery information version 18.

The six body sections are Front, Back, Top, Left, Right, and Spoiler. Window
artwork is discarded for every target except FH6. The non-FH6 targets write
seven section counters; FH6 writes eleven section counters and one trailing
counter. FM5 stores counters and
paint directly after its length-delimited artwork; later formats frame these
blocks with `yrvl` tags. FH2, FH3, FM5, FM6, and FM7 use bare `01` shape markers in embedded
artwork. Their populated sections use seventeen-byte boundary frames. The
metadata-guided recovery walk can supply virtual terminal padding when physical
framing is incomplete. Legacy formats without editable logo semantics retain
opaque records as physical occupancy during decoding and omit them from export.
Their group frames can contain an unmarked sixteen-byte transform followed by
a control byte and markerless group header. The decoder resolves this layout
before treating an arbitrary float byte as an opaque transform lead.
Livery information versions 5 through 7 contain a counted table of seven-byte
entries before the artwork length. Versions 6 and 7 place a state byte before
the count; observed values include `00` and `01`. The exporter selects `01`.
Compatible entry tables are retained, while unmatched tables are exported empty.
FH5 appends a zero `u32` after the final `yrvl` tag.
FM2023 uses independently compressed blocks of at most 32768 uncompressed bytes.

Positional paint chunks contain a status byte, fixed-size 40-byte records, and a
two-byte tail. Unchanged imported records retain their native bytes; mapped edits
patch color, selector, and finish fields. New paint uses native inherited defaults
and the mapped editor bindings. Unmapped records retain their source data.
Finish identifiers are interpreted in the source title's catalog and translated
for the destination. Export rejects finishes unavailable in that destination.
These formats have no material-hash field.
Older targets rebuild group frames with relative placements and a shared section
scale. FH4, FH5, and FM2023 retain compatible native framing; placement and color edits
are patched relative to the retained group transforms.
The modern section builder decodes rebuilt artwork and checks its occupancy
before export. Regression tests compare imported and exported scene geometry
independently of the selected record framing.
