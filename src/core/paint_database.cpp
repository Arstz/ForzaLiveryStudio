#include "paint_database.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>

#include <stdexcept>
#include <utility>

namespace fls {
namespace {

struct FinishIndex {
    QHash<quint32, QString> materials;
    QHash<QString, int> codes;
};

QJsonObject loadPaintDatabase() {
    const QString relativePath = QStringLiteral("assets/cars/paint_colors.json");
    const QStringList candidates = {
        QDir(QCoreApplication::applicationDirPath()).filePath(relativePath),
        QDir::current().filePath(relativePath),
    };
    for (const QString &path : candidates) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || document.object().value(QStringLiteral("version")).toInt() != 1) {
            throw std::runtime_error("invalid paint_colors.json");
        }

        return document.object().value(QStringLiteral("games")).toObject();
    }
    throw std::runtime_error("paint_colors.json was not found");
}

QString finishMaterial(const QJsonObject &finish) {
    const QString suffix = QStringLiteral(".materialbin");
    QString path = finish.value(QStringLiteral("Material")).toString();
    if (path.isEmpty()) {
        path = finish.value(QStringLiteral("MaterialPath")).toString();
    }
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString material = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1).toLower();
    if (material.endsWith(suffix)) {
        material.chop(suffix.size());
    }
    if (material == QStringLiteral("metalflakelarge")) {
        material = QStringLiteral("metallicpaint");
    }

    return material;
}

const QJsonObject &paintDatabase() {
    static const QJsonObject games = loadPaintDatabase();

    return games;
}

const QHash<QString, FinishIndex> &finishIndices() {
    static const QHash<QString, FinishIndex> indices = [] {
        QHash<QString, FinishIndex> result;
        const QJsonObject &games = paintDatabase();
        for (auto game = games.begin(); game != games.end(); ++game) {
            FinishIndex index;
            const QJsonArray finishes = game.value().toObject().value(QStringLiteral("finishes")).toArray();
            for (const QJsonValue &value : finishes) {
                const QJsonObject finish = value.toObject();
                const QString material = finishMaterial(finish);
                const int code = finish.value(QStringLiteral("ID")).toInt();
                index.materials.insert(static_cast<quint32>(code), material);
                index.codes.insert(material, code);
            }
            result.insert(game.key(), std::move(index));
        }

        return result;
    }();

    return indices;
}

} // namespace

QJsonObject gamePaintDatabase(const QString &game) {
    return paintDatabase().value(game.isEmpty() ? QStringLiteral("FH6") : game).toObject();
}

int matchingPaintFinish(quint32 code, const QString &sourceGame, const QString &targetGame) {
    if (code == 0) {
        return 0;
    }
    const auto &indices = finishIndices();
    const auto source = indices.constFind(sourceGame.isEmpty() ? QStringLiteral("FH6") : sourceGame);
    const auto target = indices.constFind(targetGame.isEmpty() ? QStringLiteral("FH6") : targetGame);
    if (source == indices.end() || target == indices.end()) {
        return -1;
    }

    return target->codes.value(source->materials.value(code), -1);
}

quint32 exportedPaintFinish(quint32 code, const QString &sourceGame, const QString &targetGame) {
    if (code == 0 || sourceGame == targetGame) {
        return code;
    }
    const int mapped = matchingPaintFinish(code, sourceGame, targetGame);
    if (mapped < 0) {
        throw std::runtime_error(QStringLiteral("paint finish %1 from %2 is unavailable in %3")
            .arg(code).arg(sourceGame.isEmpty() ? QStringLiteral("FH6") : sourceGame, targetGame).toStdString());
    }

    return static_cast<quint32>(mapped);
}

} // namespace fls
