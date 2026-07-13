#include "shapes_browser_widget.h"

#include "font_glyphs.h"
#include "gui_assets.h"
#include "project_codec.h"
#include "raster_decals.h"
#include "scene_codec.h"
#include "shape_registry.h"
#include "theme_manager.h"

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr const char *FavouritesCategory = "Favourites";
constexpr const char *CustomCategory = "Custom";
constexpr const char *LogoCategory = "Logos";
constexpr const char *FavouritesSettingsKey = "shapes/favourites";
constexpr const char *CustomGroupsSettingsKey = "shapes/customGroups";
const QSize TileSize(132, 132);
const QSize PreviewSize(86, 86);

QSettings settings()
{
    return QSettings();
}

QString displayCategoryName(QString name)
{
    if (name.contains(QStringLiteral("_0x"))) {
        name = name.left(name.lastIndexOf(QStringLiteral("_0x")));
    }
    return name.replace(QLatin1Char('_'), QLatin1Char(' ')).trimmed();
}

QString fontSectionForShape(int shapeId)
{
    return fontglyphs::sectionForShape(shapeId);
}

bool isFontCategory(const QString &category)
{
    return fontglyphs::fontNames().contains(category);
}

QColor previewColor(const QColor &ink, double alpha)
{
    QColor color = ink;
    color.setAlpha(std::clamp(static_cast<int>(std::round(alpha * 255.0)), 0, 255));
    return color;
}

QJsonObject clipboardToJson(const ProjectClipboard &clipboard)
{
    QJsonObject object;
    fh6::scene::Group root;
    root.id = QStringLiteral("__root__");
    root.name = QStringLiteral("Project");
    for (const auto &node : clipboard.nodes) {
        if (node) {
            root.append(node->clone());
        }
    }
    object.insert(QStringLiteral("root"), fh6::scene::sceneTreeToJson(root));
    return object;
}

std::unique_ptr<fh6::scene::Shape> legacyCustomShapeFromJson(const QJsonObject &object)
{
    auto layer = std::make_unique<fh6::scene::Shape>();
    layer->id = object.value(QStringLiteral("id")).toString();
    layer->name = object.value(QStringLiteral("name")).toString(QStringLiteral("Shape"));
    layer->transform.x = object.value(QStringLiteral("x")).toDouble(0.0);
    layer->transform.y = object.value(QStringLiteral("y")).toDouble(0.0);
    layer->transform.scaleX = object.value(QStringLiteral("scale_x")).toDouble(1.0);
    layer->transform.scaleY = object.value(QStringLiteral("scale_y")).toDouble(1.0);
    layer->transform.rotation = object.value(QStringLiteral("rotation")).toDouble(0.0);
    layer->transform.skew = object.value(QStringLiteral("skew")).toDouble(0.0);
    layer->visible = object.value(QStringLiteral("visible")).toBool(true);
    layer->locked = object.value(QStringLiteral("locked")).toBool(false);
    layer->mask = object.value(QStringLiteral("mask")).toBool(false);
    const QJsonArray color = object.value(QStringLiteral("color")).toArray();
    if (color.size() == 4) {
        for (int i = 0; i < 4; ++i) {
            layer->color[i] = static_cast<quint8>(std::clamp(color.at(i).toInt(), 0, 255));
        }
    }
    if (object.value(QStringLiteral("raster")).toBool(false)) {
        layer->setRasterShape(static_cast<quint32>(object.value(QStringLiteral("raster_id")).toInteger(0)),
                              object.value(QStringLiteral("raster_width")).toInt(256),
                              object.value(QStringLiteral("raster_height")).toInt(256));
    } else {
        layer->setVectorShape(static_cast<quint16>(object.value(QStringLiteral("shape_id")).toInt(0)));
    }
    return layer;
}

std::unique_ptr<fh6::scene::Layer> legacyCustomNodeFromJson(const QString &id,
                                                            const QHash<QString, QJsonObject> &shapes,
                                                            const QHash<QString, QJsonObject> &groups,
                                                            QSet<QString> &consumedShapes)
{
    if (const auto groupIt = groups.constFind(id); groupIt != groups.constEnd()) {
        auto group = std::make_unique<fh6::scene::Group>();
        const QJsonObject object = groupIt.value();
        group->id = object.value(QStringLiteral("id")).toString();
        group->name = object.value(QStringLiteral("name")).toString(QStringLiteral("Group"));
        group->locked = object.value(QStringLiteral("locked")).toBool(false);
        const QJsonArray children = object.value(QStringLiteral("child_ids")).toArray();
        for (const QJsonValue &value : children) {
            if (auto child = legacyCustomNodeFromJson(value.toString(), shapes, groups, consumedShapes)) {
                group->append(std::move(child));
            }
        }
        return group;
    }
    if (const auto shapeIt = shapes.constFind(id); shapeIt != shapes.constEnd()) {
        consumedShapes.insert(id);
        return legacyCustomShapeFromJson(shapeIt.value());
    }
    return nullptr;
}

ProjectClipboard legacyCustomClipboardFromJson(const QJsonObject &object)
{
    ProjectClipboard clipboard;
    QHash<QString, QJsonObject> shapes;
    QHash<QString, QJsonObject> groups;
    for (const QJsonValue &value : object.value(QStringLiteral("layers")).toArray()) {
        if (value.isObject()) {
            const QJsonObject layer = value.toObject();
            shapes.insert(layer.value(QStringLiteral("id")).toString(), layer);
        }
    }
    for (const QJsonValue &value : object.value(QStringLiteral("groups")).toArray()) {
        if (value.isObject()) {
            const QJsonObject group = value.toObject();
            groups.insert(group.value(QStringLiteral("id")).toString(), group);
        }
    }

    QSet<QString> consumedShapes;
    for (const QJsonValue &value : object.value(QStringLiteral("rootIds")).toArray()) {
        const QString id = value.toString();
        if (auto node = legacyCustomNodeFromJson(id, shapes, groups, consumedShapes)) {
            clipboard.rootIds.push_back(node->id);
            clipboard.nodes.push_back(std::move(node));
        }
    }
    for (auto it = shapes.constBegin(); it != shapes.constEnd(); ++it) {
        if (!consumedShapes.contains(it.key())) {
            auto node = legacyCustomShapeFromJson(it.value());
            clipboard.rootIds.push_back(node->id);
            clipboard.nodes.push_back(std::move(node));
        }
    }
    return clipboard;
}

ProjectClipboard clipboardFromJson(const QJsonObject &object)
{
    ProjectClipboard clipboard;
    const QJsonValue rootValue = object.value(QStringLiteral("root"));
    if (!rootValue.isObject()) {
        return legacyCustomClipboardFromJson(object);
    }
    std::unique_ptr<fh6::scene::Group> root = fh6::scene::sceneTreeFromJson(rootValue.toObject());
    while (root && !root->children.empty()) {
        std::unique_ptr<fh6::scene::Layer> node = root->takeAt(0);
        clipboard.rootIds.push_back(node->id);
        clipboard.nodes.push_back(std::move(node));
    }
    return clipboard;
}

