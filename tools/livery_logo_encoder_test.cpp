#include "binary_io.h"
#include "layer.h"
#include "livery_codec.h"
#include "project_codec.h"
#include "vinyl_decoder.h"

#include <QtCore>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <stdexcept>

namespace {

constexpr int kSectionCount = 11;
constexpr quint16 kVectorShapeId = 101;

struct ArtworkStats {
    QVector<quint32> logoIds;
    int vectors = 0;
    int groups = 0;
};

struct RawPlacement {
    quint16 shapeId = 0;
    double x = 0.0;
    double y = 0.0;
    bool mask = false;
};

std::unique_ptr<fls::scene::Shape> makeVector(const QString &id, double x, double y) {
    auto shape = std::make_unique<fls::scene::Shape>();
    shape->id = id;
    shape->setVectorShape(kVectorShapeId);
    shape->x = x;
    shape->y = y;
    shape->scaleX = 0.25;
    shape->scaleY = 0.25;
    return shape;
}

std::unique_ptr<fls::scene::Shape> makeLogo(const QString &id, quint32 rasterId,
                                            double x, double y, double scale = 0.35) {
    auto logo = std::make_unique<fls::scene::Shape>();
    logo->id = id;
    logo->name = QStringLiteral("Logo %1").arg(rasterId);
    logo->setRasterShape(rasterId);
    logo->sourceLogoId = static_cast<quint16>(0x8000u | rasterId);
    logo->x = x;
    logo->y = y;
    logo->scaleX = scale;
    logo->scaleY = scale;
    return logo;
}

fls::scene::Group *appendSection(fls::Project &project, int slot) {
    auto section = std::make_unique<fls::scene::Group>();
    section->id = QStringLiteral("section-%1").arg(slot);
    section->name = QString::fromLatin1(fls::kFH6LiverySlots[slot].name);
    section->isLiverySection = true;
    section->liverySectionSlot = slot;
    auto *result = section.get();
    project.root->append(std::move(section));
    return result;
}

fls::Project makeEncoderProject() {
    fls::Project project;
    project.name = QStringLiteral("Logo encoder test");
    project.isLivery = true;
    project.carId = 1069;
    project.root = std::make_unique<fls::scene::Group>();
    project.root->id = QStringLiteral("__root__");

    fls::scene::Group *front = appendSection(project, 0);
    front->append(makeLogo(QStringLiteral("front-logo-first"), 12, -72.0, -24.0));
    front->append(makeVector(QStringLiteral("front-vector"), 4.0, 18.0));
    auto sponsorGroup = std::make_unique<fls::scene::Group>();
    sponsorGroup->id = QStringLiteral("front-sponsor-group");
    sponsorGroup->x = 48.0;
    sponsorGroup->append(makeLogo(QStringLiteral("front-group-logo"), 47, -18.0, 12.0));
    sponsorGroup->append(makeVector(QStringLiteral("front-group-vector"), 22.0, -14.0));
    front->append(std::move(sponsorGroup));

    fls::scene::Group *top = appendSection(project, 2);
    auto logoGroup = std::make_unique<fls::scene::Group>();
    logoGroup->id = QStringLiteral("top-logo-group");
    logoGroup->append(makeLogo(QStringLiteral("top-logo-a"), 114, -34.0, 0.0));
    logoGroup->append(makeLogo(QStringLiteral("top-logo-b"), 129, 34.0, 0.0));
    top->append(std::move(logoGroup));

    return project;
}

fls::Project makeVectorCounterProject() {
    fls::Project project;
    project.name = QStringLiteral("Source counter test");
    project.isLivery = true;
    project.carId = 1069;
    project.root = std::make_unique<fls::scene::Group>();
    project.root->id = QStringLiteral("__root__");
    fls::scene::Group *top = appendSection(project, 2);
    top->append(makeVector(QStringLiteral("counter-vector-a"), -24.0, 0.0));
    top->append(makeVector(QStringLiteral("counter-vector-b"), 24.0, 0.0));
    return project;
}

void writeLeU32(QByteArray &bytes, int offset, quint32 value) {
    bytes[offset] = static_cast<char>(value & 0xff);
    bytes[offset + 1] = static_cast<char>((value >> 8) & 0xff);
    bytes[offset + 2] = static_cast<char>((value >> 16) & 0xff);
    bytes[offset + 3] = static_cast<char>((value >> 24) & 0xff);
}

void collectArtwork(const fls::VinylGroup &group, ArtworkStats &stats) {
    for (const fls::VinylItem &item : group.items) {
        if (item.isShape()) {
            const fls::VinylShape &shape = std::get<fls::VinylShape>(item.value);
            if (shape.isLogo) {
                stats.logoIds.push_back(shape.rasterId);
            } else {
                ++stats.vectors;
            }
        } else {
            ++stats.groups;
            collectArtwork(*std::get<fls::VinylGroupPtr>(item.value), stats);
        }
    }
}

const fls::VinylShape *firstShape(const fls::VinylGroup &group) {
    for (const fls::VinylItem &item : group.items) {
        if (item.isShape()) {
            return &std::get<fls::VinylShape>(item.value);
        }
        if (const fls::VinylShape *shape = firstShape(*std::get<fls::VinylGroupPtr>(item.value))) {
            return shape;
        }
    }
    return nullptr;
}

void collectRawPlacements(const fls::VinylGroup &group, double parentX, double parentY,
                          bool parentMask, QVector<RawPlacement> &out) {
    const double groupX = parentX + group.px;
    const double groupY = parentY + group.py;
    const bool groupMask = parentMask || group.isMask;
    for (const fls::VinylItem &item : group.items) {
        if (item.isShape()) {
            const fls::VinylShape &shape = std::get<fls::VinylShape>(item.value);
            out.push_back({shape.shapeId, groupX + shape.posX, groupY + shape.posY,
                           groupMask || shape.isMask});
        } else {
            collectRawPlacements(*std::get<fls::VinylGroupPtr>(item.value), groupX, groupY,
                                 groupMask, out);
        }
    }
}

int sceneLogoCount(const fls::scene::Layer &layer) {
    if (layer.kind() == fls::scene::LayerKind::Shape) {
        return static_cast<const fls::scene::Shape &>(layer).raster ? 1 : 0;
    }
    if (layer.kind() != fls::scene::LayerKind::Group) {
        return 0;
    }
    int count = 0;
    for (const auto &child : static_cast<const fls::scene::Group &>(layer).children) {
        count += sceneLogoCount(*child);
    }
    return count;
}

fls::scene::Group *section(fls::Project &project, int slot) {
    if (!project.root) {
        return nullptr;
    }
    for (const auto &child : project.root->children) {
        if (child->kind() != fls::scene::LayerKind::Group) {
            continue;
        }
        auto *group = static_cast<fls::scene::Group *>(child.get());
        if (group->isLiverySection && group->liverySectionSlot == slot) {
            return group;
        }
    }
    return nullptr;
}

bool testLogoEncoding() {
    fls::Project project = makeEncoderProject();
    std::array<int, kSectionCount> counts{};
    const QByteArray gyvl = fls::buildLiveryGyvl(project, &counts);
    if (counts[0] != 4 || counts[2] != 2) {
        qCritical() << "logo placements were not included in section counts";
        return false;
    }
    QVector<int> decodedCounts;
    decodedCounts.reserve(kSectionCount);
    for (const int count : counts) {
        decodedCounts.push_back(count);
    }
    const QByteArray body = gyvl.mid(0x15);
    const QVector<fls::LiverySection> sections =
        fls::buildLiverySections(body, decodedCounts);
    if (sections.size() != kSectionCount) {
        qCritical() << "encoded livery did not decode into all sections";
        return false;
    }
    ArtworkStats front;
    ArtworkStats top;
    collectArtwork(sections[0].subtree, front);
    collectArtwork(sections[2].subtree, top);
    std::sort(front.logoIds.begin(), front.logoIds.end());
    std::sort(top.logoIds.begin(), top.logoIds.end());
    if (front.logoIds != QVector<quint32>{12, 47} || front.vectors != 2
        || top.logoIds != QVector<quint32>{114, 129} || top.groups == 0) {
        qCritical() << "encoded logo identities or hierarchy did not round trip";
        return false;
    }
    const fls::VinylShape *firstFrontShape = firstShape(sections[0].subtree);
    if (firstFrontShape == nullptr || !firstFrontShape->isLogo
        || firstFrontShape->marker != QByteArray("\x02", 1)) {
        qCritical() << "first-child logo did not use its bare record";
        return false;
    }
    QByteArray expectedTopRemnant(9, '\0');
    fls::detail::appendLeFloat(expectedTopRemnant, 1.0f);
    fls::detail::appendLeFloat(expectedTopRemnant, 0.0f);
    expectedTopRemnant.append('\0');
    if (!body.contains(expectedTopRemnant)) {
        qCritical() << "grouped terminal metadata was not encoded canonically";
        return false;
    }
    const QByteArray payload = fls::encodeCLiveryPayload(project);
    const fls::LiveryPayload decodedPayload = fls::parseInflatedLiveryPayload(payload);
    if (decodedPayload.sectionCounts.size() != kSectionCount
        || decodedPayload.sectionCounts[0] != 4 || decodedPayload.sectionCounts[2] != 2) {
        qCritical() << "full livery payload did not retain logo section counts";
        return false;
    }
    return true;
}

bool testLogoShapeLimit() {
    if (!fls::kEnforceLiveryShapeLimits) {
        return true;
    }
    fls::Project project;
    project.isLivery = true;
    project.root = std::make_unique<fls::scene::Group>();
    fls::scene::Group *front = appendSection(project, 0);
    for (int i = 0; i < fls::liverySectionShapeLimit(0) - 1; ++i) {
        front->append(makeVector(QStringLiteral("limit-vector-%1").arg(i), 0.0, 0.0));
    }
    front->append(makeLogo(QStringLiteral("limit-logo-a"), 12, 0.0, 0.0));
    front->append(makeLogo(QStringLiteral("limit-logo-b"), 47, 0.0, 0.0));
    try {
        fls::buildLiveryGyvl(project);
    } catch (const std::runtime_error &) {
        return true;
    }
    qCritical() << "logo placements did not contribute to the livery shape limit";
    return false;
}

bool testPartialSourceRebuild() {
    constexpr int kTopSlot = 2;
    constexpr int kNativeLogicalCount = 5;
    fls::Project project = makeVectorCounterProject();
    QByteArray source = fls::encodeCLiveryPayload(project);
    const int gyvl = source.indexOf(QByteArray("gyvl", 4));
    const int stats = source.indexOf(QByteArray("yrvl", 4), gyvl + 4);
    if (gyvl < 0 || stats < 0) {
        qCritical() << "counter test payload is missing livery chunks";
        return false;
    }
    writeLeU32(source, stats + 4 + kTopSlot * 4, kNativeLogicalCount);
    project.liverySource = source;

    std::array<int, kSectionCount> counts{};
    const QByteArray rebuilt = fls::buildLiveryGyvl(project, &counts);
    const QByteArray original = source.mid(gyvl, stats - gyvl);
    if (counts[kTopSlot] != 2 || rebuilt == original) {
        qCritical() << "partially decoded source section was not rebuilt from editable artwork";
        return false;
    }
    return true;
}

bool testNestedMaskGroupTransforms() {
    fls::Project project;
    project.isLivery = true;
    project.carId = 1069;
    project.root = std::make_unique<fls::scene::Group>();
    project.root->id = QStringLiteral("__root__");
    fls::scene::Group *top = appendSection(project, 2);

    auto outer = std::make_unique<fls::scene::Group>();
    outer->id = QStringLiteral("outer");
    auto ordinary = makeVector(QStringLiteral("ordinary"), -120.0, -30.0);
    ordinary->setVectorShape(101);
    outer->append(std::move(ordinary));

    auto firstMask = std::make_unique<fls::scene::Group>();
    firstMask->id = QStringLiteral("first-mask");
    auto secondMask = std::make_unique<fls::scene::Group>();
    secondMask->id = QStringLiteral("second-mask");
    auto nestedA = makeVector(QStringLiteral("nested-a"), -24.0, 16.0);
    nestedA->setVectorShape(102);
    nestedA->mask = true;
    secondMask->append(std::move(nestedA));
    auto nestedB = makeVector(QStringLiteral("nested-b"), 28.0, -12.0);
    nestedB->setVectorShape(127);
    nestedB->mask = true;
    secondMask->append(std::move(nestedB));
    firstMask->append(std::move(secondMask));
    auto maskedSibling = makeVector(QStringLiteral("masked-sibling"), 76.0, 34.0);
    maskedSibling->setVectorShape(128);
    maskedSibling->mask = true;
    firstMask->append(std::move(maskedSibling));
    outer->append(std::move(firstMask));
    top->append(std::move(outer));

    std::array<int, kSectionCount> counts{};
    const QByteArray body = fls::buildLiveryGyvl(project, &counts).mid(0x15);
    QVector<int> decodedCounts;
    decodedCounts.reserve(kSectionCount);
    for (int count : counts) {
        decodedCounts.push_back(count);
    }
    const QVector<fls::LiverySection> sections = fls::buildLiverySections(body, decodedCounts);
    QVector<RawPlacement> placements;
    collectRawPlacements(sections[2].subtree, 0.0, 0.0, false, placements);
    const QVector<RawPlacement> expected = {
        {101, -120.0, -30.0, false},
        {102, -24.0, 16.0, true},
        {127, 28.0, -12.0, true},
        {128, 76.0, 34.0, true},
    };
    if (placements.size() != expected.size()) {
        qCritical() << "nested mask groups changed the artwork count";
        return false;
    }
    for (int i = 0; i < expected.size(); ++i) {
        if (placements[i].shapeId != expected[i].shapeId
            || std::abs(placements[i].x - expected[i].x) > 0.001
            || std::abs(placements[i].y - expected[i].y) > 0.001
            || placements[i].mask != expected[i].mask) {
            qCritical() << "nested mask group transform did not round trip at" << i;
            return false;
        }
    }
    return true;
}

fls::scene::Group *findSceneShapePair(fls::scene::Layer &layer, quint16 first, quint16 second) {
    if (layer.kind() != fls::scene::LayerKind::Group) {
        return nullptr;
    }
    auto &group = static_cast<fls::scene::Group &>(layer);
    if (group.children.size() >= 2
        && group.children[0]->kind() == fls::scene::LayerKind::Shape
        && group.children[1]->kind() == fls::scene::LayerKind::Shape
        && static_cast<fls::scene::Shape &>(*group.children[0]).shapeId == first
        && static_cast<fls::scene::Shape &>(*group.children[1]).shapeId == second) {
        return &group;
    }
    for (const auto &child : group.children) {
        if (fls::scene::Group *match = findSceneShapePair(*child, first, second)) {
            return match;
        }
    }
    return nullptr;
}

const fls::VinylGroup *findRawShapePair(const fls::VinylGroup &group,
                                       quint16 first, quint16 second) {
    if (group.items.size() >= 2 && group.items[0].isShape() && group.items[1].isShape()
        && std::get<fls::VinylShape>(group.items[0].value).shapeId == first
        && std::get<fls::VinylShape>(group.items[1].value).shapeId == second) {
        return &group;
    }
    for (const fls::VinylItem &item : group.items) {
        if (!item.isShape()) {
            if (const fls::VinylGroup *match = findRawShapePair(
                    *std::get<fls::VinylGroupPtr>(item.value), first, second)) {
                return match;
            }
        }
    }
    return nullptr;
}

bool testDeletedSiblingSourceFraming() {
    fls::Project source;
    source.name = QStringLiteral("Deleted sibling framing test");
    source.isLivery = true;
    source.carId = 1069;
    source.root = std::make_unique<fls::scene::Group>();
    source.root->id = QStringLiteral("__root__");
    fls::scene::Group *top = appendSection(source, 2);

    auto container = std::make_unique<fls::scene::Group>();
    container->id = QStringLiteral("container");
    auto outer = std::make_unique<fls::scene::Group>();
    outer->id = QStringLiteral("outer");
    auto inner = std::make_unique<fls::scene::Group>();
    inner->id = QStringLiteral("inner");
    inner->append(makeVector(QStringLiteral("removed"), -40.0, 0.0));
    auto retained = makeVector(QStringLiteral("retained"), 0.0, 0.0);
    retained->setVectorShape(102);
    inner->append(std::move(retained));
    outer->append(std::move(inner));
    auto sibling = makeVector(QStringLiteral("sibling"), 40.0, 0.0);
    sibling->setVectorShape(127);
    outer->append(std::move(sibling));
    container->append(std::move(outer));
    auto containerSibling = makeVector(QStringLiteral("container-sibling"), 80.0, 0.0);
    containerSibling->setVectorShape(128);
    container->append(std::move(containerSibling));
    top->append(std::move(container));

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        qCritical() << "could not create the deletion test folder";
        return false;
    }
    fls::exportCLivery(source, temporary.path());
    fls::Project edited = fls::importCLivery(temporary.path());
    fls::scene::Group *sourcePair = edited.root
        ? findSceneShapePair(*edited.root, 101, 102)
        : nullptr;
    if (sourcePair == nullptr) {
        qCritical() << "could not locate the source sibling pair";
        return false;
    }
    sourcePair->takeAt(0);

