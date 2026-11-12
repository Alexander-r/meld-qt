#include "sourceview.h"

#include "core/meldbuffer.h"
#include "core/syntax/highlighter.h"
#include "core/syntax/stylescheme.h"
#include "settings.h"
#include "style.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QUrl>

#include <cmath>

namespace meld {

namespace {

constexpr int GutterPadding = 3;

QList<QString> localFilesFromMime(const QMimeData *mime)
{
    QList<QString> files;
    if (!mime || !mime->hasUrls())
        return files;
    for (const QUrl &url : mime->urls()) {
        if (url.isLocalFile())
            files.append(url.toLocalFile());
    }
    return files;
}

} // namespace

// ---------------------------------------------------------------------------
// LineNumberGutter

LineNumberGutter::LineNumberGutter(SourceView *view)
    : QWidget(view)
    , view_(view)
{
    setAutoFillBackground(false);
    setCursor(Qt::ArrowCursor);
}

int LineNumberGutter::preferredWidth() const
{
    // Always calculate display size for at least two-digit line counts
    const int numLines = std::max(view_->document()->blockCount(), 99);
    QFont bold = view_->font();
    bold.setBold(true);
    const QFontMetrics metrics(bold);
    return metrics.horizontalAdvance(QString::number(numLines)) + GutterPadding * 2;
}

void LineNumberGutter::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    // The scheme's line number colours, as GtkSourceView's gutter
    const QColor background =
        schemeColour(QStringLiteral("line-numbers"), StyleAttribute::Background).value_or(palette().color(QPalette::Window));
    const QColor foreground = schemeColour(QStringLiteral("line-numbers"), StyleAttribute::Foreground)
                                  .value_or(palette().color(QPalette::WindowText));
    painter.fillRect(event->rect(), background);

    const Theme &theme = currentTheme();
    QFont bold = view_->font();
    bold.setBold(true);
    painter.setFont(bold);
    const qreal width = this->width();
    const qreal lineHeight = view_->lineHeight();
    const int current = view_->textCursor().blockNumber();

    for (const SourceView::VisibleLine &line : view_->visibleLines()) {
        std::optional<DiffChunk> chunk;
        if (view_->gutterChunkAt)
            chunk = view_->gutterChunkAt(line.number);

        const qreal y = line.top;
        // Adjustment because we want to stroke the bottom border. This needs
        // to match the chunk painting in the text view, or things will not
        // align correctly.
        const qreal height = line.height + 1;

        if (chunk && chunk->start_a != chunk->end_a) {
            const bool isCurrent = view_->currentChunkCheck && view_->currentChunkCheck(*chunk);
            painter.fillRect(QRectF(0, y + 1, width, height),
                             isCurrent ? theme.highlightedFill(chunk->tag) : theme.fill(chunk->tag));
        }

        // If we don't have a chunk, we don't draw any borders
        if (chunk) {
            painter.setPen(QPen(theme.line(chunk->tag), 1));
            if (line.number == chunk->start_a)
                painter.drawLine(QPointF(0, y + 0.5), QPointF(width, y + 0.5));
            if (line.number == chunk->end_a - 1)
                painter.drawLine(QPointF(0, y - 0.5 + height), QPointF(width, y - 0.5 + height));
            if (chunk->start_a == chunk->end_a)
                painter.drawLine(QPointF(0, y + 1.5), QPointF(width, y + 1.5));
        }

        QColor textColor = foreground;
        if (line.number != current)
            textColor.setAlphaF(0.85f);
        painter.setPen(textColor);
        painter.drawText(QRectF(0, y, width - GutterPadding, std::min(line.height, lineHeight)),
                         Qt::AlignRight | Qt::AlignVCenter, QString::number(line.number + 1));
    }

    // A chunk after the last line isn't at any line of the loop above
    const std::vector<SourceView::VisibleLine> lines = view_->visibleLines();
    const int lineCount = view_->document()->blockCount();
    if (view_->gutterChunkAt && !lines.empty() && lines.back().number == lineCount - 1) {
        const std::optional<DiffChunk> chunk = view_->gutterChunkAt(lineCount);
        if (chunk && chunk->start_a == lineCount && chunk->end_a == lineCount) {
            const qreal y = lines.back().top + lines.back().height;
            painter.setPen(QPen(theme.line(chunk->tag), 1));
            painter.drawLine(QPointF(0, y + 0.5), QPointF(width, y + 0.5));
            painter.drawLine(QPointF(0, y + 1.5), QPointF(width, y + 1.5));
        }
    }
}

void LineNumberGutter::mousePressEvent(QMouseEvent *event)
{
    // Clicking a line number selects the line, as in GtkSourceView
    const int line = view_->lineAtY(event->position().y());
    const QTextBlock block = view_->document()->findBlockByNumber(line);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor);
    if (cursor.position() == block.position())
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    view_->setTextCursor(cursor);
    view_->setFocus(Qt::MouseFocusReason);
}

