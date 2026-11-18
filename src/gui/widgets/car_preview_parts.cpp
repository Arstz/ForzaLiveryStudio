#include "car_preview_widget.h"

#include "car_part_selection.h"
#include "car_preview_wheels.h"
#include "gui_assets.h"
#include "part_radial_selector.h"
#include "theme_manager.h"

#include <QAbstractButton>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QOpenGLFramebufferObject>
#include <QOpenGLExtraFunctions>
#include <QPainter>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gui {
namespace {

constexpr int kPartHotspotExtent = 34;
constexpr int kPartHotspotMargin = 4;
constexpr float kPartAnchorOffset = 0.001f;
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

QString partTypeName(int partType, const WheelPreviewState *wheels) {
    switch (partType) {
    case fls::car_part_types::CarBody: return QStringLiteral("Body kit");
    case fls::car_part_types::RearWing: return QStringLiteral("Rear wing / spoiler");
    case fls::car_part_types::FrontBumper: return QStringLiteral("Front bumper");
    case fls::car_part_types::RearBumper: return QStringLiteral("Rear bumper");
    case fls::car_part_types::Hood: return QStringLiteral("Hood");
    case fls::car_part_types::SideSkirts: return QStringLiteral("Side skirts");
    case kFrontRimSelector:
        return wheels != nullptr && !wheels->independentRimStyles ? QStringLiteral("Rims") : QStringLiteral("Front rims");
    case kRearRimSelector: return QStringLiteral("Rear rims");
    case kTireSelector: return QStringLiteral("Tires");
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

QString partOptionName(const QString &mediaName, const fls::CarPartOption &option, int index,
                       const WheelPreviewState *wheels) {
    if (wheels != nullptr && isWheelSelector(option.partType)) {
        const auto &options = isRimSelector(option.partType) ? wheels->rims
            : wheels->tires;
        for (const auto &style : options) {
            if (style.id == option.id) {
                return !isRimSelector(option.partType) && option.stock ? QStringLiteral("Stock") : style.name;
            }
        }
    }
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

QVector3D partAnchorTarget(const fls::ModelBounds &bounds, int partType) {
    const auto &minimum = bounds.minimum;
    const auto &maximum = bounds.maximum;
    QVector3D anchor((minimum.x + maximum.x) * 0.5f,
                     (minimum.y + maximum.y) * 0.5f,
                     (minimum.z + maximum.z) * 0.5f);

    switch (partType) {
    case kFrontRimSelector:
    case kRearRimSelector:
    case kTireSelector:
        anchor.setX(minimum.x);
        if (partType == kTireSelector) anchor.setZ(maximum.z);
        break;
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
        anchor.setX(minimum.x);
        break;
    }
    anchor.setX(-anchor.x());

    return anchor;
}

struct AnchorMesh {
    const fls::CarMesh *mesh;
    fls::ModelBounds bounds;
};

std::optional<QVector3D> nearestSurfaceAnchor(const QVector<const AnchorMesh *> &surfaces, const QVector3D &target) {
    std::optional<QVector3D> anchor;
    float nearestDistance = std::numeric_limits<float>::max();
    for (const AnchorMesh *surface : surfaces) {
        const auto &mesh = *surface->mesh;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            // Triangle interiors provide stable depth samples along silhouette edges.
            const auto &a = mesh.positions[mesh.indices[i]];
            const auto &b = mesh.positions[mesh.indices[i + 1]];
            const auto &c = mesh.positions[mesh.indices[i + 2]];
            const auto p = mesh.boneTransform.transformPoint(
                {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3});
            const QVector3D point(-p.x, p.y, p.z);
            const float distance = (point - target).lengthSquared();
            if (distance < nearestDistance) {
                anchor = point;
                nearestDistance = distance;
            }
        }
    }

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
        update();
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
        if (isWheelSelector(partType) && availablePartOptions(partType).isEmpty()) continue;
        PartHotspot hotspot;
        hotspot.partType = partType;
        hotspot.button = new PartHotspotButton(this);
        hotspot.button->setObjectName(QStringLiteral("previewPart.%1").arg(partType));
        hotspot.button->setAccessibleName(partTypeName(partType, wheelPreview_.get()));
        hotspot.button->setProperty("carPartType", partType);
        const auto collectBounds = [&hotspot, partType](const std::vector<fls::CarMesh> &meshes) {
            for (const fls::CarMesh &mesh : meshes) {
                if (mesh.carPartType != partType || mesh.positions.empty()) continue;
                fls::ModelBounds bounds;
                bounds.include(mesh);
                if (partType == kTireSelector && bounds.minimum.z + bounds.maximum.z < 0.0f) continue;
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
    if (isWheelSelector(partType)) return fls::carPartOptions(model_, partType);
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
    partAnchorsDirty_ = true;
    const auto *body = findPartOption(model_, fls::car_part_types::CarBody,
        selectedPartOptions_.value(fls::car_part_types::CarBody, -1));
    const bool fullKit = body != nullptr && !body->stock;
    bool hasChoices = false;

    for (PartHotspot &hotspot : partHotspots_) {
        hotspot.visibleCenter.reset();
        QString currentName;
        const int selected = selectedPartOptions_.value(hotspot.partType, -1);

        hotspot.options = availablePartOptions(hotspot.partType);
        hasChoices = hasChoices || hotspot.options.size() > 1;
        for (qsizetype index = 0; index < hotspot.options.size(); ++index) {
            if (hotspot.options[index]->id == selected) {
                currentName = partOptionName(model_.mediaName, *hotspot.options[index], static_cast<int>(index), wheelPreview_.get());
                break;
            }
        }
        const bool replacedByKit = fullKit && fls::bodyKitReplacesPart(hotspot.partType);
        if (replacedByKit) {
            currentName = QStringLiteral("Full body kit active");
        }
        hotspot.button->setToolTip(QStringLiteral("%1\n%2\n%3")
            .arg(partTypeName(hotspot.partType, wheelPreview_.get()), currentName,
                replacedByKit
                    ? QStringLiteral("Hold to choose an individual part and leave full-kit mode")
                    : QStringLiteral("Hold, drag to a part, and release to apply")));
        hotspot.button->setAccessibleDescription(currentName);
    }
    partsToggle_->setToolTip(hasChoices
        ? QStringLiteral("Show or hide exterior part hotspots")
        : QStringLiteral("This car has no selectable exterior part variants"));
    layoutPartControls();
}

void CarPreviewWidget::rebuildPartAnchors() {
    QVector<AnchorMesh> active;
    const auto collect = [&](const std::vector<fls::CarMesh> &meshes) {
        for (const auto &mesh : meshes) {
            if (mesh.indices.size() < 3 || !isVisibleCarMesh(mesh, selectedPartOptions_)) continue;
            AnchorMesh surface{&mesh, {}};
            surface.bounds.include(mesh);
            active.push_back(surface);
        }
    };
    collect(model_.meshesForLod(selectedLodIndex_));
    collect(model_.variantMeshesForLod(selectedLodIndex_));
    QVector<const AnchorMesh *> mountingSurfaces;
    for (const auto &surface : active) {
        if (!isWheelSelector(surface.mesh->carPartType)) mountingSurfaces.push_back(&surface);
    }
    for (auto &hotspot : partHotspots_) {
        hotspot.surfaceAnchor.reset();
        if (hotspot.options.size() < 2) continue;
        QVector<const AnchorMesh *> surfaces;
        fls::ModelBounds bounds;
        for (const auto &surface : active) {
            if (surface.mesh->carPartType != hotspot.partType) continue;
            if (hotspot.partType == kTireSelector && surface.bounds.minimum.z + surface.bounds.maximum.z < 0.0f) continue;
            surfaces.push_back(&surface);
            bounds.include(surface.bounds.minimum);
            bounds.include(surface.bounds.maximum);
        }
        if (surfaces.isEmpty()) {
            if (mountingSurfaces.isEmpty()) continue;
            surfaces = mountingSurfaces;
            bounds = hotspot.optionBounds.value(kPartBoundsFallback);
        }
        hotspot.surfaceAnchor = nearestSurfaceAnchor(surfaces, partAnchorTarget(bounds, hotspot.partType));
    }
    partAnchorsDirty_ = false;
    partDepthViewProjection_.reset();
}

void CarPreviewWidget::initializePartDepthTest() {
    constexpr char kVertexShader[] = R"(#version 330 core
uniform vec3 anchor;
void main() {
    gl_Position = vec4(anchor, 1);
    gl_PointSize = 1.0;
}
)";
    constexpr char kFragmentShader[] = R"(#version 330 core
out vec4 color;
void main() {
    color = vec4(0);
}
)";
    if (!partDepthProgram_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
        || !partDepthProgram_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)
        || !partDepthProgram_.link() || !partDepthVao_.create()) {
        qWarning().noquote() << "Part selector depth test initialization failed" << partDepthProgram_.log();
    }
}

void CarPreviewWidget::updatePartVisibility() {
    if (transparentBackground_ || !partDepthVao_.isCreated() || !partsToggle_->isChecked() || partHotspots_.isEmpty()) return;
    if (partAnchorsDirty_) rebuildPartAnchors();
    const QMatrix4x4 view = cameraView();
    const QMatrix4x4 viewProjection = cameraProjection() * view;
    const QVector3D eye = view.inverted().map(QVector3D());
    const QSize viewport(previewViewportWidth(), height());
    const QSize pixels = viewport * devicePixelRatioF();
    if (partDepthViewProjection_ == viewProjection && partDepthViewport_ == pixels) return;
    partDepthViewProjection_ = viewProjection;
    partDepthViewport_ = pixels;

    struct Sample {
        PartHotspot *hotspot;
        QVector3D projected;
    };
    QVector<Sample> samples;
    for (auto &hotspot : partHotspots_) {
        hotspot.visibleCenter.reset();
        if (!hotspot.surfaceAnchor) continue;
        const QVector3D &anchor = *hotspot.surfaceAnchor;
        const QVector4D clip = viewProjection * QVector4D(anchor, 1);
        if (clip.w() <= 0 || std::abs(clip.z()) >= clip.w()
            || std::abs(clip.x()) >= clip.w() || std::abs(clip.y()) >= clip.w()) continue;
        const QVector3D offset = (eye - anchor).normalized() * (modelRadius_ * kPartAnchorOffset);
        const QVector4D depthClip = viewProjection * QVector4D(anchor + offset, 1);
        QVector3D projected = clip.toVector3DAffine();
        projected.setZ(std::max(-1.0f, depthClip.z() / depthClip.w()));
        samples.push_back({&hotspot, projected});
    }
    if (samples.isEmpty()) return;
    auto *gl = context()->extraFunctions();
    QVector<GLuint> queries(samples.size());
    gl->glGenQueries(static_cast<GLsizei>(queries.size()), queries.data());
    partDepthProgram_.bind();
    partDepthVao_.bind();
    gl->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    gl->glDepthMask(GL_FALSE);
    const int location = partDepthProgram_.uniformLocation("anchor");
    for (qsizetype i = 0; i < samples.size(); ++i) {
        partDepthProgram_.setUniformValue(location, samples[i].projected);
        gl->glBeginQuery(GL_ANY_SAMPLES_PASSED, queries[i]);
        gl->glDrawArrays(GL_POINTS, 0, 1);
        gl->glEndQuery(GL_ANY_SAMPLES_PASSED);
    }
    gl->glDepthMask(GL_TRUE);
    gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    partDepthVao_.release();
    partDepthProgram_.release();
    for (qsizetype i = 0; i < samples.size(); ++i) {
        GLuint visible = GL_FALSE;
        gl->glGetQueryObjectuiv(queries[i], GL_QUERY_RESULT, &visible);
        if (!visible) continue;
        const auto &p = samples[i].projected;
        samples[i].hotspot->visibleCenter = QPoint(qRound((1 + p.x()) * viewport.width() * 0.5f),
            qRound((1 - p.y()) * viewport.height() * 0.5f));
    }
    gl->glDeleteQueries(static_cast<GLsizei>(queries.size()), queries.constData());
}

void CarPreviewWidget::layoutPartControls() {
    if (partsToggle_ == nullptr) return;
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
        if (!ready || !partsToggle_->isChecked() || !selectable || !hotspot.visibleCenter) {
            hotspot.button->hide();
            continue;
        }
        QRect placement(QPoint(), hotspot.button->size());
        placement.moveCenter(*hotspot.visibleCenter);
        hotspot.button->move(placement.topLeft());
        hotspot.button->setVisible(viewport.contains(placement));
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
        choices.push_back({partOptionName(model_.mediaName, *options[index], static_cast<int>(index), wheelPreview_.get()),
            {}, options[index]->id, partType != fls::car_part_types::CarBody && options[index]->modelPaths.isEmpty(),
            isRimSelector(partType)});
    }
    partSelectorType_ = partType;
    anchor->setDown(false);
    partSelector_->beginSelection(mapFromGlobal(QCursor::pos()), partTypeName(partType, wheelPreview_.get()),
        std::move(choices), selectedPartOptions_.value(partType, -1),
        [this, partType](int optionId) { return renderPartOptionPreview(partType, optionId); });
}

QImage CarPreviewWidget::renderPartOptionPreview(int partType, int optionId) {
    if (isWheelSelector(partType)) return renderWheelOptionPreview(partType, optionId);
    const auto *option = findPartOption(model_, partType, optionId);
    const auto hotspot = std::find_if(partHotspots_.cbegin(), partHotspots_.cend(),
        [partType](const PartHotspot &candidate) { return candidate.partType == partType; });
    const auto selections = fls::carPartSelectionsForOption(model_, selectedPartOptions_, individualPartOptions_, partType, optionId);
    const bool wholeCar = partType == fls::car_part_types::CarBody;

    if (option == nullptr || hotspot == partHotspots_.cend() || !context()
        || (!wholeCar && option->modelPaths.isEmpty())) return {};
    auto bounds = hotspot->optionBounds.value(optionId,
        hotspot->optionBounds.value(kPartBoundsFallback));
    if (wholeCar) {
        bounds.minimum = model_.boundsMin;
        bounds.maximum = model_.boundsMax;
    }

    return renderGeometryPreview(carRenderer_, bounds, selections, partType, wholeCar);
}

QImage CarPreviewWidget::renderGeometryPreview(CarModelRenderer &renderer, const fls::ModelBounds &bounds,
    const QHash<int, int> &selections, int partType, bool wholeCar) {
    const auto &minimum = bounds.minimum;
    const auto &maximum = bounds.maximum;
    const QVector3D center(-(minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f, (minimum.z + maximum.z) * 0.5f);
    const QVector3D extent(maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z);
    QVector3D direction(0.8f, 0.5f, 1.0f);
    QImage preview;
    QOpenGLFramebufferObjectFormat format;
    QMatrix4x4 view;
    QMatrix4x4 projection;
    const int debugMode = renderer.debugMode();
    const bool wireframe = renderer.wireframeVisible();
    const float radius = std::max(0.001f, extent.length() * 0.5f);

    if (partType == fls::car_part_types::RearBumper || partType == fls::car_part_types::RearWing) {
        direction.setZ(-1.0f);
    } else if (partType == fls::car_part_types::Hood) {
        direction.setY(1.2f);
    } else if (isWheelSelector(partType)) {
        direction = QVector3D(1.0f, 0.25f, 0.45f);
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
            renderer.setPartSelections(selections);
            renderer.setDebugMode(0);
            renderer.setWireframeVisible(false);
            renderer.render(view, projection, liveryTexture_, basePaint_,
                project_ != nullptr ? &project_->liveryPaint : nullptr,
                manufacturerColors_.colors.isEmpty() ? nullptr : &manufacturerColors_,
                paintFinishes_.loaded() ? &paintFinishes_ : nullptr, wholeCar ? -1 : partType);
            preview = framebuffer.toImage();
        }
        renderer.setPartSelections(selectedPartOptions_);
        renderer.setDebugMode(debugMode);
        renderer.setWireframeVisible(wireframe);
        functions->glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
        functions->glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    }
    doneCurrent();

    return preview;
}

void CarPreviewWidget::selectPartOption(int partType, int optionId) {
    if (isWheelSelector(partType)) {
        selectWheelOption(partType, optionId);
        return;
    }
    const auto options = availablePartOptions(partType);
    const auto selected = std::find_if(options.cbegin(), options.cend(),
        [optionId](const auto *option) { return option->id == optionId; });
    const auto selections = fls::carPartSelectionsForOption(model_, selectedPartOptions_, individualPartOptions_, partType, optionId);

    if (modelLoadPending_ || modelUploadPending_ || selected == options.cend()
        || selectedPartOptions_ == selections) return;
    selectedPartOptions_ = selections;
    if (partType != fls::car_part_types::CarBody || (*selected)->stock) {
        individualPartOptions_.insert(partType, optionId);
    }
    if (wheelPreview_ && normalizeWheelPreviewSizing(*wheelPreview_,
        selectedPartOptions_.value(fls::car_part_types::CarBody, -1))) {
        applyWheelGeometry();
    }
    carRenderer_.setPartSelections(selectedPartOptions_);
    switchCarUnwrapOverlay();
    rebuildColorsPanel();
    syncPartControls();
    Q_EMIT unwrapOverlayChanged();
    update();
}

} // namespace gui
