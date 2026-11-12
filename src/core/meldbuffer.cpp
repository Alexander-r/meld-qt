#include "meldbuffer.h"

#include "archive.h"
#include "misc.h"
#include "syntax/highlighter.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QPlainTextDocumentLayout>
#include <QSignalBlocker>
#include <QTextBlock>

#include <algorithm>
#include <stdexcept>

namespace meld {

Q_LOGGING_CATEGORY(lcBuffer, "meld.buffer")

// ---------------------------------------------------------------------------
// RangeSet

RangeSet::RangeSet(QTextDocument *document)
    : document_(document)
{
}

size_t RangeSet::firstEndingAfter(int position) const
{
    // Ranges don't overlap, so their ends are ordered like their starts
    const auto it = std::partition_point(ranges_.begin(), ranges_.end(),
                                         [position](const QTextCursor &c) { return c.position() <= position; });
    return size_t(it - ranges_.begin());
}

void RangeSet::add(int start, int end)
{
    start = std::max(start, 0);
    end = std::min(end, document_->characterCount() - 1);
    if (start >= end)
        return;

    // Merge with any range that overlaps or touches the new one
    size_t first = firstEndingAfter(start - 1);
    size_t last = first;
    int newStart = start;
    int newEnd = end;
    while (last < ranges_.size() && ranges_[last].anchor() <= end) {
        newStart = std::min(newStart, ranges_[last].anchor());
        newEnd = std::max(newEnd, ranges_[last].position());
        ++last;
    }
    ranges_.erase(ranges_.begin() + qsizetype(first), ranges_.begin() + qsizetype(last));

    QTextCursor cursor(document_);
    cursor.setPosition(newStart);
    cursor.setPosition(newEnd, QTextCursor::KeepAnchor);
    // Text inserted at the end of a range shouldn't extend it
    cursor.setKeepPositionOnInsert(true);
    ranges_.insert(ranges_.begin() + qsizetype(first), cursor);
}

void RangeSet::remove(int start, int end)
{
    if (start >= end || ranges_.empty())
        return;

    size_t index = firstEndingAfter(start);
    std::vector<QTextCursor> replacements;
    size_t last = index;
    while (last < ranges_.size() && ranges_[last].anchor() < end) {
        const int rangeStart = ranges_[last].anchor();
        const int rangeEnd = ranges_[last].position();
        if (rangeStart < start) {
            QTextCursor left(document_);
            left.setPosition(rangeStart);
            left.setPosition(start, QTextCursor::KeepAnchor);
            left.setKeepPositionOnInsert(true);
            replacements.push_back(left);
        }
        if (rangeEnd > end) {
            QTextCursor right(document_);
            right.setPosition(end);
            right.setPosition(rangeEnd, QTextCursor::KeepAnchor);
            right.setKeepPositionOnInsert(true);
            replacements.push_back(right);
        }
        ++last;
    }
    ranges_.erase(ranges_.begin() + qsizetype(index), ranges_.begin() + qsizetype(last));
    ranges_.insert(ranges_.begin() + qsizetype(index), replacements.begin(), replacements.end());
}

void RangeSet::clear()
{
    ranges_.clear();
}

std::vector<std::pair<int, int>> RangeSet::ranges() const
{
    std::vector<std::pair<int, int>> result;
    for (const QTextCursor &cursor : ranges_) {
        if (cursor.anchor() < cursor.position())
            result.emplace_back(cursor.anchor(), cursor.position());
    }
    return result;
}

std::vector<std::pair<int, int>> RangeSet::rangesIn(int start, int end) const
{
    std::vector<std::pair<int, int>> result;
    for (size_t i = firstEndingAfter(start); i < ranges_.size(); ++i) {
        const int rangeStart = ranges_[i].anchor();
        if (rangeStart >= end)
            break;
        const int s = std::max(rangeStart, start);
        const int e = std::min(ranges_[i].position(), end);
        if (s < e)
            result.emplace_back(s, e);
    }
    return result;
}

// ---------------------------------------------------------------------------
// BufferData

QString BufferData::label() const
{
    if (!customLabel.isEmpty())
        return customLabel;
    if (!file.isEmpty())
        return file;
    //: This is the label of a new, currently-unnamed file.
    return QCoreApplication::translate("meld", "<unnamed>");
}

bool BufferData::writable() const
{
    const QString path = target();
    if (path.isEmpty())
        return false;
    // Archives can't be changed, as with the original's gvfs mounts
    if (archive::archiveForPath(path))
        return false;
    const QFileInfo info(path);
    if (!info.exists())
        return true;
    return info.isWritable();
}

void BufferData::reset(const QString &newFile, BufferState newState)
{
    const bool sameFile = !newFile.isEmpty() && newFile == file;
    file = newFile;
    if (!sameFile)
        customLabel.clear();
    state = newState;
    savefile.clear();
    updateMtime();
}

std::optional<QDateTime> BufferData::queryMtime(const QString &path)
{
    if (path.isEmpty())
        return std::nullopt;
    const QFileInfo info(path);
    if (!info.exists())
        return std::nullopt;
    return info.lastModified();
}

void BufferData::updateMtime()
{
    diskMtime = queryMtime(file);
    mtime = diskMtime;
}

// ---------------------------------------------------------------------------
// MeldBuffer

MeldBuffer::MeldBuffer(QObject *parent)
    : QObject(parent)
    , document_(new QTextDocument(this))
    , editCursor_(document_)
    , inline_(document_)
    , dimmed_(document_)
    , highlighter_(new syntax::SyntaxHighlighter(document_))
{
    // A layout is required for the document to emit contentsChange
    document_->setDocumentLayout(new QPlainTextDocumentLayout(document_));
    // GtkSourceView has no document margin; keep a small one for the
    // cursor. Changing the margin is an undoable frame format change that
    // also marks the document as modified, so it's only done here, before
    // undo is enabled.
    document_->setUndoRedoEnabled(false);
    document_->setDocumentMargin(2);
    document_->setUndoRedoEnabled(true);
    document_->setModified(false);
    lastLineCount_ = document_->blockCount();
    connect(document_, &QTextDocument::contentsChange, this, &MeldBuffer::onContentsChange);
    connect(highlighter_, &syntax::SyntaxHighlighter::languageChanged, this, &MeldBuffer::languageChanged);
}

MeldBuffer::~MeldBuffer() = default;

const syntax::Language *MeldBuffer::language() const
{
    return highlighter_->language();
}

void MeldBuffer::setLanguage(const syntax::Language *language)
{
    highlighter_->setLanguage(language);
}

void MeldBuffer::onContentsChange(int position, int charsRemoved, int charsAdded)
{
    Q_UNUSED(charsRemoved);
    const int newCount = document_->blockCount();
    QTextBlock startBlock = document_->findBlock(position);
    if (!startBlock.isValid())
        startBlock = document_->lastBlock();
    QTextBlock endBlock = document_->findBlock(position + charsAdded);
    if (!endBlock.isValid())
        endBlock = document_->lastBlock();

    const int startLine = startBlock.blockNumber();
    const int newEnd = std::max(endBlock.blockNumber(), startLine);
    const int delta = newCount - lastLineCount_;
    const int oldEnd = std::max(newEnd - delta, startLine);
    lastLineCount_ = newCount;
    Q_EMIT linesChanged(startLine, oldEnd - startLine, newEnd - startLine);
}

QString MeldBuffer::lineText(int line) const
{
    const QTextBlock block = document_->findBlockByNumber(line);
    return block.isValid() ? block.text() : QString();
}

QString MeldBuffer::text() const
{
    QString result = document_->toRawText();
    result.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return result;
}

QString MeldBuffer::textBetween(int start, int end) const
{
    start = std::clamp(start, 0, endPosition());
    end = std::clamp(end, 0, endPosition());
    if (end <= start)
        return {};
    QTextCursor cursor(document_);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    QString result = cursor.selectedText();
    result.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return result;
}

void MeldBuffer::setText(const QString &text)
{
    document_->setPlainText(text);
}

void MeldBuffer::replaceAllText(const QString &text)
{
    beginUserAction();
    editCursor_.select(QTextCursor::Document);
    editCursor_.insertText(text);
    endUserAction();
}

int MeldBuffer::positionAtLineOrEof(int line) const
{
    if (line >= lineCount())
        return endPosition();
    return positionAtLine(line);
}

int MeldBuffer::positionAtLine(int line) const
{
    if (line >= lineCount())
        return endPosition();
    const QTextBlock block = document_->findBlockByNumber(std::max(line, 0));
    return block.isValid() ? block.position() : 0;
}

int MeldBuffer::lineAtPosition(int position) const
{
    const QTextBlock block = document_->findBlock(std::clamp(position, 0, endPosition()));
    return block.isValid() ? block.blockNumber() : lineCount() - 1;
}

void MeldBuffer::insertText(int position, const QString &text)
{
    editCursor_.setPosition(std::clamp(position, 0, endPosition()));
    editCursor_.insertText(text);
}

void MeldBuffer::deleteRange(int start, int end)
{
    start = std::clamp(start, 0, endPosition());
    end = std::clamp(end, 0, endPosition());
    if (start == end)
        return;
    editCursor_.setPosition(start);
    editCursor_.setPosition(end, QTextCursor::KeepAnchor);
    editCursor_.removeSelectedText();
}

int MeldBuffer::insertAtLine(int line, const QString &text)
{
    QString toInsert = text;
    if (line >= lineCount()) {
        // TODO: We need to insert a linebreak here, but there is no way to
        // be certain what kind of linebreak to use.
        toInsert = QLatin1Char('\n') + toInsert;
    }
    const int position = positionAtLineOrEof(line);
    insertText(position, toInsert);
    return editCursor_.position();
}

void MeldBuffer::beginUserAction()
{
    editCursor_.beginEditBlock();
}

void MeldBuffer::endUserAction()
{
    editCursor_.endEditBlock();
}

void MeldBuffer::setModified(bool modified)
{
    document_->setModified(modified);
}

// ---------------------------------------------------------------------------
// BufferLines

BufferLines::BufferLines(MeldBuffer *buffer, Filter filter, bool cacheDebug)
    : buffer_(buffer)
    , filter_(std::move(filter))
    , cacheDebug_(cacheDebug)
    , cache_(size_t(buffer->lineCount()))
{
    connection_ = QObject::connect(buffer, &MeldBuffer::linesChanged, buffer,
                                   [this](int start, int removed, int added) {
                                       onLinesChanged(start, removed, added);
                                   });
}

BufferLines::~BufferLines()
{
    QObject::disconnect(connection_);
}

void BufferLines::clearCache()
{
    cache_.assign(size_t(buffer_->lineCount()), std::nullopt);
}

void BufferLines::onLinesChanged(int startLine, int linesRemoved, int linesAdded)
{
    // Replace the changed lines of the cache with empty entries. In the
    // single-line case this is a single element substitution; multi-line
    // inserts replace the insertion point line with several empty lines.
    const int size = int(cache_.size());
    const int lo = std::min(startLine, size);
    const int hi = std::min(startLine + linesRemoved + 1, size);
    cache_.erase(cache_.begin() + lo, cache_.begin() + std::max(lo, hi));
    cache_.insert(cache_.begin() + lo, size_t(linesAdded + 1), std::nullopt);

    if (cacheDebug_ && int(cache_.size()) != lineCount()) {
        qCCritical(lcBuffer).noquote()
            << QStringLiteral("Cache line count does not match buffer line count: %1 != %2")
                   .arg(cache_.size())
                   .arg(lineCount());
    }
}

QString BufferLines::line(int index) const
{
    if (index < 0 || index >= lineCount())
        return {};
    if (size_t(index) >= cache_.size())
        cache_.resize(size_t(lineCount()));

    std::optional<QString> &cached = cache_[size_t(index)];
    if (!cached) {
        const QTextBlock block = buffer_->document()->findBlockByNumber(index);
        QString text = block.text();
        if (filter_) {
            const int start = block.position();
            text = filter_(text, start, start + int(block.text().size()));
        }
        cached = std::move(text);
    }
    return *cached;
}

QStringList BufferLines::lines(int lo, int hi) const
{
    auto [l, h] = std::pair{lo, hi};
    const int count = lineCount();
    l = std::clamp(l, 0, count);
    h = std::clamp(h, 0, count);
    QStringList result;
    if (h <= l)
        return result;
    result.reserve(h - l);
    for (int i = l; i < h; ++i)
        result.append(line(i));
    return result;
}

QString BufferLines::at(int index) const
{
    if (index >= lineCount())
        throw std::out_of_range("BufferLines index out of range");
    if (index < 0)
        index += lineCount();
    if (index < 0)
        throw std::out_of_range("BufferLines index out of range");
    return line(index);
}

// ---------------------------------------------------------------------------
// UndoSequence

UndoSequence::UndoSequence(QObject *parent)
    : QObject(parent)
{
}

void UndoSequence::addDocument(QTextDocument *document)
{
    documents_.push_back(document);
    connect(document, &QTextDocument::undoCommandAdded, this,
            [this, document]() { onUndoCommandAdded(document); });
}

void UndoSequence::clear()
{
    const bool couldUndo = canUndo();
    const bool couldRedo = canRedo();
    actions_.clear();
    nextRedo_ = 0;
    for (QTextDocument *document : documents_) {
        // Clearing the stacks would leave Qt's clean-state marker pointing
        // at a stale index; re-establish the current modification state.
        const QSignalBlocker blocker(document);
        const bool modified = document->isModified();
        document->clearUndoRedoStacks();
        document->setModified(!modified);
        document->setModified(modified);
    }
    if (couldUndo)
        Q_EMIT canUndoChanged(false);
    if (couldRedo)
        Q_EMIT canRedoChanged(false);
}

void UndoSequence::onUndoCommandAdded(QTextDocument *document)
{
    if (busy_)
        return;
    const bool couldUndo = canUndo();
    const bool couldRedo = canRedo();
    actions_.resize(size_t(nextRedo_));
    actions_.push_back(document);
    ++nextRedo_;
    if (!couldUndo)
        Q_EMIT canUndoChanged(true);
    if (couldRedo)
        Q_EMIT canRedoChanged(false);
}

QTextDocument *UndoSequence::undo()
{
    if (!canUndo())
        return nullptr;
    const bool couldRedo = canRedo();
    --nextRedo_;
    QTextDocument *document = actions_[size_t(nextRedo_)];
    busy_ = true;
    document->undo();
    busy_ = false;
    if (!canUndo())
        Q_EMIT canUndoChanged(false);
    if (!couldRedo)
        Q_EMIT canRedoChanged(true);
    return document;
}

QTextDocument *UndoSequence::redo()
{
    if (!canRedo())
        return nullptr;
    const bool couldUndo = canUndo();
    QTextDocument *document = actions_[size_t(nextRedo_)];
    ++nextRedo_;
    busy_ = true;
    document->redo();
    busy_ = false;
    if (!couldUndo)
        Q_EMIT canUndoChanged(true);
    if (!canRedo())
        Q_EMIT canRedoChanged(false);
    return document;
}

// ---------------------------------------------------------------------------
// Text filtering

QString filterBufferText(const QString &text, RangeSet &dimmed, int start, int end,
                         const QList<QRegularExpression> &regexes, bool *ok)
{
    dimmed.remove(start, end);
    const auto highlighter = [&dimmed, start](int filterStart, int filterEnd) {
        dimmed.add(start + filterStart, start + filterEnd);
    };
    return applyTextFilters(text, regexes, highlighter, ok);
}

// ---------------------------------------------------------------------------
// Chunk operations

namespace chunkops {

EditResult copyChunk(MeldBuffer *source, MeldBuffer *target, const DiffChunk &chunk, bool copyUp)
{
    const int start = source->positionAtLineOrEof(chunk.start_a);
    const int end = source->positionAtLineOrEof(chunk.end_a);
    QString text = source->textBetween(start, end);

    EditResult result;
    target->beginUserAction();
    if (copyUp) {
        if (chunk.end_a >= source->lineCount() && chunk.start_b < target->lineCount()) {
            // TODO: We need to insert a linebreak here, but there is no way
            // to be certain what kind of linebreak to use.
            text += QLatin1Char('\n');
        }
        result.start = target->positionAtLineOrEof(chunk.start_b);
        result.end = target->insertAtLine(chunk.start_b, text);
    } else {
        result.start = target->positionAtLineOrEof(chunk.end_b);
        result.end = target->insertAtLine(chunk.end_b, text);
    }
    target->endUserAction();
    return result;
}

EditResult replaceChunk(MeldBuffer *source, MeldBuffer *target, const DiffChunk &chunk)
{
    const int srcStart = source->positionAtLineOrEof(chunk.start_a);
    const int srcEnd = source->positionAtLineOrEof(chunk.end_a);
    const int dstStart = target->positionAtLineOrEof(chunk.start_b);
    const int dstEnd = target->positionAtLineOrEof(chunk.end_b);
    const QString text = source->textBetween(srcStart, srcEnd);

    EditResult result;
    result.start = dstStart;
    target->beginUserAction();
    target->deleteRange(dstStart, dstEnd);
    result.end = target->insertAtLine(chunk.start_b, text);
    target->endUserAction();
    return result;
}

EditResult deleteChunk(MeldBuffer *buffer, const DiffChunk &chunk)
{
    int start = buffer->positionAtLineOrEof(chunk.start_a);
    if (chunk.end_a >= buffer->lineCount() && start > 0) {
        // If this is the end of the buffer, we need to remove the previous
        // newline, because the current line has none.
        start -= 1;
    }
    const int end = buffer->positionAtLineOrEof(chunk.end_a);
    buffer->beginUserAction();
    buffer->deleteRange(start, end);
    buffer->endUserAction();
    return {start, start};
}

} // namespace chunkops

} // namespace meld
