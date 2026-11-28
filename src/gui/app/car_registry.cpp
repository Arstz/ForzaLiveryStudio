#include "car_registry.h"
#include "game_paths.h"
#include "theme_manager.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <utility>

namespace gui {
namespace {

constexpr int kVersionsRole = Qt::UserRole + 1;
constexpr int kGameRankRole = Qt::UserRole + 2;
constexpr int kManufacturerColumn = 0;
constexpr int kYearColumn = 1;
constexpr int kModelColumn = 2;
constexpr int kFirstGameColumn = 3;
constexpr int kGameHeaderExtraWidth = 4;
constexpr int kCarDialogWidth = 1000;
constexpr int kCarDialogHeight = 560;
constexpr int kClosestModelYearDifference = 5;

const QStringList kLegacyGameOrder = {
    QStringLiteral("FH1"), QStringLiteral("FM4"), QStringLiteral("FM3"), QStringLiteral("FM2"),
};

QStringList supportedGames(const QString &versions) {
    QStringList games = versions.toUpper().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString &game : games) {
        game = game.trimmed();
    }
    games.removeAll(QString());
    games.removeDuplicates();

    return games;
}

QString gameTitle(const QString &game) {
    const QString title = game.startsWith(QStringLiteral("FH"))
        ? QStringLiteral("Forza Horizon %1") : QStringLiteral("Forza Motorsport %1");

    return title.arg(game.mid(2));
}

QString nameWithoutYear(const QString &name) {
    static const QRegularExpression yearPrefix(QStringLiteral("^\\d{4}\\s+"));

    return QString(name).remove(yearPrefix);
}

QString modelDisplayName(const QString &name, const QString &manufacturer) {
    QString model = nameWithoutYear(name);
    if (!manufacturer.isEmpty()) {
        const QRegularExpression makeWord(
            QStringLiteral("(?<![\\p{L}\\p{M}\\p{N}_'-])%1(?![\\p{L}\\p{M}\\p{N}_'-])")
                .arg(QRegularExpression::escape(manufacturer)), QRegularExpression::CaseInsensitiveOption);
        model.remove(makeWord);
    }

    return model.simplified();
}

QString manufacturerCode(const QString &modelCode) {
    return modelCode.section(QLatin1Char('_'), 0, 0).toUpper();
}

QHash<QString, QString> manufacturerNames(const CarRegistry &registry) {
    const QHash<QString, QString> knownNames = {
        {QStringLiteral("AC"), QStringLiteral("Alumicraft")},
        {QStringLiteral("ALF"), QStringLiteral("Alfa Romeo")},
        {QStringLiteral("AST"), QStringLiteral("Aston Martin")},
        {QStringLiteral("BM"), QStringLiteral("Hoonigan")},
        {QStringLiteral("DEL"), QStringLiteral("DeLorean")},
        {QStringLiteral("DET"), QStringLiteral("De Tomaso")},
        {QStringLiteral("FAF"), QStringLiteral("Fast and Furious")},
        {QStringLiteral("FIA"), QStringLiteral("Fiat")},
        {QStringLiteral("GMA"), QStringLiteral("Gordon Murray Automotive")},
        {QStringLiteral("GTA"), QStringLiteral("Spania GTA")},
        {QStringLiteral("HOT"), QStringLiteral("Hot Wheels")},
        {QStringLiteral("INT"), QStringLiteral("International")},
        {QStringLiteral("LR"), QStringLiteral("Land Rover")},
        {QStringLiteral("LYN"), QStringLiteral("Lynk & Co")},
        {QStringLiteral("MEY"), QStringLiteral("Meyers Manx")},
        {QStringLiteral("RJ"), QStringLiteral("RJ Anderson")},
        {QStringLiteral("TG"), QStringLiteral("Top Gear")},
        {QStringLiteral("WM"), QStringLiteral("W Motors")},
    };
    QHash<QString, QMap<QString, int>> counts;
    QHash<QString, QString> names = knownNames;

    for (const auto &entry : registry.entries()) {
        const QString code = manufacturerCode(registry.modelCode(entry.id));
        if (entry.manufacturer.isEmpty() && !knownNames.contains(code)) {
            ++counts[code][nameWithoutYear(entry.name).section(QLatin1Char(' '), 0, 0)];
        }
    }
    for (auto manufacturer = counts.cbegin(); manufacturer != counts.cend(); ++manufacturer) {
        const auto &labels = manufacturer.value();
        const auto mostCommon = std::max_element(labels.keyValueBegin(), labels.keyValueEnd(),
            [](const auto &left, const auto &right) { return left.second < right.second; });
        names.insert(manufacturer.key(), (*mostCommon).first);
    }

    return names;
}

class CarPickerItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override {
        const int column = treeWidget()->sortColumn();
        const auto key = [column](const QTreeWidgetItem &item) {
            const auto textKey = [&item](int index) { return item.text(index).toCaseFolded(); };

            return std::tuple{
                column >= kFirstGameColumn ? item.data(column, kGameRankRole).toInt() : 0,
                column == kYearColumn ? item.data(kYearColumn, Qt::DisplayRole).toInt() : 0,
                column == kModelColumn ? textKey(kModelColumn) : QString(),
                textKey(kManufacturerColumn), textKey(kModelColumn),
                item.data(kYearColumn, Qt::DisplayRole).toInt(),
                item.data(kManufacturerColumn, Qt::UserRole).toInt()
            };
        };

