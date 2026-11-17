#include "car_preview_widget.h"

#include "car_part_selection.h"
#include "gui_assets.h"
#include "part_radial_selector.h"
#include "theme_manager.h"

#include <QAbstractButton>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QToolButton>

#include <algorithm>
#include <cmath>

namespace gui {
namespace {

constexpr int kPartHotspotExtent = 34;
constexpr int kPartHotspotMargin = 4;
constexpr int kPartHotspotPlacementAttempts = 12;
constexpr int kPartHotspotOpacity = 225;
constexpr qreal kPartHotspotRingInset = 3.0;
constexpr qreal kPartHotspotHaloWidth = 4.0;
constexpr qreal kPartHotspotRingWidth = 1.5;
constexpr qreal kPartHotspotGlyphWidth = 1.8;
constexpr qreal kPartHotspotGlyphRadius = 4.0;
constexpr int kPartBoundsFallback = -1;
constexpr int kPartPreviewExtent = 192;
constexpr float kPartPreviewFieldOfView = 35.0f;
constexpr float kPartPreviewDistanceRatio = 4.0f;

class PartHotspotButton final : public QAbstractButton {
public:
    explicit PartHotspotButton(QWidget *parent) : QAbstractButton(parent) {
        setFixedSize(kPartHotspotExtent, kPartHotspotExtent);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
    }

protected:
    bool hitButton(const QPoint &position) const override {
        const QPointF offset = QPointF(position) - QRectF(rect()).center();
        const qreal radius = width() * 0.5;

        return QPointF::dotProduct(offset, offset) <= radius * radius;
    }

    void enterEvent(QEnterEvent *event) override {
        QAbstractButton::enterEvent(event);
        update();
    }

    void leaveEvent(QEvent *event) override {
        QAbstractButton::leaveEvent(event);
        update();
    }

    void paintEvent(QPaintEvent *) override {
        const UiColors colors = uiColors(currentUiTheme());
        const QRectF ring = QRectF(rect()).adjusted(kPartHotspotRingInset,
            kPartHotspotRingInset, -kPartHotspotRingInset, -kPartHotspotRingInset);
        const QPointF center = ring.center();
        QColor background;
        QPainter painter(this);
        const bool highlighted = underMouse() || hasFocus() || isDown();

        background = highlighted ? colors.selection : colors.main;
        background.setAlpha(kPartHotspotOpacity);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(background);
        painter.setPen(QPen(colors.main, kPartHotspotHaloWidth));
        painter.drawEllipse(ring);
        painter.setPen(QPen(highlighted ? colors.accent : colors.text, kPartHotspotRingWidth));
        painter.drawEllipse(ring);
        painter.setPen(QPen(highlighted ? colors.accent : colors.text,
            kPartHotspotGlyphWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center + QPointF(-kPartHotspotGlyphRadius, 0),
            center + QPointF(kPartHotspotGlyphRadius, 0));
        painter.drawLine(center + QPointF(0, -kPartHotspotGlyphRadius),
            center + QPointF(0, kPartHotspotGlyphRadius));
    }
};

QString partTypeName(int partType) {
    switch (partType) {
    case fls::car_part_types::CarBody: return QStringLiteral("Body kit");
    case fls::car_part_types::RearWing: return QStringLiteral("Rear wing / spoiler");
    case fls::car_part_types::FrontBumper: return QStringLiteral("Front bumper");
    case fls::car_part_types::RearBumper: return QStringLiteral("Rear bumper");
    case fls::car_part_types::Hood: return QStringLiteral("Hood");
    case fls::car_part_types::SideSkirts: return QStringLiteral("Side skirts");
    default: return QStringLiteral("Part");
    }
}

const fls::CarPartOption *findPartOption(const fls::CarModel &model, int partType, int optionId) {
    const auto option = std::find_if(model.partOptions.begin(), model.partOptions.end(),
        [partType, optionId](const fls::CarPartOption &candidate) {
            return candidate.partType == partType && candidate.id == optionId;
        });

    return option == model.partOptions.end() ? nullptr : &*option;
}

QString partModelKey(const fls::CarPartOption &option) {
    QStringList paths = option.modelPaths;
    for (QString &path : paths) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        path = path.toLower();
    }
    paths.removeDuplicates();
    paths.sort();

