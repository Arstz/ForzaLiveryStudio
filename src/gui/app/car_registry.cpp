#include "car_registry.h"
#include "game_paths.h"
#include "theme_manager.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QHeaderView>
#include <QPainter>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace gui {
namespace {

const QColor kMissingModelColor(255, 165, 0);
const QColor kAvailableModelColor(Qt::white);
constexpr int kVersionsRole = Qt::UserRole + 1;
constexpr int kAvailableVersionsRole = Qt::UserRole + 2;
constexpr int kVersionSpacing = 10;
constexpr int kVersionMargin = 4;
constexpr int kCarDialogWidth = 1000;
constexpr int kCarDialogHeight = 560;

class CarVersionDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem itemOption(option);
        const QStringList versions = index.data(kVersionsRole).toStringList();
        const QStringList available = index.siblingAtColumn(1).data(kAvailableVersionsRole).toStringList();
        const QWidget *widget = option.widget;
        QStyle *style = widget != nullptr ? widget->style() : QApplication::style();

        if (index.column() == 0) {
            if (available.isEmpty()) itemOption.palette.setColor(QPalette::HighlightedText, kMissingModelColor);
            QStyledItemDelegate::paint(painter, itemOption, index);
            return;
        }
        initStyleOption(&itemOption, index);
        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &itemOption, widget);
        int x = textRect.left();
        itemOption.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &itemOption, painter, widget);
        painter->save();
        painter->setClipRect(textRect);
        painter->setFont(itemOption.font);
        for (const QString &version : versions) {
            const int width = itemOption.fontMetrics.horizontalAdvance(version);
            painter->setPen(available.contains(version) ? kAvailableModelColor : kMissingModelColor);
            painter->drawText(QRect(x, textRect.top(), width, textRect.height()), Qt::AlignLeft | Qt::AlignVCenter, version);
            x += width + kVersionSpacing;
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        const QStringList versions = index.data(kVersionsRole).toStringList();
        const int width = std::accumulate(versions.cbegin(), versions.cend(), 0,
            [&option](int total, const QString &version) { return total + option.fontMetrics.horizontalAdvance(version) + kVersionSpacing; });

        if (index.column() == 1) size.setWidth(width + 2 * kVersionMargin);

        return size;
    }
};

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
    QHash<int, QHash<QString, QString>> parsedGameModels;
    for (auto it = root.begin(); it != root.end(); ++it) {
        bool ok = false;
        const int id = it.key().toInt(&ok);
        if (!ok) {
            continue;
        }
        QString name;
        QString model;
        QString version = QStringLiteral("FH6");
        if (it.value().isObject()) {
            const QJsonObject entry = it.value().toObject();
            model = entry.value(QStringLiteral("model")).toString().trimmed();
            name = entry.value(QStringLiteral("name")).toString().trimmed();
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
        }
        if (!model.isEmpty()) {
            parsedModels.insert(id, model);
        }
    }

    names_ = parsed;
    models_ = parsedModels;
    gameModels_ = parsedGameModels;
    versions_ = parsedVersions;
    sorted_.clear();
    sorted_.reserve(names_.size());
    for (auto it = names_.constBegin(); it != names_.constEnd(); ++it) {
        sorted_.push_back({it.key(), it.value(), versions_.value(it.key())});
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
        if (!isSupported(id, game)) continue;
        for (const QString &directory : fls::gameCarsDirs(gameFolders.value(game))) {
            const QString path = fls::findCarModelPath(directory, modelCode(id, game));
            if (!path.isEmpty()) return {path, game};
        }
    }

    return {};
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

const CarRegistry &sharedCarRegistry() {
    static const CarRegistry registry = [] {
        CarRegistry r;
        r.loadDefault(nullptr);
        return r;
    }();
    return registry;
}

bool chooseCarModel(QWidget *parent, int currentId, int *outId) {
    const CarRegistry &registry = sharedCarRegistry();
    const auto availability = registry.modelAvailability(loadBehaviorSettings().gameFolders);

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Choose Target Car"));
    dialog.resize(kCarDialogWidth, kCarDialogHeight);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Select the car this livery is for:"), &dialog));

    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Filter…"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    auto *list = new QTreeWidget(&dialog);
    list->setHeaderLabels({QStringLiteral("Car"), QStringLiteral("Supported versions")});
    list->setRootIsDecorated(false);
    list->setItemsExpandable(false);
    list->setUniformRowHeights(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setItemDelegate(new CarVersionDelegate(list));
    list->header()->setStretchLastSection(false);
    list->header()->setResizeContentsPrecision(-1);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(list, 1);
    for (const CarRegistry::Entry &entry : registry.entries()) {
        QStringList versions;
        QStringList versionTips;
        const QStringList available = availability.value(entry.id);
        for (const QString &token : entry.version.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString version = token.trimmed().toUpper();
            if (version.isEmpty() || versions.contains(version)) continue;
            versions.push_back(version);
            versionTips.push_back(QStringLiteral("%1: %2").arg(version,
                available.contains(version) ? QStringLiteral("model found") : QStringLiteral("model missing")));
        }
        auto *item = new QTreeWidgetItem(list, {entry.name, versions.join(QStringLiteral("  "))});
        item->setData(0, Qt::UserRole, entry.id);
        item->setData(1, kVersionsRole, versions);
        item->setData(1, kAvailableVersionsRole, available);
        item->setToolTip(0, entry.name);
        item->setToolTip(1, versionTips.join(QLatin1Char('\n')));
        if (available.isEmpty()) item->setForeground(0, kMissingModelColor);
        if (entry.id == currentId) {
            list->setCurrentItem(item);
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto *okButton = buttons->button(QDialogButtonBox::Ok);

    const auto updateOkEnabled = [&] {
        QTreeWidgetItem *current = list->currentItem();
        okButton->setEnabled(current != nullptr && !current->isHidden());
    };
    updateOkEnabled();

    QObject::connect(filter, &QLineEdit::textChanged, list, [list, &updateOkEnabled](const QString &text) {
        const QString needle = text.trimmed();
        for (int row = 0; row < list->topLevelItemCount(); ++row) {
            QTreeWidgetItem *item = list->topLevelItem(row);
            const QString rowText = item->text(0) + QLatin1Char(' ') + item->text(1);
            item->setHidden(!needle.isEmpty() && !rowText.contains(needle, Qt::CaseInsensitive));
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
    });
    QObject::connect(list, &QTreeWidget::currentItemChanged, list,
                     [&updateOkEnabled](QTreeWidgetItem *, QTreeWidgetItem *) { updateOkEnabled(); });
    QObject::connect(list, &QTreeWidget::itemDoubleClicked, &dialog, [&dialog](QTreeWidgetItem *, int) { dialog.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

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