void forEachClipboardShape(const ProjectClipboard &clipboard, const std::function<void(const fh6::scene::Shape &)> &fn)
{
    std::function<void(const fh6::scene::Layer &)> walk = [&](const fh6::scene::Layer &node) {
        if (node.kind() == fh6::scene::LayerKind::Shape) {
            fn(static_cast<const fh6::scene::Shape &>(node));
            return;
        }
        if (node.kind() == fh6::scene::LayerKind::Group) {
            for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
                walk(*child);
            }
        }
    };
    for (const auto &node : clipboard.nodes) {
        if (node) {
            walk(*node);
        }
    }
}

int clipboardShapeCount(const ProjectClipboard &clipboard)
{
    int count = 0;
    forEachClipboardShape(clipboard, [&](const fh6::scene::Shape &) { ++count; });
    return count;
}

bool clipboardEmpty(const ProjectClipboard &clipboard)
{
    return clipboard.nodes.empty();
}

QTransform customNodeTransform(const fh6::scene::Layer &node)
{
    QTransform transform;
    transform.translate(node.transform.x, node.transform.y);
    transform.rotate(node.transform.rotation);
    transform.shear(node.transform.skew, 0.0);
    transform.scale(node.transform.scaleX, node.transform.scaleY);
    return transform;
}

void forEachClipboardRoot(const ProjectClipboard &clipboard,
                          const std::function<void(const fh6::scene::Layer &)> &fn)
{
    for (const auto &node : clipboard.nodes) {
        if (node) {
            fn(*node);
        }
    }
}

QString newCustomGroupId()
{
    return QStringLiteral("custom_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void rasterizePreviewTriangle(
    QImage &image,
    const QColor &ink,
    const QPointF &p0,
    const QPointF &p1,
    const QPointF &p2,
    double alpha0,
    double alpha1,
    double alpha2)
{
    const double minX = std::floor(std::min({p0.x(), p1.x(), p2.x()}));
    const double maxX = std::ceil(std::max({p0.x(), p1.x(), p2.x()}));
    const double minY = std::floor(std::min({p0.y(), p1.y(), p2.y()}));
    const double maxY = std::ceil(std::max({p0.y(), p1.y(), p2.y()}));
    const int left = std::clamp(static_cast<int>(minX), 0, image.width() - 1);
    const int right = std::clamp(static_cast<int>(maxX), 0, image.width() - 1);
    const int top = std::clamp(static_cast<int>(minY), 0, image.height() - 1);
    const int bottom = std::clamp(static_cast<int>(maxY), 0, image.height() - 1);
    const double denominator = (p1.y() - p2.y()) * (p0.x() - p2.x()) + (p2.x() - p1.x()) * (p0.y() - p2.y());
    if (std::abs(denominator) < 1e-8) {
        return;
    }

    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const double sampleX = x + 0.5;
            const double sampleY = y + 0.5;
            const double w0 = ((p1.y() - p2.y()) * (sampleX - p2.x()) + (p2.x() - p1.x()) * (sampleY - p2.y())) / denominator;
            const double w1 = ((p2.y() - p0.y()) * (sampleX - p2.x()) + (p0.x() - p2.x()) * (sampleY - p2.y())) / denominator;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4
                || w0 > 1.0001 || w1 > 1.0001 || w2 > 1.0001) {
                continue;
            }

            const int alpha = std::clamp(static_cast<int>(std::round((w0 * alpha0 + w1 * alpha1 + w2 * alpha2) * 255.0)), 0, 255);
            if (alpha > qAlpha(image.pixel(x, y))) {
                image.setPixel(x, y, qRgba(ink.red(), ink.green(), ink.blue(), alpha));
            }
        }
    }
}

class SplitterResizeCursorFilter final : public QObject {
public:
    explicit SplitterResizeCursorFilter(Qt::Orientation orientation, QObject *parent = nullptr)
        : QObject(parent)
        , cursorShape_(orientation == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor)
    {
    }

    ~SplitterResizeCursorFilter() override
    {
        clearOverrideCursor();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::MouseMove:
            setOverrideCursor();
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
        case QEvent::MouseButtonRelease:
            clearOverrideCursor();
            break;
        default:
            break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void setOverrideCursor()
    {
        const QCursor cursor(cursorShape_);
        if (active_) {
            QApplication::changeOverrideCursor(cursor);
        } else {
            QApplication::setOverrideCursor(cursor);
            active_ = true;
        }
    }

    void clearOverrideCursor()
    {
        if (!active_) {
            return;
        }
        QApplication::restoreOverrideCursor();
        active_ = false;
    }

    Qt::CursorShape cursorShape_;
    bool active_ = false;
};

void installSplitterResizeCursor(QSplitter *splitter)
{
    if (splitter == nullptr || splitter->count() < 2) {
        return;
    }
    QWidget *handle = splitter->handle(1);
    if (handle == nullptr) {
        return;
    }
    handle->setAttribute(Qt::WA_Hover, true);
    handle->setMouseTracking(true);
    handle->setCursor(splitter->orientation() == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
    handle->installEventFilter(new SplitterResizeCursorFilter(splitter->orientation(), handle));
}

} // namespace

ShapeTile::ShapeTile(int shapeId, const QString &label, const ShapeGeometryStore *geometry, QWidget *parent)
    : QWidget(parent)
    , shapeId_(shapeId)
    , label_(label)
    , geometry_(geometry)
{
    setFixedSize(TileSize);
    setCursor(Qt::PointingHandCursor);
    const QString hex = QStringLiteral("%1").arg(shapeId_, 4, 16, QLatin1Char('0')).toUpper();
    const QString title = label_.isEmpty() ? QStringLiteral("Shape 0x%1").arg(hex) : label_;
    setToolTip(QStringLiteral("%1\nID: %2 / 0x%3")
                   .arg(title)
                   .arg(shapeId_)
                   .arg(hex));

    favourite_ = new QToolButton(this);
    favourite_->setToolTip(QStringLiteral("Favourite"));
    favourite_->setGeometry(width() - 28, height() - 28, 22, 22);
    favourite_->setCursor(Qt::PointingHandCursor);
    favourite_->setCheckable(true);
    favourite_->setAutoRaise(true);
    favourite_->setIconSize(QSize(18, 18));
    favourite_->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: 2px; background: transparent; }"));
    updateFavouriteIcon();
    connect(favourite_, &QToolButton::toggled, this, [this](bool checked) {
        updateFavouriteIcon();
        favouriteToggled(checked);
    });
}

void ShapeTile::setFavourite(bool enabled)
{
    const QSignalBlocker blocker(favourite_);
    favourite_->setChecked(enabled);
    updateFavouriteIcon();
}

void ShapeTile::refreshTheme()
{
    previewCache_.clear();
    updateFavouriteIcon();
    update();
}

void ShapeTile::setPressedCallback(std::function<void(int)> callback)
{
    pressedCallback_ = std::move(callback);
}

void ShapeTile::setFavouriteCallback(std::function<void(int, bool)> callback)
{
    favouriteCallback_ = std::move(callback);
}

