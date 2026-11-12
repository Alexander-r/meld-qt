#include "actiongutter.h"

#include "icons.h"
#include "sourceview.h"
#include "style.h"

#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionToolButton>

#include <algorithm>

namespace meld {

namespace {

//! The part of the gutter's width in which the band of an empty chunk widens
constexpr qreal EmptyChunkCurve = 0.3;

} // namespace

ActionGutter::ActionGutter(Qt::LayoutDirection iconDirection, QWidget *parent)
    : QWidget(parent)
    , iconDirection_(iconDirection)
{
    setMinimumWidth(20);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setMouseTracking(true);
}

void ActionGutter::setViews(SourceView *source, SourceView *target)
{
    source_ = source;
    target_ = target;
    for (SourceView *view : {source, target})
        connect(view, &SourceView::editableChanged, this, qOverload<>(&QWidget::update));
    update();
}

void ActionGutter::setChunks(std::vector<DiffChunk> chunks)
{
    chunks_ = std::move(chunks);
    chunkStarts_.clear();
    for (const DiffChunk &c : chunks_)
        chunkStarts_.push_back(c.start_a);
    pointerChunk_.reset();
    update();
}

void ActionGutter::setActionMode(ActionMode mode)
{
    if (mode == actionMode_)
        return;
    actionMode_ = mode;
    update();
}

QString ActionGutter::iconName(ActionMode mode) const
{
    switch (mode) {
    case ActionMode::Replace:
        return iconDirection_ == Qt::LeftToRight ? QStringLiteral("meld-change-apply-right")
                                                 : QStringLiteral("meld-change-apply-left");
    case ActionMode::Delete:
        return QStringLiteral("meld-change-delete");
    case ActionMode::Insert:
        return QStringLiteral("meld-change-copy");
    }
    return {};
}

std::optional<ActionMode> ActionGutter::classifyChangeActions(const DiffChunk &change, ActionMode mode,
                                                              bool sourceEditable, bool targetEditable)
{
    if (!sourceEditable && !targetEditable)
        return std::nullopt;

    // Reclassify conflict changes, since we treat them the same as a normal
    // two-way change as far as actions are concerned
    ChunkTag changeType = change.tag;
    if (changeType == ChunkTag::Conflict) {
        if (change.start_a == change.end_a)
            changeType = ChunkTag::Insert;
        else if (change.start_b == change.end_b)
            changeType = ChunkTag::Delete;
        else
            changeType = ChunkTag::Replace;
    }

    // This side has no text here, so there's nothing to delete or copy, but
    // the other side can be made to match it, which removes that side's text.
    // This isn't in the original, which has no action for these changes.
    if (changeType == ChunkTag::Insert) {
        if (mode == ActionMode::Replace && targetEditable)
            return ActionMode::Replace;
        return std::nullopt;
    }

    std::optional<ActionMode> action = mode;
    if (mode == ActionMode::Delete && !sourceEditable)
        action.reset();
    else if (mode == ActionMode::Insert && changeType == ChunkTag::Delete)
        action = ActionMode::Replace;
    if (!targetEditable)
        action = ActionMode::Delete;
    return action;
}

std::pair<qreal, qreal> ActionGutter::buttonExtent(const SourceView *view, int line)
{
    // A chunk after the last line has no line below it for its button, which
    // goes on the last line instead
    const qreal y = view->yForLine(line);
    const bool afterLastLine = line >= view->document()->blockCount();
    const qreal lineHeight = afterLastLine ? view->lineHeight() : view->yForLine(line + 1) - y;
    const qreal height = std::max(4.0, std::min(lineHeight, view->lineHeight()) - 2);
    return {afterLastLine ? y - height - 1 : y + 1, height};
}

std::pair<qreal, qreal> ActionGutter::emptyChunkBand(const SourceView *view, int line)
{
    const qreal y = view->yForLine(line);
    const auto [buttonTop, buttonHeight] = buttonExtent(view, line);
    if (buttonTop < y)
        return {buttonTop - 0.5, y + 1.5};
    return {y + 0.5, buttonTop + buttonHeight + 0.5};
}

int ActionGutter::lastChunkLineAtY(const SourceView *view, qreal y)
{
    const int lineCount = view->document()->blockCount();
    const int line = view->lineAtY(y);
    return line == lineCount - 1 && y > emptyChunkBand(view, lineCount).first ? lineCount : line;
}

std::vector<DiffChunk> ActionGutter::chunkRange(qreal startY, qreal endY) const
{
    if (!source_)
        return {};
    const int startLine = source_->lineAtY(startY);
    const int endLine = lastChunkLineAtY(source_, endY);

    auto startIdx = int(std::upper_bound(chunkStarts_.begin(), chunkStarts_.end(), startLine) - chunkStarts_.begin());
    const auto endIdx = int(std::upper_bound(chunkStarts_.begin(), chunkStarts_.end(), endLine) - chunkStarts_.begin());
    if (startIdx > 0 && startLine <= chunks_[size_t(startIdx - 1)].end_a)
        startIdx -= 1;
    if (startIdx >= endIdx)
        return {};
    return std::vector<DiffChunk>(chunks_.begin() + startIdx, chunks_.begin() + endIdx);
}

void ActionGutter::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    buttons_.clear();
    if (!source_ || !target_)
        return;

