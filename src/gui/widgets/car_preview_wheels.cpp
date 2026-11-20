#include "car_preview_wheels.h"

#include "car_preview_widget.h"
#include "gui_assets.h"
#include "model_bundle.h"
#include "theme_manager.h"
#include "zip_extract.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr float kMetresPerInch = 0.0254f;
constexpr float kMetresPerMillimetre = 0.001f;
constexpr float kPercent = 0.01f;
constexpr int kWheelTemplateCacheLimit = 64;
constexpr int kFrontRimDiameterPart = 10;
constexpr int kRearRimDiameterPart = 11;
constexpr int kFrontTireWidthPart = 38;
constexpr int kRearTireWidthPart = 39;

QJsonObject readCatalog(const QString &name) {
    QFile file(assetPath(QStringLiteral("cars/") + name));
    if (!file.open(QIODevice::ReadOnly)) return {};

    return QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject forGame(QJsonObject entry, const QString &game) {
    const auto overrides = entry.value(QStringLiteral("games")).toObject().value(game).toObject();
    for (auto field = overrides.begin(); field != overrides.end(); ++field) entry.insert(field.key(), field.value());

    return entry;
}

QString wheelLibrary(const QString &sourcePath) {
    QDir directory = QFileInfo(sourcePath).absoluteDir();
    while (true) {
        const QString path = directory.filePath(QStringLiteral("_library/scene"));
        if (QDir(path).exists()) return path;
        if (!directory.cdUp()) break;
    }

    return {};
}

QString tireArchive(const QString &name) {
    static const std::array<std::pair<const char *, const char *>, 12> kCompounds = {{
        {"Hot Wheels", "tire_semi_slick_Horizon"},
        {"Horizon", "tire_semi_slick_Horizon"},
        {"Semi.Slick", "tire_semi_slick"},
        {"Slick", "tire_slick"},
        {"Offroad", "tire_offRoad"},
        {"Rally", "tire_rally"},
        {"Snow", "tire_snow"},
        {"Drag", "tire_drag"},
        {"Drift", "tire_sport"},
        {"White.Wall", "tire_vintage_WhiteWall"},
        {"Vintage", "tire_vintage_race"},
        {"Sport", "tire_sport"},
    }};
    for (const auto &[pattern, archive] : kCompounds) {
        if (name.contains(QRegularExpression(QString::fromLatin1(pattern), QRegularExpression::CaseInsensitiveOption))) {
            return QString::fromLatin1(archive);
        }
    }
    if (name.contains(QStringLiteral("Race"), Qt::CaseInsensitive)) return QStringLiteral("tire_semi_slick");
    if (name.contains(QStringLiteral("Road"), Qt::CaseInsensitive)) return QStringLiteral("tire_road");

    return QStringLiteral("tire_street");
}

std::shared_ptr<const fls::CarModel> wheelTemplate(WheelPreviewState &state, const QString &archive,
                                                  bool left, bool rim, QString *error) {
    QString path = QDir(state.libraryPath).filePath((rim ? QStringLiteral("wheels/")
        : QStringLiteral("tires/")) + archive + QStringLiteral(".zip"));
    const QString key = path + (left ? QStringLiteral("|L") : QStringLiteral("|R"));
    const auto cached = state.templates.constFind(key);

    if (cached != state.templates.cend()) return *cached;
    QStringList entries = fls::listZipEntries(path, error);
    const auto findEntry = [rim, left](const QStringList &names) {
        const QString suffix = rim ? QStringLiteral("WheelLF.modelbin")
            : (left ? QStringLiteral("L_") : QStringLiteral("R_"));
        const auto found = std::find_if(names.cbegin(), names.cend(), [&suffix, rim](const QString &name) {
            return name.endsWith(QStringLiteral(".modelbin"), Qt::CaseInsensitive)
                && (rim ? name.endsWith(suffix, Qt::CaseInsensitive) : name.contains(suffix, Qt::CaseInsensitive));
        });

        return found == names.cend() ? QString() : *found;
    };
    QString entry = findEntry(entries);
    if (entry.isEmpty() && !rim && !left) {
        const QString rightPath = QDir(state.libraryPath).filePath(QStringLiteral("tires/")
            + QString(archive).replace(QStringLiteral("tire_"), QStringLiteral("tireR_")) + QStringLiteral(".zip"));
        path = rightPath;
        entries = fls::listZipEntries(path, error);
        entry = findEntry(entries);
    }
    if (entry.isEmpty() && !rim && !left) {
        const auto fallback = wheelTemplate(state, archive, true, false, error);
        if (fallback) {
            state.templates.insert(key, fallback);
            if (error != nullptr) error->clear();
        }

        return fallback;
    }
    if (entry.isEmpty()) return {};
    try {
        auto model = std::make_shared<fls::CarModel>(fls::decodeModel(fls::parseModelBundle(
            fls::readZipEntry(path, entry, error))));
        if (model->meshes.empty()) return {};
        if (!rim) {
            for (auto &mesh : model->meshes) mesh.sourceModelPath = path;
            resolveTireTemplateMaterials(*model, path, state.loadCarTextures);
        }
        if (state.templates.size() >= kWheelTemplateCacheLimit) state.templates.erase(state.templates.begin());
        state.templates.insert(key, model);

        return model;
    } catch (const std::exception &failure) {
        if (error != nullptr) *error = QString::fromUtf8(failure.what());
    }

    return {};
}

const WheelStyleOption *styleOption(const QVector<WheelStyleOption> &options, int id) {
    const auto found = std::find_if(options.cbegin(), options.cend(), [id](const auto &option) { return option.id == id; });

    return found == options.cend() ? nullptr : &*found;
}

bool wheelMesh(const fls::CarMesh &mesh) {
    QString path = mesh.sourceModelPath;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));

    return isWheelSelector(mesh.carPartType) || path.contains(QStringLiteral("/wheels/"), Qt::CaseInsensitive)
        || path.contains(QStringLiteral("/tires/"), Qt::CaseInsensitive);
}