// ---------------------------------------------------------------------------
// SourceView

SourceView::SourceView(QWidget *parent)
    : QPlainTextEdit(parent)
    , gutter_(new LineNumberGutter(this))
{
    setFrameShape(QFrame::NoFrame);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setCenterOnScroll(false);
    setAcceptDrops(true);
    setMouseTracking(false);
    gutter_->hide();

    animationTimer_.setInterval(16);
    connect(&animationTimer_, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));

    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (gutter_->isVisible()) {
            if (dy)
                gutter_->scroll(0, dy);
            else
                gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        }
        Q_EMIT viewportChanged();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (gutter_->isVisible())
            gutter_->update();
        if (highlightCurrentLine_)
            viewport()->update();
    });
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &SourceView::extendScrollRange);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { Q_EMIT viewportChanged(); });
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { Q_EMIT viewportChanged(); });
    connect(MeldSettings::instance(), &MeldSettings::changed, this, [this](const QString &key) {
        if (key == QLatin1String("style-scheme"))
            applyStyleScheme();
        else if (key == QLatin1String("highlight-syntax") && buffer_)
            buffer_->highlighter()->setHighlightSyntax(MeldSettings::instance()->boolValue(key));
    });

    applyFontSettings();
    applyStyleScheme();
}

SourceView::~SourceView() = default;

void SourceView::setBuffer(MeldBuffer *buffer)
{
    buffer_ = buffer;
    setDocument(buffer->document());
    // As MeldBuffer's highlight-syntax binding in the original
    buffer_->highlighter()->setHighlightSyntax(MeldSettings::instance()->boolValue(QStringLiteral("highlight-syntax")));
    // QPlainTextEdit resets the document's font; make sure it follows ours
    applyFontSettings();
    applyStyleScheme();
    connect(document(), &QTextDocument::blockCountChanged, this, [this]() {
        updateGutterGeometry();
        updateScrollRange();
    });
    connect(buffer_, &MeldBuffer::linesChanged, viewport(), qOverload<>(&QWidget::update));
}

void SourceView::applyFontSettings()
{
    const QFont font = editorFont();
    setFont(font);
    document()->setDefaultFont(font);
    if (buffer_)
        buffer_->highlighter()->setBaseFont(font);
    MeldSettings *settings = MeldSettings::instance();
    const int indentWidth = std::max(1, settings->intValue(QStringLiteral("indent-width")));
    setTabStopDistance(QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) * indentWidth);
    updateGutterGeometry();
    updateScrollRange();
    viewport()->update();
}

void SourceView::applyStyleScheme()
{
    // The scheme's text and selection colours, as GtkSourceView applies
    // them; other colours follow the application's palette
    setPalette(schemeTextPalette(currentStyleScheme()));
    cursorColor_ = schemeColour(QStringLiteral("cursor"), StyleAttribute::Foreground);
    spacesColor_ = schemeColour(QStringLiteral("draw-spaces"), StyleAttribute::Foreground);

    if (buffer_)
        buffer_->highlighter()->setStyleScheme(currentStyleScheme());
    gutter_->update();
    viewport()->update();
}

void SourceView::ensureVisibleHighlighted()
{
    if (!buffer_ || !buffer_->highlighter()->isActive())
        return;
    // Lines may be taller (e.g., for headings) or wrapped, so this may
    // highlight a few more lines than are shown. Far into large files that
    // aren't highlighted yet, this is limited so that painting stays
    // responsive; the lines are repainted when they're highlighted.
    const QFontMetricsF metrics(font());
    const int visibleLines = int(viewport()->height() / std::max(1.0, metrics.lineSpacing())) + 2;
    buffer_->highlighter()->ensureHighlighted(firstVisibleBlock().blockNumber() + visibleLines, 50);
}

void SourceView::setEditable(bool editable)
{
    if (editable == this->editable())
        return;
    setReadOnly(!editable);
    if (!editable) {
        // Keep the cursor so that chunk navigation still works
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    } else {
        setTextInteractionFlags(Qt::TextEditorInteraction);
    }
    Q_EMIT editableChanged(editable);
}

void SourceView::setShowLineNumbers(bool show)
{
    if (show == showLineNumbers_)
        return;
    showLineNumbers_ = show;
    gutter_->setVisible(show);
    updateGutterGeometry();
    Q_EMIT displayOptionsChanged();
}

void SourceView::setDrawSpaces(bool draw)
{
    if (draw == drawSpaces_)
        return;
    drawSpaces_ = draw;
    viewport()->update();
    Q_EMIT displayOptionsChanged();
}

void SourceView::setHighlightCurrentLine(bool highlight)
{
    if (highlight == highlightCurrentLine_)
        return;
    highlightCurrentLine_ = highlight;
    viewport()->update();
    Q_EMIT displayOptionsChanged();
}