    return paths.join(QLatin1Char('|'));
}

QString partOptionName(const QString &mediaName, const fls::CarPartOption &option, int index) {
    static const QJsonObject kPartNames = [] {
        QFile file(assetPath(QStringLiteral("cars/part_names.json")));
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};

        const QJsonObject source = QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject names;
        for (auto model = source.constBegin(); model != source.constEnd(); ++model) {
            names.insert(model.key().toLower(), model.value());
        }

        return names;
    }();
    const QJsonObject names = kPartNames.value(mediaName.toLower()).toObject();
    const QString catalogName = names.value(
        QStringLiteral("%1:%2").arg(option.partType).arg(option.id)).toString();

    if (option.partType == fls::car_part_types::RearWing && option.modelPaths.isEmpty()) {
        return QStringLiteral("No wing");
    }
    if (!catalogName.isEmpty()) return catalogName;
    if (option.stock) return QStringLiteral("Stock");
    if (option.modelPaths.isEmpty() && option.partType != fls::car_part_types::CarBody) {
        return QStringLiteral("None");
    }

    return QStringLiteral("Option %1").arg(index + 1);
}

QVector3D partAnchor(const fls::ModelBounds &bounds, int partType, const QVector3D &eye) {
    const auto &minimum = bounds.minimum;
    const auto &maximum = bounds.maximum;
    QVector3D anchor((minimum.x + maximum.x) * 0.5f,
                     (minimum.y + maximum.y) * 0.5f,
                     (minimum.z + maximum.z) * 0.5f);

    switch (partType) {
    case fls::car_part_types::CarBody:
    case fls::car_part_types::Hood:
    case fls::car_part_types::RearWing:
        anchor.setY(maximum.y);
        break;
    case fls::car_part_types::FrontBumper:
        anchor.setZ(maximum.z);
        break;
    case fls::car_part_types::RearBumper:
        anchor.setZ(minimum.z);
        break;
    case fls::car_part_types::SideSkirts:
        anchor.setX(eye.x() >= 0.0f ? minimum.x : maximum.x);
        break;
    }
    anchor.setX(-anchor.x());

    return anchor;
}

} // namespace

void CarPreviewWidget::initializePartControls() {
    partSelector_ = new PartRadialSelector(this);
    connect(partSelector_, &PartRadialSelector::optionSelected, this,
        [this](int optionId) { selectPartOption(partSelectorType_, optionId); });
    partsToggle_ = new QToolButton(this);
    partsToggle_->setObjectName(QStringLiteral("previewPartsToggle"));
    partsToggle_->setText(QStringLiteral("Parts"));
    partsToggle_->setAccessibleName(QStringLiteral("Show part hotspots"));
    partsToggle_->setCheckable(true);
    partsToggle_->setChecked(true);
    partsToggle_->setFixedHeight(kUiControlHeight);
    connect(partsToggle_, &QToolButton::toggled, this, [this]() {
        partSelector_->cancelSelection();
        layoutPartControls();
    });
}