float tireRadius(const fls::AxleSizing &sizing) {
    return sizing.rimDiameterInches * kMetresPerInch * 0.5f
        + sizing.tireWidthMillimetres * kMetresPerMillimetre * sizing.tireAspectPercent * kPercent;
}

void fitTireAspect(const fls::AxleSizing &stock, fls::AxleSizing &sizing) {
    sizing.tireAspectPercent = std::max(1.0f,
        (tireRadius(stock) - sizing.rimDiameterInches * kMetresPerInch * 0.5f)
            / (sizing.tireWidthMillimetres * kMetresPerMillimetre * kPercent));
}

void resizeStockRim(fls::CarMesh &mesh, const fls::CarWheelMount &mount,
                    const fls::AxleSizing &stock, const fls::AxleSizing &sizing) {
    const QVector3D center(mount.transform.m[12], mount.transform.m[13], mount.transform.m[14]);
    const float side = mount.left ? -1.0f : 1.0f;
    const QVector3D target(side * (sizing.trackOuterMetres - sizing.tireWidthMillimetres * kMetresPerMillimetre) * 0.5f,
        center.y() + tireRadius(sizing) - tireRadius(stock), center.z());
    const float widthScale = sizing.tireWidthMillimetres / stock.tireWidthMillimetres;
    const float radiusScale = sizing.rimDiameterInches / stock.rimDiameterInches;

    for (auto &point : mesh.positions) {
        point = mesh.boneTransform.transformPoint(point);
        point = {target.x() + (point.x - center.x()) * widthScale,
            target.y() + (point.y - center.y()) * radiusScale, target.z() + (point.z - center.z()) * radiusScale};
    }
    for (auto &normal : mesh.normals) {
        normal = mesh.boneTransform.transformVector(normal);
        const QVector3D scaled = QVector3D(normal.x / widthScale, normal.y / radiusScale, normal.z / radiusScale).normalized();
        normal = {scaled.x(), scaled.y(), scaled.z()};
    }
    mesh.tangentChannels.clear();
    mesh.boneTransform = {};
}

} // namespace

bool isWheelSelector(int partType) {
    return std::find(kWheelSelectors.cbegin(), kWheelSelectors.cend(), partType) != kWheelSelectors.cend();
}

bool isFrontWheelSelector(int partType) {
    return partType == kFrontRimSelector || partType == kTireSelector;
}