void SourceView::setWrapText(bool wrap)
{
    if (wrap == wrapText_)
        return;
    wrapText_ = wrap;
    if (wrap) {
        const QString mode = MeldSettings::instance()->stringValue(QStringLiteral("wrap-mode"));
        if (mode == QLatin1String("char"))
            setWordWrapMode(QTextOption::WrapAnywhere);
        else if (mode == QLatin1String("word-char"))
            setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        else
            setWordWrapMode(QTextOption::WordWrap);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
    } else {
        setLineWrapMode(QPlainTextEdit::NoWrap);
    }
    Q_EMIT displayOptionsChanged();
}

void SourceView::setOverscrollLines(int lines)
{
    overscrollLines_ = lines;
    updateScrollRange();
}

void SourceView::setSearchHighlight(const SearchHighlight &highlight)
{
    search_ = highlight;
    if (highlight.regex && !highlight.text.isEmpty()) {
        QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
        if (!highlight.caseSensitive)
            options |= QRegularExpression::CaseInsensitiveOption;
        searchRegex_ = QRegularExpression(highlight.text, options);
    }
    viewport()->update();
}

void SourceView::updateGutterGeometry()
{
    const int width = showLineNumbers_ ? gutter_->preferredWidth() : 0;
    setViewportMargins(width, 0, 0, 0);
    const QRect contents = contentsRect();
    gutter_->setGeometry(QRect(contents.left(), contents.top(), width, viewport()->height()));
    gutter_->update();
}

void SourceView::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateGutterGeometry();
    updateScrollRange();
    Q_EMIT viewportChanged();
}

void SourceView::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateGutterGeometry();
        updateScrollRange();
    }
}

void SourceView::scrollContentsBy(int dx, int dy)
{
    QPlainTextEdit::scrollContentsBy(dx, dy);
    Q_EMIT viewportChanged();
}

void SourceView::updateScrollRange()
{
    QScrollBar *bar = verticalScrollBar();
    extendScrollRange(bar->minimum(), bar->maximum());
}

void SourceView::extendScrollRange(int min, int max)
{
    // Emulate GtkSourceView's bottom margin of a few lines (overscroll), by
    // extending the scroll range that QPlainTextEdit calculates.
    if (adjustingRange_ || max == extendedMaximum_)
        return;
    const QTextBlock last = document()->lastBlock();
    const int totalLines = last.firstLineNumber() + std::max(1, last.lineCount());
    const int visible = std::max(1, int(viewport()->height() / std::max(1.0, lineHeight())));
    const int desired = std::max(0, totalLines + overscrollLines_ - visible);
    extendedMaximum_ = std::max(max, std::min(desired, totalLines - 1));
    if (extendedMaximum_ != max) {
        adjustingRange_ = true;
        verticalScrollBar()->setRange(min, extendedMaximum_);
        adjustingRange_ = false;
    }
}

std::vector<SourceView::VisibleLine> SourceView::visibleLines() const
{
    std::vector<VisibleLine> result;
    QTextBlock block = firstVisibleBlock();
    qreal top = contentOffset().y();
    const qreal bottom = viewport()->height();
    while (block.isValid()) {
        if (block.isVisible()) {
            const qreal height = blockBoundingRect(block).height();
            result.push_back({block.blockNumber(), top, height});
            top += height;
            if (top > bottom)
                break;
        }
        block = block.next();
    }
    return result;
}

qreal SourceView::lineHeight() const
{
    const QTextBlock block = firstVisibleBlock();
    if (block.isValid() && block.layout() && block.layout()->lineCount() > 0)
        return block.layout()->lineAt(0).height();
    return QFontMetricsF(font()).lineSpacing();
}

qreal SourceView::yForLine(int line) const
{
    const std::vector<VisibleLine> lines = visibleLines();
    const QTextDocument *doc = document();
    const int count = doc->blockCount();
    if (lines.empty())
        return 0;

    const qreal height = lineHeight();
    const VisibleLine &first = lines.front();
    const VisibleLine &last = lines.back();

    if (line >= count) {
        if (last.number == count - 1)
            return last.top + last.height;
        const QTextBlock after = doc->findBlockByNumber(last.number + 1);
        const QTextBlock end = doc->lastBlock();
        const int visual = end.firstLineNumber() + std::max(1, end.lineCount()) - after.firstLineNumber();
        return last.top + last.height + visual * height;
    }
    if (line < first.number) {
        const int visual = doc->findBlockByNumber(first.number).firstLineNumber()
            - doc->findBlockByNumber(std::max(line, 0)).firstLineNumber();
        return first.top - visual * height;
    }
    if (line > last.number) {
        const int visual = doc->findBlockByNumber(line).firstLineNumber()
            - doc->findBlockByNumber(last.number + 1).firstLineNumber();
        return last.top + last.height + visual * height;
    }
    return lines[size_t(line - first.number)].top;
}