void ShapeTile::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void ShapeTile::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void ShapeTile::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !favourite_->geometry().contains(event->position().toPoint())) {
        if (pressedCallback_) {
            pressedCallback_(shapeId_);
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ShapeTile::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect tileRect = rect().adjusted(2, 2, -2, -2);
    const bool dark = isDarkTheme(currentUiTheme());
    const QColor tileBase = dark ? QColor(34, 34, 34) : QColor(232, 235, 240);
    const QColor tileHover = dark ? QColor(42, 42, 42) : QColor(218, 223, 231);
    const QColor stroke = dark ? QColor(68, 68, 68) : QColor(145, 153, 166);
    const QColor hoverStroke = dark ? QColor(119, 119, 119) : QColor(96, 107, 124);
    const QColor previewBase = dark ? QColor(21, 21, 21) : QColor(248, 249, 251);
    const QColor labelColor = dark ? QColor(238, 238, 238) : QColor(32, 34, 37);

    painter.fillRect(tileRect, hovered_ ? tileHover : tileBase);
    painter.setPen(QPen(hovered_ ? hoverStroke : stroke, 1));
    painter.drawRect(tileRect.adjusted(0, 0, -1, -1));

    const QRect previewRect((width() - PreviewSize.width()) / 2, 12, PreviewSize.width(), PreviewSize.height());
    painter.fillRect(previewRect, previewBase);
    drawPreview(painter, previewRect);

    const QRect labelRect(8, height() - 30, width() - 40, 22);
    painter.setPen(labelColor);
    const QString display = label_.isEmpty()
        ? QStringLiteral("0x%1").arg(shapeId_, 4, 16, QLatin1Char('0')).toUpper()
        : QStringLiteral("%1 (%2)").arg(label_).arg(shapeId_);
    painter.drawText(labelRect, Qt::AlignCenter, painter.fontMetrics().elidedText(display, Qt::ElideRight, labelRect.width()));
}

void ShapeTile::updateFavouriteIcon()
{
    favourite_->setIcon(assetIcon(favourite_->isChecked()
                                      ? QStringLiteral("ShapeBrowserFavOn.xpm")
                                      : QStringLiteral("ShapeBrowserFavOff.xpm")));
}

void ShapeTile::favouriteToggled(bool checked)
{
    if (favouriteCallback_) {
        favouriteCallback_(shapeId_, checked);
    }
}

void ShapeTile::drawPreview(QPainter &painter, const QRect &rect)
{
    if (previewCache_.contains(rect.size())) {
        painter.drawImage(rect.topLeft(), previewCache_.value(rect.size()));
        return;
    }
    if (geometry_ == nullptr) {
        return;
    }
    const QColor ink = isDarkTheme(currentUiTheme()) ? QColor(245, 245, 245) : QColor(24, 26, 30);
    const QImage preview = renderShapePreviewImage(*geometry_, shapeId_, rect.size(), ink);
    if (preview.isNull()) {
        return;
    }
    previewCache_.insert(rect.size(), preview);
    painter.drawImage(rect.topLeft(), preview);
}

QImage renderShapePreviewImage(const ShapeGeometryStore &geometry, int shapeId, const QSize &size, const QColor &ink)
{
    const ShapeGeometry *shape = geometry.shape(shapeId);
    if (shape == nullptr || size.isEmpty()) {
        return {};
    }

    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const double scale = std::min(static_cast<double>(size.width()) / static_cast<double>(std::max(shape->width, 1)),
                                  static_cast<double>(size.height()) / static_cast<double>(std::max(shape->height, 1))) * 0.86;
    const QPointF center(size.width() * 0.5, size.height() * 0.5);
    const auto mapPoint = [&](const QPointF &point) {
        return QPointF(center.x() + point.x() * scale,
                       center.y() - point.y() * scale);
    };

    if (shape->triangles.isEmpty()) {
        QPainter fallback(&image);
        fallback.setPen(Qt::NoPen);
        fallback.setBrush(previewColor(ink, 0.75));
        const QSizeF scaled(shape->width * scale, shape->height * scale);
        fallback.drawRect(QRectF(center.x() - scaled.width() * 0.5,
                                 center.y() - scaled.height() * 0.5,
                                 scaled.width(),
                                 scaled.height()));
        return image;
    }

    for (const ShapeTriangle &triangle : shape->triangles) {
        rasterizePreviewTriangle(image,
                                 ink,
                                 mapPoint(triangle.p0),
                                 mapPoint(triangle.p1),
                                 mapPoint(triangle.p2),
                                 triangle.alpha0,
                                 triangle.alpha1,
                                 triangle.alpha2);
    }
    return image;
}

QColor customLayerColor(const fh6::scene::Shape &layer, double alphaScale = 1.0)
{
    return QColor(layer.color[2],
                  layer.color[1],
                  layer.color[0],
                  std::clamp(static_cast<int>(std::round(layer.color[3] * alphaScale)), 0, 255));
}

QRectF customLayerBounds(const fh6::scene::Shape &layer,
                         const ShapeGeometryStore &geometry,
                         const QTransform &parentWorld)
{
    const QSizeF size = layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight)
                                     : geometry.shapeSize(layer.shapeId);
    const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
    return (customNodeTransform(layer) * parentWorld).mapRect(local);
}

QRectF customNodeBounds(const fh6::scene::Layer &node,
                        const ShapeGeometryStore &geometry,
                        const QTransform &parentWorld)
{
    const QTransform world = customNodeTransform(node) * parentWorld;
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        return customLayerBounds(static_cast<const fh6::scene::Shape &>(node), geometry, parentWorld);
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return {};
    }
    const auto &group = static_cast<const fh6::scene::Group &>(node);
    QRectF bounds;
    bool hasBounds = false;
    for (const auto &child : group.children) {
        const QRectF childBounds = customNodeBounds(*child, geometry, world);
        if (!childBounds.isValid() || childBounds.isEmpty()) {
            continue;
        }
        bounds = hasBounds ? bounds.united(childBounds) : childBounds;
        hasBounds = true;
    }
    return bounds;
}

void blendCustomPreviewPixel(QImage &image, int x, int y, const fh6::scene::Shape &layer, double alphaScale)
{
    const double sourceAlpha = std::clamp((layer.color[3] / 255.0) * alphaScale, 0.0, 1.0);
    if (sourceAlpha <= 0.0) {
        return;
    }

    QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
    const QRgb destination = line[x];
    const double keep = 1.0 - sourceAlpha;
    if (layer.mask) {
        line[x] = qRgba(std::clamp(static_cast<int>(std::round(qRed(destination) * keep)), 0, 255),
                        std::clamp(static_cast<int>(std::round(qGreen(destination) * keep)), 0, 255),
                        std::clamp(static_cast<int>(std::round(qBlue(destination) * keep)), 0, 255),
                        std::clamp(static_cast<int>(std::round(qAlpha(destination) * keep)), 0, 255));
        return;
    }

    const QColor color = customLayerColor(layer);
    line[x] = qRgba(std::clamp(static_cast<int>(std::round(color.red() * sourceAlpha + qRed(destination) * keep)), 0, 255),
                    std::clamp(static_cast<int>(std::round(color.green() * sourceAlpha + qGreen(destination) * keep)), 0, 255),
                    std::clamp(static_cast<int>(std::round(color.blue() * sourceAlpha + qBlue(destination) * keep)), 0, 255),
                    std::clamp(static_cast<int>(std::round(sourceAlpha * 255.0 + qAlpha(destination) * keep)), 0, 255));
}

