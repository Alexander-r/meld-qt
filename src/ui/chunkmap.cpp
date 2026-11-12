#include "chunkmap.h"

#include "sourceview.h"
#include "style.h"

#include <QAbstractItemModel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextBlock>
#include <QTreeView>

#include <cmath>

namespace meld {

namespace {

double scrollUpper(const QScrollBar *bar)
{
    return bar ? double(bar->maximum() + bar->pageStep()) : 0.0;
}

} // namespace

ChunkMap::ChunkMap(QScrollBar *scrollBar, QWidget *parent)
    : QWidget(parent)
    , scrollBar_(scrollBar)
{
    setFixedWidth(15);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    if (scrollBar) {
        connect(scrollBar, &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
        connect(scrollBar, &QScrollBar::rangeChanged, this, &ChunkMap::invalidate);
    }
}

void ChunkMap::invalidate()
{
    cachedMap_.reset();
    update();
}

void ChunkMap::resizeEvent(QResizeEvent *event)
{
    cachedMap_.reset();
    QWidget::resizeEvent(event);
}

void ChunkMap::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        cachedMap_.reset();
    QWidget::changeEvent(event);
}

void ChunkMap::paintEvent(QPaintEvent *)
{
    QPainter context(this);
    if (!scrollBar_ || scrollUpper(scrollBar_) <= 0)
        return;

    const int height = this->height();
    const int width = this->width();
    if (width <= 0 || height <= 0)
        return;

    const Theme &theme = currentTheme();
    const QColor baseBackground = palette().color(QPalette::Base);
    const QColor baseOutline = palette().color(QPalette::Mid);
    QColor handleOverdraw = palette().color(QPalette::Text);
    handleOverdraw.setAlphaF(0.2f);
    QColor handleOutline = palette().color(QPalette::Text);
    handleOutline.setAlphaF(0.4f);

    const qreal x0 = OverdrawPadding + 0.5;
    const qreal x1 = width - 2 * x0;
    const qreal heightScaleValue = height * heightScale();

    if (!cachedMap_) {
        const qreal dpr = devicePixelRatioF();
        QPixmap surface(QSize(int(width * dpr), int(height * dpr)));
        surface.setDevicePixelRatio(dpr);
        surface.fill(Qt::transparent);
        QPainter cache(&surface);
        cache.fillRect(QRectF(x0, -0.5, x1, heightScaleValue + 0.5), baseBackground);

        // We get drawing coordinates by tag to minimise our source colour
        // setting, and make this loop slightly cleaner.
        const TaggedCoords tagged = chunkCoordsByTag();
        for (auto it = tagged.begin(); it != tagged.end(); ++it) {
            QPainterPath path;
            for (const auto &[f0, f1] : it.value()) {
                const qreal y0 = std::round(f0 * heightScaleValue) + 0.5;
                const qreal y1 = std::round(f1 * heightScaleValue) - 0.5;
                path.addRect(QRectF(x0, y0, x1, y1 - y0));
            }
            cache.fillPath(path, theme.fill(it.key()));
            cache.strokePath(path, QPen(theme.line(it.key()), 1));
        }

        cache.setPen(QPen(baseOutline, 1));
        cache.setBrush(Qt::NoBrush);
        cache.drawRect(QRectF(x0, 0.5, x1, heightScaleValue - 1));
        cache.end();
        cachedMap_ = surface;
    }

    context.drawPixmap(0, 0, *cachedMap_);

    // Draw our scroll position indicator
    const double upper = scrollUpper(scrollBar_);
    const double adjY = scrollBar_->value() / upper;
    const double adjH = scrollBar_->pageStep() / upper;
    const QRectF handle(x0 - OverdrawPadding, std::round(heightScaleValue * adjY) + 0.5, x1 + 2 * OverdrawPadding,
                        std::round(heightScaleValue * adjH) - 1);
    context.fillRect(handle, handleOverdraw);
    context.setPen(QPen(handleOutline, 1));
    context.drawRect(handle);
}

void ChunkMap::scrollFraction(double position)
{
    if (!scrollBar_)
        return;
    const double height = heightScale() * this->height();
    if (height <= 0)
        return;
    const double fraction = position / height;
    scrollToLocation(fraction);
}

void ChunkMap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    scrollFraction(event->position().y());
    grabbing_ = true;
}

void ChunkMap::mouseMoveEvent(QMouseEvent *event)
{
    if (grabbing_)
        scrollFraction(event->position().y());
}

void ChunkMap::mouseReleaseEvent(QMouseEvent *)
{
    grabbing_ = false;
}