int SourceView::lineAtY(qreal y) const
{
    const std::vector<VisibleLine> lines = visibleLines();
    const QTextDocument *doc = document();
    const int count = doc->blockCount();
    if (lines.empty())
        return 0;
    const qreal height = std::max(1.0, lineHeight());

    if (y < lines.front().top) {
        const int delta = int(std::ceil((lines.front().top - y) / height));
        const int visual = doc->findBlockByNumber(lines.front().number).firstLineNumber() - delta;
        if (visual <= 0)
            return 0;
        return doc->findBlockByLineNumber(visual).blockNumber();
    }
    for (const VisibleLine &line : lines) {
        if (y < line.top + line.height)
            return line.number;
    }
    const VisibleLine &last = lines.back();
    if (last.number >= count - 1)
        return count - 1;
    const int delta = int(std::floor((y - (last.top + last.height)) / height));
    const int visual = doc->findBlockByNumber(last.number + 1).firstLineNumber() + delta;
    const QTextBlock block = doc->findBlockByLineNumber(visual);
    return block.isValid() ? block.blockNumber() : count - 1;
}

int SourceView::visualColumn(int position) const
{
    const QTextBlock block = document()->findBlock(position);
    if (!block.isValid())
        return 0;
    const int tabWidth = std::max(1, MeldSettings::instance()->intValue(QStringLiteral("indent-width")));
    const QString text = block.text();
    const int offset = std::min(int(text.size()), position - block.position());
    int column = 0;
    for (int i = 0; i < offset; ++i) {
        if (text.at(i) == QLatin1Char('\t'))
            column += tabWidth - (column % tabWidth);
        else
            ++column;
    }
    return column;
}

void SourceView::addFadingHighlight(int start, int end, const QColor &color, int durationMs, AnimationType type,
                                    double startingAlpha)
{
    if (!isVisible())
        return;
    Animation animation;
    animation.start = QTextCursor(document());
    animation.start.setPosition(std::clamp(start, 0, document()->characterCount() - 1));
    animation.start.setKeepPositionOnInsert(true);
    animation.end = QTextCursor(document());
    animation.end.setPosition(std::clamp(end, 0, document()->characterCount() - 1));
    animation.end.setKeepPositionOnInsert(true);
    animation.startColor = color;
    animation.startColor.setAlphaF(float(startingAlpha));
    animation.endColor = color;
    animation.endColor.setAlphaF(0);
    animation.duration = durationMs;
    animation.type = type;
    animation.timer.start();
    animations_.push_back(std::move(animation));
    if (!animationTimer_.isActive())
        animationTimer_.start();
    viewport()->update();
}

void SourceView::scrollToLineCentered(int line)
{
    const QTextBlock block = document()->findBlockByNumber(std::clamp(line, 0, document()->blockCount() - 1));
    if (!block.isValid())
        return;
    QScrollBar *bar = verticalScrollBar();
    const int visual = block.firstLineNumber();
    const int value = visual - bar->pageStep() / 2;
    bar->setValue(std::clamp(value, bar->minimum(), bar->maximum()));
}

void SourceView::placeCursorAtLine(int line)
{
    const QTextBlock block = document()->findBlockByNumber(std::clamp(line, 0, document()->blockCount() - 1));
    QTextCursor cursor(document());
    cursor.setPosition(block.isValid() ? block.position() : 0);
    setTextCursor(cursor);
}

void SourceView::paintEvent(QPaintEvent *event)
{
    // Highlighting may change the lines' layouts, so it's done first
    ensureVisibleHighlighted();
    QPainter painter(viewport());
    paintLayers(painter);
    paintTextBlocks(painter, event->rect());
}

void SourceView::paintRanges(QPainter &painter, const std::vector<std::pair<int, int>> &ranges, const QColor &color,
                             const std::vector<VisibleLine> &lines)
{
    if (ranges.empty())
        return;
    const qreal offsetX = contentOffset().x();
    for (const VisibleLine &visible : lines) {
        const QTextBlock block = document()->findBlockByNumber(visible.number);
        QTextLayout *layout = block.layout();
        if (!layout)
            continue;
        const int blockStart = block.position();
        const int blockEnd = blockStart + int(block.text().size());
        const QPointF layoutPos = layout->position();
        for (const auto &[start, end] : ranges) {
            if (end <= blockStart || start > blockEnd)
                continue;
            const int s = std::max(start, blockStart) - blockStart;
            const int e = std::min(end, blockEnd) - blockStart;
            if (s >= e)
                continue;
            for (int i = 0; i < layout->lineCount(); ++i) {
                const QTextLine textLine = layout->lineAt(i);
                const int lineStart = textLine.textStart();
                const int lineEnd = lineStart + textLine.textLength();
                const int ls = std::max(s, lineStart);
                const int le = std::min(e, lineEnd);
                if (ls >= le)
                    continue;
                const qreal x1 = textLine.cursorToX(ls);
                const qreal x2 = textLine.cursorToX(le);
                const QRectF rect(offsetX + layoutPos.x() + std::min(x1, x2),
                                  visible.top + layoutPos.y() + textLine.y(), std::abs(x2 - x1), textLine.height());
                painter.fillRect(rect, color);
            }
        }
    }
}

