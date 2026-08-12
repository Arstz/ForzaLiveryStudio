#include "curve_fill.h"
#include "shape_geometry_store.h"

#include <QCoreApplication>
#include <QDir>

#include <iostream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QString assetDirectory = QDir::current().filePath(QStringLiteral("assets"));
    bool force = false;
    const QStringList arguments = app.arguments();
    for (int index = 1; index < arguments.size(); ++index) {
        if (arguments[index] == QStringLiteral("--force")) {
            force = true;
        } else if (arguments[index] == QStringLiteral("--assets")
                   && index + 1 < arguments.size()) {
            assetDirectory = arguments[++index];
        } else {
            std::cerr << "Usage: fls_curve_template_generator "
                         "[--assets directory] [--force]\n";
            return 2;
        }
    }

    gui::ShapeGeometryStore geometry;
    QString error;
    if (!geometry.loadDefault(&error)) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    const gui::CurveTemplateGenerationResult result =
        gui::generateCurveFillTemplates(
            geometry, QDir(assetDirectory).absolutePath(), force,
            [](const QString &, int completed, int total) {
                std::cout << "template " << completed << '/' << total << '\n';
            });
    if (!result.error.isEmpty()) {
        std::cerr << result.error.toStdString() << '\n';
        return 1;
    }
    if (result.cancelled) {
        std::cerr << "curve template generation cancelled\n";
        return 1;
    }
    std::cout << "generated=" << result.generated
              << " skipped=" << result.skipped << '\n';
    return 0;
}
