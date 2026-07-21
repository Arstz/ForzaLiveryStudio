#include "import_asset_dialog.h"

#include "car_registry.h"
#include "fm_codec.h"
#include "header_codec.h"
#include "image_io.h"
#include "livery_codec.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <exception>

namespace gui {
namespace {

enum class AssetKind {
    None,
    HorizonGroup,
    HorizonLivery,
    MotorsportGroup,
    MotorsportLivery,
};

struct AssetInfo {
    AssetKind kind = AssetKind::None;
    QString name;
    QString creator;
    QString date;
    QString thumbnailPath;

    bool valid() const { return kind != AssetKind::None; }
    bool motorsport() const {
        return kind == AssetKind::MotorsportGroup || kind == AssetKind::MotorsportLivery;
    }
};

QString findFile(const QDir &directory, const QString &name) {
    const QFileInfoList files = directory.entryInfoList(QDir::Files | QDir::Hidden | QDir::System);
    for (const QFileInfo &file : files) {
        if (file.fileName().compare(name, Qt::CaseInsensitive) == 0) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

QString findThumbnail(const QDir &directory) {
    const QStringList preferredNames = {
        QStringLiteral("bigThumb.webp"),
        QStringLiteral("thumb.webp"),
        QStringLiteral("thumbnail.png"),
        QStringLiteral("thumbnail"),
    };
    for (const QString &name : preferredNames) {
        const QString path = findFile(directory, name);
        if (!path.isEmpty()) {
            return path;
        }
    }

    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    const QFileInfoList files = directory.entryInfoList(QDir::Files, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &file : files) {
        if (formats.contains(file.suffix().toLower().toLatin1())) {
            return file.absoluteFilePath();
        }
    }
    return {};
}

QString liveryCarName(AssetKind kind, const QString &path) {
    try {
        if (kind == AssetKind::HorizonLivery) {
            return sharedCarRegistry().displayName(fh6::readLiveryPayload(path).carId);
        }
        if (kind == AssetKind::MotorsportLivery) {
            return sharedCarRegistry().displayName(fh6::readFM2023LiveryPayload(path).carId);
        }
    } catch (const std::exception &) {
    }
    return {};
}

void readHeaderMetadata(const QString &headerPath, AssetInfo &asset) {
    if (headerPath.isEmpty()) {
        return;
    }
    QFile file(headerPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    try {
        const fh6::HeaderMetadata metadata = fh6::parseHeader(file.readAll());
        asset.name = metadata.name.trimmed();
        asset.creator = metadata.creatorName.trimmed();
        if (metadata.year != 0 && metadata.month != 0) {
            asset.date = QStringLiteral("%1-%2")
                             .arg(metadata.year, 4, 10, QLatin1Char('0'))
                             .arg(metadata.month, 2, 10, QLatin1Char('0'));
            if (metadata.day != 0) {
                asset.date += QStringLiteral("-%1").arg(metadata.day, 2, 10, QLatin1Char('0'));
            }
        }
    } catch (const std::exception &) {
    }
}

AssetInfo inspectAsset(const QString &path) {
    AssetInfo asset;
    const QFileInfo directoryInfo(path);
    if (!directoryInfo.isDir()) {
        return asset;
    }

    const QDir directory(directoryInfo.absoluteFilePath());
    const QString cLivery = findFile(directory, QStringLiteral("C_livery"));
    const QString cGroup = findFile(directory, QStringLiteral("C_group"));
    const QString header = findFile(directory, QStringLiteral("header"));
    const QString data = findFile(directory, QStringLiteral("data"));

    if (!cLivery.isEmpty() && QFileInfo(cLivery).size() > 0) {
        asset.kind = AssetKind::HorizonLivery;
    } else if (!cGroup.isEmpty() && QFileInfo(cGroup).size() > 0) {
        asset.kind = AssetKind::HorizonGroup;
    } else if (!header.isEmpty() && !data.isEmpty() && QFileInfo(data).size() > 0) {
        QFile dataFile(data);
        if (dataFile.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = dataFile.readAll();
            if (fh6::isFM2023Livery(bytes)) {
                asset.kind = AssetKind::MotorsportLivery;
            } else if (fh6::isRawGyvl(bytes)) {
                asset.kind = AssetKind::MotorsportGroup;
            }
        }
    }

    if (!asset.valid()) {
        return asset;
    }
    readHeaderMetadata(header, asset);
    asset.thumbnailPath = findThumbnail(directory);
    return asset;
}

QString assetKindLabel(AssetKind kind) {
    switch (kind) {
    case AssetKind::HorizonGroup:
        return QStringLiteral("Horizon group");
    case AssetKind::HorizonLivery:
        return QStringLiteral("Horizon livery");
    case AssetKind::MotorsportGroup:
        return QStringLiteral("Motorsport group");
    case AssetKind::MotorsportLivery:
        return QStringLiteral("Motorsport livery");
    case AssetKind::None:
        break;
    }
    return {};
}

QImage readThumbnail(const QString &path) {
    if (path.isEmpty()) {
        return {};
    }
    const QImage image = readThumbnailImage(path);
    return image.isNull()
        ? QImage()
        : image.scaled(QSize(112, 76), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

class ImportAssetDialog final : public QDialog {
public:
    ImportAssetDialog(QWidget *parent, QString startDirectory)
        : QDialog(parent) {
        setWindowTitle(QStringLiteral("Import"));
        setModal(true);
        resize(760, 520);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->setSpacing(8);

        auto *navigation = new QHBoxLayout();
        backButton_ = new QToolButton(this);
        backButton_->setText(QStringLiteral("Back"));
        upButton_ = new QToolButton(this);
        upButton_->setText(QStringLiteral("Up"));
        drives_ = new QComboBox(this);
        drives_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        const QFileInfoList driveList = QDir::drives();
        for (const QFileInfo &drive : driveList) {
            drives_->addItem(style()->standardIcon(QStyle::SP_DriveHDIcon),
                             QDir::toNativeSeparators(drive.absoluteFilePath()),
                             drive.absoluteFilePath());
        }
        pathEdit_ = new QLineEdit(this);
        pathEdit_->setClearButtonEnabled(true);
        navigation->addWidget(backButton_);
        navigation->addWidget(upButton_);
        navigation->addWidget(drives_);
        navigation->addWidget(pathEdit_, 1);
        layout->addLayout(navigation);

        auto *filters = new QHBoxLayout();
        searchEdit_ = new QLineEdit(this);
        searchEdit_->setPlaceholderText(QStringLiteral("Search by name"));
        searchEdit_->setClearButtonEnabled(true);
        typeFilter_ = new QComboBox(this);
        typeFilter_->addItems({QStringLiteral("All"), QStringLiteral("Livery"), QStringLiteral("Group")});
        QSettings settings;
        searchEdit_->setText(settings.value(QStringLiteral("import/sourceBrowserSearch")).toString());
        const int savedTypeFilter = settings.value(QStringLiteral("import/sourceBrowserType"), 0).toInt();
        if (savedTypeFilter >= 0 && savedTypeFilter < typeFilter_->count()) {
            typeFilter_->setCurrentIndex(savedTypeFilter);
        }
        filters->addWidget(searchEdit_, 1);
        filters->addWidget(typeFilter_);
        layout->addLayout(filters);

        list_ = new QListWidget(this);
        list_->setViewMode(QListView::ListMode);
        list_->setIconSize(QSize(112, 76));
        list_->setSpacing(2);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setAlternatingRowColors(true);
        layout->addWidget(list_, 1);

        hint_ = new QLabel(QStringLiteral("Open a folder or select an import asset."), this);
        hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(hint_);

        buttons_ = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
        buttons_->button(QDialogButtonBox::Open)->setText(QStringLiteral("Import"));
        buttons_->button(QDialogButtonBox::Open)->setEnabled(false);
        layout->addWidget(buttons_);

        connect(backButton_, &QToolButton::clicked, this, [this]() { goBack(); });
        connect(upButton_, &QToolButton::clicked, this, [this]() { goUp(); });
        connect(drives_, &QComboBox::activated, this, [this](int index) {
            navigate(drives_->itemData(index).toString());
        });
        connect(pathEdit_, &QLineEdit::returnPressed, this, [this]() {
            const QString requested = QDir::fromNativeSeparators(pathEdit_->text().trimmed());
            if (!QFileInfo(requested).isDir()) {
                hint_->setText(QStringLiteral("Folder not found."));
                return;
            }
            navigate(requested);
        });
        connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
            QSettings().setValue(QStringLiteral("import/sourceBrowserSearch"), text);
            applyFilters();
        });
        connect(typeFilter_, &QComboBox::currentIndexChanged, this, [this](int index) {
            QSettings().setValue(QStringLiteral("import/sourceBrowserType"), index);
            applyFilters();
        });
        connect(list_, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem *current, QListWidgetItem *) { updateSelection(current); });
        connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (item->data(AssetRole).toBool()) {
                acceptSelection();
            } else {
                navigate(item->data(PathRole).toString());
            }
        });
        connect(buttons_, &QDialogButtonBox::accepted, this, [this]() { acceptSelection(); });
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

        if (!QFileInfo(startDirectory).isDir()) {
            startDirectory = QDir::homePath();
        }
        if (inspectAsset(startDirectory).valid()) {
            startDirectory = QFileInfo(startDirectory).absolutePath();
        }
        navigate(startDirectory);
    }

    ImportAssetSelection selection() const {
        ImportAssetSelection result;
        result.path = selectedPath_;
        result.directory = currentDirectory_;
        result.motorsport = selectedMotorsport_;
        return result;
    }

private:
    static constexpr int PathRole = Qt::UserRole;
    static constexpr int AssetRole = Qt::UserRole + 1;
    static constexpr int MotorsportRole = Qt::UserRole + 2;
    static constexpr int BaseTextRole = Qt::UserRole + 3;
    static constexpr int NameRole = Qt::UserRole + 4;
    static constexpr int KindRole = Qt::UserRole + 5;

    struct AssetDetailsRequest {
        QString assetPath;
        QString thumbnailPath;
        AssetKind kind = AssetKind::None;
    };

    void navigate(const QString &path, bool recordHistory = true) {
        const QFileInfo info(path);
        if (!info.isDir()) {
            return;
        }
        currentDirectory_ = info.absoluteFilePath();
        QSettings().setValue(QStringLiteral("import/sourceBrowserDirectory"), currentDirectory_);
        if (recordHistory) {
            while (history_.size() > historyIndex_ + 1) {
                history_.removeLast();
            }
            if (history_.isEmpty() || history_.last() != currentDirectory_) {
                history_.push_back(currentDirectory_);
            }
            historyIndex_ = history_.size() - 1;
        }
        refresh();
    }

    void refresh() {
        const quint64 generation = ++thumbnailGeneration_;
        list_->clear();
        selectedPath_.clear();
        selectedMotorsport_ = false;
        pathEdit_->setText(QDir::toNativeSeparators(currentDirectory_));

        const QDir directory(currentDirectory_);
        const QFileInfoList folders = directory.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::Name | QDir::IgnoreCase | QDir::DirsFirst);
        QVector<AssetDetailsRequest> detailsRequests;
        const int folderIconExtent = style()->pixelMetric(QStyle::PM_LargeIconSize);
        const QIcon folderIcon(style()->standardIcon(QStyle::SP_DirIcon)
                                   .pixmap(folderIconExtent, folderIconExtent));
        for (const QFileInfo &folder : folders) {
            const AssetInfo asset = inspectAsset(folder.absoluteFilePath());
            QString name = folder.fileName();
            QString text = name;
            QIcon icon = folderIcon;
            if (asset.valid()) {
                if (!asset.name.isEmpty()) {
                    name = asset.name;
                    text = name;
                }
                QStringList details{assetKindLabel(asset.kind)};
                if (!asset.creator.isEmpty()) {
                    details.push_back(asset.creator);
                }
                if (!asset.date.isEmpty()) {
                    details.push_back(asset.date);
                }
                text += QLatin1Char('\n') + details.join(QStringLiteral("  |  "));
                if (!asset.thumbnailPath.isEmpty()
                    || asset.kind == AssetKind::HorizonLivery
                    || asset.kind == AssetKind::MotorsportLivery) {
                    detailsRequests.push_back({folder.absoluteFilePath(), asset.thumbnailPath, asset.kind});
                }
            }

            auto *item = new QListWidgetItem(icon, text, list_);
            item->setData(PathRole, folder.absoluteFilePath());
            item->setData(AssetRole, asset.valid());
            item->setData(MotorsportRole, asset.motorsport());
            item->setData(BaseTextRole, text);
            item->setData(NameRole, name);
            item->setData(KindRole, static_cast<int>(asset.kind));
            item->setToolTip(QDir::toNativeSeparators(folder.absoluteFilePath()));
            item->setSizeHint(QSize(0, asset.valid() ? 86 : folderIconExtent + 12));
        }

        const QString parent = QFileInfo(currentDirectory_).absolutePath();
        upButton_->setEnabled(QDir::cleanPath(parent) != QDir::cleanPath(currentDirectory_));
        backButton_->setEnabled(historyIndex_ > 0);
        buttons_->button(QDialogButtonBox::Open)->setEnabled(false);
        hint_->setText(list_->count() == 0
                           ? QStringLiteral("This folder is empty.")
                           : QStringLiteral("Open a folder or select an import asset."));

        const QString root = QDir(currentDirectory_).rootPath();
        for (int index = 0; index < drives_->count(); ++index) {
            if (QDir::cleanPath(drives_->itemData(index).toString()) == QDir::cleanPath(root)) {
                QSignalBlocker blocker(drives_);
                drives_->setCurrentIndex(index);
                break;
            }
        }
        applyFilters();
        for (const AssetDetailsRequest &request : detailsRequests) {
            loadAssetDetails(request, generation);
        }
    }