void rasterizeCustomPreviewTriangle(QImage &image,
                                    const QPointF &p0,
                                    const QPointF &p1,
                                    const QPointF &p2,
                                    double alpha0,
                                    double alpha1,
                                    double alpha2,
                                    const fh6::scene::Shape &layer)
{
    const double minX = std::floor(std::min({p0.x(), p1.x(), p2.x()}));
    const double maxX = std::ceil(std::max({p0.x(), p1.x(), p2.x()}));
    const double minY = std::floor(std::min({p0.y(), p1.y(), p2.y()}));
    const double maxY = std::ceil(std::max({p0.y(), p1.y(), p2.y()}));
    const int left = std::clamp(static_cast<int>(minX), 0, image.width() - 1);
    const int right = std::clamp(static_cast<int>(maxX), 0, image.width() - 1);
    const int top = std::clamp(static_cast<int>(minY), 0, image.height() - 1);
    const int bottom = std::clamp(static_cast<int>(maxY), 0, image.height() - 1);
    const double denominator = (p1.y() - p2.y()) * (p0.x() - p2.x()) + (p2.x() - p1.x()) * (p0.y() - p2.y());
    if (std::abs(denominator) < 1e-8) {
        return;
    }

    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            const double sampleX = x + 0.5;
            const double sampleY = y + 0.5;
            const double w0 = ((p1.y() - p2.y()) * (sampleX - p2.x()) + (p2.x() - p1.x()) * (sampleY - p2.y())) / denominator;
            const double w1 = ((p2.y() - p0.y()) * (sampleX - p2.x()) + (p0.x() - p2.x()) * (sampleY - p2.y())) / denominator;
            const double w2 = 1.0 - w0 - w1;
            if (w0 < -1e-4 || w1 < -1e-4 || w2 < -1e-4) {
                continue;
            }
            blendCustomPreviewPixel(image, x, y, layer, w0 * alpha0 + w1 * alpha1 + w2 * alpha2);
        }
    }
}

void paintCustomPreviewLayer(QImage &image,
                             const fh6::scene::Shape &layer,
                             const ShapeGeometryStore &geometry,
                             const QTransform &worldToPreview,
                             const QTransform &parentWorld)
{
    if (!layer.visible) {
        return;
    }
    const ShapeGeometry *shape = geometry.shape(layer.shapeId);
    const QTransform localToWorld = customNodeTransform(layer) * parentWorld;
    if (layer.raster || shape == nullptr || shape->triangles.isEmpty()) {
        const QSizeF size = layer.raster ? QSizeF(layer.rasterWidth, layer.rasterHeight)
                                         : geometry.shapeSize(layer.shapeId);
        const QRectF local(-size.width() * 0.5, -size.height() * 0.5, size.width(), size.height());
        QPolygonF polygon;
        polygon << worldToPreview.map(localToWorld.map(local.topLeft()))
                << worldToPreview.map(localToWorld.map(local.topRight()))
                << worldToPreview.map(localToWorld.map(local.bottomRight()))
                << worldToPreview.map(localToWorld.map(local.bottomLeft()));
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setCompositionMode(layer.mask ? QPainter::CompositionMode_DestinationOut : QPainter::CompositionMode_SourceOver);
        painter.setBrush(layer.mask ? QColor(0, 0, 0, layer.color[3]) : customLayerColor(layer, 0.75));
        painter.drawPolygon(polygon);
        return;
    }

    for (const ShapeTriangle &triangle : shape->triangles) {
        rasterizeCustomPreviewTriangle(image,
                                       worldToPreview.map(localToWorld.map(triangle.p0)),
                                       worldToPreview.map(localToWorld.map(triangle.p1)),
                                       worldToPreview.map(localToWorld.map(triangle.p2)),
                                       triangle.alpha0,
                                       triangle.alpha1,
                                       triangle.alpha2,
                                       layer);
    }
}

void paintCustomPreviewNode(QImage &image,
                            const fh6::scene::Layer &node,
                            const ShapeGeometryStore &geometry,
                            const QTransform &worldToPreview,
                            const QTransform &parentWorld)
{
    if (node.kind() == fh6::scene::LayerKind::Shape) {
        paintCustomPreviewLayer(image,
                                static_cast<const fh6::scene::Shape &>(node),
                                geometry,
                                worldToPreview,
                                parentWorld);
        return;
    }
    if (node.kind() != fh6::scene::LayerKind::Group) {
        return;
    }
    const QTransform world = customNodeTransform(node) * parentWorld;
    for (const auto &child : static_cast<const fh6::scene::Group &>(node).children) {
        paintCustomPreviewNode(image, *child, geometry, worldToPreview, world);
    }
}

QImage renderCustomGroupPreviewImage(const CustomShapeGroup &group, const ShapeGeometryStore &geometry, const QSize &size)
{
    if (size.isEmpty()) {
        return {};
    }
    QRectF bounds;
    bool hasBounds = false;
    forEachClipboardRoot(group.clipboard, [&](const fh6::scene::Layer &node) {
        const QRectF entry = customNodeBounds(node, geometry, QTransform());
        if (!entry.isValid() || entry.isEmpty()) {
            return;
        }
        bounds = hasBounds ? bounds.united(entry) : entry;
        hasBounds = true;
    });
    if (!hasBounds) {
        return {};
    }

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const double available = std::max(1.0, static_cast<double>(std::min(size.width(), size.height())) - 8.0);
    const double scale = std::min(available / std::max(bounds.width(), 1.0), available / std::max(bounds.height(), 1.0));
    QTransform worldToPreview;
    worldToPreview.translate(size.width() * 0.5, size.height() * 0.5);
    worldToPreview.scale(scale, -scale);
    worldToPreview.translate(-bounds.center().x(), -bounds.center().y());

    forEachClipboardRoot(group.clipboard, [&](const fh6::scene::Layer &node) {
        paintCustomPreviewNode(image, node, geometry, worldToPreview, QTransform());
    });
    return image;
}

CustomGroupTile::CustomGroupTile(const CustomShapeGroup &group, const ShapeGeometryStore *geometry, QWidget *parent)
    : QWidget(parent)
    , group_(group)
    , geometry_(geometry)
{
    setFixedSize(TileSize);
    setCursor(Qt::PointingHandCursor);
    setToolTip(group_.name);

    delete_ = new QToolButton(this);
    delete_->setToolTip(QStringLiteral("Delete custom group"));
    delete_->setGeometry(width() - 28, height() - 28, 22, 22);
    delete_->setCursor(Qt::PointingHandCursor);
    delete_->setAutoRaise(true);
    delete_->setIconSize(QSize(18, 18));
    delete_->setStyleSheet(QStringLiteral("QToolButton { border: none; padding: 2px; background: transparent; }"));
    updateDeleteIcon();
    connect(delete_, &QToolButton::clicked, this, [this]() {
        if (deleteCallback_) {
            deleteCallback_(group_.id);
        }
    });
}