bool isRimSelector(int partType) {
    return partType == kFrontRimSelector || partType == kRearRimSelector;
}

bool selectWheelPreviewStyle(WheelPreviewState &state, int partType, int optionId) {
    if (!isWheelSelector(partType)) return false;
    if (partType == kTireSelector) {
        if (state.tireId == optionId || styleOption(state.tires, optionId) == nullptr) return false;
        state.tireId = optionId;

        return true;
    }
    if (styleOption(state.rims, optionId) == nullptr) return false;
    if (!state.independentRimStyles) {
        if (state.axles[0].rimId == optionId && state.axles[1].rimId == optionId) return false;
        for (auto &axle : state.axles) axle.rimId = optionId;

        return true;
    }
    auto &axle = state.axles[isFrontWheelSelector(partType) ? 0 : 1];
    if (axle.rimId == optionId) return false;
    axle.rimId = optionId;

    return true;
}

std::shared_ptr<WheelPreviewState> prepareWheelPreview(
    const fls::CarModel &model, const QString &sourcePath, const QString &game, const fls::WheelSizing &sizing) {
    auto state = std::make_shared<WheelPreviewState>();
    const auto catalog = readCatalog(QStringLiteral("rim_ids.json"));
    const auto parts = readCatalog(QStringLiteral("part_names.json"));
    const auto cars = readCatalog(QStringLiteral("car_ids.json"));
    const QString version = game.isEmpty() ? QStringLiteral("FH6") : game.toUpper();
    QString carId;

    state->stockSizing = sizing;
    state->independentRimStyles = !game.isEmpty() && version == QStringLiteral("FH6");
    state->sizing = sizing;
    state->mounts = model.wheelMounts;
    state->libraryPath = wheelLibrary(sourcePath);
    if (state->mounts.empty() || state->libraryPath.isEmpty()) return {};
    for (const auto &mesh : model.meshes) {
        QString path = mesh.sourceModelPath;
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (path.contains(QStringLiteral("/wheels/"), Qt::CaseInsensitive)) state->stockRims.push_back(mesh);
    }
    for (const auto &option : model.partOptions) {
        if (option.partType == fls::car_part_types::CarBody) state->bodyIds.push_back(option.id);
    }
    std::sort(state->bodyIds.begin(), state->bodyIds.end());
    for (auto car = cars.begin(); car != cars.end(); ++car) {
        if (forGame(car.value().toObject(), version).value(QStringLiteral("model")).toString()
            .compare(model.mediaName, Qt::CaseInsensitive) == 0) {
            carId = car.key();
            break;
        }
    }
    for (auto car = parts.begin(); car != parts.end(); ++car) {
        if (car.key().compare(model.mediaName, Qt::CaseInsensitive) == 0) {
            state->partNames = forGame(car.value().toObject(), version);
            break;
        }
    }
    state->rims.push_back({QStringLiteral("Stock"), {}, kStockRimStyle});
    const auto gameRules = catalog.value(QStringLiteral("games")).toObject().value(version).toObject();
    const bool locked = gameRules.value(QStringLiteral("car_restrictions")).toObject().value(carId).toObject()
        .value(QStringLiteral("no_rim_styles")).toInt() != 0;
    const auto rims = catalog.value(QStringLiteral("rims")).toObject();
    if (!locked) {
        for (auto rim = rims.begin(); rim != rims.end(); ++rim) {
            const auto value = forGame(rim.value().toObject(), version);
            const QString archive = value.value(QStringLiteral("model")).toString();
            const QStringList versions = value.value(QStringLiteral("version")).toString().split(QStringLiteral(", "));
            if (value.value(QStringLiteral("stock")).toBool() || !versions.contains(version)
                || !QFileInfo::exists(QDir(state->libraryPath).filePath(QStringLiteral("wheels/") + archive + QStringLiteral(".zip")))) continue;
            const auto manufacturer = forGame(catalog.value(QStringLiteral("manufacturers")).toObject()
                .value(QString::number(value.value(QStringLiteral("manufacturer")).toInt())).toObject(), version);
            const QString brand = manufacturer.value(QStringLiteral("name")).toString();
            QString name = value.value(QStringLiteral("name")).toString();
            if (name.isEmpty()) name = archive;
            if (!brand.isEmpty() && !name.startsWith(brand, Qt::CaseInsensitive)) name.prepend(brand + QLatin1Char(' '));
            state->rims.push_back({name, archive, rim.key().toInt()});
        }
    }
    std::sort(state->rims.begin() + 1, state->rims.end(), [](const auto &first, const auto &second) {
        return first.name.localeAwareCompare(second.name) < 0;
    });
    static const QRegularExpression kCompound(QStringLiteral(R"(^8:(\d+)$)"));
    for (auto part = state->partNames.begin(); part != state->partNames.end(); ++part) {
        const auto match = kCompound.match(part.key());
        if (!match.hasMatch()) continue;
        const QString name = part.value().toString();
        const QString archive = tireArchive(name);
        if (!QFileInfo::exists(QDir(state->libraryPath).filePath(QStringLiteral("tires/") + archive + QStringLiteral(".zip")))) continue;
        state->tires.push_back({name, archive, match.captured(1).toInt()});
        if (name.startsWith(QStringLiteral("Stock"), Qt::CaseInsensitive)) state->tireId = match.captured(1).toInt();
    }
    if (state->tires.isEmpty()) state->tires.push_back({QStringLiteral("Stock"), QStringLiteral("tire_b"), 0});
    std::sort(state->tires.begin(), state->tires.end(), [&state](const auto &first, const auto &second) {
        if ((first.id == state->tireId) != (second.id == state->tireId)) return first.id == state->tireId;
        return first.name.localeAwareCompare(second.name) < 0;
    });

    return state;
}