    void applyFilters() {
        if (list_ == nullptr || searchEdit_ == nullptr || typeFilter_ == nullptr) {
            return;
        }
        const QString search = searchEdit_->text().trimmed();
        const int type = typeFilter_->currentIndex();
        int visibleCount = 0;
        for (int row = 0; row < list_->count(); ++row) {
            QListWidgetItem *item = list_->item(row);
            const AssetKind kind = static_cast<AssetKind>(item->data(KindRole).toInt());
            const bool folder = kind == AssetKind::None;
            const bool livery = kind == AssetKind::HorizonLivery || kind == AssetKind::MotorsportLivery;
            const bool group = kind == AssetKind::HorizonGroup || kind == AssetKind::MotorsportGroup;
            const bool nameMatches = search.isEmpty()
                || item->data(NameRole).toString().contains(search, Qt::CaseInsensitive);
            const bool typeMatches = type == 0 || folder || (type == 1 && livery) || (type == 2 && group);
            const bool visible = nameMatches && typeMatches;
            item->setHidden(!visible);
            visibleCount += visible ? 1 : 0;
        }
        QListWidgetItem *current = list_->currentItem();
        if (current != nullptr && current->isHidden()) {
            list_->setCurrentItem(nullptr);
        }
        if (visibleCount == 0) {
            buttons_->button(QDialogButtonBox::Open)->setEnabled(false);
            hint_->setText(QStringLiteral("No matching folders or assets."));
        } else if (list_->currentItem() == nullptr) {
            hint_->setText(QStringLiteral("Open a folder or select an import asset."));
        }
    }