std::vector<std::pair<int, int>> SourceView::searchMatchesIn(int startLine, int endLine) const
{
    std::vector<std::pair<int, int>> matches;
    if (search_.isEmpty())
        return matches;
    if (search_.regex && !searchRegex_.isValid())
        return matches;
    QTextBlock block = document()->findBlockByNumber(startLine);
    while (block.isValid() && block.blockNumber() <= endLine) {
        const QString text = block.text();
        if (search_.regex) {
            auto it = searchRegex_.globalMatch(text);
            while (it.hasNext()) {
                const auto match = it.next();
                if (match.capturedLength() > 0)
                    matches.emplace_back(block.position() + int(match.capturedStart()),
                                         block.position() + int(match.capturedEnd()));
            }
        } else {
            const Qt::CaseSensitivity cs = search_.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
            qsizetype index = text.indexOf(search_.text, 0, cs);
            while (index >= 0) {
                const qsizetype end = index + search_.text.size();
                const bool boundaryOk = !search_.wholeWord
                    || ((index == 0 || !text.at(index - 1).isLetterOrNumber())
                        && (end >= text.size() || !text.at(end).isLetterOrNumber()));
                if (boundaryOk)
                    matches.emplace_back(block.position() + int(index), block.position() + int(end));
                index = text.indexOf(search_.text, index + std::max<qsizetype>(1, search_.text.size()), cs);
            }
        }
        block = block.next();
    }
    return matches;
}