    std::array<int, kSectionCount> counts{};
    const QByteArray body = fls::buildLiveryGyvl(edited, &counts).mid(0x15);
    QVector<int> decodedCounts;
    for (int count : counts) {
        decodedCounts.push_back(count);
    }
    const QVector<fls::LiverySection> sections = fls::buildLiverySections(body, decodedCounts);
    const fls::VinylGroup *pair = findRawShapePair(sections[2].subtree, 102, 127);
    if (pair == nullptr
        || std::get<fls::VinylShape>(pair->items[0].value).marker != QByteArray("\x00\x02", 2)
        || std::get<fls::VinylShape>(pair->items[1].value).marker != QByteArray("\x00\x02", 2)) {
        qCritical() << "shape framing did not retain source markers after deletion";
        return false;
    }
    return true;
}

fls::scene::Group &fixtureSection(fls::Project &project, int slot) {
    fls::scene::Group *target = section(project, slot);
    if (target == nullptr) {
        throw std::runtime_error("source livery is missing a section group");
    }
    return *target;
}

std::unique_ptr<fls::scene::Group> fixtureLogoGroup(const QString &id) {
    auto group = std::make_unique<fls::scene::Group>();
    group->id = id;
    group->name = QStringLiteral("Encoded Logo Group");
    group->x = 36.0;
    group->y = 22.0;
    group->rotation = 12.0;
    group->append(makeLogo(id + QStringLiteral("-a"), 12, -28.0, 0.0, 0.30));
    group->append(makeLogo(id + QStringLiteral("-b"), 47, 28.0, 0.0, 0.30));
    return group;
}