void CustomGroupTile::refreshTheme()
{
    previewCache_.clear();
    updateDeleteIcon();
    update();
}

void CustomGroupTile::setPressedCallback(std::function<void(const QString &)> callback)
{
    pressedCallback_ = std::move(callback);
}

void CustomGroupTile::setDeleteCallback(std::function<void(const QString &)> callback)
{
    deleteCallback_ = std::move(callback);
}

void CustomGroupTile::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void CustomGroupTile::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void CustomGroupTile::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !delete_->geometry().contains(event->position().toPoint())) {
        if (pressedCallback_) {
            pressedCallback_(group_.id);
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void CustomGroupTile::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect tileRect = rect().adjusted(2, 2, -2, -2);
    const bool dark = isDarkTheme(currentUiTheme());
    const QColor tileBase = dark ? QColor(34, 34, 34) : QColor(232, 235, 240);
    const QColor tileHover = dark ? QColor(42, 42, 42) : QColor(218, 223, 231);
    const QColor stroke = dark ? QColor(68, 68, 68) : QColor(145, 153, 166);
    const QColor hoverStroke = dark ? QColor(119, 119, 119) : QColor(96, 107, 124);
    const QColor labelColor = dark ? QColor(238, 238, 238) : QColor(32, 34, 37);
    const QColor previewBase = dark ? QColor(21, 21, 21) : QColor(248, 249, 251);

    painter.fillRect(tileRect, hovered_ ? tileHover : tileBase);
    painter.setPen(QPen(hovered_ ? hoverStroke : stroke, 1));
    painter.drawRect(tileRect.adjusted(0, 0, -1, -1));

    const QRect previewRect((width() - PreviewSize.width()) / 2, 12, PreviewSize.width(), PreviewSize.height());
    painter.fillRect(previewRect, previewBase);
    drawPreview(painter, previewRect);
    painter.setPen(QPen(dark ? QColor(78, 78, 78) : QColor(180, 186, 196), 1));
    painter.drawRect(previewRect.adjusted(0, 0, -1, -1));

    const QRect countRect(width() - 42, 8, 30, 18);
    painter.fillRect(countRect, dark ? QColor(58, 58, 58) : QColor(210, 216, 226));
    painter.setPen(labelColor);
    painter.drawText(countRect, Qt::AlignCenter, QString::number(layerCount()));

    const QRect labelRect(8, height() - 30, width() - 40, 22);
    painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, painter.fontMetrics().elidedText(group_.name, Qt::ElideRight, labelRect.width()));
}

void CustomGroupTile::updateDeleteIcon()
{
    delete_->setIcon(assetIcon(QStringLiteral("MenuExit.xpm")));
}

void CustomGroupTile::drawPreview(QPainter &painter, const QRect &rect)
{
    if (previewCache_.contains(rect.size())) {
        painter.drawImage(rect.topLeft(), previewCache_.value(rect.size()));
        return;
    }
    if (geometry_ == nullptr) {
        return;
    }
    const QImage preview = renderCustomGroupPreviewImage(group_, *geometry_, rect.size());
    if (preview.isNull()) {
        return;
    }
    previewCache_.insert(rect.size(), preview);
    painter.drawImage(rect.topLeft(), preview);
}

int CustomGroupTile::layerCount() const
{
    return clipboardShapeCount(group_.clipboard);
}

LogoTile::LogoTile(quint32 rasterId, const QSize &size, QWidget *parent)
    : QWidget(parent)
    , rasterId_(rasterId)
    , size_(size)
{
    setFixedSize(TileSize);
    setCursor(Qt::PointingHandCursor);
    const QString hex = QStringLiteral("%1").arg(rasterId_, 4, 16, QLatin1Char('0')).toUpper();
    setToolTip(QStringLiteral("Logo %1\nID: %1 / 0x%2").arg(rasterId_).arg(hex));
}

void LogoTile::refreshTheme()
{
    previewCache_.clear();
    update();
}

void LogoTile::setPressedCallback(std::function<void(quint32)> callback)
{
    pressedCallback_ = std::move(callback);
}

void LogoTile::enterEvent(QEnterEvent *event)
{
    hovered_ = true;
    update();
    QWidget::enterEvent(event);
}

void LogoTile::leaveEvent(QEvent *event)
{
    hovered_ = false;
    update();
    QWidget::leaveEvent(event);
}

void LogoTile::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (pressedCallback_) {
            pressedCallback_(rasterId_);
        }
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void LogoTile::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect tileRect = rect().adjusted(2, 2, -2, -2);
    const bool dark = isDarkTheme(currentUiTheme());
    const QColor tileBase = dark ? QColor(34, 34, 34) : QColor(232, 235, 240);
    const QColor tileHover = dark ? QColor(42, 42, 42) : QColor(218, 223, 231);
    const QColor stroke = dark ? QColor(68, 68, 68) : QColor(145, 153, 166);
    const QColor hoverStroke = dark ? QColor(119, 119, 119) : QColor(96, 107, 124);
    const QColor previewBase = dark ? QColor(21, 21, 21) : QColor(248, 249, 251);
    const QColor labelColor = dark ? QColor(238, 238, 238) : QColor(32, 34, 37);

    painter.fillRect(tileRect, hovered_ ? tileHover : tileBase);
    painter.setPen(QPen(hovered_ ? hoverStroke : stroke, 1));
    painter.drawRect(tileRect.adjusted(0, 0, -1, -1));

    const QRect previewRect((width() - PreviewSize.width()) / 2, 12, PreviewSize.width(), PreviewSize.height());
    painter.fillRect(previewRect, previewBase);
    drawPreview(painter, previewRect);

    const QRect labelRect(8, height() - 30, width() - 16, 22);
    painter.setPen(labelColor);
    const QString display = QStringLiteral("Logo %1").arg(rasterId_);
    painter.drawText(labelRect, Qt::AlignCenter, painter.fontMetrics().elidedText(display, Qt::ElideRight, labelRect.width()));
}

void LogoTile::drawPreview(QPainter &painter, const QRect &rect)
{
    if (previewCache_.contains(rect.size())) {
        painter.drawImage(rect.topLeft(), previewCache_.value(rect.size()));
        return;
    }
    const fh6::RasterDecal decal = fh6::sharedRasterDecals().decal(rasterId_);
    if (!decal.valid() || rect.isEmpty()) {
        return;
    }
    QImage decalImage(reinterpret_cast<const uchar *>(decal.rgba.constData()),
                      decal.width,
                      decal.height,
                      decal.width * 4,
                      QImage::Format_RGBA8888);
    decalImage = decalImage.copy();

    QImage preview(rect.size(), QImage::Format_ARGB32);
    preview.fill(Qt::transparent);
    QPainter imagePainter(&preview);
    imagePainter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QSize fit = decalImage.size().scaled(QSize(std::round(rect.width() * 0.86),
                                                     std::round(rect.height() * 0.86)),
                                               Qt::KeepAspectRatio);
    const QRect target((rect.width() - fit.width()) / 2, (rect.height() - fit.height()) / 2, fit.width(), fit.height());
    imagePainter.drawImage(target, decalImage);
    imagePainter.end();
    previewCache_.insert(rect.size(), preview);
    painter.drawImage(rect.topLeft(), preview);
}