        return key(*this) < key(other);
    }
};

enum class GameModelStatus { Unsupported, Missing, Available };

void setGameCell(QTreeWidgetItem &item, int column, const QString &game, GameModelStatus status) {
    const QString title = gameTitle(game);
    item.setData(column, kGameRankRole, static_cast<int>(status));
    item.setTextAlignment(column, Qt::AlignCenter);
    switch (status) {
    case GameModelStatus::Available:
        item.setText(column, QStringLiteral("\u2713"));
        item.setToolTip(column, QStringLiteral("%1 — model found").arg(title));
        break;
    case GameModelStatus::Missing:
        item.setText(column, QStringLiteral("~"));
        item.setToolTip(column, QStringLiteral("%1 — supported, model unavailable").arg(title));
        break;
    case GameModelStatus::Unsupported:
        item.setToolTip(column, QStringLiteral("%1 — unsupported").arg(title));
        break;
    }
}

QString modelFamily(const QString &modelCode) {
    static const QRegularExpression familyPattern(QStringLiteral(
        "^([A-Z]+[0-9]+|[0-9]+|[A-Z]?[a-z]+|[A-Z]+(?=[A-Z][a-z]|$))"));
    const QStringList parts = modelCode.split(QLatin1Char('_'));
    if (parts.size() < 3) {
        return {};
    }
    const qsizetype familyIndex = parts.size() > 3 && parts[1].toInt() > 0 ? 2 : 1;
    const QString family = familyPattern.match(parts[familyIndex]).captured(1);

    return family.isEmpty() ? QString() : (parts.front() + QLatin1Char('_') + family).toCaseFolded();
}

QStringList candidateAssetPaths() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/cars/car_ids.json")),
        QDir(cwd).filePath(QStringLiteral("assets/cars/car_ids.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/cars/car_ids.json")),
    };
}

} // namespace