    const Theme &theme = currentTheme();
    const qreal width = this->width();
    const qreal height = this->height();

    // Get our vertical offset against the linked view (e.g., for info bars
    // at the top of the view) and translate to match.
    const qreal yTranslate =
        source_->viewport()->mapTo(window(), QPoint(0, 0)).y() - mapTo(window(), QPoint(0, 0)).y();

    const qreal buttonX = 1;
    const qreal buttonWidth = width - 2;
    const bool sourceEditable = source_->editable();
    const bool targetEditable = target_->editable();

    painter.save();
    painter.setClipRect(QRectF(0, 0, width, height));
    painter.translate(0, yTranslate);

    for (const DiffChunk &chunk : chunkRange(-yTranslate, height - yTranslate)) {
        const qreal rectY = source_->yForLine(chunk.start_a);
        const bool current = source_->currentChunkCheck && source_->currentChunkCheck(chunk);

        if (chunk.start_a == chunk.end_a) {
            // This isn't in the original. The line that this chunk is in the
            // view widens around the button, to where the link map continues
            const auto [top, bottom] = emptyChunkBand(source_, chunk.start_a);
            const bool viewOnLeft = iconDirection_ == Qt::LeftToRight;
            const qreal viewX = viewOnLeft ? 0 : width;
            // Past the edge, so that it's covered at fractional display scales
            const qreal mapX = viewOnLeft ? width + 1 : -1;
            const qreal curveX = viewX + (viewOnLeft ? width : -width) * EmptyChunkCurve;
            const qreal controlX = (viewX + curveX) / 2;

            QPainterPath topEdge(QPointF(viewX, rectY + 0.5));
            topEdge.cubicTo(controlX, rectY + 0.5, controlX, top, curveX, top);
            topEdge.lineTo(mapX, top);
            QPainterPath bottomEdge(QPointF(mapX, bottom));
            bottomEdge.lineTo(curveX, bottom);
            bottomEdge.cubicTo(controlX, bottom, controlX, rectY + 1.5, viewX, rectY + 1.5);

            QPainterPath band = topEdge;
            band.connectPath(bottomEdge);
            band.closeSubpath();

            painter.setRenderHint(QPainter::Antialiasing);
            painter.fillPath(band, theme.fill(chunk.tag));
            if (current)
                painter.fillPath(band, theme.currentChunkHighlight);
            painter.strokePath(topEdge, QPen(theme.line(chunk.tag), 1));
            painter.strokePath(bottomEdge, QPen(theme.line(chunk.tag), 1));
            painter.setRenderHint(QPainter::Antialiasing, false);
        } else {
            const qreal rectHeight = std::max(2.0, source_->yForLine(chunk.end_a) - rectY + 1);

            // Fill first, then over-fill to highlight if in the focused chunk
            const QRectF rect(-0.5, rectY, width + 1, rectHeight);
            painter.fillRect(rect, theme.fill(chunk.tag));
            if (current)
                painter.fillRect(rect, theme.currentChunkHighlight);
            painter.setPen(QPen(theme.line(chunk.tag), 1));
            painter.drawLine(QPointF(0, rectY + 0.5), QPointF(width, rectY + 0.5));
            painter.drawLine(QPointF(0, rectY - 0.5 + rectHeight), QPointF(width, rectY - 0.5 + rectHeight));
        }

        // Button rendering and tracking
        const std::optional<ActionMode> action =
            classifyChangeActions(chunk, actionMode_, sourceEditable, targetEditable);
        if (!action)
            continue;

        const auto [buttonY, buttonHeight] = buttonExtent(source_, chunk.start_a);
        const QRectF buttonRect(buttonX, buttonY, buttonWidth, buttonHeight);

        const bool pressed = pressedChunk_ == chunk;
        const bool hovered = pointerChunk_ == chunk;
        if (pressed || hovered) {
            QStyleOptionToolButton option;
            option.initFrom(this);
            option.rect = buttonRect.toAlignedRect();
            option.state |= QStyle::State_Raised | QStyle::State_AutoRaise;
            if (hovered)
                option.state |= QStyle::State_MouseOver;
            if (pressed)
                option.state |= QStyle::State_Sunken;
            option.subControls = QStyle::SC_ToolButton;
            option.activeSubControls = QStyle::SC_ToolButton;
            option.toolButtonStyle = Qt::ToolButtonIconOnly;
            style()->drawPrimitive(QStyle::PE_PanelButtonTool, &option, &painter, this);
        }

        const int iconSize = std::min(16, int(std::min(buttonWidth, buttonHeight)));
        const QPixmap pixmap = icons::icon(iconName(*action)).pixmap(QSize(iconSize, iconSize), devicePixelRatioF());
        const QPointF iconPos(buttonRect.center().x() - iconSize / 2.0, buttonRect.center().y() - iconSize / 2.0);
        painter.drawPixmap(iconPos, pixmap);

        buttons_.push_back({buttonRect.translated(0, yTranslate), chunk});
    }
    painter.restore();
}

