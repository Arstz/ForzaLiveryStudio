#pragma once

#include "editor_state.h"
#include "shape_geometry_store.h"
#include "shape_name_store.h"

#include <QtCore>
#include <QtGui>
#include <QtWidgets>

#include <functional>

class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QScrollArea;
class QToolButton;

namespace gui {

QImage renderShapePreviewImage(const ShapeGeometryStore &geometry,
                               int shapeId,
                               const QSize &size,
                               const QColor &ink = QColor(245, 245, 245));

class BrowserTile : public QWidget {
protected:
    explicit BrowserTile(QWidget *parent = nullptr);

    bool hovered() const;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    bool hovered_ = false;
};

class ShapeTile final : public BrowserTile {
public:
    ShapeTile(int shapeId, const QString &label, const ShapeGeometryStore *geometry, QWidget *parent = nullptr);

    void setFavourite(bool enabled);
    void refreshTheme();
    void setPressedCallback(std::function<void(int)> callback);
    void setRightPressedCallback(std::function<void(int)> callback);
    void setFavouriteCallback(std::function<void(int, bool)> callback);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateFavouriteIcon();
    void favouriteToggled(bool checked);
    void drawPreview(QPainter &painter, const QRect &rect);

    int shapeId_ = 0;
    QString label_;
    const ShapeGeometryStore *geometry_ = nullptr;
    QToolButton *favourite_ = nullptr;
    mutable QHash<QSize, QImage> previewCache_;
    std::function<void(int)> pressedCallback_;
    std::function<void(int)> rightPressedCallback_;
    std::function<void(int, bool)> favouriteCallback_;
};

struct CustomShapeGroup {
    QString id;
    QString name;
    ProjectClipboard clipboard;
};

class CustomGroupTile final : public BrowserTile {
public:
    CustomGroupTile(const CustomShapeGroup &group, const ShapeGeometryStore *geometry, QWidget *parent = nullptr);

    void refreshTheme();
    void setPressedCallback(std::function<void(const QString &)> callback);
    void setDeleteCallback(std::function<void(const QString &)> callback);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateDeleteIcon();
    void drawPreview(QPainter &painter, const QRect &rect);
    int layerCount() const;

    CustomShapeGroup group_;
    const ShapeGeometryStore *geometry_ = nullptr;
    QToolButton *delete_ = nullptr;
    mutable QHash<QSize, QImage> previewCache_;
    std::function<void(const QString &)> pressedCallback_;
    std::function<void(const QString &)> deleteCallback_;
};

class LogoTile final : public BrowserTile {
public:
    LogoTile(quint32 rasterId, const QSize &size, QWidget *parent = nullptr);

    void refreshTheme();
    void setPressedCallback(std::function<void(quint32)> callback);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void drawPreview(QPainter &painter, const QRect &rect);

    quint32 rasterId_ = 0;
    QSize size_;
    mutable QHash<QSize, QImage> previewCache_;
    std::function<void(quint32)> pressedCallback_;
};

class ShapesBrowserWidget final : public QWidget {
public:
    explicit ShapesBrowserWidget(QWidget *parent = nullptr);

    void setShapeSelectedCallback(std::function<void(int)> callback);
    void setShapeReplaceCallback(std::function<void(int)> callback);
    void setLogoSelectedCallback(std::function<void(quint32, int, int)> callback);
    void setCustomGroupSelectedCallback(std::function<void(const CustomShapeGroup &)> callback);
    void setAddCurrentSelectionCallback(std::function<void()> callback);
    void addCustomGroup(const QString &name, const ProjectClipboard &clipboard);
    void refreshTheme();

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void populateCategories();
    void refreshGrid();
    void layoutTiles(const QVector<QWidget *> &tiles);
    void showGridMessage(const QString &text);
    ShapeTile *tileForShape(int shapeId);
    LogoTile *tileForLogo(quint32 rasterId);
    CustomGroupTile *tileForCustomGroup(const CustomShapeGroup &group);
    void setFavourite(int shapeId, bool enabled);
    void deleteCustomGroup(const QString &id);
    QVector<int> categoryShapeIds(const QString &category) const;
    QString categoryNameForShape(int shapeId, const ShapeGeometry &geometry) const;
    QString nameForShape(int shapeId, const ShapeGeometry &geometry) const;
    QSet<int> loadFavourites() const;
    void saveFavourites() const;
    QVector<CustomShapeGroup> loadCustomGroups() const;
    void saveCustomGroups() const;

    ShapeGeometryStore geometry_;
    ShapeNameStore names_;
    bool geometryLoaded_ = false;
    QHash<QString, QVector<int>> categories_;
    QHash<quint32, QSize> logos_;
    QStringList categoryOrder_;
    QSet<int> favourites_;
    QVector<CustomShapeGroup> customGroups_;
    QHash<int, ShapeTile *> tiles_;
    QHash<quint32, LogoTile *> logoTiles_;
    QHash<QString, CustomGroupTile *> customTiles_;
    QString currentCategory_;
    QLineEdit *search_ = nullptr;
    QListWidget *categoriesList_ = nullptr;
    QToolButton *addSelection_ = nullptr;
    QScrollArea *scroll_ = nullptr;
    QWidget *gridHost_ = nullptr;
    QGridLayout *grid_ = nullptr;
    QLabel *searchHint_ = nullptr;
    std::function<void(int)> shapeSelectedCallback_;
    std::function<void(int)> shapeReplaceCallback_;
    std::function<void(quint32, int, int)> logoSelectedCallback_;
    std::function<void(const CustomShapeGroup &)> customGroupSelectedCallback_;
    std::function<void()> addCurrentSelectionCallback_;
};

} // namespace gui