    void loadAssetDetails(const AssetDetailsRequest &request, quint64 generation) {
        const QPointer<ImportAssetDialog> dialog(this);
        if (!request.thumbnailPath.isEmpty()) {
            QThreadPool::globalInstance()->start([dialog, request, generation]() {
                QImage image = readThumbnail(request.thumbnailPath);
                if (image.isNull()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [dialog, assetPath = request.assetPath, generation, image = std::move(image)]() {
                        if (dialog != nullptr) {
                            dialog->applyAssetDetails(assetPath, generation, image, {});
                        }
                    },
                    Qt::QueuedConnection);
            }, 1);
        }
        if (request.kind == AssetKind::HorizonLivery
            || request.kind == AssetKind::MotorsportLivery) {
            QThreadPool::globalInstance()->start([dialog, request, generation]() {
                const QString car = liveryCarName(request.kind, request.assetPath);
                if (car.isEmpty()) {
                    return;
                }
                QMetaObject::invokeMethod(
                    QCoreApplication::instance(),
                    [dialog, assetPath = request.assetPath, generation, car]() {
                        if (dialog != nullptr) {
                            dialog->applyAssetDetails(assetPath, generation, {}, car);
                        }
                    },
                    Qt::QueuedConnection);
            }, -1);
        }
    }