std::unique_ptr<fls::scene::Group> fixtureNestedMixedGroup(const QString &id) {
    auto outer = std::make_unique<fls::scene::Group>();
    outer->id = id;
    outer->name = QStringLiteral("Nested Mixed Group");
    outer->x = 42.0;
    outer->y = -18.0;
    outer->rotation = 17.0;
    outer->scaleX = 0.85;
    outer->scaleY = 1.15;
    outer->append(makeLogo(id + QStringLiteral("-outer-a"), 12, -56.0, -24.0, 0.28));

    auto inner = std::make_unique<fls::scene::Group>();
    inner->id = id + QStringLiteral("-inner");
    inner->name = QStringLiteral("Nested Mixed Inner Group");
    inner->x = 18.0;
    inner->y = 30.0;
    inner->rotation = -31.0;
    inner->scaleX = 1.2;
    inner->scaleY = 0.75;
    inner->append(makeVector(id + QStringLiteral("-inner-vector"), -32.0, 0.0));
    inner->append(makeLogo(id + QStringLiteral("-inner-logo"), 47, 12.0, 16.0, 0.24));
    outer->append(std::move(inner));

    outer->append(makeLogo(id + QStringLiteral("-outer-b"), 114, 62.0, 20.0, 0.32));
    return outer;
}