void SourceView::paintLayers(QPainter &painter)
{
    const std::vector<VisibleLine> lines = visibleLines();
    if (lines.empty())
        return;

    const Theme &theme = currentTheme();
    const qreal width = viewport()->width() + 1;
    const qreal viewportHeight = viewport()->height();
    const int count = document()->blockCount();

    const int startLine = lineAtY(0);
    int endLine = lineAtY(viewportHeight);
    const qreal endOfText = yForLine(count);
    if (viewportHeight > endOfText)
        endLine += 1;

    // Paint chunk backgrounds and outlines
    if (chunkIter) {
        for (const DiffChunk &change : chunkIter(startLine, endLine)) {
            const qreal ypos0 = yForLine(change.start_a);
            const qreal ypos1 = yForLine(change.end_a);
            const qreal height = std::max(1.0, ypos1 - ypos0) + 1;
            const QRectF rect(0, ypos0, width, height);
            if (change.start_a != change.end_a) {
                painter.fillRect(rect, theme.fill(change.tag));
                if (currentChunkCheck && currentChunkCheck(change))
                    painter.fillRect(rect, theme.currentChunkHighlight);
            }
            painter.setPen(QPen(theme.line(change.tag), 1));
            painter.drawLine(QPointF(0, ypos0 + 0.5), QPointF(width, ypos0 + 0.5));
            painter.drawLine(QPointF(0, ypos0 + height - 0.5), QPointF(width, ypos0 + height - 0.5));
        }
    }

    // Check whether we're drawing past the last line in the buffer (i.e.,
    // the overscroll) and draw a custom background if so. We're adding 2
    // here so that we don't overlap with the chunk line drawing if the last
    // chunk is an insert.
    const qreal overscrollStart = endOfText + 2;
    if (viewportHeight > overscrollStart)
        painter.fillRect(QRectF(0, overscrollStart, width, viewportHeight - overscrollStart), theme.overscroll);

    // Paint current line highlight, only in the view being edited: the
    // focus widget of its window, which stays so while the window is
    // inactive
    if (highlightCurrentLine_ && window()->focusWidget() == this) {
        const int cursorLine = textCursor().blockNumber();
        for (const VisibleLine &line : lines) {
            if (line.number == cursorLine)
                painter.fillRect(QRectF(0, line.top, width, line.height), theme.currentLineHighlight);
        }
    }

    // Draw syncpoint indicator lines
    if (syncpointLines) {
        painter.setPen(QPen(theme.syncpointOutline, 1));
        for (int syncline : syncpointLines()) {
            if (syncline < startLine || syncline > endLine)
                continue;
            const qreal y = std::round(yForLine(syncline));
            painter.drawLine(QPointF(0, y), QPointF(width, y));
        }
    }

    // Overdraw all animated chunks, and update animation states
    std::vector<Animation> remaining;
    for (Animation &animation : animations_) {
        const double percent = std::min(1.0, double(animation.timer.elapsed()) / std::max(1, animation.duration));
        const auto scale = [percent](float start, float end) { return start + (end - start) * float(percent); };
        const QColor color = QColor::fromRgbF(scale(animation.startColor.redF(), animation.endColor.redF()),
                                              scale(animation.startColor.greenF(), animation.endColor.greenF()),
                                              scale(animation.startColor.blueF(), animation.endColor.blueF()),
                                              scale(animation.startColor.alphaF(), animation.endColor.alphaF()));
        qreal ystart = yForLine(document()->findBlock(animation.start.position()).blockNumber());
        const qreal yend = yForLine(document()->findBlock(animation.end.position()).blockNumber());
        if (ystart == yend)
            ystart -= 1;
        const QRectF rect(0, ystart, width, (yend - ystart) + 1);
        if (animation.type == AnimationType::Stroke) {
            painter.setPen(QPen(color, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
        } else {
            painter.fillRect(rect, color);
        }
        if (animation.timer.elapsed() <= animation.duration)
            remaining.push_back(std::move(animation));
    }
    animations_ = std::move(remaining);
    if (animations_.empty())
        animationTimer_.stop();

    // Backgrounds of whole lines from syntax highlighting, which are
    // paragraph backgrounds in GTK and drawn after Meld's layers
    if (buffer_ && buffer_->highlighter()->isActive()) {
        for (const VisibleLine &line : lines) {
            const QTextBlock block = document()->findBlockByNumber(line.number);
            if (const std::optional<QColor> background = buffer_->highlighter()->lineBackground(block))
                painter.fillRect(QRectF(0, line.top, width, line.height), *background);
        }
    }

    // Inline change highlighting, and search matches
    if (buffer_) {
        const int startPos = document()->findBlockByNumber(lines.front().number).position();
        const QTextBlock lastBlock = document()->findBlockByNumber(lines.back().number);
        const int endPos = lastBlock.position() + lastBlock.length();
        paintRanges(painter, buffer_->inlineHighlights().rangesIn(startPos, endPos), theme.inlineBackground, lines);
    }
    if (!search_.isEmpty())
        paintRanges(painter, searchMatchesIn(lines.front().number, lines.back().number), theme.searchMatch, lines);
}

void SourceView::paintTextBlocks(QPainter &painter, const QRect &exposed)
{
    // This follows QPlainTextEdit::paintEvent(), adding the dimmed text
    // ranges as extra formats.
    QPointF offset(contentOffset());
    QRect er = exposed;
    const QRect viewportRect = viewport()->rect();
    const bool isEditable = !isReadOnly();

    QTextBlock block = firstVisibleBlock();
    const qreal maximumWidth = document()->documentLayout()->documentSize().width();

    // Set a brush origin so that the WaveUnderline knows where the wave started
    painter.setBrushOrigin(offset);

    // keep right margin clean from full-width selection
    const int maxX = int(offset.x() + std::max(qreal(viewportRect.width()), maximumWidth)
                         - document()->documentMargin() + cursorWidth());
    er.setRight(std::min(er.right(), maxX));
    painter.setClipRect(er);

    QAbstractTextDocumentLayout::PaintContext context = getPaintContext();
    painter.setPen(context.palette.text().color());

    QTextCharFormat dimmedFormat;
    dimmedFormat.setForeground(currentTheme().dimmedForeground);

    while (block.isValid()) {
        const QRectF r = blockBoundingRect(block).translated(offset);
        QTextLayout *layout = block.layout();

        if (!block.isVisible()) {
            offset.ry() += r.height();
            block = block.next();
            continue;
        }

        if (r.bottom() >= er.top() && r.top() <= er.bottom()) {
            QList<QTextLayout::FormatRange> selections;
            const int blpos = block.position();
            const int bllen = block.length();

            if (buffer_) {
                for (const auto &[start, end] : buffer_->dimmedRanges().rangesIn(blpos, blpos + bllen)) {
                    QTextLayout::FormatRange range;
                    range.start = start - blpos;
                    range.length = end - start;
                    range.format = dimmedFormat;
                    selections.append(range);
                }
            }

            for (const QAbstractTextDocumentLayout::Selection &range : std::as_const(context.selections)) {
                const int selStart = range.cursor.selectionStart() - blpos;
                const int selEnd = range.cursor.selectionEnd() - blpos;
                if (selStart < bllen && selEnd > 0 && selEnd > selStart) {
                    QTextLayout::FormatRange o;
                    o.start = selStart;
                    o.length = selEnd - selStart;
                    o.format = range.format;
                    selections.append(o);
                } else if (!range.cursor.hasSelection() && range.format.hasProperty(QTextFormat::FullWidthSelection)
                           && block.contains(range.cursor.position())) {
                    QTextLayout::FormatRange o;
                    const QTextLine l = layout->lineForTextPosition(range.cursor.position() - blpos);
                    o.start = l.textStart();
                    o.length = l.textLength();
                    if (o.start + o.length == bllen - 1)
                        ++o.length; // include newline
                    o.format = range.format;
                    selections.append(o);
                }
            }

            const bool drawCursor = ((isEditable || (textInteractionFlags() & Qt::TextSelectableByKeyboard))
                                     && context.cursorPosition >= blpos && context.cursorPosition < blpos + bllen);
            bool drawCursorAsBlock = drawCursor && overwriteMode();
            if (drawCursorAsBlock) {
                if (context.cursorPosition == blpos + bllen - 1) {
                    drawCursorAsBlock = false;
                } else {
                    QTextLayout::FormatRange o;
                    o.start = context.cursorPosition - blpos;
                    o.length = 1;
                    o.format.setForeground(palette().base());
                    o.format.setBackground(palette().text());
                    selections.append(o);
                }
            }

            layout->draw(&painter, offset, selections, er);

            // Whitespace in inline changes is always shown, as the original's
            // inline tag has draw-spaces set
            if (buffer_) {
                const std::vector<std::pair<int, int>> inlineRanges =
                    buffer_->inlineHighlights().rangesIn(blpos, blpos + bllen);
                if (drawSpaces_ || !inlineRanges.empty())
                    paintWhitespace(painter, block, offset, inlineRanges);
            }

            if ((drawCursor && !drawCursorAsBlock)
                || (isEditable && context.cursorPosition < -1 && !layout->preeditAreaText().isEmpty())) {
                int cpos = context.cursorPosition;
                if (cpos < -1)
                    cpos = layout->preeditAreaPosition() - (cpos + 2);
                else
                    cpos -= blpos;
                painter.setPen(cursorColor_.value_or(context.palette.text().color()));
                layout->drawCursor(&painter, offset, cpos, cursorWidth());
            }
        }

        offset.ry() += r.height();
        if (offset.y() > viewportRect.height())
            break;
        block = block.next();
    }
}

namespace {

//! Characters with the "non-breaking glue" line break class, which
//! GtkSourceView draws as non-breaking spaces
bool isNonBreakingSpace(char32_t c)
{
    return c == 0x00A0 || c == 0x034F || (c >= 0x035C && c <= 0x0362) || c == 0x0F08 || c == 0x0F0C || c == 0x0F12
        || c == 0x0FD9 || c == 0x0FDA || c == 0x180E || c == 0x2007 || c == 0x2011 || c == 0x202F;
}

} // namespace

void SourceView::paintWhitespace(QPainter &painter, const QTextBlock &block, const QPointF &offset,
                                 const std::vector<std::pair<int, int>> &forced)
{
    // As GtkSourceSpaceDrawer, with the default matrix: all spaces, tabs,
    // non-breaking spaces and newlines
    QTextLayout *layout = block.layout();
    if (!layout || layout->lineCount() == 0)
        return;
    const QString text = block.text();
    const int blockStart = block.position();
    const QPointF origin = offset + layout->position();
    const auto needsDrawing = [&](int column) {
        if (drawSpaces_)
            return true;
        const int position = blockStart + column;
        return std::any_of(forced.begin(), forced.end(),
                           [position](const auto &range) { return position >= range.first && position < range.second; });
    };
    // The rectangle of a character; zero-width ones (e.g., newlines) are as
    // wide as they're high
    const auto charRect = [&](int column) {
        const QTextLine line = layout->lineForTextPosition(column);
        const qreal x = line.cursorToX(column);
        qreal width = column < text.size() ? line.cursorToX(column + 1) - x : 0;
        if (width <= 0)
            width = line.height();
        return QRectF(origin.x() + x, origin.y() + line.y(), width, line.height());
    };

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(spacesColor_.value_or(palette().color(QPalette::Disabled, QPalette::Text)), 0.8));
    painter.setBrush(Qt::NoBrush);
    painter.translate(-0.5, -0.5);

    for (int i = 0; i < text.size(); ++i) {
        char32_t c = text.at(i).unicode();
        const bool pair = text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate();
        if (pair)
            c = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
        const bool tab = c == U'\t';
        const bool nbsp = isNonBreakingSpace(c);
        const bool space = QChar::category(c) == QChar::Separator_Space;
        if ((tab || nbsp || space) && needsDrawing(i)) {
            const QRectF rect = charRect(i);
            const qreal w = rect.width();
            const qreal h = rect.height();
            if (tab) {
                const qreal y = rect.y() + h * 2 / 3;
                QPainterPath path(QPointF(rect.x() + h / 6, y));
                path.lineTo(rect.x() + w - h / 6, y);
                path.lineTo(rect.x() + w - h / 6 - h / 4, y - h / 4);
                path.moveTo(rect.x() + w - h / 6, y);
                path.lineTo(rect.x() + w - h / 6 - h / 4, y + h / 4);
                painter.drawPath(path);
            } else if (nbsp) {
                const qreal y = rect.y() + h / 2;
                QPainterPath path(QPointF(rect.x() + w / 6, y));
                path.lineTo(rect.x() + w * 5 / 6, y);
                path.lineTo(rect.x() + w / 2, y + h / 4);
                path.closeSubpath();
                if (c == 0x202F)
                    painter.fillPath(path, painter.pen().color());
                else
                    painter.drawPath(path);
            } else {
                painter.drawEllipse(QPointF(rect.x() + w / 2, rect.y() + h * 2 / 3), 0.8, 0.8);
            }
        }
        if (pair)
            ++i;
    }

    // Newlines; the last line has none
    if (block.next().isValid() && needsDrawing(int(text.size()))) {
        const QRectF rect = charRect(int(text.size()));
        const qreal w = 2 * rect.width();
        const qreal h = rect.height();
        const qreal x = rect.x();
        const qreal y = rect.y() + h / 3;
        QPainterPath path;
        if (layoutDirection() == Qt::LeftToRight) {
            path.moveTo(x + w * 7 / 8, y);
            path.lineTo(x + w * 7 / 8, y + h / 3);
            path.lineTo(x + w / 8, y + h / 3);
            path.lineTo(x + w / 8 + h / 4, y + h / 3 - h / 4);
            path.moveTo(x + w / 8, y + h / 3);
            path.lineTo(x + w / 8 + h / 4, y + h / 3 + h / 4);
        } else {
            path.moveTo(x + w / 8, y);
            path.lineTo(x + w / 8, y + h / 3);
            path.lineTo(x + w * 7 / 8, y + h / 3);
            path.lineTo(x + w * 7 / 8 - h / 4, y + h / 3 - h / 4);
            path.moveTo(x + w * 7 / 8, y + h / 3);
            path.lineTo(x + w * 7 / 8 - h / 4, y + h / 3 + h / 4);
        }
        painter.drawPath(path);
    }
    painter.restore();
}

void SourceView::keyPressEvent(QKeyEvent *event)
{
    // Undo is handled for all panes at once by the comparison
    if (event->matches(QKeySequence::Undo)) {
        Q_EMIT undoRequested();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Redo)) {
        Q_EMIT redoRequested();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        Q_EMIT escapePressed();
        QPlainTextEdit::keyPressEvent(event);
        return;
    }

    if (editable() && event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier
        && MeldSettings::instance()->boolValue(QStringLiteral("insert-spaces-instead-of-tabs"))) {
        const int tabWidth = std::max(1, MeldSettings::instance()->intValue(QStringLiteral("indent-width")));
        QTextCursor cursor = textCursor();
        const int column = visualColumn(cursor.position());
        cursor.beginEditBlock();
        cursor.insertText(QString(tabWidth - (column % tabWidth), QLatin1Char(' ')));
        cursor.endEditBlock();
        setTextCursor(cursor);
        event->accept();
        return;
    }

    // Each key press is its own undo step, as in Meld. Grouping it in an
    // edit block also stops Qt merging it with an earlier undo command,
    // which would break the cross-pane undo ordering.
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    QPlainTextEdit::keyPressEvent(event);
    cursor.endEditBlock();
}