void installWheelPartOptions(fls::CarModel &model, const WheelPreviewState &state) {
    model.partOptions.erase(std::remove_if(model.partOptions.begin(), model.partOptions.end(), [](const auto &option) {
        return isWheelSelector(option.partType);
    }), model.partOptions.end());
    for (int partType : kWheelSelectors) {
        if (partType == kRearRimSelector && !state.independentRimStyles) continue;
        const auto &options = isRimSelector(partType) ? state.rims : state.tires;
        for (qsizetype index = 0; index < options.size(); ++index) {
            fls::CarPartOption option;
            option.partType = partType;
            option.id = options[index].id;
            option.level = static_cast<int>(index);
            option.stock = option.id == (isRimSelector(partType) ? kStockRimStyle : state.tireId);
            option.parentIsStock = true;
            option.modelPaths.push_back(options[index].archive.isEmpty() ? QStringLiteral("stock") : options[index].archive);
            model.partOptions.push_back(std::move(option));
        }
    }
    const auto tag = [&state](std::vector<fls::CarMesh> &meshes) {
        for (auto &mesh : meshes) {
            if (!wheelMesh(mesh)) continue;
            QString path = mesh.sourceModelPath;
            path.replace(QLatin1Char('\\'), QLatin1Char('/'));
            fls::ModelBounds bounds;
            bounds.include(mesh);
            const bool front = bounds.minimum.z + bounds.maximum.z >= 0.0f;
            const bool rim = path.contains(QStringLiteral("/wheels/"), Qt::CaseInsensitive);
            const auto &axle = state.axles[front ? 0 : 1];
            mesh.carPartType = rim ? (front ? kFrontRimSelector : kRearRimSelector)
                : kTireSelector;
            mesh.partOptionIds = {rim ? axle.rimId : state.tireId};
        }
    };
    tag(model.meshes);
    for (auto &meshes : model.additionalLodMeshes) tag(meshes);
}