bool CarRegistry::loadDefault(QString *error) {
    for (const QString &path : candidateAssetPaths()) {
        if (QFile::exists(path)) {
            return loadFromFile(path, error);
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("car_ids.json was not found");
    }
    return false;
}

bool CarRegistry::loadFromFile(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not open car registry: %1").arg(path);
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid car registry: %1").arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject root = document.object();
    QHash<int, QString> parsed;
    QHash<int, QString> parsedModels;
    QHash<int, QString> parsedVersions;
    QHash<int, int> parsedYears;
    QHash<int, QHash<QString, QString>> parsedGameModels;
    QVector<Entry> parsedEntries;
    parsedEntries.reserve(root.size());
    for (auto it = root.begin(); it != root.end(); ++it) {
        bool ok = false;
        const int id = it.key().toInt(&ok);
        if (!ok) {
            continue;
        }
        QString name;
        QString model;
        QString version = QStringLiteral("FH6");
        QString manufacturer;
        if (it.value().isObject()) {
            const QJsonObject entry = it.value().toObject();
            model = entry.value(QStringLiteral("model")).toString().trimmed();
            name = entry.value(QStringLiteral("name")).toString().trimmed();
            manufacturer = entry.value(QStringLiteral("manufacturer")).toString().trimmed();
            parsedYears.insert(id, entry.value(QStringLiteral("year")).toInt());
            const QJsonObject games = entry.value(QStringLiteral("games")).toObject();
            for (auto game = games.begin(); game != games.end(); ++game) {
                const QString gameModel = game.value().toObject().value(QStringLiteral("model")).toString().trimmed();
                if (!gameModel.isEmpty()) parsedGameModels[id].insert(game.key().toUpper(), gameModel);
            }
            const QString parsedVersion =
                entry.value(QStringLiteral("version")).toString().trimmed();
            if (!parsedVersion.isEmpty()) {
                version = parsedVersion;
            }
            if (name.isEmpty()) {
                name = model;
            }
        } else {
            name = it.value().toString().trimmed();
        }
        if (!name.isEmpty()) {
            parsed.insert(id, name);
            parsedVersions.insert(id, version);
            parsedEntries.push_back({id, name, version, manufacturer, {}});
        }
        if (!model.isEmpty()) {
            parsedModels.insert(id, model);
        }
    }

    names_ = parsed;
    models_ = parsedModels;
    gameModels_ = parsedGameModels;
    versions_ = parsedVersions;
    years_ = parsedYears;
    sorted_ = std::move(parsedEntries);
    const auto manufacturers = manufacturerNames(*this);
    for (Entry &entry : sorted_) {
        if (entry.manufacturer.isEmpty()) {
            entry.manufacturer = manufacturers.value(manufacturerCode(modelCode(entry.id)));
        }
        entry.modelName = modelDisplayName(entry.name, entry.manufacturer);
    }
    std::sort(sorted_.begin(), sorted_.end(), [](const Entry &a, const Entry &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return true;
}

QString CarRegistry::name(int id) const {
    return names_.value(id);
}

QString CarRegistry::modelCode(int id, const QString &game) const {
    const QString gameCode = gameModels_.value(id).value(game.toUpper());
    const QString code = models_.value(id);

    if (!gameCode.isEmpty()) return gameCode;

    return code.isEmpty() ? names_.value(id) : code;
}

QString CarRegistry::version(int id) const {
    return versions_.value(id);
}

int CarRegistry::year(int id) const {
    return years_.value(id);
}

bool CarRegistry::isFh6Supported(int id) const {
    return isSupported(id, QStringLiteral("FH6"));
}

bool CarRegistry::isFh6Unsupported(int id) const {
    return names_.contains(id) && !isFh6Supported(id);
}

bool CarRegistry::isSupported(int id, const QString &game) const {
    const QStringList games = versions_.value(id).split(QLatin1Char(','), Qt::SkipEmptyParts);

    return std::any_of(games.cbegin(), games.cend(), [&game](const QString &version) {
        return version.trimmed().compare(game, Qt::CaseInsensitive) == 0;
    });
}

CarRegistry::ModelSource CarRegistry::findModel(int id, const QMap<QString, QString> &gameFolders,
                                              const QString &preferredGame) const {
    for (const QString &game : fls::carModelGames(preferredGame)) {
        const QString path = findModelPathForGame(id, game, gameFolders.value(game));
        if (!path.isEmpty()) {
            return {path, game};
        }
    }

    return {};
}

QVector<CarRegistry::ModelSource> CarRegistry::findModels(int id, const QMap<QString, QString> &gameFolders,
                                                         const QString &preferredGame) const {
    QVector<ModelSource> sources;

    for (const QString &game : fls::carModelGames(preferredGame)) {
        const QString path = findModelPathForGame(id, game, gameFolders.value(game));
        if (!path.isEmpty()) {
            sources.push_back({path, game});
        }
    }

    return sources;
}

CarRegistry::ClosestModel CarRegistry::findClosestModel(
    int id, const QMap<QString, QString> &gameFolders, const QString &preferredGame) const {
    const ModelSource exact = findModel(id, gameFolders, preferredGame);
    const QString family = modelFamily(models_.value(id));
    const QStringList games = fls::carModelGames(preferredGame);
    ClosestModel closest;
    const int year = years_.value(id);
    int closestDifference = kClosestModelYearDifference + 1;

    if (!exact.path.isEmpty()) {
        return {exact, id};
    }
    if (year <= 0 || family.isEmpty()) {
        return {};
    }
    for (const QString &game : games) {
        const QString folder = gameFolders.value(game);
        if (folder.isEmpty()) continue;

        for (const Entry &entry : sorted_) {
            const int candidateYear = years_.value(entry.id);
            const int difference = std::abs(year - candidateYear);
            if (entry.id == id || candidateYear <= 0 || difference >= closestDifference
                || modelFamily(modelCode(entry.id, game)) != family) {
                continue;
            }
            const QString path = findModelPathForGame(entry.id, game, folder);
            if (!path.isEmpty()) {
                closest = {{path, game}, entry.id};
                closestDifference = difference;
            }
        }
    }

    return closest;
}

QHash<int, QStringList> CarRegistry::modelAvailability(const QMap<QString, QString> &gameFolders) const {
    QHash<int, QStringList> available;

    for (auto folder = gameFolders.cbegin(); folder != gameFolders.cend(); ++folder) {
        const QSet<QString> models = fls::availableCarModelCodes(folder.value());
        const QString game = folder.key().trimmed().toUpper();
        if (models.isEmpty()) continue;
        for (const Entry &entry : sorted_) {
            if (isSupported(entry.id, game) && models.contains(modelCode(entry.id, game).toLower())) {
                available[entry.id].push_back(game);
            }
        }
    }

    return available;
}

QString CarRegistry::displayName(int id) const {
    if (id == 0) {
        return QString();
    }
    const QString found = names_.value(id);
    return found.isEmpty() ? QStringLiteral("Unknown car (#%1)").arg(id) : found;
}

QString CarRegistry::findModelPathForGame(int id, const QString &game, const QString &gameFolder) const {
    if (gameFolder.isEmpty() || !isSupported(id, game)) {
        return {};
    }
    const QString code = modelCode(id, game);
    for (const QString &directory : fls::gameCarsDirs(gameFolder)) {
        const QString path = fls::findCarModelPath(directory, code);
        if (!path.isEmpty()) {
            return path;
        }
    }

    return {};
}

const CarRegistry &sharedCarRegistry() {
    static const CarRegistry registry = [] {
        CarRegistry r;
        r.loadDefault(nullptr);
        return r;
    }();
    return registry;
}

CarRegistry::ModelSource chooseCarModelSource(
    QWidget *parent, const QString &carName, const QVector<CarRegistry::ModelSource> &sources) {
    if (sources.size() < 2) {
        return sources.value(0);
    }

    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Choose Car Model"),
                       QStringLiteral("Can load multiple models for %1, which to load (UV can differ between versions)?")
                           .arg(carName),
                       QMessageBox::Cancel, parent);
    QHash<QAbstractButton *, CarRegistry::ModelSource> sourcesByButton;

    dialog.setTextFormat(Qt::PlainText);
    for (const CarRegistry::ModelSource &source : sources) {
        sourcesByButton.insert(dialog.addButton(source.game, QMessageBox::AcceptRole), source);
    }
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.exec();

    return sourcesByButton.value(dialog.clickedButton());
}

MissingCarModelChoice chooseMissingCarModel(
    QWidget *parent, int carId, const QString &targetGame, const QString &alternativeModel) {
    const QString versions = sharedCarRegistry().version(carId);
    const QString expectation = versions.isEmpty() ? QStringLiteral("supported games unknown")
        : versions.contains(QLatin1Char(',')) ? QStringLiteral("expected one of %1").arg(versions)
        : QStringLiteral("expected %1").arg(versions);
    QString message = targetGame.isEmpty()
        ? QStringLiteral("No model found in the configured game folders (%1), continue without?").arg(expectation)
        : QStringLiteral("No model found in the configured %1 game folder, continue without?").arg(targetGame);
    if (!alternativeModel.isEmpty()) {
        message += QStringLiteral("\n\nCan load %1 instead.").arg(alternativeModel);
    }

    QMessageBox dialog(QMessageBox::Warning, QStringLiteral("Car Model Missing"), message,
                       QMessageBox::Cancel, parent);
    dialog.setTextFormat(Qt::PlainText);
    const QPushButton *proceed = dialog.addButton(QStringLiteral("Continue"), QMessageBox::AcceptRole);
    const QPushButton *loadClosest = alternativeModel.isEmpty() ? nullptr
        : dialog.addButton(QStringLiteral("Load closest"), QMessageBox::ActionRole);
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.exec();

    if (dialog.clickedButton() == proceed) {
        return MissingCarModelChoice::Continue;
    }
    return loadClosest != nullptr && dialog.clickedButton() == loadClosest
        ? MissingCarModelChoice::LoadClosest : MissingCarModelChoice::Cancel;
}

bool chooseCarModel(QWidget *parent, int currentId, int *outId, bool confirmMissingModel) {
    const CarRegistry &registry = sharedCarRegistry();
    const auto availability = registry.modelAvailability(loadBehaviorSettings().gameFolders);
    QSet<QString> catalogGames;
    for (const auto &entry : registry.entries()) {
        const QStringList versions = supportedGames(entry.version);
        for (const QString &game : versions) {
            catalogGames.insert(game);
        }
    }
    QStringList games = fls::carModelGames();
    for (const QString &game : kLegacyGameOrder) {
        if (catalogGames.remove(game)) {
            games.append(game);
        }
    }
    QStringList otherGames = catalogGames.values();
    otherGames.sort();
    for (const QString &game : otherGames) {
        if (!games.contains(game)) {
            games.append(game);
        }
    }

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Choose Target Car"));

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Select the car this livery is for:"), &dialog));

    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Filter…"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    auto *list = new QTreeWidget(&dialog);
    list->setHeaderLabels(QStringList{
        QStringLiteral("Manufacturer"), QStringLiteral("Year"), QStringLiteral("Model")
    } + games);
    list->setProperty("uiMainSurface", true);
    list->setProperty("uiDividedItems", true);
    list->setRootIsDecorated(false);
    list->setItemsExpandable(false);
    list->setUniformRowHeights(true);
    list->setSelectionBehavior(QAbstractItemView::SelectRows);
    list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->header()->setStretchLastSection(false);
    list->header()->setSectionsClickable(true);
    list->header()->setSectionsMovable(false);
    list->header()->setResizeContentsPrecision(-1);
    list->header()->setSortIndicatorShown(false);
    list->header()->setSectionResizeMode(kManufacturerColumn, QHeaderView::Fixed);
    list->header()->setSectionResizeMode(kYearColumn, QHeaderView::Fixed);
    list->header()->setSectionResizeMode(kModelColumn, QHeaderView::Interactive);
    for (int index = 0; index < games.size(); ++index) {
        const int column = kFirstGameColumn + index;
        list->headerItem()->setTextAlignment(column, Qt::AlignCenter);
        list->headerItem()->setToolTip(column, gameTitle(games[index]));
        list->header()->setSectionResizeMode(column, QHeaderView::Fixed);
        list->header()->resizeSection(column,
            list->header()->sectionSizeHint(column) + kGameHeaderExtraWidth);
    }
    layout->addWidget(list, 1);
    QTreeWidgetItem *currentCar = nullptr;
    for (const CarRegistry::Entry &entry : registry.entries()) {
        const QStringList versions = supportedGames(entry.version);
        const QStringList available = availability.value(entry.id);
        auto *item = new CarPickerItem(list);
        item->setText(kManufacturerColumn, entry.manufacturer);
        item->setData(kManufacturerColumn, Qt::UserRole, entry.id);
        item->setData(kManufacturerColumn, kVersionsRole, versions);
        item->setText(kModelColumn, entry.modelName);
        item->setToolTip(kModelColumn, entry.name);
        if (registry.year(entry.id) > 0) {
            item->setData(kYearColumn, Qt::DisplayRole, registry.year(entry.id));
        }
        for (int index = 0; index < games.size(); ++index) {
            const QString &game = games[index];
            const auto status = available.contains(game) ? GameModelStatus::Available
                : versions.contains(game) ? GameModelStatus::Missing : GameModelStatus::Unsupported;
            setGameCell(*item, kFirstGameColumn + index, game, status);
        }
        if (entry.id == currentId) {
            currentCar = item;
        }
    }
    int sortColumn = kManufacturerColumn;
    Qt::SortOrder sortOrder = Qt::AscendingOrder;
    list->header()->setSortIndicatorShown(true);
    for (const int column : {kManufacturerColumn, kYearColumn, kModelColumn}) {
        list->header()->setSortIndicator(column, Qt::AscendingOrder);
        list->resizeColumnToContents(column);
    }
    list->sortItems(sortColumn, sortOrder);
    if (currentCar != nullptr) {
        list->setCurrentItem(currentCar);
        list->scrollToItem(currentCar, QAbstractItemView::PositionAtCenter);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *footer = new QHBoxLayout;
    footer->addWidget(new QLabel(QStringLiteral(
        "[\u2713] Model available, [~] Supported, model unavailable, [ ] Unsupported"), &dialog), 1);
    footer->addWidget(buttons);
    layout->addLayout(footer);
    auto *okButton = buttons->button(QDialogButtonBox::Ok);

    const auto updateOkEnabled = [&] {
        QTreeWidgetItem *current = list->currentItem();
        okButton->setEnabled(current != nullptr && !current->isHidden());
    };
    updateOkEnabled();

    const auto updateFilter = [&] {
        const QString needle = filter->text().trimmed();
        const QString sortedGame = sortColumn >= kFirstGameColumn ? games[sortColumn - kFirstGameColumn] : QString();
        const auto matchedGame = std::find_if(games.cbegin(), games.cend(), [&needle](const QString &game) {
            return game.compare(needle, Qt::CaseInsensitive) == 0
                || gameTitle(game).compare(needle, Qt::CaseInsensitive) == 0;
        });
        for (int row = 0; row < list->topLevelItemCount(); ++row) {
            QTreeWidgetItem *item = list->topLevelItem(row);
            const QStringList versions = item->data(kManufacturerColumn, kVersionsRole).toStringList();
            const QString rowText = item->toolTip(kModelColumn) + QLatin1Char(' ')
                + item->text(kManufacturerColumn) + QLatin1Char(' ') + item->text(kYearColumn);
            const bool matchesSearch = matchedGame != games.cend() ? versions.contains(*matchedGame)
                : rowText.contains(needle, Qt::CaseInsensitive);
            item->setHidden(!matchesSearch || (!sortedGame.isEmpty() && !versions.contains(sortedGame)));
        }
        QTreeWidgetItem *current = list->currentItem();
        if (current == nullptr || current->isHidden()) {
            for (int row = 0; row < list->topLevelItemCount(); ++row) {
                if (!list->topLevelItem(row)->isHidden()) {
                    list->setCurrentItem(list->topLevelItem(row));
                    break;
                }
            }
        }
        updateOkEnabled();
    };
    QObject::connect(filter, &QLineEdit::textChanged, list, updateFilter);
    QObject::connect(list->header(), &QHeaderView::sectionClicked, list, [&](int column) {
        const bool gameColumn = column >= kFirstGameColumn;
        sortOrder = gameColumn || (column == sortColumn && sortOrder == Qt::AscendingOrder)
            ? Qt::DescendingOrder : Qt::AscendingOrder;
        sortColumn = column;
        list->sortItems(sortColumn, sortOrder);
        list->header()->setSortIndicatorShown(!gameColumn);
        updateFilter();
    });
    QObject::connect(list, &QTreeWidget::currentItemChanged, list,
                     [&updateOkEnabled](QTreeWidgetItem *, QTreeWidgetItem *) { updateOkEnabled(); });
    const auto acceptCar = [&] {
        const QTreeWidgetItem *current = list->currentItem();
        if (current == nullptr || current->isHidden()) {
            return;
        }
        const int carId = current->data(0, Qt::UserRole).toInt();
        if (confirmMissingModel && carId != currentId
            && registry.findModel(carId, loadBehaviorSettings().gameFolders).path.isEmpty()
            && chooseMissingCarModel(&dialog, carId, {}) == MissingCarModelChoice::Cancel) {
            return;
        }
        dialog.accept();
    };
    QObject::connect(list, &QTreeWidget::itemDoubleClicked, &dialog, acceptCar);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, acceptCar);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    const QMargins margins = layout->contentsMargins();
    const int contentsWidth = list->header()->length() + 2 * list->frameWidth()
        + list->verticalScrollBar()->sizeHint().width() + margins.left() + margins.right();
    const int screenWidth = dialog.screen()->availableGeometry().width();
    dialog.resize(std::min(std::max(kCarDialogWidth, contentsWidth), screenWidth), kCarDialogHeight);
    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    QTreeWidgetItem *current = list->currentItem();
    if (current == nullptr) {
        return false;
    }
    if (outId != nullptr) {
        *outId = current->data(0, Qt::UserRole).toInt();
    }
    return true;
}

} // namespace gui