void CarPreviewWidget::rebuildPartControls() {
    if (partsToggle_ == nullptr) return;
    partSelector_->cancelSelection();
    for (const PartHotspot &hotspot : partHotspots_) {
        hotspot.button->hide();
        hotspot.button->deleteLater();
    }
    partHotspots_.clear();
    auto partTypes = selectedPartOptions_.keys();
    std::sort(partTypes.begin(), partTypes.end());
    for (int partType : partTypes) {
        PartHotspot hotspot;
        hotspot.partType = partType;
        hotspot.button = new PartHotspotButton(this);
        hotspot.button->setObjectName(QStringLiteral("previewPart.%1").arg(partType));
        hotspot.button->setAccessibleName(partTypeName(partType));
        hotspot.button->setProperty("carPartType", partType);
        const auto collectBounds = [&hotspot, partType](const std::vector<fls::CarMesh> &meshes) {
            for (const fls::CarMesh &mesh : meshes) {
                if (mesh.carPartType != partType || mesh.positions.empty()) continue;
                fls::ModelBounds bounds;
                bounds.include(mesh);
                const auto includeBounds = [&hotspot, &bounds](int optionId) {
                    auto &target = hotspot.optionBounds[optionId];
                    target.include(bounds.minimum);
                    target.include(bounds.maximum);
                };
                includeBounds(kPartBoundsFallback);
                for (int optionId : mesh.partOptionIds) includeBounds(optionId);
            }
        };
        collectBounds(model_.meshes);
        collectBounds(model_.variantMeshes);
        if (!hotspot.optionBounds.contains(kPartBoundsFallback)) {
            fls::ModelBounds bounds;
            bounds.include(model_.boundsMin);
            bounds.include(model_.boundsMax);
            hotspot.optionBounds.insert(kPartBoundsFallback, bounds);
        }
        connect(hotspot.button, &QAbstractButton::pressed, this,
            [this, partType, anchor = hotspot.button]() { beginPartSelection(partType, anchor); });
        partHotspots_.push_back(std::move(hotspot));
    }
    syncPartControls();
}

QVector<const fls::CarPartOption *> CarPreviewWidget::availablePartOptions(int partType) const {
    const auto options = fls::individualCarPartOptions(model_, partType);
    QVector<const fls::CarPartOption *> distinctOptions;
    QHash<QString, qsizetype> modelIndexes;

    if (partType == fls::car_part_types::CarBody) return options;
    for (const auto *option : options) {
        const QString key = partModelKey(*option);
        const auto existing = modelIndexes.constFind(key);
        if (existing == modelIndexes.cend()) {
            modelIndexes.insert(key, distinctOptions.size());
            distinctOptions.push_back(option);
        } else if (option->id == selectedPartOptions_.value(partType, -1)) {
            distinctOptions[*existing] = option;
        }
    }

    return distinctOptions;
}

void CarPreviewWidget::syncPartControls() {
    const auto *body = findPartOption(model_, fls::car_part_types::CarBody,
        selectedPartOptions_.value(fls::car_part_types::CarBody, -1));
    const bool fullKit = body != nullptr && !body->stock;
    bool hasChoices = false;

    for (PartHotspot &hotspot : partHotspots_) {
        QString currentName;
        const int selected = selectedPartOptions_.value(hotspot.partType, -1);

        hotspot.options = availablePartOptions(hotspot.partType);
        hasChoices = hasChoices || hotspot.options.size() > 1;
        for (qsizetype index = 0; index < hotspot.options.size(); ++index) {
            if (hotspot.options[index]->id == selected) {
                currentName = partOptionName(model_.mediaName, *hotspot.options[index], static_cast<int>(index));
                break;
            }
        }
        if (fullKit && hotspot.partType != fls::car_part_types::CarBody) {
            currentName = QStringLiteral("Full body kit active");
        }
        hotspot.button->setToolTip(QStringLiteral("%1\n%2\n%3")
            .arg(partTypeName(hotspot.partType), currentName,
                fullKit && hotspot.partType != fls::car_part_types::CarBody
                    ? QStringLiteral("Hold to choose an individual part and leave full-kit mode")
                    : QStringLiteral("Hold, drag to a part, and release to apply")));
        hotspot.button->setAccessibleDescription(currentName);
    }
    partsToggle_->setToolTip(hasChoices
        ? QStringLiteral("Show or hide exterior part hotspots")
        : QStringLiteral("This car has no selectable exterior part variants"));
    layoutPartControls();
}

