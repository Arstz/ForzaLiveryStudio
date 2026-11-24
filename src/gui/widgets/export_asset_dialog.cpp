#include "export_asset_dialog.h"
#include "car_registry.h"
#include "theme_manager.h"

#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QVariant>
#include <QVBoxLayout>

#include <exception>

namespace gui {

bool supportsExportContainer(ExportGameVersion game, ExportContainerFormat format) {
    const bool xbox360 = fls::isXbox360ExportGame(game);

    switch (format) {
    case ExportContainerFormat::PgsFolder:
        return !xbox360;
    case ExportContainerFormat::WgsAppend:
        return game == ExportGameVersion::Horizon5;
    case ExportContainerFormat::WgsFh5ToMotorsport:
        return game == ExportGameVersion::Motorsport2023;
    case ExportContainerFormat::Xbox360PlainFile:
        return xbox360;
    case ExportContainerFormat::Xbox360SignedFile:
        return xbox360 && game != ExportGameVersion::Motorsport2;
    }

    return false;
}

namespace {

constexpr int kDialogWidth = 640;
constexpr int kDialogHeight = 400;
constexpr int kLayoutMargin = kUiMargin;
constexpr int kLayoutSpacing = kUiSpacing;
constexpr int kComboLabelInset = 2;
constexpr char kGeometrySettingsKey[] = "export/dialogGeometry";
constexpr char kNativeIndexSettingsKey[] = "export/wgsFh5Index";
constexpr char kImportIndexSettingsKey[] = "export/wgsFh5ImportIndex";
const QString kDiscardChoiceText = QStringLiteral("Discard");

struct ExportContainerOption {
    ExportContainerFormat format;
    const char *label;
};

constexpr ExportContainerOption kContainerOptions[] = {
    {ExportContainerFormat::PgsFolder, "PGS / asset folder"},
    {ExportContainerFormat::WgsAppend, "WGS / add new container"},
    {ExportContainerFormat::WgsFh5ToMotorsport, "WGS (FH5->FM2023)"},
    {ExportContainerFormat::Xbox360PlainFile, "Xbox 360 plaintext file"},
    {ExportContainerFormat::Xbox360SignedFile, "Xbox 360 encrypted file"},
};

QString wgsPathSettingsKey(ExportContainerFormat format) {
    return QLatin1String(format == ExportContainerFormat::WgsFh5ToMotorsport
        ? kImportIndexSettingsKey : kNativeIndexSettingsKey);
}

void sizeWarningChoice(QComboBox &choice) {
    QStyleOptionComboBox option;

    choice.ensurePolished();
    option.initFrom(&choice);
    const QRect labelRect = choice.style()->subControlRect(
        QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, &choice);
    const int horizontalInsets = option.rect.width() - labelRect.width();
    const int labelWidth = choice.fontMetrics().horizontalAdvance(kDiscardChoiceText);
    choice.setFixedWidth(labelWidth + kComboLabelInset + horizontalInsets);
}

struct ExportWarningRow {
    QWidget *row = nullptr;
    QLabel *label = nullptr;
    QComboBox *choice = nullptr;

    bool keepsWarning() const {
        return !row->isHidden() && choice->currentData().toBool();
    }