void generateFixture(const QString &sourceRoot, const QString &outputRoot,
                     const QString &sourceName, const QString &outputName, int expectedAddedLogos,
                     const std::function<void(fls::Project &)> &edit) {
    constexpr int kFixtureCarId = 1069;
    const QString sourceFolder = QDir(sourceRoot).filePath(sourceName);
    const QString outputFolder = QDir(outputRoot).filePath(outputName);
    fls::Project project = fls::importCLivery(sourceFolder);
    const int originalLogos = project.root ? sceneLogoCount(*project.root) : 0;
    edit(project);
    project.name = outputName;
    project.carId = kFixtureCarId;
    fls::exportCLivery(project, outputFolder);

    const fls::Project decoded = fls::importCLivery(outputFolder);
    const int decodedLogos = decoded.root ? sceneLogoCount(*decoded.root) : 0;
    if (decoded.carId != kFixtureCarId || decodedLogos != originalLogos + expectedAddedLogos) {
        throw std::runtime_error("generated fixture did not retain every appended logo");
    }
    qInfo().noquote() << QStringLiteral("WROTE %1 car=%2 logos=%3")
                             .arg(QDir::toNativeSeparators(outputFolder))
                             .arg(decoded.carId)
                             .arg(decodedLogos);
}

int generateFixtures(const QString &sourceRoot, const QString &outputRoot) {
    QDir().mkpath(outputRoot);
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Livery_1069_20260604101353"),
        QStringLiteral("Livery_Logo_00_Control_1069"), 0, [](fls::Project &) {});
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_01_SingleFront_1069"), 1, [](fls::Project &project) {
            fixtureSection(project, 0).append(
                makeLogo(QStringLiteral("fixture-single-front"), 12, 0.0, 0.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_02_TwoFront_1069"), 2, [](fls::Project &project) {
            fls::scene::Group &front = fixtureSection(project, 0);
            front.append(makeLogo(QStringLiteral("fixture-front-a"), 12, -36.0, 0.0));
            front.append(makeLogo(QStringLiteral("fixture-front-b"), 47, 36.0, 0.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("OneFront"),
        QStringLiteral("Livery_Logo_03_VectorThenLogo_1069"), 1, [](fls::Project &project) {
            fixtureSection(project, 0).append(
                makeLogo(QStringLiteral("fixture-after-vector"), 12, 52.0, 0.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("OneFront"),
        QStringLiteral("Livery_Logo_04_LogoThenVector_1069"), 1, [](fls::Project &project) {
            fixtureSection(project, 0).insert(
                0, makeLogo(QStringLiteral("fixture-before-vector"), 12, -52.0, 0.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_05_GroupedPairFront_1069"), 2, [](fls::Project &project) {
            fixtureSection(project, 0).append(
                fixtureLogoGroup(QStringLiteral("fixture-front-group")));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_06_TwoSections_1069"), 2, [](fls::Project &project) {
            fixtureSection(project, 0).append(
                makeLogo(QStringLiteral("fixture-front-boundary"), 12, 0.0, 0.0));
            fixtureSection(project, 1).append(
                makeLogo(QStringLiteral("fixture-back-boundary"), 47, 0.0, 0.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Livery_1069_20260604101353"),
        QStringLiteral("Livery_Logo_07_ExistingFrontAppend_1069"), 1, [](fls::Project &project) {
            fixtureSection(project, 0).append(
                makeLogo(QStringLiteral("fixture-existing-front"), 12, 72.0, -34.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Livery_1069_20260604101353"),
        QStringLiteral("Livery_Logo_08_ExistingBackGroup_1069"), 2, [](fls::Project &project) {
            fixtureSection(project, 1).append(
                fixtureLogoGroup(QStringLiteral("fixture-existing-back-group")));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_09_AlternatingFront_1069"), 3,
        [](fls::Project &project) {
            fls::scene::Group &front = fixtureSection(project, 0);
            front.append(makeLogo(QStringLiteral("fixture-alternating-logo-a"), 12, -96.0, -28.0));
            front.append(makeVector(QStringLiteral("fixture-alternating-vector-a"), -34.0, 24.0));
            front.append(makeLogo(QStringLiteral("fixture-alternating-logo-b"), 47, 24.0, -18.0));
            front.append(makeVector(QStringLiteral("fixture-alternating-vector-b"), 72.0, 30.0));
            front.append(makeLogo(QStringLiteral("fixture-alternating-logo-c"), 114, 118.0, -10.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_10_NestedMixedFront_1069"), 3,
        [](fls::Project &project) {
            fixtureSection(project, 0).append(
                fixtureNestedMixedGroup(QStringLiteral("fixture-nested-front")));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_11_SiblingGroupsFront_1069"), 5,
        [](fls::Project &project) {
            fls::scene::Group &front = fixtureSection(project, 0);
            front.append(fixtureLogoGroup(QStringLiteral("fixture-sibling-group-a")));
            front.append(makeLogo(QStringLiteral("fixture-sibling-direct"), 114, 0.0, -58.0));
            front.append(makeVector(QStringLiteral("fixture-sibling-vector"), 0.0, 54.0));
            front.append(fixtureLogoGroup(QStringLiteral("fixture-sibling-group-b")));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_12_AllSections_1069"), 11,
        [](fls::Project &project) {
            constexpr std::array<quint32, kSectionCount> logoIds = {
                12, 47, 114, 129, 12, 47, 114, 129, 12, 47, 114,
            };
            for (int slot = 0; slot < kSectionCount; ++slot) {
                fixtureSection(project, slot).append(makeLogo(
                    QStringLiteral("fixture-section-%1").arg(slot), logoIds[slot],
                    slot * 9.0 - 45.0, slot * 5.0 - 25.0, 0.22 + slot * 0.01));
            }
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_13_TransformStressTop_1069"), 5,
        [](fls::Project &project) {
            fls::scene::Group &top = fixtureSection(project, 2);
            auto logoA = makeLogo(QStringLiteral("fixture-transform-a"), 12, -128.0, -64.0, 0.18);
            logoA->rotation = 137.0;
            logoA->scaleY = 0.42;
            logoA->skew = 0.35;
            top.append(std::move(logoA));
            auto logoB = makeLogo(QStringLiteral("fixture-transform-b"), 47, 104.0, 72.0, 0.56);
            logoB->rotation = -223.0;
            logoB->scaleX = -0.56;
            logoB->opacity = 0.55;
            top.append(std::move(logoB));
            top.append(fixtureNestedMixedGroup(QStringLiteral("fixture-transform-nested")));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Empty"),
        QStringLiteral("Livery_Logo_14_MaskSequenceFront_1069"), 3,
        [](fls::Project &project) {
            fls::scene::Group &front = fixtureSection(project, 0);
            auto mask = makeLogo(QStringLiteral("fixture-logo-mask"), 12, -48.0, 0.0, 0.48);
            mask->mask = true;
            front.append(std::move(mask));
            front.append(makeLogo(QStringLiteral("fixture-after-logo-mask"), 47, 0.0, 0.0, 0.42));
            auto vectorMask = makeVector(QStringLiteral("fixture-vector-mask"), 48.0, 0.0);
            vectorMask->mask = true;
            front.append(std::move(vectorMask));
            front.append(makeLogo(QStringLiteral("fixture-after-vector-mask"), 114, 96.0, 0.0, 0.36));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Livery_1069_20260604101353"),
        QStringLiteral("Livery_Logo_15_ExistingTopAppend_1069"), 3,
        [](fls::Project &project) {
            fls::scene::Group &top = fixtureSection(project, 2);
            top.append(makeLogo(QStringLiteral("fixture-existing-top-a"), 12, -92.0, -46.0));
            top.append(makeVector(QStringLiteral("fixture-existing-top-vector"), 0.0, 62.0));
            top.append(makeLogo(QStringLiteral("fixture-existing-top-b"), 47, 92.0, -46.0));
            top.append(makeLogo(QStringLiteral("fixture-existing-top-c"), 114, 0.0, -96.0));
        });
    generateFixture(
        sourceRoot, outputRoot, QStringLiteral("Livery_1069_20260604101353"),
        QStringLiteral("Livery_Logo_16_ExistingTopNested_1069"), 3,
        [](fls::Project &project) {
            fixtureSection(project, 2).append(
                fixtureNestedMixedGroup(QStringLiteral("fixture-existing-top-nested")));
        });
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() == 4 && args[1] == QStringLiteral("--generate-fixtures")) {
        try {
            return generateFixtures(args[2], args[3]);
        } catch (const std::exception &error) {
            qCritical() << "fixture generation failed:" << error.what();
            return 1;
        }
    }
    if (args.size() != 1) {
        qCritical() << "usage: fls_livery_logo_encoder_tests"
                       " [--generate-fixtures <source-root> <output-root>]";
        return 2;
    }
    return testLogoEncoding() && testLogoShapeLimit() && testPartialSourceRebuild()
            && testNestedMaskGroupTransforms() && testDeletedSiblingSourceFraming()
        ? 0
        : 1;
}
