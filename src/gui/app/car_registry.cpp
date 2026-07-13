#include "car_registry.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace gui {
namespace {

QStringList candidateAssetPaths()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/cars/car_ids.json")),
        QDir(cwd).filePath(QStringLiteral("assets/cars/car_ids.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/cars/car_ids.json")),
    };
}

} // namespace

bool CarRegistry::loadDefault(QString *error)
{
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

bool CarRegistry::loadFromFile(const QString &path, QString *error)
{
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
    for (auto it = root.begin(); it != root.end(); ++it) {
        bool ok = false;
        const int id = it.key().toInt(&ok);
        if (!ok) {
            continue;
        }
        QString name;
        QString model;
        if (it.value().isObject()) {
            const QJsonObject entry = it.value().toObject();
            model = entry.value(QStringLiteral("model")).toString().trimmed();
            name = entry.value(QStringLiteral("name")).toString().trimmed();
            if (name.isEmpty()) {
                name = model;
            }
        } else {
            name = it.value().toString().trimmed();
        }
        if (!name.isEmpty()) {
            parsed.insert(id, name);
        }
        if (!model.isEmpty()) {
            parsedModels.insert(id, model);
        }
    }

    names_ = parsed;
    models_ = parsedModels;
    sorted_.clear();
    sorted_.reserve(names_.size());
    for (auto it = names_.constBegin(); it != names_.constEnd(); ++it) {
        sorted_.push_back({it.key(), it.value()});
    }
    std::sort(sorted_.begin(), sorted_.end(), [](const Entry &a, const Entry &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return true;
}

QString CarRegistry::name(int id) const
{
    return names_.value(id);
}

QString CarRegistry::modelCode(int id) const
{
    const QString code = models_.value(id);
    return code.isEmpty() ? names_.value(id) : code;
}

QString CarRegistry::displayName(int id) const
{
    if (id == 0) {
        return QString();
    }
    const QString found = names_.value(id);
    return found.isEmpty() ? QStringLiteral("Unknown car (#%1)").arg(id) : found;
}

const CarRegistry &sharedCarRegistry()
{
    static const CarRegistry registry = [] {
        CarRegistry r;
        r.loadDefault(nullptr);
        return r;
    }();
    return registry;
}

bool chooseCarModel(QWidget *parent, int currentId, int *outId)
{
    const CarRegistry &registry = sharedCarRegistry();

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Choose Target Car"));
    dialog.resize(360, 460);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("Select the car this livery is for:"), &dialog));

    auto *filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Filter…"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    auto *list = new QListWidget(&dialog);
    layout->addWidget(list, 1);
    for (const CarRegistry::Entry &entry : registry.entries()) {
        auto *item = new QListWidgetItem(entry.name, list);
        item->setData(Qt::UserRole, entry.id);
        if (entry.id == currentId) {
            list->setCurrentItem(item);
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    auto *okButton = buttons->button(QDialogButtonBox::Ok);

    const auto updateOkEnabled = [&] {
        QListWidgetItem *current = list->currentItem();
        okButton->setEnabled(current != nullptr && !current->isHidden());
    };
    updateOkEnabled();

    QObject::connect(filter, &QLineEdit::textChanged, list, [list, &updateOkEnabled](const QString &text) {
        const QString needle = text.trimmed();
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            item->setHidden(!needle.isEmpty() && !item->text().contains(needle, Qt::CaseInsensitive));
        }
        QListWidgetItem *current = list->currentItem();
        if (current == nullptr || current->isHidden()) {
            for (int row = 0; row < list->count(); ++row) {
                if (!list->item(row)->isHidden()) {
                    list->setCurrentRow(row);
                    break;
                }
            }
        }
        updateOkEnabled();
    });
    QObject::connect(list, &QListWidget::currentItemChanged, list,
                     [&updateOkEnabled](QListWidgetItem *, QListWidgetItem *) { updateOkEnabled(); });
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem *) { dialog.accept(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    filter->setFocus();
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }
    QListWidgetItem *current = list->currentItem();
    if (current == nullptr) {
        return false;
    }
    if (outId != nullptr) {
        *outId = current->data(Qt::UserRole).toInt();
    }
    return true;
}

} // namespace gui