    void applyAssetDetails(const QString &assetPath, quint64 generation,
                           const QImage &image, const QString &car) {
        if (generation != thumbnailGeneration_) {
            return;
        }
        for (int row = 0; row < list_->count(); ++row) {
            QListWidgetItem *item = list_->item(row);
            if (item->data(PathRole).toString() == assetPath) {
                if (!image.isNull()) {
                    item->setIcon(QIcon(QPixmap::fromImage(image)));
                }
                if (!car.isEmpty()) {
                    item->setText(item->data(BaseTextRole).toString()
                                  + QStringLiteral("  |  %1").arg(car));
                }
                return;
            }
        }
    }

    void goBack() {
        if (historyIndex_ <= 0) {
            return;
        }
        --historyIndex_;
        navigate(history_.at(historyIndex_), false);
    }

    void goUp() {
        const QString parent = QFileInfo(currentDirectory_).absolutePath();
        if (QDir::cleanPath(parent) != QDir::cleanPath(currentDirectory_)) {
            navigate(parent);
        }
    }

    void updateSelection(QListWidgetItem *item) {
        const bool asset = item != nullptr && item->data(AssetRole).toBool();
        buttons_->button(QDialogButtonBox::Open)->setEnabled(asset);
        if (!asset) {
            hint_->setText(item == nullptr
                               ? QStringLiteral("Open a folder or select an import asset.")
                               : QStringLiteral("Double-click to open this folder."));
            return;
        }
        hint_->setText(QDir::toNativeSeparators(item->data(PathRole).toString()));
    }

    void acceptSelection() {
        QListWidgetItem *item = list_->currentItem();
        if (item == nullptr || !item->data(AssetRole).toBool()) {
            return;
        }
        selectedPath_ = item->data(PathRole).toString();
        selectedMotorsport_ = item->data(MotorsportRole).toBool();
        accept();
    }

    QToolButton *backButton_ = nullptr;
    QToolButton *upButton_ = nullptr;
    QComboBox *drives_ = nullptr;
    QLineEdit *pathEdit_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    QComboBox *typeFilter_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *hint_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    QString currentDirectory_;
    QString selectedPath_;
    QStringList history_;
    int historyIndex_ = -1;
    bool selectedMotorsport_ = false;
    quint64 thumbnailGeneration_ = 0;
};

} // namespace

ImportAssetSelection showImportAssetDialog(QWidget *parent, const QString &startDirectory) {
    ImportAssetDialog dialog(parent, startDirectory);
    dialog.exec();
    return dialog.selection();
}

} // namespace gui
