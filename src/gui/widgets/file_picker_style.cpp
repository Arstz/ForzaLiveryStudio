#include "file_picker_style.h"

#include "browser_scroll.h"
#include "browser_tile_palette.h"
#include "gui_assets.h"

#include <QCache>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileSystemModel>
#include <QGridLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QListView>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

#include <algorithm>

namespace gui {
namespace {

constexpr int kTileWidth = 108;
constexpr int kTileIconExtent = 24;
constexpr int kTileMargin = 2;
constexpr int kTilePadding = 4;
constexpr int kTileLayoutBatchSize = 256;
constexpr int kFilterMinimumCharacters = 14;
constexpr int kCachedIconCount = 512;
constexpr auto kStyledFilePicker = "flsStyledFilePicker";

class FilePickerItemDelegate final : public QStyledItemDelegate {
public:
    FilePickerItemDelegate(QAbstractItemView &view, QSize tileSize = {})
        : QStyledItemDelegate(&view),
          view_(view),
          folderIcon_(view.style()->standardIcon(QStyle::SP_DirIcon, nullptr, &view)),
          driveIcon_(view.style()->standardIcon(QStyle::SP_DriveHDIcon, nullptr, &view)),
          tileSize_(tileSize) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        return tileSize_.isValid() ? tileSize_ : QStyledItemDelegate::sizeHint(option, index);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (!tileSize_.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const QRect tile = option.rect.adjusted(kTileMargin, kTileMargin, -kTileMargin, -kTileMargin);
        const QRect content = tile.adjusted(kTilePadding, kTilePadding, -kTilePadding, -kTilePadding);
        const QRect icon(content.center().x() - kTileIconExtent / 2, content.top(),
                         kTileIconExtent, kTileIconExtent);
        const QRect label = content.adjusted(0, kTileIconExtent, 0, 0);
        const BrowserTilePalette colors = browserTilePalette(currentUiTheme());
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool enabled = option.state.testFlag(QStyle::State_Enabled);
        const QColor text = !enabled ? option.palette.color(QPalette::Disabled, QPalette::Text)
            : (hovered || selected ? colors.hoverLabel : colors.label);
        const QFontMetrics metrics(option.font);
        const QString name = index.data(Qt::DisplayRole).toString();

        painter->save();
        paintBrowserTileBackground(*painter, tile, colors, hovered, selected);
        itemIcon(index, icon.size()).paint(painter, icon, Qt::AlignCenter, enabled ? QIcon::Normal : QIcon::Disabled);
        painter->setFont(option.font);
        painter->setPen(text);
        painter->drawText(label, Qt::AlignCenter, metrics.elidedText(name, Qt::ElideMiddle, label.width()));
        painter->restore();
    }

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override {
        const QSize requestedIconSize = option->decorationSize;
        QStyledItemDelegate::initStyleOption(option, index);
        if (tileSize_.isValid()) {
            option->decorationPosition = QStyleOptionViewItem::Top;
            option->decorationSize = QSize(kTileIconExtent, kTileIconExtent);
            option->displayAlignment = Qt::AlignHCenter | Qt::AlignBottom;
        }
        if (index.column() == 0 && !requestedIconSize.isEmpty()) {
            option->icon = itemIcon(index, requestedIconSize, &option->icon);
            option->features.setFlag(QStyleOptionViewItem::HasDecoration, !option->icon.isNull());
        }
    }

private:
    QIcon itemIcon(const QModelIndex &index, QSize size, const QIcon *decoration = nullptr) const {
        const auto *model = qobject_cast<const QFileSystemModel *>(index.model());
        if (model != nullptr && model->isDir(index)) {
            return index.parent().isValid() ? folderIcon_ : driveIcon_;
        }
        const QIcon file = decoration != nullptr ? *decoration : index.data(Qt::DecorationRole).value<QIcon>();
        if (file.isNull()) {
            return {};
        }
        const qreal pixelRatio = view_.devicePixelRatioF();
        if (size != cachedIconSize_ || pixelRatio != cachedPixelRatio_) {
            fileIcons_.clear();
            cachedIconSize_ = size;
            cachedPixelRatio_ = pixelRatio;
        }
        const qint64 key = file.cacheKey();
        if (const QIcon *cached = fileIcons_.object(key)) {
            return *cached;
        }
        const QIcon rendered(file.pixmap(size, pixelRatio));
        fileIcons_.insert(key, new QIcon(rendered));

        return rendered;
    }

