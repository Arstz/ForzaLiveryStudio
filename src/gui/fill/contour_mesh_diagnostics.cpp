#include "contour_mesh_diagnostics.h"

#include "scene_codec.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace gui {
namespace {

const QString kMeshReportName = QStringLiteral("mesh_reconstruction_latest.json");
const QString kMeshLogName = QStringLiteral("mesh_reconstruction.log");

QJsonObject meshSnapshot(const fls::scene::MeshData &data, const QString &id) {
    fls::scene::Group root;
    auto mesh = std::make_unique<fls::scene::Mesh>();

    mesh->id = id;
    mesh->data = data;
    root.append(std::move(mesh));

    return fls::scene::sceneTreeToJson(root);
}

QString writeReport(const QString &directory, const QJsonObject &report, const QJsonObject &summary, QString *error) {
    const QString path = QDir(directory).filePath(kMeshReportName);
    QSaveFile latest(path);
    QFile log(QDir(directory).filePath(kMeshLogName));
    const QByteArray reportBytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const QByteArray summaryBytes = QJsonDocument(summary).toJson(QJsonDocument::Compact) + '\n';

    if (!QDir().mkpath(directory) || !latest.open(QIODevice::WriteOnly)) {
        *error = latest.errorString();
        return {};
    }
    if (latest.write(reportBytes) != reportBytes.size() || !latest.commit()) {
        *error = latest.errorString();
        return {};
    }
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append) || log.write(summaryBytes) != summaryBytes.size()) {
        qWarning().noquote() << QStringLiteral("Mesh reconstruction summary log: %1").arg(log.errorString());
    }

    return path;
}

} // namespace

QString writeMeshReconstructionDiagnostics(const fls::scene::MeshData &input, const fls::scene::MeshData *output,
                                           QJsonObject report, const QVector<QString> &sourceEntries,
                                           const QString &directory) {
    QJsonArray entries;
    QString error;

    for (const auto &entry : sourceEntries) entries.append(entry);
    report.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    report.insert(QStringLiteral("runId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    report.insert(QStringLiteral("sourceEntries"), entries);
    auto summary = report;
    summary.remove(QStringLiteral("decisions"));
    summary.remove(QStringLiteral("subdivision"));
    summary.remove(QStringLiteral("corridorProfiles"));
    report.insert(QStringLiteral("inputScene"), meshSnapshot(input, QStringLiteral("input_mesh")));
    if (output != nullptr) report.insert(QStringLiteral("outputScene"), meshSnapshot(*output, QStringLiteral("output_mesh")));
    QString path = writeReport(directory.isEmpty() ? QCoreApplication::applicationDirPath() : directory, report, summary, &error);
    if (path.isEmpty() && directory.isEmpty()) {
        const QString fallback = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
            .filePath(QStringLiteral("diagnostics"));
        path = writeReport(fallback, report, summary, &error);
    }
    if (path.isEmpty()) qWarning().noquote() << QStringLiteral("Mesh reconstruction report failed: %1").arg(error);
    else qInfo().noquote() << QStringLiteral("Mesh reconstruction %1: %2")
        .arg(report.value(QStringLiteral("accepted")).toBool() ? QStringLiteral("accepted") : QStringLiteral("rejected"), path);

    return path;
}

} // namespace gui
