#include "shape_name_store.h"

#include <QtCore>

namespace fls {
namespace {

QStringList candidateAssetPaths() {
    const QString cwd = QDir::currentPath();
    const QString catalog = QStringLiteral("assets/vector/shape_names.json");
    QStringList paths;
    if (QCoreApplication::instance() != nullptr) {
        paths.append(QDir(QCoreApplication::applicationDirPath()).filePath(catalog));
    }
    paths.append(QDir(cwd).filePath(catalog));
    paths.append(QDir(cwd).filePath(QStringLiteral("cpp-port/") + catalog));

    return paths;
}

} // namespace

bool ShapeNameStore::loadDefault(QString *error) {
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

bool ShapeNameStore::loadFromFile(const QString &path, QString *error) {
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

QString ShapeNameStore::name(int shapeId) const {
    return names_.value(shapeId);
}

bool ShapeNameStore::contains(int shapeId) const {
    return names_.contains(shapeId);
}

QString friendlyShapeLayerName(quint16 shapeId) {
    static const ShapeNameStore names = []() {
        ShapeNameStore store;
        store.loadDefault();

        return store;
    }();
    const QString name = names.name(shapeId);

    return name.isEmpty() ? QString() : QStringLiteral("%1 (%2)").arg(name).arg(shapeId);
}

} // namespace fls
