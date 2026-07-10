# FM2023 Group Import — Implementation Plan

Add FM2023 livery group import to the editor. The feature reads an FM2023
`data` file (multi-container format), extracts the embedded gyvl artwork, and
imports it as an **editable group project** (not read-only like the existing
FH6 livery import). All FH6 export paths are unchanged.

---

## Background: FM2023 File Format

An FM2023 livery folder contains a `header` file and a `data` file. The `data`
file packs **three zlib-wrapped containers** concatenated:

```
+0x0000  Container 1: C_livery       (vlrc + yrvl + gyvl chunks)
+0x1DF2  Container 2: C_group body   (raw section stream, no header)
+0x49D7  Container 3: yrvl metadata  (stats + descriptor + terminator)
```

- **Container 1** is a standard C_livery: `vlrc` root header, `yrvl` info
  record, `gyvl` embedded C_group. The gyvl body (the artwork) starts at
  `gyvl+0x15` and extends to the end of the decompressed buffer (no inline
  yrvl statistics).
- **Container 2** is a raw C_group body with no wrapper — same shapes/groups
  as Container 1 (19 of 20 shape IDs match), likely an edited/working copy.
- **Container 3** holds the yrvl metadata chunks: **7** section decal counts
  (not 11 as in FH6), the descriptor table, and the terminator.

Key differences from FH6:

