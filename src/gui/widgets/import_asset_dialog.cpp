#include "import_asset_dialog.h"

#include "car_registry.h"
#include "fm_codec.h"
#include "gui_assets.h"
#include "header_codec.h"
#include "image_io.h"
#include "livery_codec.h"
#include "theme_manager.h"
#include "wgs_save_reader.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <exception>

namespace gui {
namespace {

constexpr int kPathRole = Qt::UserRole;
constexpr int kAssetRole = Qt::UserRole + 1;
constexpr int kMotorsportRole = Qt::UserRole + 2;
constexpr int kBaseTextRole = Qt::UserRole + 3;
constexpr int kNameRole = Qt::UserRole + 4;
constexpr int kKindRole = Qt::UserRole + 5;
constexpr int kThumbnailRole = Qt::UserRole + 6;
constexpr int kWgsAssetIndexRole = Qt::UserRole + 7;
constexpr int kGridWidth = 184;
constexpr int kGridMargin = 2;
constexpr int kTilePadding = 4;
constexpr int kLabelHeight = 42;
constexpr int kGridPreviewHeight = 97;
constexpr int kGridHeight = 2 * (kGridMargin + kTilePadding)
    + kGridPreviewHeight + kLabelHeight;
constexpr int kFolderGridIconExtent = 64;
constexpr int kThumbnailCacheWidth = 512;
constexpr int kAssetRowHeight = 86;
constexpr int kFolderRowPadding = 12;
constexpr bool kShowGenericForzaFoldersDefault = true;

enum class AssetKind {
    None,
    HorizonGroup,
    HorizonLivery,
    MotorsportGroup,
    MotorsportLivery,
};

bool isLiveryKind(AssetKind kind) {
    return kind == AssetKind::HorizonLivery || kind == AssetKind::MotorsportLivery;
}

bool isGroupKind(AssetKind kind) {
    return kind == AssetKind::HorizonGroup || kind == AssetKind::MotorsportGroup;
}

bool isNonEmptyFile(const QString &path) {
    return !path.isEmpty() && QFileInfo(path).size() > 0;
}

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

using FilePathIndex = QHash<QString, QString>;

FilePathIndex indexFilePaths(const QFileInfoList &files) {
    FilePathIndex index;
    index.reserve(files.size());
    for (const QFileInfo &file : files) {
        index.insert(file.fileName().toCaseFolded(), file.absoluteFilePath());
    }
    return index;
}

QString indexedFile(const FilePathIndex &index, const QString &name) {
    return index.value(name.toCaseFolded());
}

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
        QStringLiteral("bigThumb.png"),
        QStringLiteral("thumb.png"),
        QStringLiteral("thumbnail.png"),
        QStringLiteral("bigThumb.webp"),
        QStringLiteral("thumb.webp"),
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
            return sharedCarRegistry().displayName(fls::readLiveryPayload(path).carId);
        }
        if (kind == AssetKind::MotorsportLivery) {
            return sharedCarRegistry().displayName(fls::readFM2023LiveryPayload(path).carId);
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
        const fls::HeaderMetadata metadata = fls::parseHeader(file.readAll());
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

    if (isNonEmptyFile(cLivery)) {
        asset.kind = AssetKind::HorizonLivery;
    } else if (isNonEmptyFile(cGroup)) {
        asset.kind = AssetKind::HorizonGroup;
    } else if (!header.isEmpty() && isNonEmptyFile(data)) {
        QFile dataFile(data);
        if (dataFile.open(QIODevice::ReadOnly)) {
            const QByteArray bytes = dataFile.readAll();
            if (fls::isFM2023Livery(bytes)) {
                asset.kind = AssetKind::MotorsportLivery;
            } else if (fls::isRawGyvl(bytes)) {
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

AssetInfo inspectFlatLivery(const QFileInfo &file, const FilePathIndex &files) {
    AssetInfo asset;
    if (!file.isFile() || file.size() <= 0
        || !fls::isLiveryAssetFileName(file.fileName())
        || file.fileName().compare(QStringLiteral("C_livery"), Qt::CaseInsensitive) == 0) {
        return asset;
    }

    QString stem = file.fileName();
    stem.chop(QStringLiteral(".C_livery").size());
    asset.kind = AssetKind::HorizonLivery;
    readHeaderMetadata(indexedFile(files, stem + QStringLiteral(".header")), asset);
    const QStringList thumbnailSuffixes = {
        QStringLiteral(".bigThumb.png"),
        QStringLiteral(".bigThumb.webp"),
        QStringLiteral(".smallThumb.png"),
        QStringLiteral(".smallThumb.webp"),
    };
    for (const QString &suffix : thumbnailSuffixes) {
        asset.thumbnailPath = indexedFile(files, stem + suffix);
        if (!asset.thumbnailPath.isEmpty()) {
            break;
        }
    }

    return asset;
}

QString assetKindLabel(AssetKind kind) {
    switch (kind) {
    case AssetKind::HorizonGroup:
        return QStringLiteral("Group");
    case AssetKind::HorizonLivery:
        return QStringLiteral("Livery");
    case AssetKind::MotorsportGroup:
        return QStringLiteral("Group");
    case AssetKind::MotorsportLivery:
        return QStringLiteral("Livery");
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
    if (image.isNull() || image.width() <= kThumbnailCacheWidth) {
        return image;
    }

    return image.scaledToWidth(
        kThumbnailCacheWidth, Qt::SmoothTransformation);
}

class ImportAssetGridDelegate final : public QStyledItemDelegate {
public:
    explicit ImportAssetGridDelegate(QListWidget *view)
        : QStyledItemDelegate(view),
          view_(view) {}

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override {
        if (view_ == nullptr
            || !view_->property("flsGridMode").toBool()) {
            return QStyledItemDelegate::sizeHint(option, index);
        }

        return QSize(kGridWidth, kGridHeight);
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        if (view_ == nullptr
            || !view_->property("flsGridMode").toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const bool dark = isDarkTheme(currentUiTheme());
        const bool hovered =
            option.state.testFlag(QStyle::State_MouseOver);
        const bool selected =
            option.state.testFlag(QStyle::State_Selected);
        const QColor tileBase =
            dark ? QColor(34, 34, 34) : QColor(232, 235, 240);
        const QColor tileHover =
            dark ? QColor(42, 42, 42) : QColor(218, 223, 231);
        const QColor stroke =
            dark ? QColor(68, 68, 68) : QColor(145, 153, 166);
        const QColor activeStroke =
            dark ? QColor(119, 119, 119) : QColor(96, 107, 124);
        const QColor previewBase =
            dark ? QColor(21, 21, 21) : QColor(248, 249, 251);
        const QColor labelColor =
            dark ? QColor(238, 238, 238) : QColor(32, 34, 37);
        const QRect tileRect =
            option.rect.adjusted(
                kGridMargin, kGridMargin,
                -kGridMargin, -kGridMargin);
        const QRect contentRect =
            tileRect.adjusted(
                kTilePadding, kTilePadding,
                -kTilePadding, -kTilePadding);
        const QImage thumbnail =
            index.data(kThumbnailRole).value<QImage>();
        QRect previewRect;
        QRect labelRect;
        QRect iconRect;
        if (!thumbnail.isNull()) {
            previewRect = QRect(
                contentRect.left(), contentRect.top(),
                contentRect.width(), kGridPreviewHeight);
            labelRect = QRect(
                contentRect.left(), previewRect.bottom() + 1,
                contentRect.width(), kLabelHeight);
        } else {
            previewRect = QRect(
                contentRect.left(), contentRect.top(),
                contentRect.width(), kGridPreviewHeight);
            iconRect = QRect(
                previewRect.center().x() - kFolderGridIconExtent / 2,
                previewRect.center().y() - kFolderGridIconExtent / 2,
                kFolderGridIconExtent, kFolderGridIconExtent);
            labelRect = QRect(
                contentRect.left(), previewRect.bottom() + 1,
                contentRect.width(), kLabelHeight);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->fillRect(
            tileRect,
            hovered || selected ? tileHover : tileBase);
        painter->setPen(QPen(
            hovered || selected ? activeStroke : stroke, 1));
        painter->drawRect(tileRect.adjusted(0, 0, -1, -1));

        if (!thumbnail.isNull()) {
            painter->fillRect(previewRect, previewBase);
            const QImage fitted = thumbnail.scaled(
                previewRect.size(), Qt::KeepAspectRatio,
                Qt::SmoothTransformation);
            painter->drawImage(
                QPoint(previewRect.center().x() - fitted.width() / 2,
                       previewRect.center().y() - fitted.height() / 2),
                fitted);
        } else {
            const QPixmap icon = index.data(Qt::DecorationRole)
                                     .value<QIcon>()
                                     .pixmap(
                                         kFolderGridIconExtent,
                                         kFolderGridIconExtent);
            painter->drawPixmap(
                iconRect.center().x() - icon.width() / 2,
                iconRect.center().y() - icon.height() / 2,
                icon);
        }

        const QStringList lines =
            index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n'));
        const QFontMetrics metrics(painter->font());
        painter->setPen(labelColor);
        if (!lines.isEmpty()) {
            const QString title =
                metrics.elidedText(
                    lines.front(), Qt::ElideRight, labelRect.width());
            painter->drawText(
                QRect(labelRect.left(), labelRect.top() + 3,
                      labelRect.width(), metrics.height()),
                Qt::AlignHCenter | Qt::AlignTop,
                title);
        }
        if (lines.size() > 1) {
            QFont detailFont = painter->font();
            detailFont.setPointSizeF(
                std::max(1.0, detailFont.pointSizeF() - 1.0));
            painter->setFont(detailFont);
            const QFontMetrics detailMetrics(detailFont);
            const QString details =
                detailMetrics.elidedText(
                    lines.mid(1).join(QStringLiteral("  |  ")),
                    Qt::ElideRight, labelRect.width());
            painter->drawText(
                QRect(labelRect.left(),
                      labelRect.bottom() - detailMetrics.height(),
                      labelRect.width(), detailMetrics.height()),
                Qt::AlignHCenter | Qt::AlignBottom,
                details);
        }
        painter->restore();
    }

private:
    QListWidget *view_ = nullptr;
};

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
        gridViewButton_ = new QToolButton(this);
        gridViewButton_->setCheckable(true);
        gridViewButton_->setAutoRaise(true);
        gridViewButton_->setIcon(assetIcon(QStringLiteral("ViewGrid.xpm")));
        gridViewButton_->setToolTip(QStringLiteral("Grid view"));
        listViewButton_ = new QToolButton(this);
        listViewButton_->setCheckable(true);
        listViewButton_->setAutoRaise(true);
        listViewButton_->setIcon(assetIcon(QStringLiteral("ViewList.xpm")));
        listViewButton_->setToolTip(QStringLiteral("List view"));
        auto *viewButtonGroup = new QButtonGroup(this);
        viewButtonGroup->setExclusive(true);
        viewButtonGroup->addButton(gridViewButton_);
        viewButtonGroup->addButton(listViewButton_);
        QSettings settings;
        searchEdit_->setText(settings.value(QStringLiteral("import/sourceBrowserSearch")).toString());
        const int savedTypeFilter = settings.value(QStringLiteral("import/sourceBrowserType"), 0).toInt();
        if (savedTypeFilter >= 0 && savedTypeFilter < typeFilter_->count()) {
            typeFilter_->setCurrentIndex(savedTypeFilter);
        }
        filters->addWidget(searchEdit_, 1);
        filters->addWidget(typeFilter_);
        filters->addWidget(gridViewButton_);
        filters->addWidget(listViewButton_);
        layout->addLayout(filters);

        list_ = new QListWidget(this);
        list_->setIconSize(QSize(112, 76));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setDragDropMode(QAbstractItemView::NoDragDrop);
        list_->setItemDelegate(new ImportAssetGridDelegate(list_));
        layout->addWidget(list_, 1);
        setGridView(settings.value(
            QStringLiteral("import/sourceBrowserGridView"), true).toBool(),
            false);

        hint_ = new QLabel(QStringLiteral("Open a folder or select an import asset."), this);
        hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(hint_);

        auto *footer = new QHBoxLayout();
        showGenericForzaFolders_ = new QCheckBox(
            QStringLiteral("Show generic forza folders"), this);
        showGenericForzaFolders_->setChecked(settings.value(
            QStringLiteral("import/showGenericForzaFolders"),
            kShowGenericForzaFoldersDefault).toBool());
        buttons_ = new QDialogButtonBox(
            QDialogButtonBox::Open | QDialogButtonBox::Cancel, this);
        buttons_->button(QDialogButtonBox::Open)->setText(QStringLiteral("Import"));
        buttons_->button(QDialogButtonBox::Open)->setEnabled(false);
        footer->addWidget(showGenericForzaFolders_);
        footer->addStretch(1);
        footer->addWidget(buttons_);
        layout->addLayout(footer);

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
        connect(gridViewButton_, &QToolButton::clicked, this, [this]() {
            setGridView(true);
        });
        connect(listViewButton_, &QToolButton::clicked, this, [this]() {
            setGridView(false);
        });
        connect(
            showGenericForzaFolders_,
            &QCheckBox::toggled,
            this,
            [this](bool enabled) {
                QSettings().setValue(
                    QStringLiteral("import/showGenericForzaFolders"), enabled);
                refresh();
            });
        connect(list_, &QListWidget::currentItemChanged, this,
                [this](QListWidgetItem *current, QListWidgetItem *) { updateSelection(current); });
        connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (item->data(kAssetRole).toBool()) {
                acceptSelection();
            } else {
                navigate(item->data(kPathRole).toString());
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
        restoreGeometry(settings.value(
            QStringLiteral("import/sourceBrowserGeometry")).toByteArray());
        initialDirectory_ = startDirectory;
    }

    ~ImportAssetDialog() override {
        QSettings().setValue(
            QStringLiteral("import/sourceBrowserGeometry"), saveGeometry());
    }

    ImportAssetSelection selection() const {
        ImportAssetSelection result;
        result.path = selectedPath_;
        result.directory = currentDirectory_;
        result.wgsAsset = selectedWgsAsset_;
        result.motorsport = selectedMotorsport_;
        return result;
    }

protected:
    void showEvent(QShowEvent *event) override {
        QDialog::showEvent(event);
        if (!initialDirectory_.isEmpty()) {
            const QString directory = initialDirectory_;
            initialDirectory_.clear();
            navigate(directory);
        }
        QTimer::singleShot(0, this, [this]() {
            applyFilters();
            list_->doItemsLayout();
            list_->viewport()->update();
        });
    }

private:
    struct AssetDetailsRequest {
        QString assetPath;
        QString thumbnailPath;
        AssetKind kind = AssetKind::None;
    };

    void setGridView(bool enabled, bool persist = true) {
        list_->setUpdatesEnabled(false);
        gridViewButton_->setChecked(enabled);
        listViewButton_->setChecked(!enabled);
        list_->setProperty("flsGridMode", enabled);
        list_->setViewMode(QListView::ListMode);
        list_->setFlow(enabled ? QListView::LeftToRight : QListView::TopToBottom);
        list_->setWrapping(enabled);
        list_->setResizeMode(enabled ? QListView::Adjust : QListView::Fixed);
        list_->setMovement(QListView::Static);
        list_->setSpacing(enabled ? 2 : 0);
        list_->setGridSize(enabled ? QSize(kGridWidth, kGridHeight) : QSize());
        list_->setUniformItemSizes(enabled);
        list_->setWordWrap(false);
        list_->setAlternatingRowColors(!enabled);
        for (int row = 0; row < list_->count(); ++row) {
            QListWidgetItem *item = list_->item(row);
            if (!enabled) {
                const QImage thumbnail = item->data(kThumbnailRole).value<QImage>();
                if (!thumbnail.isNull()) {
                    item->setIcon(QIcon(QPixmap::fromImage(thumbnail)));
                }
            }
            updateItemSizeHint(item);
        }
        list_->setUpdatesEnabled(true);
        list_->doItemsLayout();
        list_->viewport()->update();
        if (persist) {
            QSettings().setValue(
                QStringLiteral("import/sourceBrowserGridView"), enabled);
        }
    }

    void updateItemSizeHint(QListWidgetItem *item) const {
        if (item == nullptr) {
            return;
        }
        if (list_->property("flsGridMode").toBool()) {
            item->setSizeHint(QSize(kGridWidth, kGridHeight));
            return;
        }
        const int folderIconExtent =
            style()->pixelMetric(QStyle::PM_LargeIconSize);
        item->setSizeHint(QSize(
            0,
            item->data(kAssetRole).toBool()
                ? kAssetRowHeight
                : folderIconExtent + kFolderRowPadding));
    }

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
        list_->setUpdatesEnabled(false);
        list_->clear();
        selectedPath_.clear();
        selectedWgsAsset_.reset();
        selectedMotorsport_ = false;
        pathEdit_->setText(QDir::toNativeSeparators(currentDirectory_));

        const QDir directory(currentDirectory_);
        const bool containersRoot = directory.dirName().compare(
            QStringLiteral("ContainersRoot"), Qt::CaseInsensitive) == 0;
        const bool showGenericForzaFolders =
            showGenericForzaFolders_->isChecked();
        QString refreshError;
        QVector<AssetDetailsRequest> detailsRequests;
        const int folderIconExtent = style()->pixelMetric(QStyle::PM_LargeIconSize);
        const QIcon folderIcon(style()->standardIcon(QStyle::SP_DirIcon)
                                   .pixmap(folderIconExtent, folderIconExtent));
        if (fls::isWgsSaveDirectory(currentDirectory_)) {
            try {
                if (wgsDirectory_ != currentDirectory_) {
                    wgsAssets_ = fls::readWgsSave(currentDirectory_).assets;
                    wgsDirectory_ = currentDirectory_;
                }
                for (int index = 0; index < wgsAssets_.size(); ++index) {
                    const fls::WgsAsset &wgsAsset = wgsAssets_.at(index);
                    const bool userAsset = wgsAsset.containerName.startsWith(
                                               QStringLiteral("Livery_"), Qt::CaseInsensitive)
                        || wgsAsset.containerName.startsWith(
                            QStringLiteral("LayerGroup_"), Qt::CaseInsensitive);
                    if (!showGenericForzaFolders && !userAsset) {
                        continue;
                    }
                    AssetInfo asset;
                    asset.kind = wgsAsset.kind == fls::WgsAssetKind::Livery
                        ? AssetKind::HorizonLivery
                        : AssetKind::HorizonGroup;
                    asset.thumbnailPath = wgsAsset.thumbnailPath;
                    readHeaderMetadata(wgsAsset.headerPath, asset);
                    QString name = wgsAsset.containerName;
                    QString text = name;
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
                    detailsRequests.push_back({
                        wgsAsset.payloadPath, wgsAsset.thumbnailPath, asset.kind});

                    auto *item = new QListWidgetItem(folderIcon, text, list_);
                    item->setData(kPathRole, wgsAsset.payloadPath);
                    item->setData(kAssetRole, true);
                    item->setData(kMotorsportRole, false);
                    item->setData(kBaseTextRole, text);
                    item->setData(kNameRole, name);
                    item->setData(kKindRole, static_cast<int>(asset.kind));
                    item->setData(kWgsAssetIndexRole, index);
                    item->setToolTip(QStringLiteral("WGS: %1").arg(wgsAsset.containerName));
                    updateItemSizeHint(item);
                }
            } catch (const std::exception &error) {
                wgsAssets_.clear();
                wgsDirectory_.clear();
                refreshError = QStringLiteral("Could not read WGS save: %1")
                                   .arg(QString::fromUtf8(error.what()));
            }
        } else {
            wgsAssets_.clear();
            wgsDirectory_.clear();
            const QFileInfoList folders = directory.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                QDir::Name | QDir::IgnoreCase | QDir::DirsFirst);
            for (const QFileInfo &folder : folders) {
                const bool userAssetFolder =
                    folder.fileName().startsWith(
                        QStringLiteral("Livery_"), Qt::CaseInsensitive)
                    || folder.fileName().startsWith(
                        QStringLiteral("LayerGroup_"), Qt::CaseInsensitive);
                if (containersRoot
                    && !showGenericForzaFolders
                    && !userAssetFolder) {
                    continue;
                }

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
                item->setData(kPathRole, folder.absoluteFilePath());
                item->setData(kAssetRole, asset.valid());
                item->setData(kMotorsportRole, asset.motorsport());
                item->setData(kBaseTextRole, text);
                item->setData(kNameRole, name);
                item->setData(kKindRole, static_cast<int>(asset.kind));
                item->setToolTip(QDir::toNativeSeparators(folder.absoluteFilePath()));
                updateItemSizeHint(item);
            }

            const QFileInfoList files = directory.entryInfoList(
                QDir::Files | QDir::Hidden | QDir::System,
                QDir::Name | QDir::IgnoreCase);
            const FilePathIndex filePaths = indexFilePaths(files);
            for (const QFileInfo &file : files) {
                const AssetInfo asset = inspectFlatLivery(file, filePaths);
                if (!asset.valid()) {
                    continue;
                }

                QString name = file.completeBaseName();
                QString text = name;
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
                detailsRequests.push_back({
                    file.absoluteFilePath(), asset.thumbnailPath, asset.kind});

                auto *item = new QListWidgetItem(folderIcon, text, list_);
                item->setData(kPathRole, file.absoluteFilePath());
                item->setData(kAssetRole, true);
                item->setData(kMotorsportRole, false);
                item->setData(kBaseTextRole, text);
                item->setData(kNameRole, name);
                item->setData(kKindRole, static_cast<int>(asset.kind));
                item->setToolTip(QDir::toNativeSeparators(file.absoluteFilePath()));
                updateItemSizeHint(item);
            }
        }

        const QString parent = QFileInfo(currentDirectory_).absolutePath();
        upButton_->setEnabled(QDir::cleanPath(parent) != QDir::cleanPath(currentDirectory_));
        backButton_->setEnabled(historyIndex_ > 0);
        buttons_->button(QDialogButtonBox::Open)->setEnabled(false);
        const QString root = QDir(currentDirectory_).rootPath();
        for (int index = 0; index < drives_->count(); ++index) {
            if (QDir::cleanPath(drives_->itemData(index).toString()) == QDir::cleanPath(root)) {
                QSignalBlocker blocker(drives_);
                drives_->setCurrentIndex(index);
                break;
            }
        }
        applyFilters();
        if (!refreshError.isEmpty()) {
            hint_->setText(refreshError);
        } else if (list_->count() == 0) {
            hint_->setText(QStringLiteral("This folder is empty."));
        }
        list_->setUpdatesEnabled(true);
        list_->doItemsLayout();
        list_->viewport()->update();
        for (const AssetDetailsRequest &request : detailsRequests) {
            loadAssetDetails(request, generation);
        }
    }

    void applyFilters() {
        if (list_ == nullptr
            || searchEdit_ == nullptr
            || typeFilter_ == nullptr) {
            return;
        }
        const QString search = searchEdit_->text().trimmed();
        const int type = typeFilter_->currentIndex();
        int visibleCount = 0;
        for (int row = 0; row < list_->count(); ++row) {
            QListWidgetItem *item = list_->item(row);
            const AssetKind kind = static_cast<AssetKind>(item->data(kKindRole).toInt());
            const bool folder = kind == AssetKind::None;
            const bool livery = isLiveryKind(kind);
            const bool group = isGroupKind(kind);
            const bool nameMatches = search.isEmpty()
                || item->data(kNameRole).toString().contains(search, Qt::CaseInsensitive);
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
        if (isLiveryKind(request.kind)) {
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
            if (item->data(kPathRole).toString() == assetPath) {
                if (!image.isNull()) {
                    item->setData(kThumbnailRole, image);
                    if (!list_->property("flsGridMode").toBool()) {
                        item->setIcon(QIcon(QPixmap::fromImage(image)));
                    }
                }
                if (!car.isEmpty()) {
                    item->setText(item->data(kBaseTextRole).toString()
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
        const bool asset = item != nullptr && item->data(kAssetRole).toBool();
        buttons_->button(QDialogButtonBox::Open)->setEnabled(asset);
        if (!asset) {
            hint_->setText(item == nullptr
                               ? QStringLiteral("Open a folder or select an import asset.")
                               : QStringLiteral("Double-click to open this folder."));
            return;
        }
        hint_->setText(item->toolTip());
    }

    void acceptSelection() {
        QListWidgetItem *item = list_->currentItem();
        if (item == nullptr || !item->data(kAssetRole).toBool()) {
            return;
        }
        selectedPath_ = item->data(kPathRole).toString();
        selectedMotorsport_ = item->data(kMotorsportRole).toBool();
        const QVariant wgsIndex = item->data(kWgsAssetIndexRole);
        if (wgsIndex.isValid()) {
            selectedWgsAsset_ = wgsAssets_.at(wgsIndex.toInt());
        }
        accept();
    }

    QToolButton *backButton_ = nullptr;
    QToolButton *upButton_ = nullptr;
    QComboBox *drives_ = nullptr;
    QLineEdit *pathEdit_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    QComboBox *typeFilter_ = nullptr;
    QToolButton *gridViewButton_ = nullptr;
    QToolButton *listViewButton_ = nullptr;
    QCheckBox *showGenericForzaFolders_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *hint_ = nullptr;
    QDialogButtonBox *buttons_ = nullptr;
    QString initialDirectory_;
    QString currentDirectory_;
    QString selectedPath_;
    QString wgsDirectory_;
    QStringList history_;
    QVector<fls::WgsAsset> wgsAssets_;
    std::optional<fls::WgsAsset> selectedWgsAsset_;
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