QVector<WheelSizeOption> wheelSizeOptions(const WheelPreviewState &state, bool front, bool diameter, int bodyOptionId) {
    const fls::AxleSizing &stock = front ? state.stockSizing.front : state.stockSizing.rear;
    const int partType = diameter ? (front ? kFrontRimDiameterPart : kRearRimDiameterPart)
        : (front ? kFrontTireWidthPart : kRearTireWidthPart);
    const QString prefix = QString::number(partType) + QLatin1Char(':');
    const float stockValue = diameter ? stock.rimDiameterInches : stock.tireWidthMillimetres;
    QVector<WheelSizeOption> options;
    const QRegularExpression number(diameter ? QStringLiteral(R"(^([\d.]+)\s+in\b)") : QStringLiteral(R"(^([\d.]+)\s+mm\b)"));
    QVector<int> stockIds;
    int familyStart = 0;
    int familyEnd = std::numeric_limits<int>::max();

    for (auto part = state.partNames.begin(); part != state.partNames.end(); ++part) {
        if (!part.key().startsWith(prefix) || !part.value().toString().startsWith(QStringLiteral("Stock"))) continue;
        const int id = part.key().mid(prefix.size()).toInt();
        stockIds.push_back(id);
    }
    std::sort(stockIds.begin(), stockIds.end());
    const qsizetype bodyIndex = state.bodyIds.indexOf(bodyOptionId);
    if (!stockIds.isEmpty()) {
        const qsizetype index = stockIds.size() == state.bodyIds.size() && bodyIndex >= 0 ? bodyIndex : 0;
        familyStart = stockIds[index];
        if (index + 1 < stockIds.size()) familyEnd = stockIds[index + 1];
    }
    options.push_back({QStringLiteral("Stock (%1 %2)").arg(stockValue).arg(diameter ? QStringLiteral("in") : QStringLiteral("mm")), familyStart, stockValue});
    for (auto part = state.partNames.begin(); part != state.partNames.end(); ++part) {
        if (!part.key().startsWith(prefix)) continue;
        const int id = part.key().mid(prefix.size()).toInt();
        const auto match = number.match(part.value().toString());
        if (!match.hasMatch() || id < familyStart || id >= familyEnd) continue;
        const float value = match.captured(1).toFloat();
        const bool present = std::any_of(options.cbegin(), options.cend(), [value](const auto &option) {
            return qFuzzyCompare(value, option.value);
        });
        if (value > 0 && !present) {
            options.push_back({QStringLiteral("%1 %2").arg(value).arg(diameter ? QStringLiteral("in") : QStringLiteral("mm")), id, value});
        }
    }
    std::sort(options.begin() + 1, options.end(), [](const auto &first, const auto &second) { return first.value < second.value; });

    return options;
}

bool normalizeWheelPreviewSizing(WheelPreviewState &state, int bodyOptionId) {
    bool changed = false;

    for (bool front : {true, false}) {
        auto &sizing = front ? state.sizing.front : state.sizing.rear;
        const auto &stock = front ? state.stockSizing.front : state.stockSizing.rear;
        for (bool diameter : {true, false}) {
            const auto options = wheelSizeOptions(state, front, diameter, bodyOptionId);
            float &value = diameter ? sizing.rimDiameterInches : sizing.tireWidthMillimetres;
            const bool available = std::any_of(options.cbegin(), options.cend(), [value](const auto &option) {
                return qFuzzyCompare(value, option.value);
            });
            if (!available && !options.isEmpty()) {
                value = options.front().value;
                changed = true;
            }
        }
        fitTireAspect(stock, sizing);
    }

    return changed;
}