| Feature | FH6 | FM2023 |
|---------|-----|--------|
| File name | `C_livery` | `data` |
| Containers | 1 | 3 |
| Section count | 11 | 7 |
| yrvl stats location | Inline after gyvl | Separate container (#3) |
| yrvl stats size | 52 B (11 × u32) | 32 B (7 × u32) |
| yrvl terminator | `00 00 00 00` | `00 00 00 3f` |
| Header version | 7 | 9 |

---

## Phase 1 — Core Codec: `inflateFirstContainer()`

**Files:** `src/core/cgroup_codec.h`, `src/core/cgroup_codec.cpp`

### Problem

`inflateContainer()` reads the entire remaining file as the zlib payload and
checks `compressedSize == fileSize - 8`. This **throws** on the FM2023
multi-container file because `fileSize - 8` includes Containers 2 and 3.

### Solution

Add `inflateFirstContainer()` — reads only `compressedSize` bytes after the
8-byte header, regardless of trailing data:

```cpp
// cgroup_codec.h — add declaration
QByteArray inflateFirstContainer(const QByteArray &wrapped);
```

```cpp
// cgroup_codec.cpp — add implementation
QByteArray inflateFirstContainer(const QByteArray &wrapped)
{
    if (wrapped.size() < 8) {
        throw std::runtime_error("container is shorter than wrapper header");
    }
    const quint32 compressedSize = readLeU32(wrapped, 0);
    const quint32 decompressedSize = readLeU32(wrapped, 4);
    const QByteArray compressed = wrapped.mid(8, compressedSize);

    QByteArray output;
    output.resize(static_cast<int>(decompressedSize));
    uLongf destinationSize = decompressedSize;
    const int status = uncompress(
        reinterpret_cast<Bytef *>(output.data()),
        &destinationSize,
        reinterpret_cast<const Bytef *>(compressed.constData()),
        static_cast<uLong>(compressed.size()));
    if (status != Z_OK || destinationSize != decompressedSize) {
        throw std::runtime_error("zlib decompression failed for container");
    }
    return output;
}
```

Refactor `inflateContainer()` to delegate to `inflateFirstContainer()` with
an added size guard:

```cpp
QByteArray inflateContainer(const QByteArray &wrapped)
{
    if (wrapped.size() < 8) {
        throw std::runtime_error("container is shorter than wrapper header");
    }
    const quint32 compressedSize = readLeU32(wrapped, 0);
    if (compressedSize != static_cast<quint32>(wrapped.size()) - 8) {
        throw std::runtime_error(
            "container compressed-size header does not match file size");
    }
    return inflateFirstContainer(wrapped);
}
```

---

## Phase 2 — New File: `fm_codec.h/cpp`

**Files:** `src/core/fm_codec.h` (new), `src/core/fm_codec.cpp` (new)

### 2.1 Header: `fm_codec.h`

```cpp
#pragma once

#include "core_types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace fh6 {

// FM2023 section slot definitions (7 panels — no windows).
inline constexpr int kFM2023SectionCount = 7;
extern const char *kFM2023SlotNames[7];
extern const double kFM2023SlotRotations[7];

// Decompressed FM2023 livery payload, with gyvl body and section counts.
struct FM2023LiveryPayload {
    QByteArray container1;         // full decompressed Container 1
    int gyvlOffset = 0;            // offset of "gyvl" within container1
    QByteArray gyvlBody;           // section stream (gyvl+0x15 -> EOF)
    QVector<int> sectionCounts;    // 7 per-section decal counts
};

// Read an FM2023 data file (or folder containing one), inflate Container 1,
// locate the gyvl body, and parse section counts from Container 3.
FM2023LiveryPayload readFM2023Payload(const QString &folderOrFile);

// Import the gyvl body as an editable group project.
// Unlike importCLivery() which returns a read-only project with section tabs,
// this returns a fully editable project with all decals in a single tree.
Project importFM2023Group(const QString &folderOrFile);

} // namespace fh6
```

### 2.2 Implementation: `fm_codec.cpp`

```cpp
#include "fm_codec.h"
#include "binary_io.h"
#include "cgroup_codec.h"
#include "vinyl_decoder.h"
#include "project_codec.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <stdexcept>

namespace fh6 {
namespace {

QString resolveFMDataPath(const QString &folderOrFile)
{
    QFileInfo info(folderOrFile);
    if (info.isDir()) {
        return QDir(folderOrFile).filePath(QStringLiteral("data"));
    }
    return folderOrFile;
}

} // namespace

FM2023LiveryPayload readFM2023Payload(const QString &folderOrFile)
{
    const QString path = resolveFMDataPath(folderOrFile);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            ("could not open FM2023 data: " + path).toStdString());
    }
    const QByteArray fileBytes = file.readAll();
    file.close();

    if (fileBytes.size() < 8) {
        throw std::runtime_error("FM2023 data file is too small");
    }

    // --- Container 1: C_livery ---
    const QByteArray raw1 = inflateFirstContainer(fileBytes);

    FM2023LiveryPayload payload;
    payload.container1 = raw1;

    const int gyvl = raw1.indexOf(QByteArray("gyvl", 4));
    if (gyvl < 0) {
        throw std::runtime_error(
            "FM2023 data has no gyvl chunk in Container 1");
    }
    payload.gyvlOffset = gyvl;

    const int bodyStart = gyvl + 0x15;
    if (bodyStart >= raw1.size()) {
        throw std::runtime_error(
            "FM2023 gyvl body start is beyond container size");
    }
    payload.gyvlBody = raw1.mid(bodyStart);

    // --- Container 3: yrvl metadata ---
    const quint32 compSize1 = detail::readLeU32(fileBytes, 0);
    const qint64 c2Off = 8 + compSize1;

    if (c2Off + 8 > fileBytes.size()) {
        payload.sectionCounts = QVector<int>(kFM2023SectionCount, 0);
        return payload;
    }
    const quint32 compSize2 = detail::readLeU32(
        fileBytes, static_cast<int>(c2Off));
    const qint64 c3Off = c2Off + 8 + compSize2;

    if (c3Off + 8 > fileBytes.size()) {
        payload.sectionCounts = QVector<int>(kFM2023SectionCount, 0);
        return payload;
    }

    const QByteArray raw3 = inflateFirstContainer(
        fileBytes.mid(static_cast<int>(c3Off)));

    const int yrvlStats = raw3.indexOf(QByteArray("yrvl", 4));
    if (yrvlStats < 0) {
        payload.sectionCounts = QVector<int>(kFM2023SectionCount, 0);
        return payload;
    }

    payload.sectionCounts.reserve(kFM2023SectionCount);
    for (int i = 0; i < kFM2023SectionCount; ++i) {
        const int off = yrvlStats + 4 + i * 4;
        if (off + 4 <= raw3.size()) {
            payload.sectionCounts.push_back(
                static_cast<int>(detail::readLeU32(raw3, off)));
        } else {
            payload.sectionCounts.push_back(0);
        }
    }

    return payload;
}

Project importFM2023Group(const QString &folderOrFile)
{
    const FM2023LiveryPayload payload = readFM2023Payload(folderOrFile);

    // Use parameterized buildLiverySections with FM2023 slot definitions
    const QVector<LiverySection> sections =
        VinylTreeDecoder{}.buildLiverySections(
            payload.gyvlBody,
            payload.sectionCounts,
            kFM2023SlotNames,
            kFM2023SlotRotations,
            kFM2023SectionCount);

    // Build an editable project (isLivery=false)
    Project project;
    const QFileInfo info(folderOrFile);
    if (info.isDir()) {
        project.name = info.fileName();
        project.sourceFolder = info.absoluteFilePath();
    } else {
        project.name = info.absoluteDir().dirName();
        project.sourceFolder = info.absoluteDir().absolutePath();
    }

    project.sourceDecPrefix = payload.container1.left(0x1d);

    const QString headerPath = QDir(project.sourceFolder)
        .filePath(QStringLiteral("header"));
    QFile hf(headerPath);
    if (hf.open(QIODevice::ReadOnly)) {
        project.sourceHeader = hf.readAll();
    }

    int shapeIndex = 0;
    int groupIndex = 0;

    // importShape / importGroup lambdas follow importCGroupNested pattern
    // (see project_codec.cpp lines 503-571 for the full implementation).

    for (const LiverySection &section : sections) {
        if (!section.populated) continue;

        ++groupIndex;
        const QString sectionId = QStringLiteral("group-%1")
            .arg(groupIndex, 4, 10, QLatin1Char('0'));
        LayerGroup sectionGroup;
        sectionGroup.id = sectionId;
        sectionGroup.name = section.name;
        sectionGroup.isLiverySection = true;
        sectionGroup.liverySectionSlot = section.slot;
        sectionGroup.sourceAbsPos = section.absPos;
        project.groups.push_back(sectionGroup);
        const int sectionIdx = project.groups.size() - 1;

        const Matrix3 sectionMatrix = nodeMatrix(section.subtree);
        const bool sectionMask = section.subtree.isMask;
        for (const VinylItem &item : section.subtree.items) {
            QString childId;
            if (item.isShape()) {
                childId = importShape(
                    std::get<VinylShape>(item.value),
                    sectionMatrix, sectionMask,
                    shapeIndex, project);
            } else {
                childId = importGroup(
                    *std::get<VinylGroupPtr>(item.value),
                    sectionMatrix, sectionMask,
                    sectionId, QString(),
                    groupIndex, shapeIndex, project);
            }
            project.groups[sectionIdx].childIds.push_back(childId);
        }
        project.groups[sectionIdx].sourceChildIds =
            project.groups[sectionIdx].childIds;
        project.rootChildIds.push_back(sectionId);
    }

    return project;
}

} // namespace fh6
```

---

## Phase 3 — Parameterized `buildLiverySections()`

**Files:** `src/core/vinyl_decoder.h`, `src/core/vinyl_decoder.cpp`

### Current state

`kLiverySlots[11]` is a file-local array. `buildLiverySections()` iterates 11
slots and indexes into it. The slot count and definitions are hardcoded.

### Changes

**`vinyl_decoder.h`** — Move slot definitions to the header and add an overload:

```cpp
struct LiverySlotDef {
    const char *name;
    double rotationDeg;
};

// FH6: 11 slots
extern const LiverySlotDef kFH6LiverySlots[11];
inline constexpr int kFH6SectionCount = 11;

// FM2023: 7 slots
extern const LiverySlotDef kFM2023LiverySlots[7];
inline constexpr int kFM2023SectionCount = 7;

class VinylTreeDecoder {
public:
    // Existing: FH6 11-slot walk (delegates to new overload).
    QVector<LiverySection> buildLiverySections(
        const QByteArray &body,
        const QVector<int> &sectionCounts) const;

    // New: parameterized walk.
    QVector<LiverySection> buildLiverySections(
        const QByteArray &body,
        const QVector<int> &sectionCounts,
        const LiverySlotDef *slots,
        int slotCount) const;

    // ... rest unchanged
};
```

**`vinyl_decoder.cpp`** — Define the slot tables at namespace scope, then
reimplement the existing method as a wrapper:

```cpp
const LiverySlotDef kFH6LiverySlots[11] = {
    {"Front", 0.0},   {"Back", 0.0},   {"Top", 0.0},
    {"Left", 180.0},  {"Right", 90.0}, {"Spoiler", -90.0},
    {"FrontWindshield", 90.0}, {"BackWindshield", 0.0}, {"TopWindow", 0.0},
    {"LeftWindow", 180.0}, {"RightWindow", 0.0},
};

const LiverySlotDef kFM2023LiverySlots[7] = {
    {"Front", 0.0},  {"Back", 0.0},  {"Top", 0.0},
    {"Left", 180.0}, {"Right", 90.0},{"Spoiler", -90.0},
    {"FrontWindshield", 90.0},
};
```

The new overload body is identical to the current `buildLiverySections()`
implementation but uses `slotCount` instead of `kLiverySectionCount` and
`slots` instead of `kLiverySlots`. Empty-slot scaffold size (23) and remnant
size (18) are shared — no change needed.

---

## Phase 4 — Project Codec Entry Point

**Files:** `src/core/project_codec.h`

Add:

```cpp
Project importFM2023Group(const QString &folderOrFile);
```

The implementation lives in `fm_codec.cpp` (Phase 2.2). `project_codec.cpp`
already has `nodeMatrix()`, `importShape()`, and `importGroup()` in its
anonymous namespace; the FM import lambdas follow the same pattern. If the
`importShape`/`importGroup` lambdas are reused, extract them into static
helper functions in a shared internal header.

---

## Phase 5 — GUI Integration

**File:** `src/gui/app/main_window.cpp`

### 5.1 Detection in `importAny()`

Insert FM2023 detection before the existing FH6 checks:

```cpp
bool MainWindow::importAny(const QString &path, QString *error)
{
    const QFileInfo info(path);

    // --- FM2023 detection ---
    QString fmDataPath;
    if (info.isDir()) {
        fmDataPath = QDir(path).filePath(QStringLiteral("data"));
    } else if (info.fileName().compare(
                   QStringLiteral("data"), Qt::CaseInsensitive) == 0) {
        fmDataPath = path;
    }
    if (!fmDataPath.isEmpty()) {
        QFile f(fmDataPath);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray hdr = f.read(8);
            bool isFM2023 = false;
            if (hdr.size() == 8) {
                quint32 compSize = fh6::detail::readLeU32(hdr, 0);
                isFM2023 = f.size() > static_cast<qint64>(8 + compSize + 8);
            }
            f.close();
            if (isFM2023) {
                rememberImportDirectory(path, QStringLiteral("cgroupFolder"));
                return loadFM2023Group(path, error);
            }
        }
    }
    // --- end FM2023 detection ---

    bool isLivery = false;
    // ... existing FH6 logic unchanged ...
}
```

### 5.2 `classifyExternalDropPath()`

Add `"data"` to the file-name checks:

```cpp
if (info.fileName().compare(QStringLiteral("data"), Qt::CaseInsensitive) == 0) {
    return ExternalDropKind::CGroup;
}
```

Also add folder detection:

```cpp
if (info.isDir()) {
    const QDir dir(info.absoluteFilePath());
    if (QFileInfo(dir.filePath(QStringLiteral("C_group"))).isFile()
        || QFileInfo(dir.filePath(QStringLiteral("C_livery"))).isFile()
        || QFileInfo(dir.filePath(QStringLiteral("data"))).isFile()) {
        return ExternalDropKind::CGroup;
    }
    return ExternalDropKind::Unsupported;
}
```

### 5.3 `loadFM2023Group()` method

```cpp
bool MainWindow::loadFM2023Group(const QString &path, QString *error)
{
    try {
        if (!confirmDiscardUnsavedChanges()) {
            return false;
        }
        fh6::Project project = fh6::importFM2023Group(path);
        setProject(std::move(project));
        return true;
    } catch (const std::exception &e) {
        if (error) *error = QString::fromStdString(e.what());
        return false;
    }
}
```

### 5.4 File dialog filter

```cpp
// line 2064, update the filter string:
QStringLiteral("Forza source (C_group C_livery data);;All files (*)")
```

---

## Phase 6 — FM2023 Header Codec (Optional / Follow-up)

**Files:** `src/core/header_codec.h`, `src/core/header_codec.cpp`

Add `parseFM2023Header()` for the version-9 header format. Not required for
the initial group import — the header is read as opaque bytes.

---

## Phase 7 — Tests

### Unit tests

1. **`inflateFirstContainer`**: Concatenate 3 valid zlib containers. Verify
   only the first container's decompressed data is returned.

2. **`readFM2023Payload`**: Feed the `tmp/data` file. Verify:
   - `container1` size = 32768
   - gyvl found at offset 0x168
   - gyvl body size = 32387
   - 7 section counts match the expected values

3. **`importFM2023Group`**: Full end-to-end with `tmp/data` + `tmp/header`.
   Verify:
   - `project.isLivery == false` (editable)
   - Total shapes match Container 1's count (945)
   - Groups are created with correct hierarchy
   - Section groups are present as top-level entries
   - `project.sourceDecPrefix` is non-empty

4. **`buildLiverySections` with 7 slots**: Walk the FM2023 gyvl body with
   FM2023 slot definitions. Verify 7 sections returned, populated counts
   match Container 3 stats, no parse errors.

---

## File Change Summary

| File | Action |
|------|--------|
| `src/core/fm_codec.h` | **New** — FM2023 structs and function declarations |
| `src/core/fm_codec.cpp` | **New** — `inflateFirstContainer`, `readFM2023Payload`, `importFM2023Group` |
| `src/core/cgroup_codec.h` | Add `inflateFirstContainer()` declaration |
| `src/core/cgroup_codec.cpp` | Add implementation, refactor `inflateContainer()` to delegate |
| `src/core/vinyl_decoder.h` | Add `LiverySlotDef`, extern slot tables, parameterized `buildLiverySections()` |
| `src/core/vinyl_decoder.cpp` | Move constants to header scope, add 7-slot overload, keep 11-slot wrapper |
| `src/core/project_codec.h` | Add `importFM2023Group()` declaration |
| `src/gui/app/main_window.cpp` | FM2023 detection in `importAny()`, `classifyExternalDropPath()`, dialog filter |

---

## Risk Areas

1. **`inflateContainer()` regression**: The refactored version must still
   throw on malformed single-container files. The size guard is preserved.

2. **Privacy policy**: `enforcePrivacyPolicyForCLivery()` checks
   `livery.raw[8] == 1`. FM2023 locked flag may be at a different offset.
   Do not enforce privacy on FM2023 imports initially.

3. **Container 2 (working copy)**: This plan ignores Container 2. If the
   working copy diverges from Container 1, add an option to choose between
   Container 1 (original) and Container 2 (edited) as a follow-up.

4. **Empty livery**: All-zero section counts produce 7 x 23 = 161 B of
   scaffold data. `buildLiverySections()` with `target <= 0` skips each
   slot — verify this path does not throw.

5. **Backward compatibility**: `importCLivery()` and all FH6 detection paths
   are unchanged. A folder with a `C_livery` file still routes to the FH6
   codec.

---

## Implementation Order

| Step | Depends On | Effort |
|------|-----------|--------|
| 1. `inflateFirstContainer()` + test | — | Small |
| 2. Parameterized `buildLiverySections()` | — | Medium |
| 3. `readFM2023Payload()` + test | 1 | Medium |
| 4. `importFM2023Group()` + test | 2, 3 | Medium |
| 5. GUI detection + routing | 4 | Small |
| 6. FM2023 header codec (optional) | — | Small |