    QAbstractItemView &view_;
    QIcon folderIcon_;
    QIcon driveIcon_;
    QSize tileSize_;
    mutable QCache<qint64, QIcon> fileIcons_{kCachedIconCount};
    mutable QSize cachedIconSize_;
    mutable qreal cachedPixelRatio_ = 0;
};

class FilePickerGridLayout final : public QObject {
public:
    explicit FilePickerGridLayout(QListView &view)
        : QObject(&view), view_(view), columns_(columnCount()) {
        relayout_.setSingleShot(true);
        connect(&relayout_, &QTimer::timeout, this, [this]() {
            const int columns = columnCount();
            if (columns != columns_) {
                columns_ = columns;
                view_.doItemsLayout();
            }
        });
        view_.viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if ((event->type() == QEvent::Resize || event->type() == QEvent::Show)
            && !relayout_.isActive() && columnCount() != columns_) {
            relayout_.start(0);
        }

        return QObject::eventFilter(watched, event);
    }

private:
    int columnCount() const {
        int width = view_.maximumViewportSize().width();
        const QScrollBar *scrollBar = view_.verticalScrollBar();
        const QStyle *style = view_.style();
        if (view_.verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded
            && !style->pixelMetric(QStyle::PM_ScrollView_ScrollBarOverlap, nullptr, scrollBar)) {
            width -= style->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, scrollBar);
            if (style->styleHint(QStyle::SH_ScrollView_FrameOnlyAroundContents, nullptr, &view_)) {
                width -= 2 * view_.frameWidth();
            }
        }

        return std::max(0, (width - 1) / view_.gridSize().width());
    }

    QListView &view_;
    QTimer relayout_;
    int columns_;
};

void styleNavigation(QFileDialog &dialog) {
    const auto labelButton = [&](const QString &name, const QString &label) {
        if (auto *button = dialog.findChild<QToolButton *>(name)) {
            button->setText(label);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            button->setMinimumWidth(0);
            button->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    };
    labelButton(QStringLiteral("backButton"), QStringLiteral("Back"));
    labelButton(QStringLiteral("toParentButton"), QStringLiteral("Up"));
    if (auto *button = dialog.findChild<QToolButton *>(QStringLiteral("forwardButton"))) {
        button->hide();
    }
    if (auto *button = dialog.findChild<QToolButton *>(QStringLiteral("newFolderButton"))) {
        button->setIcon(uiAddIcon());
    }
    if (auto *button = dialog.findChild<QToolButton *>(QStringLiteral("listModeButton"))) {
        const QString label = QStringLiteral("Grid view");
        button->setIcon(assetIcon(QStringLiteral("ViewGrid.xpm")));
        button->setToolTip(label);
        button->setAccessibleName(label);
        button->setAccessibleDescription(QStringLiteral("Show files and folders as tiles"));
    }
    if (auto *button = dialog.findChild<QToolButton *>(QStringLiteral("detailModeButton"))) {
        button->setIcon(assetIcon(QStringLiteral("ViewList.xpm")));
    }
}

void styleFilters(QFileDialog &dialog) {
    if (auto *filter = dialog.findChild<QComboBox *>(QStringLiteral("fileTypeCombo"))) {
        filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        filter->setMinimumContentsLength(kFilterMinimumCharacters);
        filter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    if (auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
        name->setClearButtonEnabled(true);
    }
    if (auto *layout = qobject_cast<QGridLayout *>(dialog.layout())) {
        layout->setColumnStretch(1, 1);
        layout->setRowStretch(1, 1);
        if (auto *buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"))) {
            layout->removeWidget(buttons);
            buttons->setOrientation(Qt::Horizontal);
            layout->addWidget(buttons, layout->rowCount(), 0, 1, layout->columnCount());
        }
    }
}

} // namespace

void styleFilePicker(QFileDialog &dialog) {
    auto *tiles = dialog.findChild<QListView *>(QStringLiteral("listView"));
    auto *details = dialog.findChild<QTreeView *>(QStringLiteral("treeView"));
    if (tiles == nullptr || details == nullptr || dialog.property(kStyledFilePicker).toBool()) {
        return;
    }
    dialog.setProperty(kStyledFilePicker, true);
    dialog.setOption(QFileDialog::DontUseCustomDirectoryIcons);

    const QSize tileSize(kTileWidth, kTileIconExtent + tiles->fontMetrics().height()
        + 2 * (kTileMargin + kTilePadding));
    tiles->setMouseTracking(true);
    tiles->setFlow(QListView::LeftToRight);
    tiles->setGridSize(tileSize);
    tiles->setUniformItemSizes(true);
    tiles->setResizeMode(QListView::Fixed);
    tiles->setLayoutMode(QListView::Batched);
    tiles->setBatchSize(kTileLayoutBatchSize);
    tiles->setItemDelegate(new FilePickerItemDelegate(*tiles, tileSize));
    new FilePickerGridLayout(*tiles);
    enableBrowserScrolling(*tiles);

    details->setMouseTracking(true);
    details->setAlternatingRowColors(true);
    details->setUniformRowHeights(true);
    details->setItemDelegate(new FilePickerItemDelegate(*details));
    details->header()->setSectionResizeMode(QHeaderView::Stretch);
    enableBrowserScrolling(*details);
    if (auto *sidebar = dialog.findChild<QListView *>(QStringLiteral("sidebar"))) {
        enableBrowserScrolling(*sidebar);
    }

    styleNavigation(dialog);
    styleFilters(dialog);
}

} // namespace gui
