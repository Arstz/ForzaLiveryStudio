#include "shape_name_store.h"

#include <QtCore>

namespace gui {
namespace {

QStringList candidateAssetPaths()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    return {
        QDir(appDir).filePath(QStringLiteral("assets/vector/shape_names.json")),
        QDir(cwd).filePath(QStringLiteral("assets/vector/shape_names.json")),
        QDir(cwd).filePath(QStringLiteral("cpp-port/assets/vector/shape_names.json")),
    };
}

} // namespace

bool ShapeNameStore::loadDefault(QString *error)
{
    for (const QString &path : candidateAssetPaths()) {
        if (QFile::exists(path)) {
            return loadFromFile(path, error);
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("shape_names.json was not found");
    }
    return false;
}

bool ShapeNameStore::loadFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("could not open names asset: %1").arg(path);
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("invalid names asset: %1").arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject root = document.object();
    QHash<int, QString> parsed;
    for (auto it = root.begin(); it != root.end(); ++it) {
        bool ok = false;
        const int shapeId = it.key().toInt(&ok);
        if (!ok || !it.value().isObject()) {
            continue;
        }
        const QString name = it.value().toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            parsed.insert(shapeId, name);
        }
    }

    names_ = parsed;
    return true;
}

QString ShapeNameStore::name(int shapeId) const
{
    return names_.value(shapeId);
}

bool ShapeNameStore::contains(int shapeId) const
{
    return names_.contains(shapeId);
}

} // namespace gui
