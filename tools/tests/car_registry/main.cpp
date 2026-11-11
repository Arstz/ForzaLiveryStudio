#include "car_registry.h"
#include "game_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>

#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void verifyGameSupport() {
    QTemporaryFile file;
    gui::CarRegistry registry;
    QString error;
    const QByteArray catalog = R"({
        "1":{"model":"First","name":"First car","version":"FH1, FH5, FM7","games":{"FM7":{"model":"FirstLegacy"}}},
        "2":{"model":"Second","name":"Second car","version":"FM4, FH4"},
        "3":{"model":"Third","name":"Third car","version":"FH5, fh6 , FM2023"},
        "4":{"model":"Fourth","name":"Fourth car","version":"FH5"},
        "5":{"model":"Fifth","name":"Fifth car"}
    })";

    require(file.open(), "Could not create car registry fixture");
    require(file.write(catalog) == catalog.size() && file.flush(), "Could not write car registry fixture");
    require(registry.loadFromFile(file.fileName(), &error), "Could not load car registry fixture");
    require(registry.isFh6Unsupported(1), "Mixed game list bypasses FH6 warning");
    require(registry.isFh6Unsupported(2), "Older car bypasses FH6 warning");
    require(registry.isFh6Supported(3) && !registry.isFh6Unsupported(3), "FH6 tag was not recognized");
    require(registry.isFh6Unsupported(4), "FH5 car bypasses FH6 warning");
    require(registry.isFh6Supported(5), "Default game compatibility changed");
    require(!registry.isFh6Supported(0) && !registry.isFh6Unsupported(0), "Unset car is classified as known");
    require(!registry.isFh6Supported(99) && !registry.isFh6Unsupported(99), "Unknown car is classified as known");
    require(registry.modelCode(1) == QStringLiteral("First"), "Model code changed during loading");
    require(registry.modelCode(1, QStringLiteral("fm7")) == QStringLiteral("FirstLegacy"), "Game model override was ignored");
    require(registry.modelCode(1, QStringLiteral("FH5")) == QStringLiteral("First"), "Canonical model fallback was lost");
    require(registry.isSupported(1, QStringLiteral("FM7")), "Game availability was ignored");
}