TextViewChunkMap::TextViewChunkMap(SourceView *view, QWidget *parent)
    : ChunkMap(view->verticalScrollBar(), parent)
    , view_(view)
{
    connect(view, &SourceView::displayOptionsChanged, this, &ChunkMap::invalidate);
}

void TextViewChunkMap::setChunks(std::vector<DiffChunk> chunks)
{
    chunks_ = std::move(chunks);
    invalidate();
}

void TextViewChunkMap::setPairedViews(const QList<SourceView *> &views)
{
    paired_.clear();
    for (SourceView *view : views) {
        paired_.append(view);
        connect(view->verticalScrollBar(), &QScrollBar::rangeChanged, this, &ChunkMap::invalidate);
    }
}

double TextViewChunkMap::heightScale() const
{
    const double upper = scrollUpper(scrollBar_);
    double maximum = upper;
    for (const auto &view : paired_) {
        if (view && view->isVisible())
            maximum = std::max(maximum, scrollUpper(view->verticalScrollBar()));
    }
    return maximum > 0 ? upper / maximum : 1.0;
}

ChunkMap::TaggedCoords TextViewChunkMap::chunkCoordsByTag() const
{
    TaggedCoords tagged;
    if (!view_)
        return tagged;
    const QTextDocument *doc = view_->document();
    const QTextBlock last = doc->lastBlock();
    const double maxY = last.firstLineNumber() + std::max(1, last.lineCount());
    if (maxY <= 0)
        return tagged;

    const auto top = [doc](int line) {
        const QTextBlock block = doc->findBlockByNumber(line);
        if (!block.isValid())
            return double(doc->lastBlock().firstLineNumber() + std::max(1, doc->lastBlock().lineCount()));
        return double(block.firstLineNumber());
    };
    for (const DiffChunk &chunk : chunks_) {
        const double y0 = top(chunk.start_a);
        double y1;
        if (chunk.start_a == chunk.end_a) {
            y1 = y0;
        } else {
            const QTextBlock end = doc->findBlockByNumber(chunk.end_a - 1);
            y1 = end.isValid() ? end.firstLineNumber() + std::max(1, end.lineCount()) : maxY;
        }
        tagged[chunk.tag].emplace_back(y0 / maxY, y1 / maxY);
    }
    return tagged;
}

void TextViewChunkMap::scrollToLocation(double fraction)
{
    if (!scrollBar_)
        return;
    const double upper = scrollUpper(scrollBar_);
    const double location = fraction * (upper - scrollBar_->minimum());
    const int value = int(std::round(location - scrollBar_->pageStep() / 2.0));
    scrollBar_->setValue(std::clamp(value, scrollBar_->minimum(), scrollBar_->maximum()));
}

TreeViewChunkMap::TreeViewChunkMap(QTreeView *view, std::function<std::optional<ChunkTag>(const QModelIndex &)> tagFor,
                                   QWidget *parent)
    : ChunkMap(view->verticalScrollBar(), parent)
    , view_(view)
    , tagFor_(std::move(tagFor))
{
    connect(view, &QTreeView::expanded, this, &ChunkMap::invalidate);
    connect(view, &QTreeView::collapsed, this, &ChunkMap::invalidate);
}

ChunkMap::TaggedCoords TreeViewChunkMap::chunkCoordsByTag() const
{
    TaggedCoords tagged;
    if (!view_ || !view_->model())
        return tagged;

    std::vector<std::optional<ChunkTag>> rowStates;
    QModelIndex index = view_->model()->index(0, 0);
    while (index.isValid()) {
        rowStates.push_back(tagFor_(index));
        index = view_->indexBelow(index);
    }
    if (rowStates.empty())
        return tagged;

    const double numLines = double(rowStates.size());
    size_t chunkStart = 0;
    std::optional<ChunkTag> lastState = rowStates.front();
    for (size_t i = 1; i <= rowStates.size(); ++i) {
        const std::optional<ChunkTag> state = i < rowStates.size() ? rowStates[i] : std::optional<ChunkTag>();
        const bool terminate = i == rowStates.size();
        if (terminate || state != lastState) {
            if (lastState)
                tagged[*lastState].emplace_back(chunkStart / numLines, i / numLines);
            chunkStart = i;
            lastState = state;
        }
    }
    return tagged;
}

void TreeViewChunkMap::scrollToLocation(double fraction)
{
    if (!scrollBar_)
        return;
    const double upper = scrollUpper(scrollBar_);
    const double location = fraction * (upper - scrollBar_->minimum()) - scrollBar_->pageStep() / 2.0;
    scrollBar_->setValue(std::clamp(int(std::round(location)), scrollBar_->minimum(), scrollBar_->maximum()));
}

} // namespace meld
