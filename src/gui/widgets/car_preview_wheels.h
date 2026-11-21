#pragma once

#include "car_scene.h"

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QVector>

#include <memory>

namespace gui {

inline constexpr int kFrontRimSelector = 10010;
inline constexpr int kRearRimSelector = 10011;
inline constexpr int kTireSelector = 10008;
inline constexpr int kStockRimStyle = 0;
inline constexpr std::array<int, 3> kWheelSelectors = {
    kFrontRimSelector, kRearRimSelector, kTireSelector,
};

struct WheelStyleOption {
    QString name;
    QString archive;
    int id = 0;
};

struct WheelSizeOption {
    QString name;
    int id = 0;
    float value = 0;
};

struct WheelAxlePreview {
    int rimId = kStockRimStyle;
    int diameterId = 0;
    int widthId = 0;
};

struct WheelTemplateCache {
    QMutex mutex;
    QHash<QString, std::shared_ptr<const fls::CarModel>> models;
};

struct WheelPreviewState {
    fls::WheelSizing stockSizing;
    fls::WheelSizing sizing;
    QJsonObject partNames;
    QVector<int> bodyIds;
    QString libraryPath;
    QVector<WheelStyleOption> rims;
    QVector<WheelStyleOption> tires;
    std::array<WheelAxlePreview, 2> axles;
    std::vector<fls::CarWheelMount> mounts;
    std::vector<fls::CarMesh> stockRims;
    std::shared_ptr<WheelTemplateCache> templates = std::make_shared<WheelTemplateCache>();
    int tireId = 0;
    bool independentRimStyles = false;
    bool loadCarTextures = false;
};

struct WheelOptionPreview {
    fls::CarModel model;
    fls::ModelBounds bounds;
};

bool isWheelSelector(int partType);
bool isFrontWheelSelector(int partType);
bool isRimSelector(int partType);
bool selectWheelPreviewStyle(WheelPreviewState &state, int partType, int optionId);
void resolveTireTemplateMaterials(fls::CarModel &model, const QString &archivePath, bool loadTextures);
std::shared_ptr<WheelPreviewState> prepareWheelPreview(
    const fls::CarModel &model, const QString &sourcePath, const QString &game, const fls::WheelSizing &sizing,
    const QString &gameFolder = {});
void installWheelPartOptions(fls::CarModel &model, const WheelPreviewState &state);
QVector<WheelSizeOption> wheelSizeOptions(const WheelPreviewState &state, bool front, bool diameter,
    int bodyOptionId);
bool normalizeWheelPreviewSizing(WheelPreviewState &state, int bodyOptionId);
fls::CarModel buildWheelPreviewGeometry(const WheelPreviewState &state, bool front, int rimId, int tireId,
    QString *error = nullptr);
WheelOptionPreview buildWheelOptionPreview(const WheelPreviewState &state, int partType, int optionId);
void replaceWheelPreviewGeometry(fls::CarModel &model, const fls::CarModel &wheels);

} // namespace gui