void CarPreviewWidget::layoutPartControls() {
    if (partsToggle_ == nullptr) return;
    const QMatrix4x4 view = cameraView();
    const QMatrix4x4 viewProjection = cameraProjection() * view;
    const QVector3D eye = view.inverted().map(QVector3D());
    QVector<QRect> occupied;
    QRect viewport;
    const int viewportWidth = previewViewportWidth();
    const int controlsTop = referenceNote_->geometry().bottom() + kUiSpacing;
    const bool ready = hasModel() && graphicsApiSuitable_
        && !modelLoadPending_ && !modelUploadPending_ && !transparentBackground_;
    bool hasChoices = false;

    viewport = QRect(kPartHotspotMargin, controlsTop + kUiControlHeight + kUiSpacing,
        std::max(0, viewportWidth - 2 * kPartHotspotMargin),
        std::max(0, height() - controlsTop - kUiControlHeight - kUiSpacing - kPartHotspotMargin));
    partsToggle_->resize(partsToggle_->sizeHint().width(), kUiControlHeight);
    partsToggle_->move(kUiMargin, controlsTop);
    for (const PartHotspot &hotspot : partHotspots_) {
        const bool selectable = hotspot.options.size() > 1;
        hasChoices = hasChoices || selectable;
        if (!ready || !partsToggle_->isChecked() || !selectable) {
            hotspot.button->hide();
            continue;
        }
        const auto bounds = hotspot.optionBounds.value(selectedPartOptions_.value(hotspot.partType, -1),
            hotspot.optionBounds.value(kPartBoundsFallback));
        const QVector4D clip = viewProjection * QVector4D(partAnchor(bounds, hotspot.partType, eye), 1.0f);
        if (clip.w() <= 0.0f || std::abs(clip.z()) > clip.w()) {
            hotspot.button->hide();
            continue;
        }
        const QVector3D projected = clip.toVector3DAffine();
        const QPoint center(qRound((projected.x() + 1.0f) * viewportWidth * 0.5f),
                            qRound((1.0f - projected.y()) * height() * 0.5f));
        const QPoint origin = center - QPoint(kPartHotspotExtent / 2, kPartHotspotExtent / 2);
        QRect placement(origin, hotspot.button->size());
        bool placed = false;
        for (int attempt = 0; attempt < kPartHotspotPlacementAttempts; ++attempt) {
            const int displacement = ((attempt + 1) / 2)
                * (kPartHotspotExtent + kPartHotspotMargin) * (attempt % 2 == 0 ? 1 : -1);
            placement.moveTop(origin.y() + displacement);
            const bool overlaps = std::any_of(occupied.cbegin(), occupied.cend(),
                [&placement](const QRect &other) {
                    return placement.adjusted(-kPartHotspotMargin, -kPartHotspotMargin,
                        kPartHotspotMargin, kPartHotspotMargin).intersects(other);
                });
            if (viewport.contains(placement) && !overlaps) {
                placed = true;
                break;
            }
        }
        hotspot.button->setVisible(placed);
        if (!placed) continue;
        occupied.push_back(placement);
        hotspot.button->move(placement.topLeft());
    }
    partsToggle_->setEnabled(ready && hasChoices);
    partsToggle_->setVisible(!transparentBackground_
        && viewportWidth >= partsToggle_->sizeHint().width() + 2 * kUiMargin);
    if (!ready || partSelector_->size() != QSize(viewportWidth, height())) {
        partSelector_->cancelSelection();
    }
    partSelector_->setGeometry(0, 0, viewportWidth, height());
}

void CarPreviewWidget::beginPartSelection(int partType, QAbstractButton *anchor) {
    const auto options = availablePartOptions(partType);
    QVector<PartRadialChoice> choices;

    if (options.size() < 2 || modelLoadPending_ || modelUploadPending_) return;
    for (qsizetype index = 0; index < options.size(); ++index) {
        choices.push_back({partOptionName(model_.mediaName, *options[index], static_cast<int>(index)),
            {}, options[index]->id, partType != fls::car_part_types::CarBody && options[index]->modelPaths.isEmpty()});
    }
    partSelectorType_ = partType;
    anchor->setDown(false);
    partSelector_->beginSelection(mapFromGlobal(QCursor::pos()), partTypeName(partType),
        std::move(choices), selectedPartOptions_.value(partType, -1),
        [this, partType](int optionId) { return renderPartOptionPreview(partType, optionId); });
}