void verifyModelSources() {
    QTemporaryDir temporary;
    QTemporaryFile catalog;
    gui::CarRegistry registry;
    QString error;
    const QByteArray metadata = R"({
        "1":{"model":"Current","name":"Car","version":"FH6, FM7, FM2023","games":{"FM7":{"model":"Legacy"}}},
        "2":{"model":"Current","name":"Older car","version":"FM7","games":{"FM7":{"model":"Legacy"}}},
        "3":{"model":"Alias","name":"Folder-named car","version":"FM7"},
        "4":{"model":"Missing","name":"Missing car","version":"FH6, FM7"}
    })";

    require(temporary.isValid() && catalog.open(), "Could not create model lookup fixture");
    require(catalog.write(metadata) == metadata.size() && catalog.flush(), "Could not write model lookup catalog");
    require(registry.loadFromFile(catalog.fileName(), &error), "Could not load model lookup catalog");
    const QDir root(temporary.path());
    const auto createFile = [&root](const QString &relative) {
        const QString path = root.filePath(relative);
        require(QDir().mkpath(QFileInfo(path).absolutePath()), "Could not create model fixture directory");
        QFile file(path);
        require(file.open(QIODevice::WriteOnly), "Could not create model fixture");
        require(file.write("fixture") == 7, "Could not write model fixture");

        return path;
    };
    const QString legacy = createFile(QStringLiteral("FM7/media/pcfamily/Cars/Legacy/Legacy.carbin"));
    createFile(QStringLiteral("FM7/media/pcfamily/Cars/Alias/parts.CARBIN"));
    require(root.mkpath(QStringLiteral("FM7/media/Cars")), "Could not create alternate car directory");
    require(root.mkpath(QStringLiteral("FH6/media/Cars")), "Could not create current car directory");
    QMap<QString, QString> folders = {
        {QStringLiteral("FH6"), root.filePath(QStringLiteral("FH6"))},
        {QStringLiteral("FM7"), root.filePath(QStringLiteral("FM7"))},
    };
    auto source = registry.findModel(1, folders);
    const auto initialAvailability = registry.modelAvailability(folders);
    require(initialAvailability.value(1) == QStringList{QStringLiteral("FM7")}, "Model availability ignored a game override");
    require(initialAvailability.value(2) == QStringList{QStringLiteral("FM7")}, "Older car was marked missing");
    require(initialAvailability.value(3) == QStringList{QStringLiteral("FM7")}, "Named model folder was marked missing");
    require(!initialAvailability.contains(4), "Missing car was marked available");
    require(registry.modelAvailability({}).isEmpty(), "Unset folders reported available models");
    require(source.path == legacy && source.game == QStringLiteral("FM7"), "Missing current model did not fall back to pcfamily");
    require(fls::gameCarsDirs(folders.value(QStringLiteral("FM7"))).size() == 2, "Both car directories were not probed");
    require(fls::gameCarsDir(root.filePath(QStringLiteral("FM7/media/pcfamily"))) ==
            root.filePath(QStringLiteral("FM7/media/pcfamily/Cars")), "Resource folder selection failed");
    const QString motorsport = createFile(QStringLiteral("FM2023/media/pcfamily/Cars/Current.zip"));
    folders.insert(QStringLiteral("FM2023"), root.filePath(QStringLiteral("FM2023/media")));
    source = registry.findModel(1, folders);
    require(source.path == motorsport && source.game == QStringLiteral("FM2023"), "Newer configured fallback did not take priority");
    const QString current = createFile(QStringLiteral("FH6/media/Cars/Current.zip"));
    const auto availability = registry.modelAvailability(folders);
    require(availability.value(1).size() == 3 && availability.value(1).contains(QStringLiteral("FH6"))
        && availability.value(1).contains(QStringLiteral("FM7")) && availability.value(1).contains(QStringLiteral("FM2023")),
        "Availability did not include every configured game");
    require(availability.value(2) == QStringList{QStringLiteral("FM7")}, "Availability included an unsupported game");
    require(!availability.contains(4), "Missing model inherited another car's availability");
    source = registry.findModel(1, folders);
    require(source.path == current && source.game == QStringLiteral("FH6"), "FH6 did not take priority");
    source = registry.findModel(1, folders, QStringLiteral("FM7"));
    require(source.path == legacy && source.game == QStringLiteral("FM7"), "Imported game's model did not take priority");
    source = registry.findModel(1, folders, QStringLiteral("FH5"));
    require(source.path == current && source.game == QStringLiteral("FH6"), "Missing imported game's model blocked fallback");
    source = registry.findModel(2, folders);
    require(source.path == legacy && source.game == QStringLiteral("FM7"), "Unavailable game bypassed car support");
    require(registry.findModel(99, folders).path.isEmpty(), "Unknown car resolved a model");
    require(registry.findModel(1, {}).path.isEmpty(), "Unset game folders searched the working directory");
    require(fls::gameCarsDirs(root.filePath(QStringLiteral("missing"))).isEmpty(), "Missing game folder resolved car directories");
}

void verifyDefaultCatalog() {
    gui::CarRegistry registry;
    QString error;

    require(registry.loadDefault(&error), "Could not load default car registry");
    require(!registry.isEmpty(), "Default car registry is empty");
    for (const auto &entry : registry.entries()) {
        require(!registry.modelCode(entry.id).isEmpty(), "Car has no model code");
        require(registry.isFh6Supported(entry.id) != registry.isFh6Unsupported(entry.id),
                "Known car has ambiguous FH6 support");
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);

    try {
        verifyGameSupport();
        verifyModelSources();
        verifyDefaultCatalog();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << Qt::endl;
        return 1;
    }

    return 0;
}
