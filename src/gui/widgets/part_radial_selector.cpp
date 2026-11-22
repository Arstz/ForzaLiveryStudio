#include "part_radial_selector.h"

#include "theme_manager.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace gui {
namespace {

constexpr int kChoicesPerPage = 8;
constexpr int kRadialMargin = 8;
constexpr int kRadialShadeAlpha = 110;
constexpr qreal kMaximumRadius = 190.0;
constexpr qreal kInnerRadiusRatio = 0.34;
constexpr qreal kTileRadiusRatio = 0.68;
constexpr qreal kTileExtentRatio = 0.43;
constexpr qreal kFullTurn = std::numbers::pi_v<qreal> * 2.0;

struct PartChoiceName {
    QString brand;
    QString type;
};

QString stripPartName(QString text) {
    static const QRegularExpression kPartNames(
        QStringLiteral(R"(\b(?:(?:front|rear)\s+(?:bumper|wing|spoiler|rim|tire)|tire\s+compound|side\s+skirts?|(?:wide[\s-]?body|body)[\s-]*kit|wide[\s-]?body|body|bumper|hood|bonnet|wing|spoiler|skirts?)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kOuterSeparators(QStringLiteral(R"(^[\s:/–—-]+|[\s:/–—-]+$)"));

    text.remove(kPartNames);
    text.remove(kOuterSeparators);

    return text.simplified();
}

PartChoiceName parseChoiceName(QString name) {
    static const QRegularExpression kNoWing(
        QStringLiteral(R"(\b(?:remove\s+(?:(?:front|rear)\s+)?(?:wing|spoiler)|no\s+(?:wing|spoiler))\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kRemoval(QStringLiteral(R"(\b(?:remove|none)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kHorizon(QStringLiteral(R"(\bForza\s+Horizon\b(?:\s*\(?(\d+)\b\)?)?)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kMotorsport(QStringLiteral(R"(\bForza\s+Motorsport\b(?:\s*\(?(\d+)\b\)?)?)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kBrandSeparator(QStringLiteral(R"(^(.+?)\s+[-–—]\s+(.+)$)"));
    static const QRegularExpression kGamePrefix(QStringLiteral(R"(^((?:FH|FM)\d*|Forza)\b\s*(.*)$)"),
        QRegularExpression::CaseInsensitiveOption);

    if (name.contains(kNoWing)) return {QStringLiteral("No wing"), {}};
    if (name.contains(kRemoval)) return {QStringLiteral("None"), {}};
    name.replace(kHorizon, QStringLiteral("FH\\1"));
    name.replace(kMotorsport, QStringLiteral("FM\\1"));
    const auto separated = kBrandSeparator.match(name);
    if (separated.hasMatch()) {
        return {stripPartName(separated.captured(1)), stripPartName(separated.captured(2))};
    }
    const auto game = kGamePrefix.match(name);
    if (game.hasMatch()) {
        return {game.captured(1), stripPartName(game.captured(2))};
    }
    const QString brand = stripPartName(name);

    return {brand.isEmpty() ? QStringLiteral("Option") : brand, {}};
}

void compactChoiceNames(QVector<PartRadialChoice> &choices) {
    QVector<PartChoiceName> names;
    QHash<QString, int> brandCounts;
    QHash<QString, int> labelCounts;
    QHash<QString, int> labelIndexes;

    names.reserve(choices.size());
    for (const auto &choice : choices) {
        names.push_back(choice.preserveName ? PartChoiceName{choice.name, {}} : parseChoiceName(choice.name));
        ++brandCounts[names.back().brand.toCaseFolded()];
    }
    for (qsizetype index = 0; index < choices.size(); ++index) {
        const auto &name = names[index];
        choices[index].name = brandCounts.value(name.brand.toCaseFolded()) > 1 && !name.type.isEmpty()
            ? name.brand + QLatin1Char(' ') + name.type : name.brand;
        ++labelCounts[choices[index].name.toCaseFolded()];
    }
    for (auto &choice : choices) {
        const QString key = choice.name.toCaseFolded();
        if (labelCounts.value(key) > 1 && choice.name != QStringLiteral("No wing")
            && choice.name != QStringLiteral("None")) {
            choice.name += QStringLiteral(" %1").arg(++labelIndexes[key]);
        }
    }
}

QPointF radialDirection(qreal angle) {
    return QPointF(std::cos(angle), std::sin(angle));
}

} // namespace

PartRadialSelector::PartRadialSelector(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("previewPartRadialSelector"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) cancelSelection();
    });
    hide();
}

void PartRadialSelector::beginSelection(const QPoint &origin, const QString &title,
    QVector<PartRadialChoice> choices, int selectedOptionId, PreviewProvider previewProvider) {
    const QFontMetrics helpMetrics(uiSecondaryTextFont());
    const bool multiplePages = choices.size() > kChoicesPerPage;
    const qreal hintHeight = helpMetrics.height() + kUiMargin * 2;
    const bool counterBelow = origin.y() <= (height() - hintHeight) * 0.5;
    const qreal counterSpace = multiplePages ? helpMetrics.height() + kUiSpacing : 0;
    const QRectF available(kRadialMargin, kRadialMargin + (counterBelow ? 0 : counterSpace),
        std::max(1, width() - kRadialMargin * 2),
        std::max(1.0, height() - kRadialMargin * 2 - hintHeight - counterSpace));

    cancelSelection();
    if (choices.isEmpty()) return;
    compactChoiceNames(choices);
    choices_ = std::move(choices);
    previewProvider_ = std::move(previewProvider);
    previousFocus_ = QApplication::focusWidget();
    title_ = title;
    setAccessibleName(title);
    setAccessibleDescription(QStringLiteral("Drag to a part and release to apply. Escape cancels. Scroll changes page."));
    origin_ = origin;
    outerRadius_ = std::max(1.0, std::min(kMaximumRadius, std::min(available.width(), available.height()) * 0.5));
    center_ = QPointF(std::clamp<qreal>(origin.x(), available.left() + outerRadius_,
        std::max(available.left() + outerRadius_, available.right() - outerRadius_)),
        std::clamp<qreal>(origin.y(), available.top() + outerRadius_,
        std::max(available.top() + outerRadius_, available.bottom() - outerRadius_)));
    pageCounterBounds_ = multiplePages
        ? QRectF(center_.x() - outerRadius_, counterBelow ? center_.y() + outerRadius_ + kUiSpacing
            : center_.y() - outerRadius_ - kUiSpacing - helpMetrics.height(), outerRadius_ * 2, helpMetrics.height())
        : QRectF();
    selectedOptionId_ = selectedOptionId;
    hoveredChoice_ = -1;
    page_ = 0;
    keyboardSelection_ = false;
    for (qsizetype index = 0; index < choices_.size(); ++index) {
        if (choices_[index].optionId == selectedOptionId) {
            page_ = static_cast<int>(index) / kChoicesPerPage;
            break;
        }
    }
    preparePage();
    selecting_ = true;
    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    grabMouse(Qt::ArrowCursor);
    update();
}

void PartRadialSelector::setChoicePreview(int optionId, const QImage &preview) {
    const auto choice = std::find_if(choices_.begin(), choices_.end(), [optionId](const PartRadialChoice &item) {
        return item.optionId == optionId;
    });

    if (choice == choices_.end()) return;
    choice->preview = preview;
    choice->previewReady = true;
    const auto index = choice - choices_.begin();
    if (index / kChoicesPerPage == page_) update();
}

void PartRadialSelector::cancelSelection() {
    finishSelection(false);
}

bool PartRadialSelector::isSelecting() const {
    return selecting_;
}

bool PartRadialSelector::event(QEvent *event) {
    if (selecting_ && (event->type() == QEvent::WindowDeactivate
        || event->type() == QEvent::UngrabMouse || event->type() == QEvent::FocusOut)) {
        cancelSelection();
    }

    return QWidget::event(event);
}

void PartRadialSelector::paintEvent(QPaintEvent *) {
    if (choices_.isEmpty()) return;
    const UiColors colors = uiColors(currentUiTheme());
    const QRectF outer(center_.x() - outerRadius_, center_.y() - outerRadius_,
        outerRadius_ * 2.0, outerRadius_ * 2.0);
    QString name = title_;
    QColor shade = colors.main;
    QRectF label;
    QPainter painter(this);
    const int count = pageChoiceCount();
    const qreal innerRadius = outerRadius_ * kInnerRadiusRatio;
    const qreal tileExtent = outerRadius_ * kTileExtentRatio;
    const qreal sectorAngle = kFullTurn / count;

    if (hoveredChoice_ >= 0) {
        const auto &choice = choices_[hoveredChoice_];
        name = choice.fullName.isEmpty() ? choice.name : choice.fullName;
    }
    shade.setAlpha(kRadialShadeAlpha);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), shade);
    for (int index = 0; index < count; ++index) {
        const PartRadialChoice &choice = choices_[page_ * kChoicesPerPage + index];
        const QPointF direction = radialDirection(-kFullTurn * 0.25 + index * sectorAngle);
        const QPointF tileCenter = center_ + direction * (outerRadius_ * kTileRadiusRatio);
        const QRectF tile(tileCenter.x() - tileExtent * 0.5, tileCenter.y() - tileExtent * 0.5,
            tileExtent, tileExtent);
        const QRectF caption(tile.left() - kUiSpacing, tile.bottom() + 2,
            tile.width() + kUiSpacing * 2, fontMetrics().height());
        QPainterPath sector;
        QPainterPath clip;
        const int choiceIndex = page_ * kChoicesPerPage + index;
        const bool hovered = choiceIndex == hoveredChoice_;
        const qreal startDegrees = 90.0 - (index - 0.5) * 360.0 / count;

        sector.moveTo(center_);
        sector.arcTo(outer, startDegrees, -360.0 / count);
        sector.closeSubpath();
        painter.setPen(QPen(hovered ? colors.accent : colors.border, hovered ? 2.0 : 1.0));
        painter.setBrush(hovered ? colors.selection : colors.main);
        painter.drawPath(sector);
        painter.setPen(QPen(choice.optionId == selectedOptionId_ ? colors.accent : colors.border, 2.0));
        painter.setBrush(colors.secondary);
        painter.drawEllipse(tile);
        clip.addEllipse(tile.adjusted(2, 2, -2, -2));
        painter.save();
        painter.setClipPath(clip);
        if (!choice.preview.isNull()) {
            painter.drawImage(tile.adjusted(3, 3, -3, -3), choice.preview);
        } else {
            painter.setPen(colors.mutedText);
            painter.drawText(tile.adjusted(4, 4, -4, -4), Qt::AlignCenter | Qt::TextWordWrap,
                choice.empty ? QStringLiteral("None") : previewProvider_ && !choice.previewReady
                    ? QStringLiteral("Loading…") : QStringLiteral("No preview"));
        }
        painter.restore();
        painter.setPen(colors.text);
        painter.drawText(caption, Qt::AlignCenter,
            fontMetrics().elidedText(choice.name, Qt::ElideRight, qRound(caption.width())));
    }
    painter.setPen(QPen(colors.border, 1));
    painter.setBrush(colors.main);
    painter.drawEllipse(center_, innerRadius, innerRadius);
    painter.setPen(colors.text);
    label = QRectF(center_.x() - innerRadius + kUiSpacing,
        center_.y() - innerRadius, (innerRadius - kUiSpacing) * 2, innerRadius * 2);
    painter.drawText(label, Qt::AlignCenter | Qt::TextWordWrap, name);
    painter.setFont(uiSecondaryTextFont());
    painter.setPen(colors.mutedText);
    if (!pageCounterBounds_.isEmpty()) {
        painter.drawText(pageCounterBounds_, Qt::AlignCenter,
            QStringLiteral("%1/%2").arg(page_ + 1).arg((choices_.size() + kChoicesPerPage - 1) / kChoicesPerPage));
    }
    painter.drawText(QRectF(kUiMargin, height() - fontMetrics().height() - kUiMargin,
        width() - kUiMargin * 2, fontMetrics().height()), Qt::AlignCenter,
        choices_.size() > kChoicesPerPage
            ? QStringLiteral("Release to apply · Center / Esc to cancel · Scroll to browse")
            : QStringLiteral("Release to apply · Center / Esc to cancel"));
}

void PartRadialSelector::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::RightButton) cancelSelection();
    event->accept();
}

void PartRadialSelector::mouseMoveEvent(QMouseEvent *event) {
    if (!selecting_) return;
    keyboardSelection_ = false;
    updateHoveredChoice(event->position());
    event->accept();
}

void PartRadialSelector::mouseReleaseEvent(QMouseEvent *event) {
    if (!selecting_) return;
    if (event->button() == Qt::LeftButton) {
        if (!keyboardSelection_) updateHoveredChoice(event->position());
        finishSelection(true);
    }
    event->accept();
}

void PartRadialSelector::wheelEvent(QWheelEvent *event) {
    if (!selecting_) return;
    if (event->angleDelta().y() != 0) {
        changePage(event->angleDelta().y() < 0 ? 1 : -1);
        updateHoveredChoice(event->position());
    }
    event->accept();
}

void PartRadialSelector::keyPressEvent(QKeyEvent *event) {
    if (!selecting_) return;
    switch (event->key()) {
    case Qt::Key_Escape:
        cancelSelection();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        finishSelection(true);
        break;
    case Qt::Key_PageDown:
    case Qt::Key_PageUp:
        changePage(event->key() == Qt::Key_PageDown ? 1 : -1);
        break;
    case Qt::Key_Right:
    case Qt::Key_Left: {
        const int first = page_ * kChoicesPerPage;
        const int count = pageChoiceCount();
        const int direction = event->key() == Qt::Key_Right ? 1 : -1;
        const int index = hoveredChoice_ < first ? (direction > 0 ? -1 : 0) : hoveredChoice_ - first;

        hoveredChoice_ = first + (index + direction + count) % count;
        keyboardSelection_ = true;
        update();
        break;
    }
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void PartRadialSelector::hideEvent(QHideEvent *event) {
    cancelSelection();
    QWidget::hideEvent(event);
}

void PartRadialSelector::preparePage() {
    for (int index = 0; index < pageChoiceCount(); ++index) {
        PartRadialChoice &choice = choices_[page_ * kChoicesPerPage + index];
        if (!choice.empty && !choice.previewReady && choice.preview.isNull() && previewProvider_) {
            choice.preview = previewProvider_(choice.optionId);
        }
    }
}

void PartRadialSelector::changePage(int direction) {
    const int pageCount = (static_cast<int>(choices_.size()) + kChoicesPerPage - 1) / kChoicesPerPage;

    if (pageCount < 2) return;
    page_ = (page_ + direction + pageCount) % pageCount;
    hoveredChoice_ = -1;
    preparePage();
    update();
}

void PartRadialSelector::updateHoveredChoice(const QPointF &position) {
    const QPointF offset = position - center_;
    int hovered = -1;
    const bool dragged = (position - origin_).manhattanLength() >= QApplication::startDragDistance();
    const qreal distance = std::hypot(offset.x(), offset.y());
    const qreal sectorAngle = kFullTurn / pageChoiceCount();

    if (dragged && rect().contains(position.toPoint())
        && distance >= outerRadius_ * kInnerRadiusRatio && distance <= outerRadius_) {
        const qreal angle = std::atan2(offset.y(), offset.x()) + kFullTurn * 0.25 + sectorAngle * 0.5;
        const qreal normalized = std::fmod(angle + kFullTurn, kFullTurn);
        hovered = page_ * kChoicesPerPage + static_cast<int>(normalized / sectorAngle);
    }
    if (hoveredChoice_ == hovered) return;
    hoveredChoice_ = hovered;
    update();
}

void PartRadialSelector::finishSelection(bool apply) {
    if (!selecting_) return;
    const int optionId = apply && hoveredChoice_ >= 0 ? choices_[hoveredChoice_].optionId : -1;

    selecting_ = false;
    if (mouseGrabber() == this) releaseMouse();
    hide();
    previewProvider_ = {};
    choices_.clear();
    if (previousFocus_ && previousFocus_->isVisible()) previousFocus_->setFocus(Qt::OtherFocusReason);
    if (optionId >= 0) Q_EMIT optionSelected(optionId);
}

int PartRadialSelector::pageChoiceCount() const {
    return std::min(kChoicesPerPage, static_cast<int>(choices_.size()) - page_ * kChoicesPerPage);
}

} // namespace gui
