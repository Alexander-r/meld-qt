#include "linkmap.h"

#include "actiongutter.h"
#include "core/diffutil.h"
#include "sourceview.h"
#include "style.h"

#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

namespace meld {

namespace {

// Rounded rectangle corner radius for culled changes display
constexpr qreal Radius = 3;

} // namespace

int wheelSteps(int &accumulator, int delta)
{
    accumulator += delta;
    if (accumulator >= 120) {
        accumulator = 0;
        return -1;
    }
    if (accumulator <= -120) {
        accumulator = 0;
        return 1;
    }
    return 0;
}

LinkMap::LinkMap(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(50);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void LinkMap::associate(ChunkSource *source, SourceView *left, SourceView *right, int leftPane, int rightPane)
{
    source_ = source;
    views_[0] = left;
    views_[1] = right;
    panes_[0] = leftPane;
    panes_[1] = rightPane;
    if (layoutDirection() == Qt::RightToLeft) {
        std::swap(views_[0], views_[1]);
        std::swap(panes_[0], panes_[1]);
    }
    update();
}

void LinkMap::wheelEvent(QWheelEvent *event)
{
    const int step = wheelSteps(wheelAccumulator_, event->angleDelta().y());
    if (step)
        Q_EMIT scrollRequested(step > 0);
    event->accept();
}

void LinkMap::paintEvent(QPaintEvent *)
{
    QPainter context(this);
    context.fillRect(rect(), palette().color(QPalette::Window));
    if (!source_ || !source_->differ() || !views_[0] || !views_[1])
        return;

    const Theme &theme = currentTheme();
    context.setRenderHint(QPainter::Antialiasing);

    const qreal height = this->height();
    const qreal width = this->width();

    // Offsets of each view's visible area relative to this widget
    const qreal yOffset[2] = {
        qreal(views_[0]->viewport()->mapTo(window(), QPoint(0, 0)).y() - mapTo(window(), QPoint(0, 0)).y()) + 1,
        qreal(views_[1]->viewport()->mapTo(window(), QPoint(0, 0)).y() - mapTo(window(), QPoint(0, 0)).y()) + 1,
    };

    const std::array<int, 4> visible{
        views_[0]->lineAtY(0),
        ActionGutter::lastChunkLineAtY(views_[0], height),
        views_[1]->lineAtY(0),
        ActionGutter::lastChunkLineAtY(views_[1], height),
    };

    // For bezier control points
    const qreal xSteps[3] = {-0.5, width / 2, width + 0.5};

    const auto viewOffsetLine = [&](int viewIdx, int lineNum) {
        return views_[viewIdx]->yForLine(lineNum) + yOffset[viewIdx];
    };

    const Differ *differ = source_->differ();
    const std::optional<int> current = source_->currentChunk();
    for (const DiffChunk &c : differ->pairChanges(panes_[0], panes_[1], visible)) {
        // f and t are short for "from" and "to"
        const qreal f0 = viewOffsetLine(0, c.start_a);
        qreal f1 = viewOffsetLine(0, c.end_a);
        const qreal t0 = viewOffsetLine(1, c.start_b);
        qreal t1 = viewOffsetLine(1, c.end_b);
        // We want the last pixel of the previous line
        f1 = f1 == f0 ? f1 : f1 - 1;
        t1 = t1 == t0 ? t1 : t1 - 1;

        // A side with no lines continues from the band in its gutter
        const auto edges = [&](int viewIdx, int line, bool empty, qreal start, qreal end) {
            if (!empty)
                return std::pair{start - 0.5, end + 0.5};
            const auto [top, bottom] = ActionGutter::emptyChunkBand(views_[viewIdx], line);
            return std::pair{top + yOffset[viewIdx] - 1, bottom + yOffset[viewIdx] - 1};
        };
        const auto [fromTop, fromBottom] = edges(0, c.start_a, c.start_a == c.end_a, f0, f1);
        const auto [toTop, toBottom] = edges(1, c.start_b, c.start_b == c.end_b, t0, t1);

        QPainterPath path;
        // If either endpoint is completely off-screen, we cull for clarity
        if ((toTop < 0 && toBottom < 0) || (toTop > height && toBottom > height)) {
            const QRectF top(xSteps[0] - Radius, fromTop, 2 * Radius, 2 * Radius);
            const QRectF bottom(xSteps[0] - Radius, fromBottom - 2 * Radius, 2 * Radius, 2 * Radius);
            path.arcMoveTo(top, 90);
            path.arcTo(top, 90, -90);
            path.arcTo(bottom, 0, -90);
            path.closeSubpath();
        } else if ((fromTop < 0 && fromBottom < 0) || (fromTop > height && fromBottom > height)) {
            const QRectF top(xSteps[2] - Radius, toTop, 2 * Radius, 2 * Radius);
            const QRectF bottom(xSteps[2] - Radius, toBottom - 2 * Radius, 2 * Radius, 2 * Radius);
            path.arcMoveTo(top, 90);
            path.arcTo(top, 90, 90);
            path.arcTo(bottom, 180, 90);
            path.closeSubpath();
        } else {
            path.moveTo(xSteps[0], fromTop);
            path.cubicTo(xSteps[1], fromTop, xSteps[1], toTop, xSteps[2], toTop);
            path.lineTo(xSteps[2], toBottom);
            path.cubicTo(xSteps[1], toBottom, xSteps[1], fromBottom, xSteps[0], fromBottom);
            path.closeSubpath();
        }

        context.fillPath(path, theme.fill(c.tag));
        const std::optional<int> chunkIdx = differ->locateChunk(panes_[0], c.start_a).chunk;
        if (chunkIdx && chunkIdx == current)
            context.fillPath(path, theme.currentChunkHighlight);
        context.strokePath(path, QPen(theme.line(c.tag), 1));
    }
}

ScrollLinkMap::ScrollLinkMap(QWidget *parent)
    : QWidget(parent)
{
    setMinimumWidth(50);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
}

void ScrollLinkMap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
}

void ScrollLinkMap::wheelEvent(QWheelEvent *event)
{
    const int step = wheelSteps(wheelAccumulator_, event->angleDelta().y());
    if (step)
        Q_EMIT scrollRequested(step > 0);
    event->accept();
}

} // namespace meld