ShapesBrowserWidget::ShapesBrowserWidget(QWidget *parent)
    : QWidget(parent)
{
    geometryLoaded_ = geometry_.loadDefault();
    names_.loadDefault();
    const fh6::RasterDecalPack &logoPack = fh6::sharedRasterDecals();
    if (logoPack.isLoaded()) {
        for (quint32 id : logoPack.ids()) {
            const QSize size = logoPack.decalSize(id);
            if (size.isValid()) {
                logos_.insert(id, size);
            }
        }
    }
    favourites_ = loadFavourites();
    customGroups_ = loadCustomGroups();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    search_ = new QLineEdit(this);
    search_->setPlaceholderText(QStringLiteral("Filter shapes..."));
    search_->addAction(assetIcon(QStringLiteral("ShapeBrowserSearch.xpm")), QLineEdit::LeadingPosition);
    layout->addWidget(search_);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setHandleWidth(6);
    layout->addWidget(splitter, 1);

    auto *categoryPane = new QWidget(splitter);
    auto *categoryLayout = new QVBoxLayout(categoryPane);
    categoryLayout->setContentsMargins(0, 0, 0, 0);
    categoryLayout->setSpacing(6);
    categoriesList_ = new QListWidget(categoryPane);
    categoriesList_->setMinimumWidth(150);
    categoryLayout->addWidget(categoriesList_, 1);
    addSelection_ = new QToolButton(categoryPane);
    addSelection_->setText(QStringLiteral("Add current selection"));
    addSelection_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    addSelection_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    categoryLayout->addWidget(addSelection_);
    splitter->addWidget(categoryPane);
    splitter->setCollapsible(0, false);

    scroll_ = new QScrollArea(splitter);
    scroll_->setWidgetResizable(true);
    gridHost_ = new QWidget(scroll_);
    grid_ = new QGridLayout(gridHost_);
    grid_->setContentsMargins(8, 8, 8, 8);
    grid_->setHorizontalSpacing(8);
    grid_->setVerticalSpacing(8);
    grid_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    scroll_->setWidget(gridHost_);
    splitter->addWidget(scroll_);
    splitter->setStretchFactor(1, 1);
    installSplitterResizeCursor(splitter);
    refreshTheme();

    connect(search_, &QLineEdit::textChanged, this, [this]() { refreshGrid(); });
    connect(addSelection_, &QToolButton::clicked, this, [this]() {
        if (addCurrentSelectionCallback_) {
            addCurrentSelectionCallback_();
        }
    });
    connect(categoriesList_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *) {
        if (current == nullptr) {
            return;
        }
        currentCategory_ = current->text();
        refreshGrid();
    });

    populateCategories();
    int initialRow = 0;
    for (int row = 0; row < categoryOrder_.size(); ++row) {
        if (!categoryShapeIds(categoryOrder_[row]).isEmpty()) {
            initialRow = row;
            break;
        }
    }
    categoriesList_->setCurrentRow(initialRow);
}

void ShapesBrowserWidget::setShapeSelectedCallback(std::function<void(int)> callback)
{
    shapeSelectedCallback_ = std::move(callback);
}

void ShapesBrowserWidget::setLogoSelectedCallback(std::function<void(quint32, int, int)> callback)
{
    logoSelectedCallback_ = std::move(callback);
}

void ShapesBrowserWidget::setCustomGroupSelectedCallback(std::function<void(const CustomShapeGroup &)> callback)
{
    customGroupSelectedCallback_ = std::move(callback);
}

void ShapesBrowserWidget::setAddCurrentSelectionCallback(std::function<void()> callback)
{
    addCurrentSelectionCallback_ = std::move(callback);
}

void ShapesBrowserWidget::addCustomGroup(const QString &name, const ProjectClipboard &clipboard)
{
    CustomShapeGroup group;
    group.id = newCustomGroupId();
    group.name = name.trimmed().isEmpty() ? QStringLiteral("Custom Group") : name.trimmed();
    group.clipboard = clipboard;
    customGroups_.push_back(group);
    saveCustomGroups();
    populateCategories();
    QList<QListWidgetItem *> matches = categoriesList_->findItems(QString::fromLatin1(CustomCategory), Qt::MatchExactly);
    if (!matches.isEmpty()) {
        categoriesList_->setCurrentItem(matches.front());
    }
    refreshGrid();
}

void ShapesBrowserWidget::refreshTheme()
{
    const QPalette pal = paletteForTheme(currentUiTheme());
    const QString base = pal.color(QPalette::Base).name();
    const QString text = pal.color(QPalette::Text).name();
    categoriesList_->setStyleSheet(QStringLiteral("QListWidget { background: %1; color: %2; }").arg(base, text));
    scroll_->setStyleSheet(QStringLiteral("QScrollArea { background: %1; border: none; }").arg(base));
    gridHost_->setStyleSheet(QStringLiteral("QWidget { background: %1; }").arg(base));
    if (search_ != nullptr) {
        const QList<QAction *> actions = search_->actions();
        if (!actions.isEmpty()) {
            search_->removeAction(actions.front());
        }
        search_->addAction(assetIcon(QStringLiteral("ShapeBrowserSearch.xpm")), QLineEdit::LeadingPosition);
    }
    for (ShapeTile *tile : tiles_) {
        if (tile != nullptr) {
            tile->refreshTheme();
        }
    }
    for (LogoTile *tile : logoTiles_) {
        if (tile != nullptr) {
            tile->refreshTheme();
        }
    }
    for (CustomGroupTile *tile : customTiles_) {
        if (tile != nullptr) {
            tile->refreshTheme();
        }
    }
    if (searchHint_ != nullptr && searchHint_->isVisible()) {
        showGridMessage(searchHint_->text());
    }
}

void ShapesBrowserWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    refreshGrid();
}

void ShapesBrowserWidget::populateCategories()
{
    categories_.clear();
    categoryOrder_.clear();
    categoryOrder_ << QString::fromLatin1(FavouritesCategory) << QString::fromLatin1(CustomCategory) << QString::fromLatin1(LogoCategory);
    categories_.insert(QString::fromLatin1(FavouritesCategory), {});
    categories_.insert(QString::fromLatin1(CustomCategory), {});

    QVector<int> ids = geometry_.shapeIds();
    std::sort(ids.begin(), ids.end());
    for (int shapeId : ids) {
        const ShapeGeometry *shape = geometry_.shape(shapeId);
        if (shape == nullptr) {
            continue;
        }
        const QString category = categoryNameForShape(shapeId, *shape);
        if (!categories_.contains(category)) {
            categories_.insert(category, {});
            categoryOrder_.push_back(category);
        }
        categories_[category].push_back(shapeId);
    }

    QStringList vinylCategories;
    QStringList fontCategories;
    for (auto it = categories_.constBegin(); it != categories_.constEnd(); ++it) {
        if (it.key() == QString::fromLatin1(FavouritesCategory)
            || it.key() == QString::fromLatin1(CustomCategory)
            || it.key() == QString::fromLatin1(LogoCategory)) {
            continue;
        }
        if (isFontCategory(it.key())) {
            fontCategories.push_back(it.key());
        } else {
            vinylCategories.push_back(it.key());
        }
    }
    vinylCategories.sort(Qt::CaseInsensitive);
    fontCategories.sort(Qt::CaseInsensitive);
    categoryOrder_ = QStringList{QString::fromLatin1(FavouritesCategory),
                                 QString::fromLatin1(CustomCategory),
                                 QString::fromLatin1(LogoCategory)}
        + vinylCategories
        + fontCategories;

    categoriesList_->clear();
    for (const QString &category : categoryOrder_) {
        categoriesList_->addItem(category);
    }
}