    void update(const QString &text, bool visible) const {
        label->setText(text);
        choice->setAccessibleName(text);
        label->setVisible(visible);
        row->setVisible(visible);
    }
};

class ExportAssetDialog final : public QDialog {
public:
    ExportAssetDialog(QWidget *parent, const QString &startDirectory,
                      const QString &projectName, bool livery, int carId,
                      const std::function<fls::ExportLayerWarnings(ExportGameVersion)> &layerWarnings)
        : QDialog(parent), pgsDirectory_(startDirectory), layerWarnings_(layerWarnings), livery_(livery), carId_(carId) {
        setWindowTitle(QStringLiteral("Export"));
        setModal(true);
        resize(kDialogWidth, kDialogHeight);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(kLayoutMargin, kLayoutMargin, kLayoutMargin, kLayoutMargin);
        layout->setSpacing(kLayoutSpacing);

        auto *project = new QLabel(
            QStringLiteral("%1 — %2").arg(
                projectName.isEmpty() ? QStringLiteral("Untitled") : projectName,
                livery ? QStringLiteral("Livery") : QStringLiteral("Layer group")), this);
        project->setTextFormat(Qt::PlainText);
        project->setWordWrap(true);
        project->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(project);

        auto *form = new QFormLayout();
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setSpacing(kLayoutSpacing);
        auto *destination = new QHBoxLayout();
        directoryEdit_ = new QLineEdit(QDir::toNativeSeparators(startDirectory), this);
        directoryEdit_->setClearButtonEnabled(true);
        directoryEdit_->setPlaceholderText(QStringLiteral("Destination folder"));
        browse_ = new QPushButton(QStringLiteral("Browse…"), this);
        browse_->setAutoDefault(false);
        destination->addWidget(directoryEdit_, 1);
        destination->addWidget(browse_);
        form->addRow(QStringLiteral("Destination path"), destination);

        gameVersion_ = new QComboBox(this);
        gameVersion_->addItem(QStringLiteral("Forza Horizon 6"),
                              static_cast<int>(ExportGameVersion::Horizon6));
        gameVersion_->addItem(QStringLiteral("Forza Horizon 5"),
                              static_cast<int>(ExportGameVersion::Horizon5));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 2023"),
                              static_cast<int>(ExportGameVersion::Motorsport2023));
        gameVersion_->addItem(QStringLiteral("Forza Horizon 2"),
                              static_cast<int>(ExportGameVersion::Horizon2));
        gameVersion_->addItem(QStringLiteral("Forza Horizon 3"),
                              static_cast<int>(ExportGameVersion::Horizon3));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 5"),
                              static_cast<int>(ExportGameVersion::Motorsport5));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 6"),
                              static_cast<int>(ExportGameVersion::Motorsport6));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 7"),
                              static_cast<int>(ExportGameVersion::Motorsport7));
        gameVersion_->addItem(QStringLiteral("Forza Horizon 4"),
                              static_cast<int>(ExportGameVersion::Horizon4));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 2"),
                              static_cast<int>(ExportGameVersion::Motorsport2));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 3"),
                              static_cast<int>(ExportGameVersion::Motorsport3));
        gameVersion_->addItem(QStringLiteral("Forza Motorsport 4"),
                              static_cast<int>(ExportGameVersion::Motorsport4));
        gameVersion_->addItem(QStringLiteral("Forza Horizon 1"),
                              static_cast<int>(ExportGameVersion::Horizon1));
        form->addRow(QStringLiteral("Game version"), gameVersion_);

        containerFormat_ = new QComboBox(this);
        form->addRow(QStringLiteral("Container format"), containerFormat_);
        layout->addLayout(form);

        folderHint_ = new QLabel(QStringLiteral(
            "The export folder is named from the project metadata. "
            "An existing asset folder can also be selected as the destination."), this);
        folderHint_->setWordWrap(true);
        layout->addWidget(folderHint_);

        wgsOptions_ = new QGroupBox(QStringLiteral("WGS save profile"), this);
        auto *wgsForm = new QFormLayout(wgsOptions_);
        auto *indexRow = new QHBoxLayout();
        indexEdit_ = new QLineEdit(wgsOptions_);
        indexEdit_->setPlaceholderText(QStringLiteral("Select containers.index"));
        auto *indexBrowse = new QPushButton(QStringLiteral("Browse…"), wgsOptions_);
        indexBrowse->setAutoDefault(false);
        indexRow->addWidget(indexEdit_, 1);
        indexRow->addWidget(indexBrowse);
        wgsForm->addRow(QStringLiteral("Profile index"), indexRow);
        profileEdit_ = new QLineEdit(wgsOptions_);
        profileEdit_->setReadOnly(true);
        wgsForm->addRow(QStringLiteral("Save profile"), profileEdit_);
        gameClosed_ = new QCheckBox(wgsOptions_);
        wgsForm->addRow(gameClosed_);
        layout->addWidget(wgsOptions_);
        layout->addStretch(1);

        hint_ = new QLabel(this);
        hint_->setTextFormat(Qt::PlainText);
        hint_->setWordWrap(true);
        hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(hint_);
        carWarning_ = addWarningRow(layout, QStringLiteral("exportCarWarning"), true);
        maskWarning_ = addWarningRow(layout, QStringLiteral("exportMaskWarning"));
        logoWarning_ = addWarningRow(layout, QStringLiteral("exportLogoWarning"));
        shapeWarning_ = addWarningRow(layout, QStringLiteral("exportShapeWarning"));
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        buttons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("Export"));
        layout->addWidget(buttons_);

        connect(browse_, &QPushButton::clicked, this, [this]() { browseDirectory(); });
        connect(indexBrowse, &QPushButton::clicked, this, [this]() { browseIndex(); });
        connect(indexEdit_, &QLineEdit::textChanged, this, [this]() {
            gameClosed_->setChecked(false);
            updateDestination();
        });
        connect(gameClosed_, &QCheckBox::toggled, this, [this]() { updateDestination(); });
        connect(directoryEdit_, &QLineEdit::textChanged, this, [this]() { updateDestination(); });
        connect(gameVersion_, &QComboBox::currentIndexChanged, this, [this]() { updateFormatOptions(); });
        connect(containerFormat_, &QComboBox::currentIndexChanged, this, [this]() { updateFormat(); });
        connect(buttons_, &QDialogButtonBox::accepted, this, [this]() {
            updateDestination();
            if (buttons_->button(QDialogButtonBox::Save)->isEnabled()) {
                if (wgsTarget_) {
                    QSettings().setValue(wgsPathSettingsKey(selectedFormat()), indexEdit_->text());
                }
                accept();
            }
        });
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

        restoreGeometry(QSettings().value(QLatin1String(kGeometrySettingsKey)).toByteArray());
        updateFormatOptions();
    }

    ~ExportAssetDialog() override {
        QSettings().setValue(QLatin1String(kGeometrySettingsKey), saveGeometry());
    }

    ExportAssetSelection selection() const {
        ExportAssetSelection selected;
        selected.directory = QFileInfo(destinationDirectory()).absoluteFilePath();
        selected.gameVersion = static_cast<ExportGameVersion>(gameVersion_->currentData().toInt());
        selected.containerFormat = selectedFormat();
        selected.wgsTarget = wgsTarget_;
        selected.layerChoices = {maskWarning_.keepsWarning(), logoWarning_.keepsWarning(),
                                 shapeWarning_.keepsWarning()};

        return selected;
    }