fls::CarModel buildWheelPreviewGeometry(WheelPreviewState &state, bool front, int rimId, int tireId, QString *error) {
    const auto *rimOption = styleOption(state.rims, rimId);
    const auto *tireOption = styleOption(state.tires, tireId);
    const auto &stock = front ? state.stockSizing.front : state.stockSizing.rear;
    const auto &sizing = front ? state.sizing.front : state.sizing.rear;
    fls::CarModel result;
    const auto rim = rimId == kStockRimStyle || rimOption == nullptr ? std::shared_ptr<const fls::CarModel>{}
        : wheelTemplate(state, rimOption->archive, true, true, error);

    if (rimOption == nullptr || tireOption == nullptr || (rimId != kStockRimStyle && !rim)) return result;
    for (auto mount : state.mounts) {
        if (mount.front != front) continue;
        if (rimId == kStockRimStyle) {
            for (const auto &source : state.stockRims) {
                if (source.modelInstanceId != mount.instanceId) continue;
                auto mesh = source;
                resizeStockRim(mesh, mount, stock, sizing);
                result.meshes.push_back(std::move(mesh));
            }
        } else {
            mount.transform.m[12] = (mount.left ? -1.0f : 1.0f)
                * (sizing.trackOuterMetres - sizing.tireWidthMillimetres * kMetresPerMillimetre) * 0.5f;
            mount.transform.m[13] += tireRadius(sizing) - tireRadius(stock);
            fls::appendCarRim(result, *rim, mount, sizing);
        }
    }
    for (auto &mesh : result.meshes) {
        mesh.carPartType = front ? kFrontRimSelector : kRearRimSelector;
        mesh.partOptionIds = {rimId};
        mesh.stockPart = true;
        if (!rim) continue;
        mesh.sourceModelPath = QStringLiteral("_library/scene/wheels/") + rimOption->archive;
        for (const auto &source : state.stockRims) {
            if (source.materialName.compare(mesh.materialName, Qt::CaseInsensitive) == 0 && source.material) {
                mesh.material = source.material;
                break;
            }
        }
    }
    const auto left = wheelTemplate(state, tireOption->archive, true, false, error);
    const auto right = wheelTemplate(state, tireOption->archive, false, false, error);
    if (!left || !right) return {};
    const size_t firstTire = result.meshes.size();
    fls::appendApproximateTires(result, *left, *right, state.sizing);
    for (size_t index = firstTire; index < result.meshes.size(); ++index) {
        auto &mesh = result.meshes[index];
        mesh.carPartType = kTireSelector;
        mesh.partOptionIds = {tireId};
        mesh.sourceModelPath = QStringLiteral("_library/scene/tires/") + tireOption->archive;
    }

    return result;
}

void replaceWheelPreviewGeometry(fls::CarModel &model, const fls::CarModel &wheels) {
    const auto replace = [&wheels](std::vector<fls::CarMesh> &meshes, bool append) {
        meshes.erase(std::remove_if(meshes.begin(), meshes.end(), wheelMesh), meshes.end());
        if (append) meshes.insert(meshes.end(), wheels.meshes.cbegin(), wheels.meshes.cend());
    };
    replace(model.meshes, true);
    for (auto &meshes : model.additionalLodMeshes) replace(meshes, true);
    replace(model.variantMeshes, false);
    for (auto &meshes : model.additionalVariantLodMeshes) replace(meshes, false);
}

QImage CarPreviewWidget::renderWheelOptionPreview(int partType, int optionId) {
    if (!wheelPreview_ || !context()) return {};
    const bool front = isFrontWheelSelector(partType);
    const auto &axle = wheelPreview_->axles[front ? 0 : 1];
    auto geometry = buildWheelPreviewGeometry(*wheelPreview_, front,
        isRimSelector(partType) ? optionId : axle.rimId, isRimSelector(partType) ? wheelPreview_->tireId : optionId);
    fls::ModelBounds bounds;

    geometry.meshes.erase(std::remove_if(geometry.meshes.begin(), geometry.meshes.end(),
        [partType](const fls::CarMesh &mesh) {
            fls::ModelBounds bounds;
            bounds.include(mesh);
            return mesh.carPartType != partType || bounds.minimum.x + bounds.maximum.x > 0.0f;
        }), geometry.meshes.end());
    if (geometry.meshes.empty()) return {};
    for (const auto &mesh : geometry.meshes) bounds.include(mesh);
    makeCurrent();
    if (!wheelRenderer_.isInitialized()) wheelRenderer_.initialize();
    wheelRenderer_.uploadModel(geometry);
    doneCurrent();

    return renderGeometryPreview(wheelRenderer_, bounds, {{partType, optionId}}, partType, false);
}

void CarPreviewWidget::selectWheelOption(int partType, int optionId) {
    if (!wheelPreview_ || modelLoadPending_ || modelUploadPending_) return;
    const auto previousAxles = wheelPreview_->axles;
    const int previousTire = wheelPreview_->tireId;

    if (!selectWheelPreviewStyle(*wheelPreview_, partType, optionId)) return;
    if (!applyWheelGeometry()) {
        wheelPreview_->axles = previousAxles;
        wheelPreview_->tireId = previousTire;
        return;
    }
    rebuildColorsPanel();
}