void ShapesBrowserWidget::refreshGrid()
{
    if (grid_ == nullptr || scroll_ == nullptr) {
        return;
    }
    while (grid_->count() > 0) {
        QLayoutItem *item = grid_->takeAt(0);
        if (QWidget *widget = item->widget()) {
            widget->setParent(nullptr);
        }
        delete item;
    }
    grid_->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    const QString query = search_->text().trimmed().toLower();

    constexpr int MinSearchLength = 3;
    if (!query.isEmpty() && query.size() < MinSearchLength) {
        showGridMessage(QStringLiteral("Type at least %1 characters to search").arg(MinSearchLength));
        return;
    }
    const bool searching = query.size() >= MinSearchLength;

    if (!searching && currentCategory_ == QString::fromLatin1(CustomCategory)) {
        const int columns = std::max(1, scroll_->viewport()->width() / (TileSize.width() + 10));
        for (int column = 0; column <= columns; ++column) {
            grid_->setColumnStretch(column, 0);
        }
        for (int index = 0; index < customGroups_.size(); ++index) {
            CustomGroupTile *tile = tileForCustomGroup(customGroups_[index]);
            grid_->addWidget(tile, index / columns, index % columns);
        }
        grid_->setColumnStretch(columns, 1);
        grid_->setRowStretch((customGroups_.size() + columns - 1) / columns, 1);
        return;
    }

    if (!searching && currentCategory_ == QString::fromLatin1(LogoCategory)) {
        const int columns = std::max(1, scroll_->viewport()->width() / (TileSize.width() + 10));
        for (int column = 0; column <= columns; ++column) {
            grid_->setColumnStretch(column, 0);
        }
        QVector<quint32> ids;
        ids.reserve(logos_.size());
        for (auto it = logos_.constBegin(); it != logos_.constEnd(); ++it) {
            ids.push_back(it.key());
        }
        std::sort(ids.begin(), ids.end());
        for (int index = 0; index < ids.size(); ++index) {
            LogoTile *tile = tileForLogo(ids[index]);
            grid_->addWidget(tile, index / columns, index % columns);
        }
        grid_->setColumnStretch(columns, 1);
        grid_->setRowStretch((ids.size() + columns - 1) / columns, 1);
        return;
    }

    QVector<CustomShapeGroup> matchedCustom;
    if (searching) {
        for (const CustomShapeGroup &group : customGroups_) {
            const QString haystack = QStringLiteral("%1 %2")
                                         .arg(group.name)
                                         .arg(clipboardShapeCount(group.clipboard))
                                         .toLower();
            if (haystack.contains(query)) {
                matchedCustom.push_back(group);
            }
        }
    }

    QVector<quint32> matchedLogos;
    if (searching) {
        for (auto it = logos_.constBegin(); it != logos_.constEnd(); ++it) {
            const quint32 rasterId = it.key();
            const QString haystack = QStringLiteral("logo %1 0x%2")
                                         .arg(rasterId)
                                         .arg(rasterId, 4, 16, QLatin1Char('0'))
                                         .toLower();
            if (haystack.contains(query)) {
                matchedLogos.push_back(rasterId);
            }
        }
        std::sort(matchedLogos.begin(), matchedLogos.end());
    }

    QVector<int> searchIds = searching ? geometry_.shapeIds() : categoryShapeIds(currentCategory_);
    if (searching) {
        std::sort(searchIds.begin(), searchIds.end());
    }

    QVector<int> filtered;
    for (int shapeId : searchIds) {
        const ShapeGeometry *shape = geometry_.shape(shapeId);
        if (shape == nullptr) {
            continue;
        }
        if (!searching) {
            filtered.push_back(shapeId);
            continue;
        }
        const QString name = nameForShape(shapeId, *shape);
        const QString haystack = QStringLiteral("%1 0x%2 %3")
                                     .arg(shapeId)
                                     .arg(shapeId, 4, 16, QLatin1Char('0'))
                                     .arg(name)
                                     .toLower();
        if (haystack.contains(query)) {
            filtered.push_back(shapeId);
        }
    }

    const int columns = std::max(1, scroll_->viewport()->width() / (TileSize.width() + 10));
    for (int column = 0; column <= columns; ++column) {
        grid_->setColumnStretch(column, 0);
    }
    int index = 0;
    for (const CustomShapeGroup &group : matchedCustom) {
        CustomGroupTile *tile = tileForCustomGroup(group);
        grid_->addWidget(tile, index / columns, index % columns);
        ++index;
    }
    for (quint32 rasterId : matchedLogos) {
        LogoTile *tile = tileForLogo(rasterId);
        grid_->addWidget(tile, index / columns, index % columns);
        ++index;
    }
    for (int shapeId : filtered) {
        ShapeTile *tile = tileForShape(shapeId);
        tile->setFavourite(favourites_.contains(shapeId));
        grid_->addWidget(tile, index / columns, index % columns);
        ++index;
    }
    grid_->setColumnStretch(columns, 1);
    grid_->setRowStretch((index + columns - 1) / columns, 1);
}

void ShapesBrowserWidget::showGridMessage(const QString &text)
{
    if (searchHint_ == nullptr) {
        searchHint_ = new QLabel(gridHost_);
        searchHint_->setAlignment(Qt::AlignCenter);
        searchHint_->setWordWrap(true);
        searchHint_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    const QPalette pal = paletteForTheme(currentUiTheme());
    const QColor textColor = pal.color(QPalette::Text);
    const QColor baseColor = pal.color(QPalette::Base);
    const QColor muted((textColor.red() + baseColor.red()) / 2,
                       (textColor.green() + baseColor.green()) / 2,
                       (textColor.blue() + baseColor.blue()) / 2);
    searchHint_->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 13px; }").arg(muted.name()));
    searchHint_->setText(text);
    for (int column = 0; column < grid_->columnCount(); ++column) {
        grid_->setColumnStretch(column, 0);
    }
    for (int row = 0; row < grid_->rowCount(); ++row) {
        grid_->setRowStretch(row, 0);
    }
    grid_->setAlignment(Qt::Alignment());
    grid_->addWidget(searchHint_, 0, 0);
    grid_->setColumnStretch(0, 1);
    grid_->setRowStretch(0, 1);
    searchHint_->show();
}