private:
    ExportWarningRow addWarningRow(QVBoxLayout *layout, const QString &name, bool keepOnly = false) {
        ExportWarningRow warning;
        warning.row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(warning.row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(kLayoutSpacing);
        warning.label = new QLabel(warning.row);
        warning.label->setObjectName(name);
        warning.label->setTextFormat(Qt::PlainText);
        warning.label->setWordWrap(true);
        setUiState(warning.label, QStringLiteral("warning"));
        warning.choice = new QComboBox(warning.row);
        warning.choice->setProperty("uiCompactDropDown", true);
        warning.choice->setObjectName(name + QStringLiteral("Choice"));
        if (!keepOnly) {
            warning.choice->addItem(kDiscardChoiceText, false);
        }
        warning.choice->addItem(QStringLiteral("Keep"), true);
        sizeWarningChoice(*warning.choice);
        rowLayout->addWidget(warning.label, 1);
        rowLayout->addWidget(warning.choice);
        layout->addWidget(warning.row);
        connect(warning.choice, &QComboBox::currentIndexChanged, this, [this]() { updateExportButtonText(); });

        return warning;
    }

    void updateWarnings(ExportGameVersion game) {
        const auto warnings = layerWarnings_ ? layerWarnings_(game) : fls::ExportLayerWarnings{};
        const bool importProfile = selectedFormat() == ExportContainerFormat::WgsFh5ToMotorsport;
        const QString gameName = gameVersion_->currentText();
        const QString name = importProfile ? QStringLiteral("Forza Horizon 5") : gameName;
        const bool carWarning = livery_ && (carId_ != 0 || game != ExportGameVersion::Motorsport2)
            && !sharedCarRegistry().isSupported(carId_, fls::exportGameCode(game));
        const bool motorsportCarWarning = livery_ && importProfile && !sharedCarRegistry().isSupported(
            carId_, fls::exportGameCode(ExportGameVersion::Motorsport2023));

        carWarning_.update(QStringLiteral("Car model is not supported in %1")
            .arg(carWarning ? name : gameName), carWarning || motorsportCarWarning);
        maskWarning_.update(QStringLiteral("Masks are not supported in %1 (%2 layers)").arg(name).arg(warnings.masks),
                            warnings.masks > 0);
        maskWarning_.choice->setToolTip(fls::isXbox360ExportGame(game)
            ? QStringLiteral("Keep exports these layers as ordinary vinyl.") : QString());
        logoWarning_.update(QStringLiteral("Incompatible logos for %1 (%2 layers)").arg(name).arg(warnings.logos),
                            warnings.logos > 0);
        shapeWarning_.update(QStringLiteral("Incompatible shapes for %1 (%2 layers)").arg(name).arg(warnings.shapes),
                             warnings.shapes > 0);
        updateExportButtonText();
    }

    void updateExportButtonText() {
        const bool keepsWarning = carWarning_.keepsWarning() || maskWarning_.keepsWarning()
            || logoWarning_.keepsWarning() || shapeWarning_.keepsWarning();
        buttons_->button(QDialogButtonBox::Save)->setText(keepsWarning
            ? QStringLiteral("Export anyway (I know what I am doing)") : QStringLiteral("Export"));
    }

    ExportContainerFormat selectedFormat() const {
        return static_cast<ExportContainerFormat>(containerFormat_->currentData().toInt());
    }

    bool wgsSelected() const {
        return selectedFormat() == ExportContainerFormat::WgsAppend
            || selectedFormat() == ExportContainerFormat::WgsFh5ToMotorsport;
    }

    void updateFormatOptions() {
        const QSignalBlocker blocker(containerFormat_);
        const QVariant previousFormat = containerFormat_->currentData();
        const auto game = static_cast<ExportGameVersion>(gameVersion_->currentData().toInt());

        containerFormat_->clear();
        for (const ExportContainerOption &option : kContainerOptions) {
            if (supportsExportContainer(game, option.format)) {
                containerFormat_->addItem(QString::fromLatin1(option.label), static_cast<int>(option.format));
            }
        }
        const int previousIndex = containerFormat_->findData(previousFormat);
        if (previousIndex >= 0) {
            containerFormat_->setCurrentIndex(previousIndex);
        }
        updateFormat();
    }

    void updateFormat() {
        const auto game = static_cast<ExportGameVersion>(gameVersion_->currentData().toInt());
        const bool xbox360 = fls::isXbox360ExportGame(game);
        const bool wgs = wgsSelected();
        const bool importProfile = selectedFormat() == ExportContainerFormat::WgsFh5ToMotorsport;
        const QSignalBlocker gameClosedBlocker(gameClosed_);

        if (wgs && selectedFormat() != wgsPathFormat_) {
            const QSignalBlocker blocker(indexEdit_);
            wgsPaths_.insert(wgsPathFormat_, indexEdit_->text());
            wgsPathFormat_ = selectedFormat();
            indexEdit_->setText(wgsPaths_.value(wgsPathFormat_,
                QSettings().value(wgsPathSettingsKey(wgsPathFormat_)).toString()));
            gameClosed_->setChecked(false);
        }
        if (wgs != wgsActive_) {
            const QSignalBlocker blocker(directoryEdit_);
            if (wgs) {
                pgsDirectory_ = directoryEdit_->text();
                directoryEdit_->clear();
            } else {
                directoryEdit_->setText(pgsDirectory_);
            }
            gameClosed_->setChecked(false);
            wgsActive_ = wgs;
        }
        directoryEdit_->setEnabled(!wgs);
        browse_->setEnabled(!wgs);
        folderHint_->setVisible(!wgs && !xbox360);
        wgsOptions_->setVisible(wgs);
        gameClosed_->setText(importProfile
            ? QStringLiteral("FM2023 is closed and cloud synchronization has finished")
            : QStringLiteral("FH5 is closed and cloud synchronization has finished"));
        updateWarnings(importProfile ? ExportGameVersion::Horizon5 : game);
        updateDestination();
    }

    void browseIndex() {
        const QString current = QDir::fromNativeSeparators(indexEdit_->text().trimmed());
        const QString start = QFileInfo(current).exists() ? current : pgsDirectory_;
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Select WGS Profile"), start,
            QStringLiteral("WGS profile index (containers.index)"),
            nullptr, QFileDialog::DontUseNativeDialog);
        if (!path.isEmpty()) {
            indexEdit_->setText(QDir::toNativeSeparators(path));
        }
    }

    QString destinationDirectory() const {
        const QString path = QDir::fromNativeSeparators(directoryEdit_->text().trimmed());

        return path.isEmpty() ? QString() : QDir::cleanPath(path);
    }

    void browseDirectory() {
        const QString current = destinationDirectory();
        const QString startDirectory = QFileInfo(current).isDir() ? current : QDir::homePath();
        const QString directory = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Export Destination"), startDirectory,
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
        if (!directory.isEmpty()) {
            directoryEdit_->setText(QDir::toNativeSeparators(directory));
        }
    }

    void updateDestination() {
        const auto game = static_cast<ExportGameVersion>(gameVersion_->currentData().toInt());
        const bool unsupportedFormat = containerFormat_->currentIndex() < 0
            || !supportsExportContainer(game, selectedFormat());

        wgsTarget_.reset();
        if (unsupportedFormat) {
            setUiState(hint_, QStringLiteral("error"));
            hint_->setText(QStringLiteral("Choose a container format supported by %1.").arg(gameVersion_->currentText()));
            buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
            return;
        }
        setUiState(hint_, QString());
        if (wgsSelected()) {
            const QString path = QDir::fromNativeSeparators(indexEdit_->text().trimmed());
            profileEdit_->clear();
            buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
            try {
                const auto kind = livery_ ? fls::WgsAssetKind::Livery : fls::WgsAssetKind::Group;
                wgsTarget_ = fls::inspectWgsAppendTarget(path, game, kind, carId_);
                const QString profile = QFileInfo(wgsTarget_->indexPath).absolutePath();
                const QSignalBlocker blocker(directoryEdit_);
                directoryEdit_->setText(QDir::toNativeSeparators(profile));
                profileEdit_->setText(QDir(profile).dirName());
                buttons_->button(QDialogButtonBox::Save)->setEnabled(gameClosed_->isChecked());
                hint_->setText(QStringLiteral("Adds a new container and backs up the profile index."));
            } catch (const std::exception &error) {
                const QSignalBlocker blocker(directoryEdit_);
                directoryEdit_->clear();
                hint_->setText(path.isEmpty()
                    ? QStringLiteral("Select containers.index from the WGS profile ending in 7900C3C7.")
                    : QString::fromUtf8(error.what()));
            }
            return;
        }
        const QString directory = destinationDirectory();
        const bool valid = !directory.isEmpty() && QDir::isAbsolutePath(directory)
            && QFileInfo(directory).isDir();

        buttons_->button(QDialogButtonBox::Save)->setEnabled(valid);
        hint_->setText(valid
            ? QStringLiteral("Ready to export to %1.").arg(gameVersion_->currentText())
            : QStringLiteral("Choose an existing destination folder with an absolute path."));
    }

    QLineEdit *directoryEdit_ = nullptr;
    QLineEdit *indexEdit_ = nullptr;
    QLineEdit *profileEdit_ = nullptr;
    QComboBox *gameVersion_ = nullptr;
    QComboBox *containerFormat_ = nullptr;
    QLabel *hint_ = nullptr;
    ExportWarningRow carWarning_;
    ExportWarningRow maskWarning_;
    ExportWarningRow logoWarning_;
    ExportWarningRow shapeWarning_;
    QLabel *folderHint_ = nullptr;
    QPushButton *browse_ = nullptr;
    QGroupBox *wgsOptions_ = nullptr;
    QCheckBox *gameClosed_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    QString pgsDirectory_;
    QMap<ExportContainerFormat, QString> wgsPaths_;
    std::optional<fls::WgsExportTarget> wgsTarget_;
    std::function<fls::ExportLayerWarnings(ExportGameVersion)> layerWarnings_;
    bool livery_ = false;
    bool wgsActive_ = false;
    ExportContainerFormat wgsPathFormat_ = ExportContainerFormat::PgsFolder;
    int carId_ = 0;
};

} // namespace

std::optional<ExportAssetSelection> showExportAssetDialog(
    QWidget *parent, const QString &startDirectory, const QString &projectName, bool livery,
    int carId, const std::function<fls::ExportLayerWarnings(ExportGameVersion)> &layerWarnings) {
    ExportAssetDialog dialog(parent, startDirectory, projectName, livery, carId, layerWarnings);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }

    return dialog.selection();
}

} // namespace gui
