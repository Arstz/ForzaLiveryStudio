#include "binary_io.h"
#include "cgroup_codec.h"
#include "export_asset_dialog.h"
#include "game_export.h"
#include "header_codec.h"
#include "layer.h"
#include "project_codec.h"

#include <QtWidgets>

#include <memory>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not read export");
    return file.readAll();
}

void testMaskWarning(const QString &directory, bool livery, quint64 maskCount) {
    bool checked = false;
    QTimer::singleShot(0, [&checked, maskCount]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            QApplication::exit(1);
            return;
        }
        auto *warning = dialog->findChild<QLabel *>("exportMaskWarning");
        for (auto *combo : dialog->findChildren<QComboBox *>()) {
            if (combo->findText("Forza Horizon 6") < 0 || !warning) {
                continue;
            }
            checked = warning->isHidden();
            for (const auto game : {fls::ExportGameVersion::Horizon2, fls::ExportGameVersion::Horizon3,
                    fls::ExportGameVersion::Motorsport5, fls::ExportGameVersion::Motorsport6,
                    fls::ExportGameVersion::Horizon4, fls::ExportGameVersion::Horizon5,
                    fls::ExportGameVersion::Horizon6, fls::ExportGameVersion::Motorsport7,
                    fls::ExportGameVersion::Motorsport2023}) {
                combo->setCurrentIndex(combo->findData(static_cast<int>(game)));
                const bool expected = maskCount > 0 && !fls::exportGameSupportsMasks(game);
                checked &= warning->isHidden() == !expected;
                if (expected) {
                    checked &= warning->text() == QString("Masks are not supported in %1 (%2 layers)")
                        .arg(combo->currentText()).arg(maskCount);
                }
                checked &= warning->property("uiState").toString() == "warning";
                checked &= dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled();
            }
            break;
        }
        dialog->reject();
    });
    gui::showExportAssetDialog(nullptr, directory, "Mask test", livery, 1069, [maskCount](auto game) {
        return fls::ExportLayerWarnings{fls::exportGameSupportsMasks(game) ? 0 : maskCount};
    });
    require(checked, "mask warning visibility, count, color, or export availability");
}

void testSelection(const QString &directory, bool livery) {
    require(QDir().mkpath(directory), "export destination directory");
    bool selected = false;
    QTimer::singleShot(0, [&selected]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            QApplication::exit(1);
            return;
        }
        for (auto *combo : dialog->findChildren<QComboBox *>()) {
            const int index = combo->findText("Forza Horizon 5");
            if (index >= 0) {
                combo->setCurrentIndex(index);
                auto *buttons = dialog->findChild<QDialogButtonBox *>();
                if (buttons != nullptr && buttons->button(QDialogButtonBox::Save)->isEnabled()) {
                    selected = true;
                    buttons->button(QDialogButtonBox::Save)->click();
                    return;
                }
            }
        }
        dialog->reject();
    });
    const auto selection = gui::showExportAssetDialog(nullptr, directory, "Target test", livery, 1069);
    require(selected && selection.has_value(), "could not select and accept FH5 export");
    require(selection->gameVersion == fls::ExportGameVersion::Horizon5, "FH5 selection returned another game");
    require(selection->containerFormat == gui::ExportContainerFormat::PgsFolder, "PGS selection changed");

    fls::Project project;
    project.name = "Target test";
    project.isLivery = livery;
    project.carId = livery ? 1069 : 0;
    project.root = std::make_unique<fls::scene::Group>();
    project.headerMetadata = fls::defaultDraftHeader(project.name, "Roundtrip", project.carId);
    for (int slot = 0; slot < (livery ? 6 : 1); ++slot) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->setVectorShape(101);
        shape->x = slot;
        if (livery) {
            auto section = std::make_unique<fls::scene::Group>();
            section->isLiverySection = true;
            section->liverySectionSlot = slot;
            section->append(std::move(shape));
            project.root->append(std::move(section));
        } else {
            project.root->append(std::move(shape));
        }
    }
    if (livery) {
        fls::exportGameLivery(project, selection->directory, selection->gameVersion);
    } else {
        fls::exportGameGroup(project, selection->directory, selection->gameVersion);
    }
    const auto header = fls::parseHeader(readFile(QDir(selection->directory).filePath("header")));
    require(header.formatVersion == 6, "FH5 selection wrote another header format");
    const QByteArray payload = fls::inflateContainer(readFile(
        QDir(selection->directory).filePath(livery ? "C_livery" : "C_group")));
    require(livery ? fls::detail::readLeU32(payload, 29) == 18 : payload[12] == 3,
        "FH5 selection wrote another payload format");
}

