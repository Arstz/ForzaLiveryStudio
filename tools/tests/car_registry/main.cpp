#include "car_registry.h"

#include <QCoreApplication>
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
        "1":{"model":"First","name":"First car","version":"FH1, FH5, FM7"},
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
        verifyDefaultCatalog();
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << Qt::endl;
        return 1;
    }

    return 0;
}