QImage CarPreviewWidget::renderPartOptionPreview(int partType, int optionId) {
    const auto *option = findPartOption(model_, partType, optionId);
    const auto hotspot = std::find_if(partHotspots_.cbegin(), partHotspots_.cend(),
        [partType](const PartHotspot &candidate) { return candidate.partType == partType; });
    QImage preview;
    const auto selections = fls::carPartSelectionsForOption(model_, individualPartOptions_, partType, optionId);
    QOpenGLFramebufferObjectFormat format;
    QMatrix4x4 view;
    QMatrix4x4 projection;
    const bool wholeCar = partType == fls::car_part_types::CarBody;

    if (option == nullptr || hotspot == partHotspots_.cend() || !context()
        || (!wholeCar && option->modelPaths.isEmpty())) return preview;
    const auto bounds = hotspot->optionBounds.value(optionId,
        hotspot->optionBounds.value(kPartBoundsFallback));
    const auto &minimum = wholeCar ? model_.boundsMin : bounds.minimum;
    const auto &maximum = wholeCar ? model_.boundsMax : bounds.maximum;
    const QVector3D center(-(minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f, (minimum.z + maximum.z) * 0.5f);
    const QVector3D extent(maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z);
    QVector3D direction(0.8f, 0.5f, 1.0f);
    const int debugMode = carRenderer_.debugMode();
    const bool wireframe = carRenderer_.wireframeVisible();
    const float radius = std::max(0.001f, extent.length() * 0.5f);

    if (partType == fls::car_part_types::RearBumper || partType == fls::car_part_types::RearWing) {
        direction.setZ(-1.0f);
    } else if (partType == fls::car_part_types::Hood) {
        direction.setY(1.2f);
    }
    view.lookAt(center + direction.normalized() * (radius * kPartPreviewDistanceRatio),
        center, QVector3D(0, 1, 0));
    projection.perspective(kPartPreviewFieldOfView, 1.0f, radius * 0.01f, radius * 10.0f);
    format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
    makeCurrent();
    {
        auto *functions = context()->functions();
        GLint previousFramebuffer = 0;
        GLint previousViewport[4] = {};

        functions->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
        functions->glGetIntegerv(GL_VIEWPORT, previousViewport);
        QOpenGLFramebufferObject framebuffer(kPartPreviewExtent, kPartPreviewExtent, format);
        if (framebuffer.isValid() && framebuffer.bind()) {
            functions->glViewport(0, 0, kPartPreviewExtent, kPartPreviewExtent);
            functions->glClearColor(0, 0, 0, 0);
            functions->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            carRenderer_.setPartSelections(selections);
            carRenderer_.setDebugMode(0);
            carRenderer_.setWireframeVisible(false);
            carRenderer_.render(view, projection, liveryTexture_, basePaint_,
                project_ != nullptr ? &project_->liveryPaint : nullptr,
                manufacturerColors_.colors.isEmpty() ? nullptr : &manufacturerColors_,
                paintFinishes_.loaded() ? &paintFinishes_ : nullptr, wholeCar ? -1 : partType);
            preview = framebuffer.toImage();
        }
        carRenderer_.setPartSelections(selectedPartOptions_);
        carRenderer_.setDebugMode(debugMode);
        carRenderer_.setWireframeVisible(wireframe);
        functions->glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
        functions->glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    }
    doneCurrent();

    return preview;
}

void CarPreviewWidget::selectPartOption(int partType, int optionId) {
    const auto options = availablePartOptions(partType);
    const auto selected = std::find_if(options.cbegin(), options.cend(),
        [optionId](const auto *option) { return option->id == optionId; });
    const auto selections = fls::carPartSelectionsForOption(model_, individualPartOptions_, partType, optionId);

    if (modelLoadPending_ || modelUploadPending_ || selected == options.cend()
        || selectedPartOptions_ == selections) return;
    selectedPartOptions_ = selections;
    if (partType != fls::car_part_types::CarBody || (*selected)->stock) {
        individualPartOptions_ = selections;
    }
    carRenderer_.setPartSelections(selectedPartOptions_);
    switchCarUnwrapOverlay();
    rebuildColorsPanel();
    syncPartControls();
    Q_EMIT unwrapOverlayChanged();
    update();
}

} // namespace gui