bool CarPreviewWidget::applyWheelGeometry() {
    QString error;
    auto front = buildWheelPreviewGeometry(*wheelPreview_, true, wheelPreview_->axles[0].rimId,
        wheelPreview_->tireId, &error);
    auto rear = buildWheelPreviewGeometry(*wheelPreview_, false, wheelPreview_->axles[1].rimId,
        wheelPreview_->tireId, &error);

    if (front.meshes.empty() || rear.meshes.empty()) {
        qWarning().noquote() << error;
        QMessageBox::warning(this, QStringLiteral("Wheels"), QStringLiteral("The selected wheel model could not be loaded."));
        return false;
    }
    front.meshes.insert(front.meshes.end(), rear.meshes.begin(), rear.meshes.end());
    replaceWheelPreviewGeometry(model_, front);
    for (int type : kWheelSelectors) {
        const auto &axle = wheelPreview_->axles[isFrontWheelSelector(type) ? 0 : 1];
        const int id = isRimSelector(type) ? axle.rimId : wheelPreview_->tireId;
        selectedPartOptions_.insert(type, id);
        individualPartOptions_.insert(type, id);
    }
    carRenderer_.setPartSelections(selectedPartOptions_);
    modelUploadPending_ = true;
    rebuildPartControls();
    update();

    return true;
}

void CarPreviewWidget::rebuildWheelSizingControls() {
    if (!wheelPreview_) return;
    auto *heading = new QLabel(QStringLiteral("Wheel sizing"), colorsPage_);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    colorsLayout_->addWidget(heading);
    for (bool front : {true, false}) {
        auto *label = new QLabel(front ? QStringLiteral("Front axle") : QStringLiteral("Rear axle"), colorsPage_);
        colorsLayout_->addWidget(label);
        auto *row = new QWidget(colorsPage_);
        auto *layout = new QFormLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        for (bool diameter : {true, false}) {
            auto *combo = new QComboBox(row);
            const auto options = wheelSizeOptions(*wheelPreview_, front, diameter,
                selectedPartOptions_.value(fls::car_part_types::CarBody, -1));
            const auto &axle = front ? wheelPreview_->sizing.front : wheelPreview_->sizing.rear;
            const float current = diameter ? axle.rimDiameterInches : axle.tireWidthMillimetres;
            int selectedIndex = -1;
            combo->setObjectName(QStringLiteral("previewWheelSize.%1.%2")
                .arg(front ? QStringLiteral("front") : QStringLiteral("rear"),
                    diameter ? QStringLiteral("diameter") : QStringLiteral("width")));
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(5);
            for (const auto &option : options) {
                combo->addItem(option.name, option.id);
                if (qFuzzyCompare(current, option.value)) selectedIndex = combo->count() - 1;
            }
            if (selectedIndex < 0) {
                combo->addItem(QStringLiteral("%1 %2 (current)").arg(current)
                    .arg(diameter ? QStringLiteral("in") : QStringLiteral("mm")), -1);
                selectedIndex = combo->count() - 1;
            }
            combo->setCurrentIndex(selectedIndex);
            combo->setEnabled(options.size() > 1);
            layout->addRow(diameter ? QStringLiteral("Rim diameter") : QStringLiteral("Tire width"), combo);
            connect(combo, qOverload<int>(&QComboBox::activated), this, [this, front, diameter, options](int index) {
                if (index >= 0 && index < options.size()) {
                    selectWheelSize(front, diameter, options[index].id, options[index].value);
                }
            });
        }
        colorsLayout_->addWidget(row);
    }
}

void CarPreviewWidget::selectWheelSize(bool front, bool diameter, int optionId, float value) {
    if (!wheelPreview_ || modelLoadPending_ || value <= 0.0f) return;
    auto &sizing = front ? wheelPreview_->sizing.front : wheelPreview_->sizing.rear;
    auto &axle = wheelPreview_->axles[front ? 0 : 1];
    float &selected = diameter ? sizing.rimDiameterInches : sizing.tireWidthMillimetres;
    const auto previous = sizing;
    const auto &stock = front ? wheelPreview_->stockSizing.front : wheelPreview_->stockSizing.rear;

    if (qFuzzyCompare(selected, value)) return;
    selected = value;
    fitTireAspect(stock, sizing);
    if (!applyWheelGeometry()) {
        sizing = previous;
        rebuildColorsPanel();
        return;
    }
    (diameter ? axle.diameterId : axle.widthId) = optionId;
}

} // namespace gui