void testShapeWarning(const QString &directory) {
    using Game = fls::ExportGameVersion;
    fls::Project project;
    for (const quint16 id : {2101, 2301}) {
        auto shape = std::make_unique<fls::scene::Shape>();
        shape->setVectorShape(id);
        project.root->append(std::move(shape));
    }
    bool checked = false;
    QTimer::singleShot(0, [&checked]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            QApplication::exit(1);
            return;
        }
        const auto *warning = dialog->findChild<QLabel *>("exportShapeWarning");
        for (auto *combo : dialog->findChildren<QComboBox *>()) {
            if (combo->findText("Forza Horizon 6") < 0 || !warning) {
                continue;
            }
            checked = warning->isHidden();
            combo->setCurrentIndex(combo->findData(static_cast<int>(Game::Motorsport3)));
            checked &= !warning->isHidden() && warning->text().endsWith("(2 layers)");
            combo->setCurrentIndex(combo->findData(static_cast<int>(Game::Motorsport4)));
            checked &= !warning->isHidden() && warning->text().endsWith("(1 layers)");
            checked &= dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled();
            combo->setCurrentIndex(combo->findData(static_cast<int>(Game::Horizon6)));
            checked &= warning->isHidden();
            break;
        }
        dialog->reject();
    });
    gui::showExportAssetDialog(nullptr, directory, "Shape test", false, 0,
        [&project](Game game) { return fls::exportLayerWarnings(project, game); });
    require(checked, "shape warning responds to the selected game");
}

void testWarningChoices(const QString &directory) {
    using Game = fls::ExportGameVersion;
    bool checked = false;
    QTimer::singleShot(0, [&checked]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *car = dialog->findChild<QLabel *>("exportCarWarning");
        auto *carChoice = dialog->findChild<QComboBox *>("exportCarWarningChoice");
        auto *maskChoice = dialog->findChild<QComboBox *>("exportMaskWarningChoice");
        auto *logoChoice = dialog->findChild<QComboBox *>("exportLogoWarningChoice");
        auto *shapeChoice = dialog->findChild<QComboBox *>("exportShapeWarningChoice");
        auto *button = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
        for (auto *combo : dialog->findChildren<QComboBox *>()) {
            if (combo->findText("Forza Horizon 6") < 0) continue;
            const QString anyway = "Export anyway (I know what I am doing)";
            if (!car || !carChoice || !maskChoice || !logoChoice || !shapeChoice) break;
            checked = car->isHidden() && button->text() == "Export";
            combo->setCurrentIndex(combo->findData(static_cast<int>(Game::Horizon2)));
            checked &= button->isEnabled() && !maskChoice->parentWidget()->isHidden()
                && !logoChoice->parentWidget()->isHidden() && !shapeChoice->parentWidget()->isHidden();
            checked &= maskChoice->currentText() == "Discard" && logoChoice->currentText() == "Discard"
                && shapeChoice->currentText() == "Discard";
            maskChoice->setCurrentIndex(maskChoice->findText("Keep"));
            checked &= button->text() == anyway;
            maskChoice->setCurrentIndex(maskChoice->findText("Discard"));
            logoChoice->setCurrentIndex(logoChoice->findText("Keep"));
            checked &= button->text() == anyway;
            combo->setCurrentIndex(combo->findData(static_cast<int>(Game::Horizon6)));
            checked &= logoChoice->parentWidget()->isHidden() && button->text() == "Export";
            combo->setCurrentIndex(combo->findData(static_cast<int>(Game::Horizon2)));
            shapeChoice->setCurrentIndex(shapeChoice->findText("Discard"));
            if (!button->isEnabled()) break;
            button->click();
            return;
        }
        dialog->reject();
    });
    const auto selected = gui::showExportAssetDialog(nullptr, directory, "Warnings", true, 1069, [](Game game) {
        return game == Game::Horizon2 ? fls::ExportLayerWarnings{2, 3, 4} : fls::ExportLayerWarnings{};
    });
    require(checked && selected && selected->layerChoices.keepLogos
        && !selected->layerChoices.keepMasks && !selected->layerChoices.keepShapes, "independent warning choices");

    bool acceptedUnsupportedCar = false;
    QTimer::singleShot(0, [&acceptedUnsupportedCar]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        const auto *car = dialog->findChild<QLabel *>("exportCarWarning");
        const auto *choice = dialog->findChild<QComboBox *>("exportCarWarningChoice");
        auto *button = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
        acceptedUnsupportedCar = car && !car->isHidden() && choice && choice->currentText() == "Keep"
            && choice->count() == 1 && button->isEnabled()
            && button->text() == "Export anyway (I know what I am doing)";
        button->click();
        if (!acceptedUnsupportedCar) dialog->reject();
    });
    const auto unsupported = gui::showExportAssetDialog(nullptr, directory, "Car test", true, 999999);
    require(acceptedUnsupportedCar && unsupported, "car warning must not block export");
}

} // namespace

int main(int argc, char *argv[]) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("FLS tests");
    QCoreApplication::setApplicationName("Export target");
    QTemporaryDir output;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, output.path());
    try {
        require(output.isValid(), "temporary directory");
        testSelection(QDir(output.path()).filePath("group"), false);
        testSelection(QDir(output.path()).filePath("livery"), true);
        for (bool livery : {false, true}) {
            testMaskWarning(output.path(), livery, 7);
            testMaskWarning(output.path(), livery, 0);
        }
        testShapeWarning(output.path());
        testWarningChoices(output.path());
        qInfo() << "FH5 export target selection passed for groups and liveries";
        return 0;
    } catch (const std::exception &error) {
        qCritical() << error.what();
        return 1;
    }
}