ShapeTile *ShapesBrowserWidget::tileForShape(int shapeId)
{
    ShapeTile *tile = tiles_.value(shapeId, nullptr);
    if (tile != nullptr) {
        return tile;
    }
    const ShapeGeometry *shape = geometry_.shape(shapeId);
    const QString label = shape == nullptr ? QString() : nameForShape(shapeId, *shape);
    tile = new ShapeTile(shapeId, label, &geometry_);
    tile->setPressedCallback([this](int id) {
        if (shapeSelectedCallback_) {
            shapeSelectedCallback_(id);
        }
    });
    tile->setFavouriteCallback([this](int id, bool enabled) { setFavourite(id, enabled); });
    tiles_.insert(shapeId, tile);
    return tile;
}

LogoTile *ShapesBrowserWidget::tileForLogo(quint32 rasterId)
{
    LogoTile *tile = logoTiles_.value(rasterId, nullptr);
    if (tile != nullptr) {
        return tile;
    }
    const QSize size = logos_.value(rasterId);
    tile = new LogoTile(rasterId, size);
    tile->setPressedCallback([this](quint32 id) {
        if (logoSelectedCallback_) {
            const QSize logoSize = logos_.value(id);
            logoSelectedCallback_(id, logoSize.width(), logoSize.height());
        }
    });
    logoTiles_.insert(rasterId, tile);
    return tile;
}

CustomGroupTile *ShapesBrowserWidget::tileForCustomGroup(const CustomShapeGroup &group)
{
    CustomGroupTile *tile = customTiles_.value(group.id, nullptr);
    if (tile != nullptr) {
        return tile;
    }
    tile = new CustomGroupTile(group, &geometry_);
    tile->setPressedCallback([this](const QString &id) {
        for (const CustomShapeGroup &group : customGroups_) {
            if (group.id == id && customGroupSelectedCallback_) {
                customGroupSelectedCallback_(group);
                return;
            }
        }
    });
    tile->setDeleteCallback([this](const QString &id) { deleteCustomGroup(id); });
    customTiles_.insert(group.id, tile);
    return tile;
}

void ShapesBrowserWidget::setFavourite(int shapeId, bool enabled)
{
    if (enabled) {
        favourites_.insert(shapeId);
    } else {
        favourites_.remove(shapeId);
    }
    saveFavourites();
    if (currentCategory_ == QString::fromLatin1(FavouritesCategory)) {
        refreshGrid();
    }
}

void ShapesBrowserWidget::deleteCustomGroup(const QString &id)
{
    auto it = std::find_if(customGroups_.begin(), customGroups_.end(), [&](const CustomShapeGroup &group) {
        return group.id == id;
    });
    if (it == customGroups_.end()) {
        return;
    }
    const QString name = it->name;
    if (QMessageBox::question(this,
                              QStringLiteral("Delete Custom Group"),
                              QStringLiteral("Delete custom group \"%1\"?").arg(name))
        != QMessageBox::Yes) {
        return;
    }
    customGroups_.erase(it);
    if (CustomGroupTile *tile = customTiles_.take(id)) {
        tile->deleteLater();
    }
    saveCustomGroups();
    populateCategories();
    QList<QListWidgetItem *> matches = categoriesList_->findItems(QString::fromLatin1(CustomCategory), Qt::MatchExactly);
    if (!matches.isEmpty()) {
        categoriesList_->setCurrentItem(matches.front());
    }
    refreshGrid();
}

QVector<int> ShapesBrowserWidget::categoryShapeIds(const QString &category) const
{
    if (category == QString::fromLatin1(FavouritesCategory)) {
        QVector<int> ids;
        ids.reserve(favourites_.size());
        for (int shapeId : favourites_) {
            if (geometry_.shape(shapeId) != nullptr) {
                ids.push_back(shapeId);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }
    if (category == QString::fromLatin1(CustomCategory)) {
        return {};
    }
    return categories_.value(category);
}

QString ShapesBrowserWidget::categoryNameForShape(int shapeId, const ShapeGeometry &geometry) const
{
    const QString fontSection = fontSectionForShape(shapeId);
    if (!fontSection.isEmpty()) {
        return fontSection;
    }
    QString prefix = geometry.source.section(QLatin1Char('_'), 0, 0);
    if (prefix.isEmpty()) {
        prefix = QStringLiteral("Other");
    }
    const QString registryName = fh6::detail::shapeName(static_cast<quint16>(shapeId));
    const QString display = displayCategoryName(registryName);
    return display.isEmpty() || display.startsWith(QStringLiteral("0x")) ? prefix : display;
}

QString ShapesBrowserWidget::nameForShape(int shapeId, const ShapeGeometry &geometry) const
{
    const QString visionName = names_.name(shapeId);
    if (!visionName.isEmpty()) {
        return visionName;
    }
    const QString registryName = fh6::detail::shapeName(static_cast<quint16>(shapeId));
    if (!registryName.startsWith(QStringLiteral("0x"))) {
        return registryName;
    }
    return geometry.source;
}

QSet<int> ShapesBrowserWidget::loadFavourites() const
{
    QSet<int> result;
    const QString value = settings().value(QString::fromLatin1(FavouritesSettingsKey), QString()).toString();
    for (const QString &part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int shapeId = part.toInt(&ok);
        if (ok) {
            result.insert(shapeId);
        }
    }
    return result;
}

void ShapesBrowserWidget::saveFavourites() const
{
    QVector<int> ids;
    ids.reserve(favourites_.size());
    for (int shapeId : favourites_) {
        ids.push_back(shapeId);
    }
    std::sort(ids.begin(), ids.end());
    QStringList parts;
    for (int shapeId : ids) {
        parts.push_back(QString::number(shapeId));
    }
    settings().setValue(QString::fromLatin1(FavouritesSettingsKey), parts.join(QLatin1Char(',')));
}

QVector<CustomShapeGroup> ShapesBrowserWidget::loadCustomGroups() const
{
    QVector<CustomShapeGroup> groups;
    const QByteArray bytes = settings().value(QString::fromLatin1(CustomGroupsSettingsKey)).toByteArray();
    if (bytes.isEmpty()) {
        return groups;
    }
    const QJsonDocument document = QJsonDocument::fromJson(bytes);
    if (!document.isArray()) {
        return groups;
    }
    for (const QJsonValue &value : document.array()) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        CustomShapeGroup group;
        group.id = object.value(QStringLiteral("id")).toString();
        group.name = object.value(QStringLiteral("name")).toString();
        group.clipboard = clipboardFromJson(object.value(QStringLiteral("clipboard")).toObject());
        if (!group.id.isEmpty() && !group.name.isEmpty() && !clipboardEmpty(group.clipboard)) {
            groups.push_back(group);
        }
    }
    return groups;
}

void ShapesBrowserWidget::saveCustomGroups() const
{
    QJsonArray array;
    for (const CustomShapeGroup &group : customGroups_) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), group.id);
        object.insert(QStringLiteral("name"), group.name);
        object.insert(QStringLiteral("clipboard"), clipboardToJson(group.clipboard));
        array.append(object);
    }
    settings().setValue(QString::fromLatin1(CustomGroupsSettingsKey), QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

} // namespace gui