void SourceView::inputMethodEvent(QInputMethodEvent *event)
{
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    QPlainTextEdit::inputMethodEvent(event);
    cursor.endEditBlock();
}

void SourceView::focusInEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusInEvent(event);
    if (highlightCurrentLine_)
        viewport()->update();
    Q_EMIT focusChanged(true);
}

void SourceView::focusOutEvent(QFocusEvent *event)
{
    QPlainTextEdit::focusOutEvent(event);
    if (highlightCurrentLine_)
        viewport()->update();
    Q_EMIT focusChanged(false);
}

void SourceView::contextMenuEvent(QContextMenuEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    Q_EMIT contextMenuRequested(event->globalPos());
    event->accept();
}

void SourceView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int delta = event->angleDelta().y();
        if (delta != 0)
            updateFontZoom(delta > 0 ? 1 : -1);
        event->accept();
        return;
    }
    QPlainTextEdit::wheelEvent(event);
}

void SourceView::dragEnterEvent(QDragEnterEvent *event)
{
    if (!localFilesFromMime(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragEnterEvent(event);
}

void SourceView::dragMoveEvent(QDragMoveEvent *event)
{
    if (!localFilesFromMime(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
        return;
    }
    QPlainTextEdit::dragMoveEvent(event);
}

void SourceView::dropEvent(QDropEvent *event)
{
    const QList<QString> files = localFilesFromMime(event->mimeData());
    if (!files.isEmpty()) {
        event->acceptProposedAction();
        Q_EMIT filesDropped(files);
        return;
    }
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    QPlainTextEdit::dropEvent(event);
    cursor.endEditBlock();
}

bool SourceView::canInsertFromMimeData(const QMimeData *source) const
{
    return source->hasText();
}

void SourceView::insertFromMimeData(const QMimeData *source)
{
    // Only ever paste plain text, without any formatting
    if (!source->hasText() || !editable())
        return;
    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    cursor.insertText(source->text());
    cursor.endEditBlock();
    setTextCursor(cursor);
    ensureCursorVisible();
}

} // namespace meld