void ActionGutter::updatePointerChunk(const QPointF &position)
{
    // This is the simplest button/intersection implementation in the world,
    // but it basically works for our purposes.
    // The last match, so that a chunk's button isn't hidden by that of an
    // empty chunk just before it, which has its line in common.
    std::optional<DiffChunk> newPointerChunk;
    for (const Button &button : buttons_) {
        if (button.rect.contains(position))
            newPointerChunk = button.chunk;
    }
    if (newPointerChunk != pointerChunk_) {
        pointerChunk_ = newPointerChunk;
        update();
    }
}

void ActionGutter::mouseMoveEvent(QMouseEvent *event)
{
    updatePointerChunk(event->position());
}

void ActionGutter::leaveEvent(QEvent *)
{
    if (pointerChunk_) {
        pointerChunk_.reset();
        update();
    }
}

void ActionGutter::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    updatePointerChunk(event->position());
    if (pointerChunk_) {
        pressedChunk_ = pointerChunk_;
        update();
    }
}

void ActionGutter::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    updatePointerChunk(event->position());
    const std::optional<DiffChunk> pressed = pressedChunk_;
    pressedChunk_.reset();
    update();
    if (pointerChunk_ && pointerChunk_ == pressed)
        activate(*pressed, event->position());
}

void ActionGutter::activate(const DiffChunk &chunk, const QPointF &position)
{
    if (!source_ || !target_)
        return;
    const std::optional<ActionMode> action =
        classifyChangeActions(chunk, actionMode_, source_->editable(), target_->editable());
    if (!action)
        return;

    if (*action == ActionMode::Replace && chunk.start_a == chunk.end_a) {
        // Replacing text with nothing is deleting it, from the target
        Q_EMIT chunkActionActivated(ChunkAction::Delete, target_, source_, chunk.reversed());
    } else if (*action == ActionMode::Replace) {
        Q_EMIT chunkActionActivated(ChunkAction::Replace, source_, target_, chunk);
    } else if (*action == ActionMode::Delete) {
        Q_EMIT chunkActionActivated(ChunkAction::Delete, source_, target_, chunk);
    } else {
        // Try and get the button coordinates and base the menu positioning
        // on them
        QPointF point = position;
        for (const Button &button : buttons_) {
            if (button.chunk == chunk) {
                point = QPointF(iconDirection_ == Qt::LeftToRight ? button.rect.right() : button.rect.left(),
                                button.rect.center().y());
                break;
            }
        }
        auto *menu = new QMenu(this);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<SourceView> from = source_;
        QPointer<SourceView> to = target_;
        menu->addAction(tr("Copy Up"), this, [this, from, to, chunk]() {
            if (from && to)
                Q_EMIT chunkActionActivated(ChunkAction::CopyUp, from, to, chunk);
        });
        menu->addAction(tr("Copy Down"), this, [this, from, to, chunk]() {
            if (from && to)
                Q_EMIT chunkActionActivated(ChunkAction::CopyDown, from, to, chunk);
        });
        menu->popup(mapToGlobal(point.toPoint()));
    }
}

} // namespace meld
