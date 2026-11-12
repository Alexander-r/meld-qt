#include "filediff.h"

#include "chunkmap.h"
#include "conflictcomparison.h"
#include "core/archive.h"
#include "core/conflictmarkers.h"
#include "core/iohelpers.h"
#include "core/merge.h"
#include "core/misc.h"
#include "core/syntax/language.h"
#include "externalhelpers.h"
#include "findbar.h"
#include "icons.h"
#include "inlinematcher.h"
#include "msgarea.h"
#include "patchdialog.h"
#include "pathlabel.h"
#include "settings.h"
#include "sourceview.h"
#include "statusbar.h"
#include "style.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace meld {

namespace {

//! Line length at which we'll cancel loads because of potential hangs
constexpr int LineLengthLimit = 16 * 1024;
constexpr int MaskShift = 1;
constexpr int MaskCtrl = 2;

QString textFilterActionName(int index)
{
    return QStringLiteral("text-custom-filter-%1").arg(index);
}

QString fileId(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : QString();
}

} // namespace

// ---------------------------------------------------------------------------
// Keyboard modifiers and focus

bool FileDiff::eventFilter(QObject *watched, QEvent *event)
{
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) && isVisible()) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && widget->window() == window()) {
            int mod = 0;
            if (keyEvent->key() == Qt::Key_Shift)
                mod = MaskShift;
            else if (keyEvent->key() == Qt::Key_Control)
                mod = MaskCtrl;
            if (mod) {
                if (event->type() == QEvent::KeyPress)
                    setKeymask(keymask_ | mod);
                else
                    setKeymask(keymask_ & ~mod);
            }
        }
    } else if (event->type() == QEvent::ApplicationDeactivate || event->type() == QEvent::WindowDeactivate) {
        if (keymask_)
            setKeymask(0);
    }
    return MeldDoc::eventFilter(watched, event);
}

void FileDiff::setKeymask(int mask)
{
    keymask_ = mask;
    if (mask & MaskShift)
        setActionMode(ActionMode::Delete);
    else if (mask & MaskCtrl)
        setActionMode(ActionMode::Insert);
    else
        setActionMode(ActionMode::Replace);
}

void FileDiff::setActionMode(ActionMode mode)
{
    actionMode_ = mode;
    for (ActionGutter *gutter : actiongutter_)
        gutter->setActionMode(mode);
}

int FileDiff::focusedPane(bool useLastFocusedPane) const
{
    const QWidget *focus = window() ? window()->focusWidget() : nullptr;
    for (int i = 0; i < numPanes_; ++i) {
        if (focus == textview_[size_t(i)] && textview_[size_t(i)]->isEnabled())
            return i;
    }
    if (useLastFocusedPane && focusPane_) {
        for (int i = 0; i < numPanes_; ++i) {
            if (focusPane_ == textview_[size_t(i)])
                return i;
        }
    }
    return -1;
}

QWidget *FileDiff::focusPane() const
{
    if (focusPane_)
        return focusPane_;
    return textview_[size_t(numPanes_ > 1 ? 1 : 0)];
}

void FileDiff::onContainerSwitchIn()
{
    MeldDoc::onContainerSwitchIn();
}

void FileDiff::setFocusedTextview(SourceView *view)
{
    if (view) {
        focusPane_ = view;
        findbar_->setTextView(view);
        for (int pane = 0; pane < 3; ++pane) {
            if (textview_[size_t(pane)] == view)
                onCursorPositionChanged(pane, true);
        }
    } else {
        setKeymask(0);
    }

    onCurrentDiffChanged();
    setSaveActionSensitivity();
    setMergeActionSensitivity();
    setExternalActionSensitivity();
}

// ---------------------------------------------------------------------------
// Text filters

QList<QPair<QString, QString>> FileDiff::textFilterActions() const
{
    QList<QPair<QString, QString>> result;
    for (int i = 0; i < textFilters_.size(); ++i)
        result.append({textFilterActionName(i), textFilters_.at(i).label});
    return result;
}

bool FileDiff::createTextFilters()
{
    MeldSettings *settings = MeldSettings::instance();

    // In contrast to file filters, ordering of text filters can matter
    QStringList oldActive;
    for (const FilterEntry &f : std::as_const(textFilters_)) {
        if (f.active)
            oldActive.append(f.filterString);
    }
    QStringList newActive;
    for (const FilterEntry &f : settings->textFilters()) {
        if (f.active)
            newActive.append(f.filterString);
    }
    const bool activeFiltersChanged = oldActive != newActive;

    for (int i = 0; i < textFilters_.size(); ++i)
        removeAction(textFilterActionName(i));

    textFilters_ = settings->textFilters();
    for (int i = 0; i < textFilters_.size(); ++i) {
        QAction *a = addToggleAction(textFilterActionName(i), textFilters_.at(i).active, [this, i](bool checked) {
            if (i < textFilters_.size()) {
                textFilters_[i].active = checked;
                refreshComparison();
            }
        });
        a->setText(textFilters_.at(i).label);
        a->setEnabled(textFilters_.at(i).filter.has_value());
    }
    return activeFiltersChanged;
}

QString FileDiff::filterText(int pane, const QString &text, int start, int end)
{
    QList<QRegularExpression> regexes;
    for (const FilterEntry &f : std::as_const(textFilters_)) {
        if (f.active && f.filter)
            regexes.append(*f.filter);
    }
    MeldBuffer *buffer = textbuffer_[size_t(pane)];
    if (regexes.isEmpty()) {
        buffer->dimmedRanges().remove(start, end);
        return text;
    }
    bool ok = true;
    const QString result = filterBufferText(text, buffer->dimmedRanges(), start, end, regexes, &ok);
    if (!ok && !warnedBadComparison_) {
        warnedBadComparison_ = true;
        QMessageBox *box = new QMessageBox(QMessageBox::Warning, tr("Comparison results will be inaccurate"),
                                           tr("A filter changed the number of lines in the file, which is "
                                              "unsupported. The comparison will not be accurate."),
                                           QMessageBox::Ok, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    }
    return result;
}

// ---------------------------------------------------------------------------
// Buffer handlers

void FileDiff::disconnectBufferHandlers()
{
    for (SourceView *view : textview_)
        view->setEnabled(false);
    buffersConnected_ = false;
}

void FileDiff::connectBufferHandlers()
{
    for (SourceView *view : textview_)
        view->setEnabled(true);
    buffersConnected_ = true;

    if (comparisonMode_ == FileComparisonMode::AutoMerge) {
        textview_[0]->setEditable(false);
        textview_[2]->setEditable(false);
    }
}

void FileDiff::onLinesChanged(int pane, int startLine, int linesRemoved, int linesAdded)
{
    if (!buffersConnected_ || pane >= numPanes_ || numPanes_ <= 1)
        return;

    // Combined replacements (which GTK reports as separate deletions and
    // insertions) may modify several chunks; remember the edited range so
    // that their highlighting is refreshed.
    if (linesRemoved > 0 && linesAdded > 0)
        lastEdit_ = std::pair{pane, std::pair{startLine, startLine + linesAdded}};
    if (linediffer_->syncpoints.empty()) {
        Texts texts;
        for (int i = 0; i < numPanes_; ++i)
            texts.push_back(bufferFiltered_[size_t(i)].get());
        linediffer_->replaceInSequence(pane, startLine, linesRemoved, linesAdded, texts);
    }
    lastEdit_.reset();
    // TODO: We should have a diff-changed signal for the current buffer
    // instead of passing everything through cursor change logic.
    const int focused = focusedPane();
    if (focused != -1)
        onCursorPositionChanged(focused, true);
}

// ---------------------------------------------------------------------------
// Cursor tracking

void FileDiff::updateStatusbarCursor(int pane)
{
    SourceView *view = textview_[size_t(pane)];
    const QTextCursor cursor = view->textCursor();
    statusbar_[size_t(pane)]->setCursorPosition(cursor.blockNumber(), view->visualColumn(cursor.position()));
}

void FileDiff::onCursorPositionChanged(int pane, bool force)
{
    // Avoid storing cursor changes for non-focused panes. These happen when
    // we e.g., copy a chunk between panes.
    if (!focusPane_ || focusPane_ != textview_[size_t(pane)])
        return;

    const int pos = textview_[size_t(pane)]->textCursor().position();
    if (cursor_.pane == pane && cursor_.pos == pos && !force)
        return;
    cursor_.pane = pane;
    cursor_.pos = pos;

    const int line = textview_[size_t(pane)]->textCursor().blockNumber();

    if (cursor_.line != line || force) {
        const ChunkLocation location = linediffer_->locateChunk(pane, line);
        if (location.chunk != cursor_.chunk || force) {
            cursor_.chunk = location.chunk;
            onCurrentDiffChanged();
        }
        if (location.prev != cursor_.prev || location.next != cursor_.next || force) {
            setActionEnabled(QStringLiteral("previous-change"), location.prev.has_value());
            setActionEnabled(QStringLiteral("next-change"), location.next.has_value());
        }

        std::optional<int> prevConflict;
        std::optional<int> nextConflict;
        for (int conflict : linediffer_->conflicts) {
            if (location.prev && conflict <= *location.prev)
                prevConflict = conflict;
            if (location.next && conflict >= *location.next) {
                nextConflict = conflict;
                break;
            }
        }
        if (prevConflict != cursor_.prevConflict || nextConflict != cursor_.nextConflict || force)
            Q_EMIT nextConflictChanged(prevConflict.has_value(), nextConflict.has_value());

        cursor_.prev = location.prev;
        cursor_.next = location.next;
        cursor_.prevConflict = prevConflict;
        cursor_.nextConflict = nextConflict;
    }
    cursor_.line = line;
}

void FileDiff::onCurrentDiffChanged()
{
    int pane = -1;
    for (int i = 0; i < 3; ++i) {
        if (focusPane_ && focusPane_ == textview_[size_t(i)])
            pane = i;
    }
    std::optional<int> chunkId;
    if (pane != -1) {
        // While this *should* be redundant, it's possible for focus pane and
        // cursor pane to be different in several situations.
        pane = cursor_.pane.value_or(pane);
        chunkId = cursor_.chunk;
    }

    bool pushLeft = false, pushRight = false, pullLeft = false, pullRight = false;
    bool deleteChunkEnabled = false, copyLeft = false, copyRight = false;
    if (pane != -1 && chunkId) {
        const bool threeWay = numPanes_ == 3;

        // Push and Delete are active if the current pane has something to act
        // on, and the target pane exists and is editable. Pull is sensitive
        // if the source pane has something to get, and the current pane is
        // editable. Copy actions are sensitive if the conditions for push are
        // met, *and* there is some content in the target pane.
        const bool editable = textview_[size_t(pane)]->editable();
        const bool editableLeft = pane > 0 && textview_[size_t(pane - 1)]->editable();
        const bool editableRight = pane < numPanes_ - 1 && textview_[size_t(pane + 1)]->editable();
        if (pane == 0 || pane == 2) {
            const OptChunk chunk = linediffer_->getChunk(*chunkId, pane);
            if (chunk) {
                const bool isInsert = chunk->start_a == chunk->end_a;
                const bool isDelete = chunk->start_b == chunk->end_b;
                pushLeft = editableLeft;
                pushRight = editableRight;
                pullLeft = pane == 2 && editable && !isDelete;
                pullRight = pane == 0 && editable && !isDelete;
                deleteChunkEnabled = editable && !isInsert;
                copyLeft = editableLeft && !(isInsert || isDelete);
                copyRight = editableRight && !(isInsert || isDelete);
            }
        } else if (pane == 1) {
            const OptChunk chunk0 = linediffer_->getChunk(*chunkId, 1, 0);
            OptChunk chunk2;
            if (threeWay)
                chunk2 = linediffer_->getChunk(*chunkId, 1, 2);
            const bool leftMidExists = chunk0 && chunk0->start_a != chunk0->end_a;
            const bool leftExists = chunk0 && chunk0->start_b != chunk0->end_b;
            const bool rightMidExists = chunk2 && chunk2->start_a != chunk2->end_a;
            const bool rightExists = chunk2 && chunk2->start_b != chunk2->end_b;
            pushLeft = editableLeft && (!threeWay || chunk0);
            pushRight = editableRight && (!threeWay || chunk2);
            pullLeft = editable && leftExists;
            pullRight = editable && rightExists;
            deleteChunkEnabled = editable && (leftMidExists || rightMidExists);
            copyLeft = editableLeft && leftMidExists && leftExists;
            copyRight = editableRight && rightMidExists && rightExists;
        }

        // If there is chunk and there are only two panes (#25)
        if (numPanes_ == 2) {
            pushLeft = textview_[0]->editable();
            pushRight = textview_[1]->editable();
        }
    }

    setActionEnabled(QStringLiteral("file-push-left"), pushLeft);
    setActionEnabled(QStringLiteral("file-push-right"), pushRight);
    setActionEnabled(QStringLiteral("file-pull-left"), pullLeft);
    setActionEnabled(QStringLiteral("file-pull-right"), pullRight);
    setActionEnabled(QStringLiteral("file-delete"), deleteChunkEnabled);
    setActionEnabled(QStringLiteral("file-copy-left-up"), copyLeft);
    setActionEnabled(QStringLiteral("file-copy-left-down"), copyLeft);
    setActionEnabled(QStringLiteral("file-copy-right-up"), copyRight);
    setActionEnabled(QStringLiteral("file-copy-right-down"), copyRight);
    setActionEnabled(QStringLiteral("previous-pane"), pane > 0);
    setActionEnabled(QStringLiteral("next-pane"), pane != -1 && pane < numPanes_ - 1);
    setActionEnabled(QStringLiteral("swap-2-panes"), numPanes_ == 2);

    updateTextActionsSensitivity();
    // FIXME: don't queue_draw() on everything... just on what changed
    queueDraw();
}

void FileDiff::updateTextActionsSensitivity()
{
    bool cut = false, copy = false, paste = false;
    if (focusPane_) {
        cut = copy = focusPane_->textCursor().hasSelection();
        paste = focusPane_->editable();
    }
    setActionEnabled(QStringLiteral("cut"), cut && paste);
    setActionEnabled(QStringLiteral("copy"), copy);
    setActionEnabled(QStringLiteral("paste"), paste);
}

void FileDiff::setSaveActionSensitivity()
{
    const int pane = focusedPane();
    bool anyModified = false;
    for (int i = 0; i < numPanes_; ++i)
        anyModified = anyModified || textbuffer_[size_t(i)]->isModified();
    setActionEnabled(QStringLiteral("save"), pane != -1 && textbuffer_[size_t(pane)]->isModified());
    setActionEnabled(QStringLiteral("save-all"), anyModified);
}

void FileDiff::setMergeActionSensitivity()
{
    bool editable = false;
    std::pair<bool, bool> mergeable{false, false};
    if (focusPane_) {
        editable = focusPane_->editable();
        for (int i = 0; i < numPanes_; ++i) {
            if (focusPane_ == textview_[size_t(i)])
                mergeable = linediffer_->hasMergeableChanges(i);
        }
    }
    setActionEnabled(QStringLiteral("merge-all-left"), mergeable.first && editable);
    setActionEnabled(QStringLiteral("merge-all-right"), mergeable.second && editable);

    if (numPanes_ == 3 && textview_[1]->editable())
        mergeable = linediffer_->hasMergeableChanges(1);
    else
        mergeable = {false, false};
    setActionEnabled(QStringLiteral("merge-all"), mergeable.first || mergeable.second);
}

void FileDiff::setExternalActionSensitivity()
{
    // FIXME: This sensitivity is very confused, as in the original
    setActionEnabled(QStringLiteral("open-external"), !focusPane_.isNull());
}

// ---------------------------------------------------------------------------
// Navigation

void FileDiff::scrollToChunkIndex(int chunkIndex)
{
    // Scrolls chunks with the given index on screen in all panes
    const auto starts = linediffer_->getChunkStarts(chunkIndex);
    for (int pane = 0; pane < numPanes_; ++pane) {
        if (starts[size_t(pane)])
            textview_[size_t(pane)]->scrollToLineCentered(*starts[size_t(pane)]);
    }
}

void FileDiff::goToChunk(std::optional<int> target, std::optional<int> paneOpt, bool centered)
{
    Q_UNUSED(centered);
    if (!target) {
        errorBell();
        return;
    }

    int pane = paneOpt.value_or(-1);
    if (!paneOpt) {
        pane = focusedPane();
        if (pane == -1)
            pane = numPanes_ > 1 ? 1 : 0;
    }

    const OptChunk chunk = linediffer_->getChunk(*target, pane);
    if (!chunk) {
        errorBell();
        return;
    }

    // Warp the cursor to the first line of the chunk
    SourceView *view = textview_[size_t(pane)];
    if (cursor_.line != chunk->start_a)
        view->placeCursorAtLine(chunk->start_a);

    // Scroll all panes to the given chunk, and then ensure that the newly
    // placed cursor is definitely on-screen.
    scrollToChunkIndex(*target);
    view->scrollToLineCentered(chunk->start_a);

    // If we've moved to a valid chunk (or stayed in the first/last chunk)
    // then briefly highlight the chunk for better visual orientation.
    MeldBuffer *buf = textbuffer_[size_t(pane)];
    view->addFadingHighlight(buf->positionAtLineOrEof(chunk->start_a), buf->positionAtLineOrEof(chunk->end_a),
                             currentTheme().focusHighlight, 400, SourceView::AnimationType::Stroke, 0.3);
}

bool FileDiff::isChunkInArea(std::optional<int> chunkId, int pane, qreal top, qreal bottom) const
{
    if (!chunkId)
        return false;
    const OptChunk chunk = linediffer_->getChunk(*chunkId, pane);
    if (!chunk)
        return false;
    const qreal y = textview_[size_t(pane)]->yForLine(chunk->start_a);
    return top <= y && y <= bottom;
}

void FileDiff::nextDiff(bool down, bool centered, bool useViewport)
{
    // useViewport: seek next and previous diffs based on where the user is
    // currently scrolling at.
    std::optional<int> target = down ? cursor_.next : cursor_.prev;

    if (useViewport) {
        const int pane = cursor_.pane.value_or(numPanes_ > 1 ? 1 : 0);
        SourceView *view = textview_[size_t(pane)];
        const qreal height = view->viewport()->height();

        // Only do viewport-relative calculations if the chunk we'd otherwise
        // scroll to is *not* on screen. This avoids 3-way comparison cases
        // where scrolling won't go past a chunk because the scroll doesn't go
        // past 50% of the screen.
        if (!isChunkInArea(target, pane, 0, height)) {
            const int halfline = view->lineAtY(height / 2);
            const ChunkLocation location = linediffer_->locateChunk(1, halfline);
            target = down ? location.next : location.prev;
        }
    }

    goToChunk(target, std::nullopt, centered);
}

DiffChunk FileDiff::synthChunk(int pane0, int pane1, int line) const
{
    // Returns the Same chunk that would exist at the given location if we
    // didn't remove Same chunks
    int start0 = 0;
    int end0 = textbuffer_[size_t(pane0)]->lineCount() - 1;
    int start1 = 0;
    int end1 = textbuffer_[size_t(pane1)]->lineCount() - 1;

    const ChunkLocation location = linediffer_->locateChunk(pane0, line);
    if (location.prev) {
        int prev = *location.prev;
        while (prev >= 0) {
            const OptChunk prev0 = linediffer_->getChunk(prev, pane0, pane1);
            const OptChunk prev1 = linediffer_->getChunk(prev, pane1, pane0);
            if (prev0 && prev1) {
                start0 = prev0->end_a;
                start1 = prev1->end_a;
                break;
            }
            --prev;
        }
    }
    if (location.next) {
        int next = *location.next;
        while (next < linediffer_->diffCount()) {
            const OptChunk next0 = linediffer_->getChunk(next, pane0, pane1);
            const OptChunk next1 = linediffer_->getChunk(next, pane1, pane0);
            if (next0 && next1) {
                end0 = next0->start_a;
                end1 = next1->start_a;
                break;
            }
            ++next;
        }
    }
    return {ChunkTag::Equal, start0, end0, start1, end1};
}

int FileDiff::correspondingChunkLine(std::optional<int> chunk, int line, int pane, int newPane) const
{
    // Special-case cross-pane jumps
    if ((pane == 0 && newPane == 2) || (pane == 2 && newPane == 0)) {
        const int proxy = correspondingChunkLine(chunk, line, pane, 1);
        return correspondingChunkLine(chunk, proxy, 1, newPane);
    }

    // Either we are currently in a identifiable chunk, or we are in a Same
    // chunk; if we establish the start/end of that chunk in both panes, we
    // can figure out what our new offset should be.
    OptChunk curChunk;
    if (chunk)
        curChunk = linediffer_->getChunk(*chunk, pane, newPane);
    if (!curChunk)
        curChunk = synthChunk(pane, newPane, line);
    const int curStart = curChunk->start_a;
    const int curEnd = curChunk->end_a;
    const int newStart = curChunk->start_b;
    const int newEnd = curChunk->end_b;

    // If the new buffer's current cursor is already in the correct chunk,
    // assume that we have in-progress editing, and don't move it.
    const int cursorLine = textview_[size_t(newPane)]->textCursor().blockNumber();
    const std::optional<int> cursorChunk = linediffer_->locateChunk(newPane, cursorLine).chunk;
    bool alreadyInChunk;
    if (cursorChunk) {
        alreadyInChunk = cursorChunk == chunk;
    } else {
        const DiffChunk synth = synthChunk(pane, newPane, cursorLine);
        alreadyInChunk = synth.start_b == newStart && synth.end_b == newEnd;
    }

    if (alreadyInChunk)
        return cursorLine;

    // Guess where to put the cursor: in the same chunk, at about the same
    // place within the chunk, calculated proportionally by line. Insert
    // chunks and one-line chunks are placed at the top.
    const double chunkOffset = curEnd == curStart ? 0.0 : double(line - curStart) / double(curEnd - curStart);
    return newStart + int(chunkOffset * (newEnd - newStart));
}

void FileDiff::moveCursor(int pane, int line, bool focus)
{
    SourceView *view = textview_[size_t(pane)];
    if (focus)
        view->setFocus();
    view->placeCursorAtLine(line);
    view->scrollToLineCentered(line);
}

void FileDiff::moveCursorPane(int pane, int newPane)
{
    const int newLine = correspondingChunkLine(cursor_.chunk, cursor_.line.value_or(0), pane, newPane);
    moveCursor(newPane, newLine);
}

// ---------------------------------------------------------------------------
// Chunk actions

std::optional<DiffChunk> FileDiff::actionChunk(int src, int dst) const
{
    if (src < 0 || src >= numPanes_ || dst < 0 || dst >= numPanes_)
        return std::nullopt; // Action was taken on invalid panes
    if (!cursor_.chunk)
        return std::nullopt; // Action was taken without chunk
    return linediffer_->getChunk(*cursor_.chunk, src, dst);
}

std::pair<int, int> FileDiff::actionPanes(int direction, bool reverse) const
{
    const int src = focusedPane(true);
    const int dst = src + direction;
    return reverse ? std::pair{dst, src} : std::pair{src, dst};
}

void FileDiff::pushChange(int direction)
{
    int src;
    int dst;
    if (numPanes_ == 2) {
        src = direction < 0 ? 1 : 0;
        dst = direction < 0 ? 0 : 1;
    } else {
        std::tie(src, dst) = actionPanes(direction);
    }
    if (const auto chunk = actionChunk(src, dst))
        replaceChunk(src, dst, *chunk);
}

void FileDiff::pullChange(int direction)
{
    const auto [src, dst] = actionPanes(direction, true);
    if (const auto chunk = actionChunk(src, dst))
        replaceChunk(src, dst, *chunk);
}

void FileDiff::copyChange(int direction, bool copyUp)
{
    const auto [src, dst] = actionPanes(direction);
    if (const auto chunk = actionChunk(src, dst))
        copyChunk(src, dst, *chunk, copyUp);
}

void FileDiff::onChunkActionActivated(ChunkAction action, SourceView *from, SourceView *to, const DiffChunk &chunk)
{
    int fromPane = -1;
    int toPane = -1;
    for (int i = 0; i < 3; ++i) {
        if (textview_[size_t(i)] == from)
            fromPane = i;
        if (textview_[size_t(i)] == to)
            toPane = i;
    }
    if (fromPane < 0 || toPane < 0)
        return;

    switch (action) {
    case ChunkAction::Replace:
        replaceChunk(fromPane, toPane, chunk);
        break;
    case ChunkAction::Delete:
        deleteChunk(fromPane, chunk);
        break;
    case ChunkAction::CopyUp:
        copyChunk(fromPane, toPane, chunk, true);
        break;
    case ChunkAction::CopyDown:
        copyChunk(fromPane, toPane, chunk, false);
        break;
    }
}

void FileDiff::pullAllNonConflictingChanges(int src, int dst)
{
    if (src < 0 || src >= numPanes_ || dst < 0 || dst >= numPanes_)
        return;
    Merger merger;
    merger.setDiffer(linediffer_.get());
    for (int i = 0; i < 3; ++i)
        merger.texts.push_back(bufferTexts_[size_t(i)].get());
    const QString merged = merger.merge2Files(src, dst);
    syncVScrollLock_ = true;
    textbuffer_[size_t(dst)]->replaceAllText(merged);

    QPointer<FileDiff> guard(this);
    scheduler_.addOneShot([guard, src]() {
        if (!guard)
            return;
        guard->syncVScrollLock_ = false;
        guard->syncVScroll(src);
    });
}

void FileDiff::mergeAllChanges()
{
    const int dst = 1;
    Merger merger;
    merger.setDiffer(linediffer_.get());
    for (int i = 0; i < 3; ++i)
        merger.texts.push_back(bufferTexts_[size_t(i)].get());
    const QString merged = merger.merge3Files(false);
    syncVScrollLock_ = true;
    textbuffer_[size_t(dst)]->replaceAllText(merged);

    QPointer<FileDiff> guard(this);
    scheduler_.addOneShot([guard]() {
        if (!guard)
            return;
        guard->syncVScrollLock_ = false;
        guard->syncVScroll(0);
    });
}

void FileDiff::deleteChange()
{
    const int pane = focusedPane();
    if (pane == -1 || !cursor_.chunk)
        return;
    const OptChunk chunk = linediffer_->getChunk(*cursor_.chunk, pane);
    if (!chunk)
        return;
    deleteChunk(pane, *chunk);
}

void FileDiff::copyChunk(int src, int dst, const DiffChunk &chunk, bool copyUp)
{
    const chunkops::EditResult result =
        chunkops::copyChunk(textbuffer_[size_t(src)], textbuffer_[size_t(dst)], chunk, copyUp);
    // FIXME: If the inserted chunk ends up being an insert chunk, then this
    // animation is not visible; this happens often in three-way diffs
    textview_[size_t(dst)]->addFadingHighlight(result.start, result.end, currentTheme().fill(ChunkTag::Insert), 500);
}

void FileDiff::replaceChunk(int src, int dst, const DiffChunk &chunk)
{
    const chunkops::EditResult result =
        chunkops::replaceChunk(textbuffer_[size_t(src)], textbuffer_[size_t(dst)], chunk);
    SourceView *target = textview_[size_t(dst)];
    {
        // Place the target cursor at the start of the replaced chunk
        QTextCursor cursor(textbuffer_[size_t(dst)]->document());
        cursor.setPosition(textbuffer_[size_t(dst)]->positionAtLine(chunk.start_b));
        target->setTextCursor(cursor);
    }
    // TODO: Need a more specific colour here; conflict is wrong
    const ChunkTag colour = chunk.start_a == chunk.end_a ? ChunkTag::Conflict : ChunkTag::Insert;
    target->addFadingHighlight(result.start, result.end, currentTheme().fill(colour), 500);
}

void FileDiff::deleteChunk(int src, const DiffChunk &chunk)
{
    const chunkops::EditResult result = chunkops::deleteChunk(textbuffer_[size_t(src)], chunk);
    // TODO: Need a more specific colour here; conflict is wrong
    textview_[size_t(src)]->addFadingHighlight(result.start, result.end, currentTheme().fill(ChunkTag::Conflict), 500);
}

// ---------------------------------------------------------------------------
// Undo

void FileDiff::scrollToAffected(QTextDocument *document)
{
    for (SourceView *view : textview_) {
        if (view->document() == document)
            view->ensureCursorVisible();
    }
}

void FileDiff::undo()
{
    if (undosequence_.canUndo()) {
        if (QTextDocument *document = undosequence_.undo())
            scrollToAffected(document);
    }
}

void FileDiff::redo()
{
    if (undosequence_.canRedo()) {
        if (QTextDocument *document = undosequence_.redo())
            scrollToAffected(document);
    }
}

// ---------------------------------------------------------------------------
// Misc actions

void FileDiff::copyFullPath()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    const QString file = textbuffer_[size_t(pane)]->data.file;
    if (!file.isEmpty())
        QApplication::clipboard()->setText(QDir::toNativeSeparators(file));
}

void FileDiff::openFolder()
{
    const int pane = focusedPane();
    if (pane == -1)
        return;
    const QString file = textbuffer_[size_t(pane)]->data.file;
    if (!file.isEmpty())
        openContainingFolder(file);
}

void FileDiff::openExternal()
{
    SourceView *view = focusPane_;
    if (!view || !view->buffer() || view->buffer()->data.file.isEmpty())
        return;
    const int line = view->textCursor().blockNumber() + 1;
    openFilesExternal({view->buffer()->data.file}, this, line);
}

void FileDiff::findAction(bool replace)
{
    QString selected;
    const int pane = focusedPane();
    if (pane != -1) {
        const QTextCursor cursor = textview_[size_t(pane)]->textCursor();
        if (cursor.hasSelection())
            selected = cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    }
    findbar_->startFind(focusPane_ ? focusPane_.data() : textview_[size_t(numPanes_ > 1 ? 1 : 0)], replace,
                        selected);
}

void FileDiff::formatAsPatch()
{
    QStringList texts;
    QStringList labels;
    for (int i = 0; i < 3; ++i) {
        texts.append(textbuffer_[size_t(i)]->text());
        labels.append(textbuffer_[size_t(i)]->data.label());
    }
    auto *dialog = new PatchDialog(texts, labels, numPanes_, window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void FileDiff::setSyncpointMenuItems(int pane)
{
    const SyncpointAction current = syncpoints_.action(pane, textview_[size_t(pane)]->textCursor().position());
    for (SyncpointAction a : {SyncpointAction::Move, SyncpointAction::Delete, SyncpointAction::Match,
                              SyncpointAction::Add}) {
        setActionEnabled(syncpointActionName(a), a == current);
    }
}

void FileDiff::showContextMenu(int pane, const QPoint &globalPos)
{
    setSyncpointMenuItems(pane);
    updateTextActionsSensitivity();

    QMenu menu(this);
    const auto add = [&](const QString &name, const QString &label, bool hideWhenDisabled = false) {
        QAction *a = action(name);
        if (hideWhenDisabled && !a->isEnabled())
            return;
        QAction *item = menu.addAction(label);
        item->setEnabled(a->isEnabled());
        connect(item, &QAction::triggered, a, &QAction::trigger);
    };
    add(QStringLiteral("undo"), tr("&Undo"));
    add(QStringLiteral("redo"), tr("&Redo"));
    menu.addSeparator();
    add(QStringLiteral("cut"), tr("Cu&t"));
    add(QStringLiteral("copy"), tr("&Copy"));
    add(QStringLiteral("paste"), tr("&Paste"));
    menu.addSeparator();
    add(QStringLiteral("open-folder"), tr("Open Containing Folder"));
    add(QStringLiteral("copy-full-path"), tr("Copy Full Path"));
    menu.addSeparator();
    add(QStringLiteral("add-sync-point"), tr("Add Synchronization Point"), true);
    add(QStringLiteral("move-sync-point"), tr("Move Synchronization Point"), true);
    add(QStringLiteral("match-sync-point"), tr("Match Synchronization Point"), true);
    add(QStringLiteral("remove-sync-point"), tr("Remove Synchronization Point"), true);
    add(QStringLiteral("clear-sync-point"), tr("Clear Synchronization Points"));
    menu.exec(globalPos);
}

void FileDiff::refreshSyncPoints()
{
    const std::vector<std::vector<int>> validPoints = syncpoints_.validPoints();
    linediffer_->syncpoints.clear();
    if (!validPoints.empty() && numPanes_ >= 2) {
        // Lines are captured now; points are refreshed whenever the
        // comparison is refreshed, which is when they're used.
        for (const std::vector<int> &point : validPoints) {
            Differ::SyncPoint syncpoint;
            syncpoint.push_back({[line = point[1]]() { return line; }, [line = point[0]]() { return line; }});
            if (numPanes_ == 3 && point.size() == 3)
                syncpoint.push_back({[line = point[1]]() { return line; }, [line = point[2]]() { return line; }});
            linediffer_->syncpoints.push_back(syncpoint);
        }
    }

    for (MsgAreaController *mgr : msgarea_) {
        if (!validPoints.empty()) {
            mgr->newFromTextAndIcon(tr("Live comparison updating disabled"),
                                    tr("Live updating of comparisons is disabled when synchronization points are "
                                       "active. You can still manually refresh the comparison, and live updates "
                                       "will resume when synchronization points are cleared."));
            mgr->setMsgId(MsgSyncpoints);
        } else if (mgr->msgId() == MsgSyncpoints) {
            mgr->clear();
        }
    }

    refreshComparison();
}

void FileDiff::swapPanes()
{
    bool haveUnnamed = false;
    bool haveModified = false;
    for (int i = 0; i < numPanes_; ++i) {
        haveUnnamed = haveUnnamed || textbuffer_[size_t(i)]->data.file.isEmpty();
        haveModified = haveModified || textbuffer_[size_t(i)]->isModified();
    }
    if (haveUnnamed || haveModified) {
        QMessageBox::warning(this, tr("Can't swap unsaved files"),
                             tr("Files must be saved to disk before swapping panes."));
        return;
    }
    if (meta_ && !meta_->tabLabel.isEmpty()) {
        QMessageBox::warning(this, tr("Can't swap version control comparisons"),
                             tr("Swapping panes is not supported in version control mode."));
        return;
    }
    const QStringList files{textbuffer_[0]->data.file, textbuffer_[1]->data.file};
    setLabels({filelabel_[1]->customLabel(), filelabel_[0]->customLabel()});
    setFiles({files[1], files[0]});
}

void FileDiff::dropFiles(int pane, const QStringList &files)
{
    if (files.size() == numPanes_) {
        confirmUnsavedChangeAction([this, files]() { setFiles(files); });
    } else if (files.size() == 1) {
        confirmUnsavedChangeAction([this, pane, files]() { setFile(pane, files.first()); }, {pane});
    }
}

// ---------------------------------------------------------------------------
// Labels and files

void FileDiff::setLabels(const QStringList &labels)
{
    for (int i = 0; i < std::min<int>(int(labels.size()), numPanes_); ++i) {
        if (!labels.at(i).isEmpty()) {
            textbuffer_[size_t(i)]->data.customLabel = labels.at(i);
            filelabel_[size_t(i)]->setCustomLabel(labels.at(i));
        }
    }
    recomputeLabel();
}

void FileDiff::setMergeOutputFile(const QString &file)
{
    if (numPanes_ < 2)
        return;
    MeldBuffer *buf = textbuffer_[1];
    buf->data.savefile = file;
    buf->data.customLabel = file;
    updateBufferWritable(1);
    filelabel_[1]->setFile(file);
    recomputeLabel();
}

void FileDiff::recomputeLabel()
{
    setSaveActionSensitivity();
    QStringList filenames;
    for (int i = 0; i < numPanes_; ++i) {
        const BufferData &data = textbuffer_[size_t(i)]->data;
        QString name = data.label();
        // Files in extracted archives are named by the archives, rather
        // than by where they're extracted
        if (name == data.file) {
            if (const QString archivePath = archive::formatArchiveFilePath(name); !archivePath.isEmpty())
                name = archivePath;
        }
        filenames.append(name);
    }
    QStringList shortnames = shortenNames(filenames);

    for (int i = 0; i < numPanes_; ++i) {
        MeldBuffer *buf = textbuffer_[size_t(i)];
        if (buf->isModified())
            shortnames[i] += QLatin1Char('*');
        fileSaveButton_[size_t(i)]->setEnabled(buf->isModified());
        fileSaveButton_[size_t(i)]->setIcon(icons::icon(buf->data.writable()
                                                            ? QStringLiteral("document-save-symbolic")
                                                            : QStringLiteral("document-save-as-symbolic")));
    }

    QList<std::optional<QString>> targets;
    for (int i = 0; i < numPanes_; ++i) {
        const QString target = textbuffer_[size_t(i)]->data.target();
        targets.append(target.isEmpty() ? std::nullopt : std::optional<QString>(target));
    }
    const std::optional<QString> parentPath = findSharedParentPath(targets);
    for (PathLabel *label : filelabel_)
        label->setParentFile(parentPath);

    const QString label = meta_ ? meta_->tabLabel : QString();
    QString title;
    QStringList tooltipNames;
    if (!label.isEmpty()) {
        title = label;
        tooltipNames = {label};
    } else {
        title = shortnames.join(QStringLiteral(" — "));
        tooltipNames = filenames;
    }
    setTabLabel(title, (QStringList{tr("File comparison:")} + tooltipNames).join(QLatin1Char('\n')));
}

std::optional<Comparison> FileDiff::comparison() const
{
    Comparison c;
    c.type = comparisonMode_ == FileComparisonMode::AutoMerge ? RecentType::Merge : RecentType::File;
    for (int i = 0; i < numPanes_; ++i)
        c.paths.append(textbuffer_[size_t(i)]->data.file);
    return c;
}

void FileDiff::setMeta(const ComparisonMeta &meta)
{
    meta_ = meta;
    for (int i = 0; i < std::min<int>(int(meta.labels.size()), 3); ++i)
        filelabel_[size_t(i)]->setCustomLabel(meta.labels.at(i));
    recomputeLabel();
}

void FileDiff::preComparisonInit()
{
    disconnectBufferHandlers();
    linediffer_->clear();
    for (auto &lines : bufferFiltered_)
        lines->clearCache();
    for (MeldBuffer *buf : textbuffer_)
        buf->inlineHighlights().clear();
    for (MsgAreaController *mgr : msgarea_) {
        const std::optional<int> id = mgr->msgId();
        if (id == MsgSame || id == MsgSlowHighlight)
            mgr->clear();
    }
}

void FileDiff::setVirtualFiles(const QMap<int, VirtualFile> &files)
{
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (it.key() >= 0 && it.key() < numPanes_)
            virtualFiles_[size_t(it.key())] = it.value();
    }
}

void FileDiff::setFiles(const QStringList &files, const QList<QByteArray> &encodings)
{
    if (files.size() != numPanes_)
        return;

    preComparisonInit();
    undosequence_.clear();

    QList<int> toLoad;
    for (int pane = 0; pane < numPanes_; ++pane) {
        if (!files.at(pane).isEmpty() || virtualFiles_[size_t(pane)])
            toLoad.append(pane);
        else
            textbuffer_[size_t(pane)]->data.state = BufferState::LoadFinished;
    }

    if (toLoad.isEmpty())
        scheduler_.addTask(compareFilesInternal());

    QPointer<FileDiff> guard(this);
    for (int pane : std::as_const(toLoad)) {
        if (!files.at(pane).isEmpty() && virtualFiles_[size_t(pane)]) {
            virtualFiles_[size_t(pane)].reset();
            filelabel_[size_t(pane)]->setCustomLabel(QString());
        }
        const QString file = files.at(pane).isEmpty() ? QString() : QFileInfo(files.at(pane)).absoluteFilePath();
        const QByteArray encoding = encodings.value(pane);
        prepareLoadInPane(pane, file);
        scheduler_.addOneShot([guard, pane, file, encoding]() {
            if (guard)
                guard->readFileInPane(pane, file, encoding);
        });
    }
}

void FileDiff::setFile(int pane, const QString &file, const QByteArray &encoding)
{
    preComparisonInit();
    undosequence_.clear();
    loadFileInPane(pane, file, encoding);
}

void FileDiff::loadFileInPane(int pane, const QString &rawFile, const QByteArray &encoding)
{
    if (!rawFile.isEmpty() && virtualFiles_[size_t(pane)]) {
        virtualFiles_[size_t(pane)].reset();
        filelabel_[size_t(pane)]->setCustomLabel(QString());
    }
    const QString file = rawFile.isEmpty() ? QString() : QFileInfo(rawFile).absoluteFilePath();
    prepareLoadInPane(pane, file);
    readFileInPane(pane, file, encoding);
}

void FileDiff::prepareLoadInPane(int pane, const QString &file)
{
    msgarea_[size_t(pane)]->clear();

    MeldBuffer *buf = textbuffer_[size_t(pane)];
    if (!buf->data.file.isEmpty())
        watcher_.removePath(buf->data.file);
    buf->data.reset(file, BufferState::Loading);
    fileOpenButton_[size_t(pane)]->setFile(file);
    filelabel_[size_t(pane)]->setParentFile(std::nullopt);
    filelabel_[size_t(pane)]->setFile(file);
    if (const std::optional<VirtualFile> &virtualFile = virtualFiles_[size_t(pane)]; virtualFile && file.isEmpty()) {
        buf->data.customLabel = virtualFile->label;
        filelabel_[size_t(pane)]->setCustomLabel(virtualFile->label);
    }
}

void FileDiff::readFileInPane(int pane, const QString &file, const QByteArray &encoding)
{
    MeldBuffer *buf = textbuffer_[size_t(pane)];
    QString primary = tr("There was a problem opening the file “%1”.").arg(QDir::toNativeSeparators(file));
    QString errorText;
    bool lineTooLong = false;
    std::optional<DecodedText> decoded;

    // Text made in memory, or read from the file
    std::optional<QByteArray> contents;
    std::optional<VirtualFile::Content> made;
    if (const std::optional<VirtualFile> &virtualFile = virtualFiles_[size_t(pane)]; virtualFile && file.isEmpty()) {
        made = virtualFile->load ? virtualFile->load() : VirtualFile::Content{};
        if (!made->errorTitle.isEmpty())
            primary = made->errorTitle;
        else
            primary = tr("There was a problem opening “%1”.").arg(virtualFile->label);
        if (made->data)
            contents = made->data;
        else
            errorText = made->error;
    } else {
        QFile handle(file);
        if (handle.open(QIODevice::ReadOnly))
            contents = handle.readAll();
        else
            errorText = handle.errorString();
    }

    if (contents) {
        const QByteArray &data = *contents;
        QList<QByteArray> candidates;
        if (!encoding.isEmpty()) {
            candidates = {encoding};
        } else {
            for (const QString &charset :
                 MeldSettings::instance()->stringListValue(QStringLiteral("detect-encodings"))) {
                candidates.append(charset.toLatin1());
            }
            candidates.append(defaultCandidateEncodings());
        }
        decoded = decodeText(data, candidates);
        if (!decoded) {
            errorText = tr("The file could not be decoded using the candidate encodings.");
        } else if (!forceLoad_) {
            qsizetype lineStart = 0;
            int lineNumber = 0;
            const QString &text = decoded->text;
            while (lineStart <= text.size()) {
                qsizetype lineEnd = text.indexOf(QLatin1Char('\n'), lineStart);
                if (lineEnd < 0)
                    lineEnd = text.size();
                const qsizetype length = lineEnd - lineStart;
                if (length > LineLengthLimit) {
                    lineTooLong = true;
                    errorText = tr("Line %1 exceeded maximum line length (%2 > %3).\n\nLoading files with very "
                                   "long lines may make Meld Qt slow or unresponsive.")
                                    .arg(lineNumber + 1)
                                    .arg(length)
                                    .arg(LineLengthLimit);
                    break;
                }
                lineStart = lineEnd + 1;
                ++lineNumber;
            }
        }
    }

    if (!errorText.isEmpty()) {
        buf->setText(QString());
        if (lineTooLong) {
            promptLoadLongLines(pane, primary, errorText);
        } else {
            msgarea_[size_t(pane)]->addDismissableMsg(QStringLiteral("dialog-error-symbolic"), primary, errorText);
        }
        buf->data.state = BufferState::LoadError;
    } else {
        buf->setText(decoded->text);
        buf->data.format = decoded->format;
        buf->data.state = BufferState::LoadFinished;
        statusbar_[size_t(pane)]->setEncoding(decoded->format.encoding);

        if (made && !made->notes.isEmpty()) {
            msgarea_[size_t(pane)]->addDismissableMsg(QStringLiteral("dialog-information-symbolic"), made->noteTitle,
                                                      made->notes.join(QLatin1Char('\n')));
        }

        // A file with conflict markers can be merged; files are only
        // checked for them as they're opened
        if (!file.isEmpty() && !msgarea_[size_t(pane)]->hasMessage()) {
            if (const int conflicts = conflict::countConflicts(*contents); conflicts > 0) {
                QPointer<FileDiff> guard(this);
                msgarea_[size_t(pane)]->addActionMsg(
                    QStringLiteral("dialog-warning-symbolic"),
                    tr("%1 has %n unresolved merge conflicts.", nullptr, conflicts).arg(QFileInfo(file).fileName()),
                    tr("The local and remote versions can be merged in a 3-way comparison."), tr("Resolve"),
                    [guard, file]() {
                        if (guard)
                            Q_EMIT guard->createDiff({QString(), QString(), QString()}, conflictResolution(file));
                    });
            }
        }

        // Don't risk overwriting a more-important "we didn't load the file
        // correctly" message with this semi-helpful "is it binary?" prompt
        if (!file.isEmpty() && decoded->text.contains(QChar(0)) && !msgarea_[size_t(pane)]->hasMessage()) {
            msgarea_[size_t(pane)]->addActionMsg(
                QStringLiteral("dialog-warning-symbolic"),
                tr("File %1 appears to be a binary file.").arg(QDir::toNativeSeparators(file)),
                tr("Do you want to open the file using the default application?"), tr("Open"),
                [file]() { openFilesExternal({file}); });
        }
    }

    // We checkpoint first, which will set modified state. We then check
    // whether we're saving the file to a different location than it was
    // loaded from, in which case we assume that this needs to be saved to
    // persist what the user is seeing. Finally, we update the writability,
    // which does label calculation.
    buf->document()->clearUndoRedoStacks();
    buf->setModified(false);
    if (!buf->data.savefile.isEmpty())
        buf->setModified(true);
    updateBufferWritable(pane);
    buf->data.updateMtime();
    if (QFileInfo::exists(file))
        watcher_.addPath(file);

    bool allFinished = true;
    for (int i = 0; i < numPanes_; ++i)
        allFinished = allFinished && textbuffer_[size_t(i)]->data.state == BufferState::LoadFinished;
    if (allFinished)
        scheduler_.addTask(compareFilesInternal());

    recomputeLabel();
}

TaskGenerator FileDiff::mergeFiles()
{
    if (comparisonMode_ == FileComparisonMode::AutoMerge) {
        Merger merger;
        Texts sequences;
        Texts texts;
        for (int i = 0; i < 3; ++i) {
            sequences.push_back(bufferFiltered_[size_t(i)].get());
            texts.push_back(bufferTexts_[size_t(i)].get());
        }
        for (bool done : merger.initialize(sequences, texts)) {
            if (done)
                break;
            co_yield QString();
        }
        const QString mergedText = merger.merge3Files();
        co_yield QString();
        if (auto *autoDiffer = qobject_cast<AutoMergeDiffer *>(linediffer_.get()))
            autoDiffer->unresolved = merger.unresolved;
        textbuffer_[1]->setText(mergedText);
        textbuffer_[1]->setModified(true);
        recomputeLabel();
    } else {
        co_yield QString();
    }
}

TaskGenerator FileDiff::diffFiles(bool refresh)
{
    Texts texts;
    for (int i = 0; i < numPanes_; ++i)
        texts.push_back(bufferFiltered_[size_t(i)].get());
    linediffer_->ignoreBlanks = ignoreBlankLines_;
    for (bool done : linediffer_->setSequencesIter(texts)) {
        if (done)
            break;
        co_yield tr("Computing differences");
    }

    if (!refresh) {
        for (SourceView *view : textview_)
            view->placeCursorAtLine(0);

        std::optional<int> targetChunk;
        if (!linediffer_->conflicts.empty()) {
            targetChunk = linediffer_->conflicts.front();
        } else {
            const ChunkLocation location = linediffer_->locateChunk(1, 0);
            targetChunk = location.chunk ? location.chunk : location.next;
        }

        if (targetChunk) {
            QPointer<FileDiff> guard(this);
            scheduler_.addOneShot(
                [guard, targetChunk]() {
                    if (guard)
                        guard->goToChunk(targetChunk, std::nullopt, true);
                },
                true);
        }
    }

    queueDraw();
    connectBufferHandlers();
    setMergeActionSensitivity();

    // Changing textview sensitivity removes focus and so triggers our
    // focus-out sensitivity handling. We manually trigger the focus-in here
    // to restablish the previous state.
    if (cursor_.pane) {
        SourceView *view = textview_[size_t(*cursor_.pane)];
        if (refresh && isVisible() && window()->isActiveWindow())
            view->setFocus();
        setFocusedTextview(view);
    }

    const syntax::LanguageManager &languages = syntax::LanguageManager::instance();
    std::vector<const syntax::Language *> langs;
    for (int i = 0; i < numPanes_; ++i)
        langs.push_back(languages.guessLanguageForFile(textbuffer_[size_t(i)]->data.file));

    // If we have only one identified language then we assume that all of
    // the files are actually of that type.
    std::vector<const syntax::Language *> realLangs;
    std::copy_if(langs.begin(), langs.end(), std::back_inserter(realLangs),
                 [](const syntax::Language *language) { return language != nullptr; });
    if (!realLangs.empty() && std::count(realLangs.begin(), realLangs.end(), realLangs.front()) == qsizetype(realLangs.size()))
        std::fill(langs.begin(), langs.end(), realLangs.front());

    for (int i = 0; i < numPanes_; ++i)
        textbuffer_[size_t(i)]->setLanguage(langs[size_t(i)]);
}

TaskGenerator FileDiff::compareFilesInternal()
{
    for (const QString &status : mergeFiles())
        co_yield status;
    for (const QString &status : diffFiles(false))
        co_yield status;
    const int pane = numPanes_ < 2 ? 0 : 1;
    if (isVisible())
        textview_[size_t(pane)]->setFocus();
    else
        focusPane_ = textview_[size_t(pane)];
}

void FileDiff::refreshComparison()
{
    // Refresh the view by clearing and redoing all comparisons
    preComparisonInit();
    queueDraw();
    scheduler_.addTask(diffFiles(true));
}

// ---------------------------------------------------------------------------
// Diff updates and highlighting

void FileDiff::onDiffsChanged(const ChunkChanges &changes)
{
    for (int pane = 0; pane < numPanes_; ++pane)
        chunkmap_[size_t(pane)]->setChunks(linediffer_->singleChanges(pane));

    // TODO: Break out highlight recalculation to its own method, and just
    // update chunk lists in children here.
    static constexpr int gutterPanes[4][2] = {{0, 1}, {1, 0}, {1, 2}, {2, 1}};
    for (int i = 0; i < 4; ++i)
        actiongutter_[size_t(i)]->setChunks(linediffer_->pairedAllSingleChanges(gutterPanes[i][0], gutterPanes[i][1]));

    applyInlineHighlights(changes);
    setMergeActionSensitivity();
    queueDraw();

    // Check for self-comparison using file IDs, so that we catch symlinks
    // and similar situations.
    QString duplicateFile;
    QStringList seenFileIds;
    for (MeldBuffer *buf : textbuffer_) {
        if (buf->data.file.isEmpty())
            continue;
        const QString id = fileId(buf->data.file);
        if (id.isEmpty())
            continue;
        if (seenFileIds.contains(id)) {
            duplicateFile = buf->data.label();
            break;
        }
        seenFileIds.append(id);
    }

    if (!duplicateFile.isEmpty()) {
        QList<MsgAreaController *> panes(msgarea_.begin(), msgarea_.end());
        for (int index = 0; index < numPanes_; ++index) {
            msgarea_[size_t(index)]->addDismissableMsg(QStringLiteral("dialog-warning-symbolic"),
                                                       tr("File %1 is being compared to itself").arg(duplicateFile),
                                                       QString(), panes);
        }
    } else if (linediffer_->sequencesIdentical()) {
        showSameFilesMessage();
    } else {
        for (MsgAreaController *mgr : msgarea_) {
            if (mgr->msgId() == MsgSame)
                mgr->clear();
        }
    }
}

void FileDiff::showSameFilesMessage()
{
    bool errorMessage = false;
    for (MsgAreaController *mgr : msgarea_)
        errorMessage = errorMessage || mgr->hasMessage();
    if (numPanes_ == 1 || errorMessage)
        return;

    // TODO: Currently this only checks to see whether text filters are
    // active, and may be altering the comparison. It would be better if we
    // only showed this message if the filters *did* change the text.
    bool activeFilters = false;
    for (const FilterEntry &f : std::as_const(textFilters_))
        activeFilters = activeFilters || f.active;

    QList<NewlineType> newlines;
    for (int i = 0; i < numPanes_; ++i)
        newlines.append(textbuffer_[size_t(i)]->data.format.newline);
    const bool differentNewlines = !allSame(newlines);

    for (int index = 0; index < 3; ++index) {
        MsgAreaController *mgr = msgarea_[size_t(index)];
        QString primary = tr("Files are identical");
        QString secondary;
        if (activeFilters) {
            secondary = tr("Text filters are being used, and may be masking differences between files. Would you "
                           "like to compare the unfiltered files?");
        } else if (differentNewlines) {
            primary = tr("Files differ in line endings only");
            QStringList lines;
            for (int i = 0; i < numPanes_; ++i) {
                lines.append(QStringLiteral("\t%1: %2").arg(textbuffer_[size_t(i)]->data.label(),
                                                             newlineDisplayName(newlines.at(i))));
            }
            secondary = tr("Files are identical except for differing line endings:\n%1").arg(lines.join(u'\n'));
        }

        MsgArea *msgarea = mgr->newFromTextAndIcon(primary, secondary);
        mgr->setMsgId(MsgSame);
        msgarea->addButton(index == 0 ? tr("Hi&de") : tr("Hide"), Response::Close);
        if (activeFilters)
            msgarea->addButton(tr("Show without filters"), Response::Ok);
        connect(msgarea, &MsgArea::response, this, [this](int response) {
            for (MsgAreaController *m : msgarea_)
                m->clear();
            if (response == Response::Ok) {
                for (FilterEntry &f : textFilters_)
                    f.active = false;
                refreshComparison();
            }
        });
    }
}

void FileDiff::applyInlineHighlights(const ChunkChanges &changes)
{
    // We need to clear removed and modified chunks, and need to re-highlight
    // added and modified chunks.
    std::vector<MergedChunk> needClearing(changes.removed.begin(), changes.removed.end());
    std::sort(needClearing.begin(), needClearing.end(),
              [](const MergedChunk &a, const MergedChunk &b) { return mergedChunkOrder(a) < mergedChunkOrder(b); });

    struct Highlight {
        MergedChunk chunks;
        bool clear;
    };
    std::vector<Highlight> needHighlighting;
    for (const MergedChunk &c : changes.added)
        needHighlighting.push_back({c, false});
    if (changes.modified)
        needHighlighting.push_back({*changes.modified, true});
    // Qt can report several lines changing in one edit; re-highlight any
    // chunk touched by the edit, in case it kept its old coordinates.
    if (lastEdit_) {
        const auto &[editPane, range] = *lastEdit_;
        for (const MergedChunk &c : linediffer_->allChanges()) {
            bool touched = false;
            for (int i = 0; i < 2; ++i) {
                const OptChunk &chunk = c[i];
                if (!chunk)
                    continue;
                const bool middle = editPane == 1;
                const int otherPane = i == 1 ? 2 : 0;
                if (!middle && editPane != otherPane)
                    continue;
                const int start = middle ? chunk->start_a : chunk->start_b;
                const int end = middle ? chunk->end_a : chunk->end_b;
                touched = touched || (start <= range.second && range.first < std::max(end, start + 1));
            }
            if (touched && !changes.added.contains(c))
                needHighlighting.push_back({c, true});
        }
    }
    std::sort(needHighlighting.begin(), needHighlighting.end(), [](const Highlight &a, const Highlight &b) {
        return mergedChunkOrder(a.chunks) < mergedChunkOrder(b.chunks);
    });

    for (const MergedChunk &chunks : needClearing) {
        for (int i = 0; i < 2; ++i) {
            const OptChunk &chunk = chunks[i];
            if (!chunk || chunk->tag != ChunkTag::Replace)
                continue;
            const int toIdx = i == 1 ? 2 : 0;
            MeldBuffer *a = textbuffer_[1];
            MeldBuffer *b = textbuffer_[size_t(toIdx)];
            a->inlineHighlights().remove(a->positionAtLineOrEof(chunk->start_a), a->positionAtLineOrEof(chunk->end_a));
            b->inlineHighlights().remove(b->positionAtLineOrEof(chunk->start_b), b->positionAtLineOrEof(chunk->end_b));
        }
    }

    for (const Highlight &highlight : needHighlighting) {
        for (int mergeCacheIndex = 0; mergeCacheIndex < 2; ++mergeCacheIndex) {
            const OptChunk &chunkOpt = highlight.chunks[mergeCacheIndex];
            if (!chunkOpt || chunkOpt->tag != ChunkTag::Replace)
                continue;
            const DiffChunk chunk = *chunkOpt;
            const int toPane = mergeCacheIndex == 1 ? 2 : 0;
            MeldBuffer *bufs[2] = {textbuffer_[1], textbuffer_[size_t(toPane)]};

            const int fromStart = bufs[0]->positionAtLineOrEof(chunk.start_a);
            const int fromEnd = bufs[0]->positionAtLineOrEof(chunk.end_a);
            const int toStart = bufs[1]->positionAtLineOrEof(chunk.start_b);
            const int toEnd = bufs[1]->positionAtLineOrEof(chunk.end_b);

            // We don't use the buffer line texts here, as removing line
            // breaks messes with inline highlighting
            const QString text1 = bufs[0]->textBetween(fromStart, fromEnd);
            const QString textn = bufs[1]->textBetween(toStart, toEnd);

            // Bail on long sequences, rather than try a slow comparison
            constexpr qsizetype InlineLimit = 20000;
            if (text1.size() + textn.size() > InlineLimit && !forceHighlight_) {
                bufs[0]->inlineHighlights().add(fromStart, fromEnd);
                bufs[1]->inlineHighlights().add(toStart, toEnd);
                promptLongHighlighting();
                continue;
            }

            const auto makeMark = [](MeldBuffer *buf, int position) {
                QTextCursor mark(buf->document());
                mark.setPosition(position);
                mark.setKeepPositionOnInsert(true);
                return mark;
            };
            const QTextCursor startMarks[2] = {makeMark(bufs[0], fromStart), makeMark(bufs[1], toStart)};
            const QTextCursor endMarks[2] = {makeMark(bufs[0], fromEnd), makeMark(bufs[1], toEnd)};
            const bool clear = highlight.clear;
            QPointer<FileDiff> guard(this);

            cachedMatch_->match(text1, textn, [guard, this, bufs0 = bufs[0], bufs1 = bufs[1], startMarks0 = startMarks[0],
                                               startMarks1 = startMarks[1], endMarks0 = endMarks[0],
                                               endMarks1 = endMarks[1], text1, textn, toPane, chunk,
                                               clear](const std::vector<DiffChunk> &matches) {
                if (!guard)
                    return;
                MeldBuffer *b[2] = {bufs0, bufs1};
                const int starts[2] = {startMarks0.position(), startMarks1.position()};
                const int ends[2] = {endMarks0.position(), endMarks1.position()};

                if (!linediffer_->hasChunk(toPane, chunk))
                    return;
                if (b[0]->textBetween(starts[0], ends[0]) != text1 || b[1]->textBetween(starts[1], ends[1]) != textn)
                    return;

                if (clear) {
                    b[0]->inlineHighlights().remove(starts[0], ends[0]);
                    b[1]->inlineHighlights().remove(starts[1], ends[1]);
                }

                const int offsets[2] = {ends[0] - starts[0], ends[1] - starts[1]};
                const auto keep = [&offsets](const DiffChunk &match) {
                    if (match.tag != ChunkTag::Equal)
                        return true;
                    // Always keep matches occurring at the start or end
                    const bool isStart = match.start_a == 0 && match.start_b == 0;
                    const bool isEnd = match.end_a == offsets[0] && match.end_b == offsets[1];
                    if (isStart || isEnd)
                        return false;
                    // Remove equal matches of size less than 3
                    return (match.end_a - match.start_a < 3) || (match.end_b - match.start_b < 3);
                };

                for (int i = 0; i < 2; ++i) {
                    QTextDocument *doc = b[i]->document();
                    for (const DiffChunk &match : matches) {
                        if (!keep(match))
                            continue;
                        int start = starts[i] + (i == 0 ? match.start_a : match.start_b);
                        int end = starts[i] + (i == 0 ? match.end_a : match.end_b);

                        // Check whether the identified difference is just a
                        // combining diacritic. If so, we want to highlight
                        // the visual character it's a part of.
                        const QTextBlock startBlock = doc->findBlock(start);
                        if (startBlock.isValid() && startBlock.layout()) {
                            const int rel = start - startBlock.position();
                            if (rel > 0 && !startBlock.layout()->isValidCursorPosition(rel))
                                start = startBlock.position() + startBlock.layout()->previousCursorPosition(rel);
                        }
                        const QTextBlock endBlock = doc->findBlock(end);
                        if (endBlock.isValid() && endBlock.layout()) {
                            const int rel = end - endBlock.position();
                            if (rel < endBlock.length() - 1 && !endBlock.layout()->isValidCursorPosition(rel))
                                end = endBlock.position() + endBlock.layout()->nextCursorPosition(rel);
                        }
                        b[i]->inlineHighlights().add(start, end);
                    }
                }
                queueDraw();
            });
        }
    }

    cachedMatch_->clean(linediffer_->diffCount());
}

void FileDiff::promptLongHighlighting()
{
    for (int index = 0; index < 3; ++index) {
        MsgAreaController *mgr = msgarea_[size_t(index)];
        if (mgr->msgId() == MsgSlowHighlight)
            continue;
        MsgArea *msgarea =
            mgr->newFromTextAndIcon(tr("Change highlighting incomplete"),
                                    tr("Some changes were not highlighted because they were too large. You can "
                                       "force Meld Qt to take longer to highlight larger changes, though this may be "
                                       "slow."));
        mgr->setMsgId(MsgSlowHighlight);
        msgarea->addButton(tr("Hi&de"), Response::Close);
        msgarea->addButton(index == 0 ? tr("&Keep highlighting") : tr("Keep highlighting"), Response::Ok);
        connect(msgarea, &MsgArea::response, this, [this](int response) {
            for (MsgAreaController *m : msgarea_)
                m->clear();
            if (response == Response::Ok) {
                forceHighlight_ = true;
                refreshComparison();
            }
        });
    }
}

void FileDiff::promptLoadLongLines(int pane, const QString &primary, const QString &secondary)
{
    // Rather than failing outright on very long lines, offer the user the
    // chance to load the files anyway.
    MsgAreaController *mgr = msgarea_[size_t(pane)];
    MsgArea *msgarea = mgr->newFromTextAndIcon(primary, secondary, QStringLiteral("dialog-warning-symbolic"));
    mgr->setMsgId(MsgLineTooLong);
    msgarea->addButton(tr("Hi&de"), Response::Close);
    msgarea->addButton(tr("&Load anyway"), Response::Ok);
    connect(msgarea, &MsgArea::response, this, [this](int response) {
        for (MsgAreaController *m : msgarea_)
            m->clear();
        if (response == Response::Ok) {
            forceLoad_ = true;
            QStringList files;
            QList<QByteArray> encodings;
            for (int i = 0; i < numPanes_; ++i) {
                files.append(textbuffer_[size_t(i)]->data.file);
                encodings.append(textbuffer_[size_t(i)]->data.format.encoding);
            }
            setFiles(files, encodings);
        }
    });
}

// ---------------------------------------------------------------------------
// Saving

bool FileDiff::saveFile(int pane, bool saveas, bool forceOverwrite)
{
    MeldBuffer *buf = textbuffer_[size_t(pane)];
    BufferData &data = buf->data;
    if (saveas || (data.file.isEmpty() && data.savefile.isEmpty()) || !data.writable()) {
        QString prompt;
        if (pane == 0)
            prompt = tr("Save Left Pane As");
        else if (pane == 1 && numPanes_ == 3)
            prompt = tr("Save Middle Pane As");
        else
            prompt = tr("Save Right Pane As");

        auto *dialog = new QFileDialog(window(), prompt);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setAcceptMode(QFileDialog::AcceptSave);
        dialog->setFileMode(QFileDialog::AnyFile);
        if (!data.file.isEmpty())
            dialog->selectFile(data.file);
        QPointer<FileDiff> guard(this);
        connect(dialog, &QFileDialog::fileSelected, this, [guard, pane, forceOverwrite](const QString &file) {
            if (!guard || file.isEmpty())
                return;
            MeldBuffer *b = guard->textbuffer_[size_t(pane)];
            if (!b->data.file.isEmpty())
                guard->watcher_.removePath(b->data.file);
            b->data.customLabel.clear();
            b->data.file = QFileInfo(file).absoluteFilePath();
            b->data.savefile.clear();
            b->data.updateMtime();
            guard->filelabel_[size_t(pane)]->setFile(b->data.file);
            guard->doSaveFile(pane, forceOverwrite);
        });
        dialog->open();
        return false;
    }

    doSaveFile(pane, forceOverwrite);
    return true;
}

void FileDiff::doSaveFile(int pane, bool forceOverwrite)
{
    MeldBuffer *buf = textbuffer_[size_t(pane)];
    BufferData &data = buf->data;

    // Saving a merge with conflicts left drops them, as they're empty or
    // the common ancestor's text
    const bool askAboutConflicts = meta_ && meta_->warnUnresolved && pane == 1 && numPanes_ == 3;
    if (askAboutConflicts && !saveUnresolved_[size_t(pane)] && !linediffer_->conflicts.empty()) {
        const int conflicts = int(linediffer_->conflicts.size());
        MsgArea *msgarea = msgarea_[size_t(pane)]->newFromTextAndIcon(
            tr("%n conflicts aren’t resolved", nullptr, conflicts),
            tr("If you save the merge now, the conflicts are saved as they are here, without conflict markers."),
            QStringLiteral("dialog-warning-symbolic"));
        msgarea->addButton(tr("Save Anyway"), Response::Accept);
        msgarea->addButton(tr("Don’t Save"), Response::Close);
        connect(msgarea, &MsgArea::response, this, [this, pane, forceOverwrite](int response) {
            msgarea_[size_t(pane)]->clear();
            if (response == Response::Accept) {
                saveUnresolved_[size_t(pane)] = true;
                doSaveFile(pane, forceOverwrite);
            }
        });
        return;
    }

    if (!forceOverwrite) {
        // Check whether the file changed on disk since we loaded it
        const std::optional<QDateTime> onDisk = BufferData::queryMtime(data.file);
        if (!data.currentOnDisk() || (onDisk && data.mtime && *onDisk != *data.mtime)) {
            const QString primary =
                tr("File %1 has changed on disk since it was opened").arg(QDir::toNativeSeparators(data.file));
            MsgArea *msgarea = msgarea_[size_t(pane)]->newFromTextAndIcon(
                primary, tr("If you save it, any external changes will be lost."),
                QStringLiteral("dialog-warning-symbolic"));
            msgarea->addButton(tr("Save Anyway"), Response::Accept);
            msgarea->addButton(tr("Don’t Save"), Response::Close);
            connect(msgarea, &MsgArea::response, this, [this, pane](int response) {
                msgarea_[size_t(pane)]->clear();
                if (response == Response::Accept)
                    doSaveFile(pane, true);
            });
            return;
        }
    }

    saveUnresolved_[size_t(pane)] = false;
    const QString target = data.target();
    const std::optional<QByteArray> bytes = encodeText(buf->text(), data.format);
    if (!bytes) {
        QMessageBox::critical(window(), tr("Could not save file %1.").arg(QDir::toNativeSeparators(target)),
                              tr("File “%1” contains characters that can’t be encoded using its current encoding "
                                 "“%2”.")
                                  .arg(QDir::toNativeSeparators(target), QString::fromLatin1(data.format.encoding)));
        setState(State::SavingError);
        return;
    }

    watcher_.removePath(target);
    QSaveFile file(target);
    QString error;
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
    } else {
        file.write(*bytes);
        if (!file.commit())
            error = file.errorString();
    }
    if (!error.isEmpty()) {
        QMessageBox::critical(window(), tr("Could not save file %1.").arg(QDir::toNativeSeparators(target)),
                              tr("Couldn’t save file due to:\n%1").arg(error));
        setState(State::SavingError);
        return;
    }
    onFileSaved(pane);
}

void FileDiff::onFileSaved(int pane)
{
    MeldBuffer *buf = textbuffer_[size_t(pane)];
    const QString target = buf->data.target();
    if (!target.isEmpty())
        watcher_.addPath(target);
    Q_EMIT fileChanged(target);
    buf->setModified(false);
    buf->data.updateMtime();
    updateBufferWritable(pane);
    if (pane == 1 && numPanes_ == 3)
        middleSaved_ = true;

    if (state() == State::Closing) {
        bool anyModified = false;
        for (int i = 0; i < numPanes_; ++i)
            anyModified = anyModified || textbuffer_[size_t(i)]->isModified();
        if (!anyModified)
            requestClose();
    } else {
        setState(State::Normal);
    }
    recomputeLabel();
}

void FileDiff::updateBufferWritable(int pane)
{
    const bool writable = textbuffer_[size_t(pane)]->data.writable();
    recomputeLabel();
    readonlyToggle_[size_t(pane)]->setVisible(!writable);
    setBufferEditable(pane, writable);
}

void FileDiff::setBufferEditable(int pane, bool editable)
{
    {
        const QSignalBlocker blocker(readonlyToggle_[size_t(pane)]);
        readonlyToggle_[size_t(pane)]->setChecked(!editable);
    }
    readonlyToggle_[size_t(pane)]->setIcon(icons::icon(editable ? QStringLiteral("changes-allow-symbolic")
                                                                : QStringLiteral("changes-prevent-symbolic")));
    textview_[size_t(pane)]->setEditable(editable);
    onCursorPositionChanged(pane, true);
}

void FileDiff::notifyFileChanged(int pane)
{
    const QString file = textbuffer_[size_t(pane)]->data.file;
    msgarea_[size_t(pane)]->addActionMsg(
        QStringLiteral("dialog-warning-symbolic"), tr("File %1 has changed on disk").arg(QDir::toNativeSeparators(file)),
        tr("Do you want to reload the file?"), tr("&Reload"), [this, pane]() { revertPane(pane); });
}

void FileDiff::onWatchedFileChanged(const QString &path)
{
    for (int pane = 0; pane < numPanes_; ++pane) {
        BufferData &data = textbuffer_[size_t(pane)]->data;
        if (data.file != path)
            continue;
        const std::optional<QDateTime> mtime = BufferData::queryMtime(path);
        if (data.diskMtime && mtime && *mtime > *data.diskMtime)
            notifyFileChanged(pane);
        if (mtime)
            data.diskMtime = mtime;
    }
    // Some editors replace files when saving, which removes the watch
    if (QFileInfo::exists(path) && !watcher_.files().contains(path))
        watcher_.addPath(path);
}

void FileDiff::confirmUnsavedChangeAction(std::function<void()> onConfirm, const QList<int> &panes)
{
    // Confirm discard of any unsaved changes. Unlike closing, this does
    // *not* prompt the user to save, but rather just confirms whether they
    // want to discard changes.
    QStringList unsaved;
    const QList<int> checked = panes.isEmpty() ? QList<int>{0, 1, 2} : panes;
    for (int pane : checked) {
        if (pane < numPanes_ && textbuffer_[size_t(pane)]->isModified())
            unsaved.append(textbuffer_[size_t(pane)]->data.label());
    }
    if (unsaved.isEmpty()) {
        onConfirm();
        return;
    }

    auto *box = new QMessageBox(QMessageBox::Question, tr("Discard unsaved changes to documents?"),
                                tr("Discard unsaved changes to documents?"), QMessageBox::NoButton, window());
    box->setAttribute(Qt::WA_DeleteOnClose);
    QStringList bullets;
    for (const QString &label : std::as_const(unsaved))
        bullets.append(QStringLiteral("\t• ") + label);
    box->setInformativeText(tr("Changes made to the following documents will be permanently lost:") + QLatin1Char('\n')
                            + bullets.join(QLatin1Char('\n')));
    box->addButton(tr("&Cancel"), QMessageBox::RejectRole);
    QPushButton *discard = box->addButton(tr("&Discard"), QMessageBox::DestructiveRole);
    QPointer<FileDiff> guard(this);
    connect(box, &QMessageBox::finished, this, [guard, box, discard, onConfirm = std::move(onConfirm)]() {
        if (guard && box->clickedButton() == discard)
            onConfirm();
    });
    box->open();
}

void FileDiff::revert()
{
    confirmUnsavedChangeAction([this]() {
        QStringList files;
        QList<QByteArray> encodings;
        for (int i = 0; i < numPanes_; ++i) {
            files.append(textbuffer_[size_t(i)]->data.file);
            encodings.append(textbuffer_[size_t(i)]->data.format.encoding);
        }
        setFiles(files, encodings);
    });
}

void FileDiff::revertPane(int pane)
{
    confirmUnsavedChangeAction(
        [this, pane]() {
            const BufferData &data = textbuffer_[size_t(pane)]->data;
            setFile(pane, data.file, data.format.encoding);
        },
        {pane});
}

// ---------------------------------------------------------------------------
// Closing

void FileDiff::requestClose()
{
    setState(State::Closing);

    bool anyModified = false;
    for (int i = 0; i < numPanes_; ++i)
        anyModified = anyModified || textbuffer_[size_t(i)]->isModified();
    if (!anyModified) {
        requestCloseResponse(QStringLiteral("discard"), {});
        return;
    }

    auto *dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("Save changes to documents before closing?"));
    auto *layout = new QVBoxLayout(dialog);
    auto *heading = new QLabel(tr("Save changes to documents before closing?"), dialog);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.2);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    auto *body = new QLabel(tr("Open documents contain unsaved changes. Changes which are not saved will be "
                               "permanently lost."),
                            dialog);
    body->setWordWrap(true);
    layout->addWidget(body);

    QList<QCheckBox *> boxes;
    for (int i = 0; i < numPanes_; ++i) {
        MeldBuffer *buf = textbuffer_[size_t(i)];
        auto *check = new QCheckBox(buf->data.label(), dialog);
        check->setEnabled(buf->isModified());
        check->setChecked(buf->isModified());
        layout->addWidget(check);
        boxes.append(check);
    }

    auto *buttons = new QDialogButtonBox(dialog);
    QPushButton *cancel = buttons->addButton(tr("&Cancel"), QDialogButtonBox::RejectRole);
    QPushButton *discard = buttons->addButton(tr("&Discard"), QDialogButtonBox::DestructiveRole);
    QPushButton *save = buttons->addButton(tr("&Save"), QDialogButtonBox::AcceptRole);
    save->setDefault(true);
    layout->addWidget(buttons);
    Q_UNUSED(cancel);

    QPointer<FileDiff> guard(this);
    auto response = std::make_shared<QString>(QStringLiteral("cancel"));
    connect(discard, &QPushButton::clicked, dialog, [dialog, response]() {
        *response = QStringLiteral("discard");
        dialog->done(QDialog::Accepted);
    });
    connect(save, &QPushButton::clicked, dialog, [dialog, response]() {
        *response = QStringLiteral("save");
        dialog->done(QDialog::Accepted);
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [guard, boxes, response]() {
        if (!guard)
            return;
        QList<bool> trySave;
        for (QCheckBox *box : boxes)
            trySave.append(box->isChecked());
        guard->requestCloseResponse(*response, trySave);
    });
    dialog->open();
}

void FileDiff::requestCloseResponse(const QString &response, const QList<bool> &trySave)
{
    if (response == QLatin1String("cancel")) {
        setState(State::Normal);
        return;
    }

    if (response == QLatin1String("save")) {
        for (int i = 0; i < trySave.size() && i < numPanes_; ++i) {
            if (trySave.at(i))
                saveFile(i);
        }
        // Regardless of whether these saves are successful or not, we return
        // here, so that other closing logic doesn't run. Instead, the
        // file-saved callback handles closing files and setting state.
        return;
    }

    // As with file saving above, the resolve-conflict prompt will take care
    // of re-requesting the close.
    if (meta_ && !promptResolveConflict())
        return;

    for (MeldBuffer *buf : textbuffer_) {
        if (!buf->data.file.isEmpty())
            watcher_.removePath(buf->data.file);
    }
    cachedMatch_->stop();
    MeldDoc::requestClose();
}

bool FileDiff::promptResolveConflict()
{
    if (!meta_->promptResolve || !middleSaved_ || !meta_->resolve)
        return true;
    // Only ask once; closing is requested again after the response
    meta_->promptResolve = false;

    auto *box = new QMessageBox(QMessageBox::Question, tr("Mark conflict as resolved?"),
                                tr("Mark conflict as resolved?"), QMessageBox::NoButton, window());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(tr("If the conflict was resolved successfully, you may mark it as resolved now."));
    box->addButton(tr("Cancel"), QMessageBox::RejectRole);
    QPushButton *resolve = box->addButton(tr("Mark &Resolved"), QMessageBox::AcceptRole);
    box->setDefaultButton(resolve);
    QPointer<FileDiff> guard(this);
    connect(box, &QMessageBox::finished, this, [guard, box, resolve]() {
        if (!guard)
            return;
        if (box->clickedButton() == resolve) {
            // It's possible that here we're in a quit callback, so the
            // resolve action has to run immediately rather than in a task.
            guard->meta_->resolve(guard->textbuffer_[1]->data.target());
        }
        // We can only get here from the close callback once we've already
        // saved files, etc.
        guard->requestCloseResponse(QStringLiteral("discard"), {});
    });
    box->open();
    return false;
}

// ---------------------------------------------------------------------------
// Panes and scrolling

void FileDiff::queueDraw()
{
    for (SourceView *view : textview_) {
        view->viewport()->update();
        if (QWidget *gutter = view->findChild<LineNumberGutter *>())
            gutter->update();
    }
    for (int i = 0; i < numPanes_ - 1; ++i)
        linkmap_[size_t(i)]->update();
    for (ActionGutter *gutter : actiongutter_)
        gutter->update();
}

void FileDiff::syncHScroll(int master)
{
    if (syncHScrollLock_ || lockScrolling_)
        return;
    syncHScrollLock_ = true;
    const int value = textview_[size_t(master)]->horizontalScrollBar()->value();
    for (int i = 0; i < numPanes_; ++i) {
        if (i != master)
            textview_[size_t(i)]->horizontalScrollBar()->setValue(value);
    }
    syncHScrollLock_ = false;
}

void FileDiff::syncVScroll(int masterPane)
{
    if (syncVScrollLock_ || lockScrolling_)
        return;
    syncVScrollLock_ = true;

    int master = masterPane;
    const QScrollBar *masterBar = textview_[size_t(master)]->verticalScrollBar();
    const Adjustment adjustment{double(masterBar->value()), double(masterBar->minimum()),
                                double(masterBar->maximum() + masterBar->pageStep()), double(masterBar->pageStep())};
    const double syncpoint = calcSyncpoint(adjustment);

    // Sync point in scroll units (visual lines); this will usually be the
    // middle of the screen, except at the top and bottom of the document.
    const double syncY = adjustment.value + adjustment.pageSize * syncpoint;

    // Find the target line. This is a float because, especially for wrapped
    // lines, the sync point may be half way through a line. Not doing this
    // calculation makes scrolling jerky.
    const QTextDocument *masterDoc = textview_[size_t(master)]->document();
    QTextBlock syncBlock = masterDoc->findBlockByLineNumber(int(syncY));
    if (!syncBlock.isValid())
        syncBlock = masterDoc->lastBlock();
    const double lineY = syncBlock.firstLineNumber();
    const double height = std::max(1, syncBlock.lineCount());
    double targetLine = syncBlock.blockNumber() + std::clamp((syncY - lineY) / height, 0.0, 1.0);

    // In the case of two pane scrolling, it's clear how to bind scrollbars:
    // if the user moves the left pane, we move the right pane, and vice
    // versa. For three pane scrolling, we want panes to be tied, but need an
    // influence mapping. In Meld, all influence flows through the middle
    // pane, e.g., the user moves the left pane, that moves the middle pane,
    // and the middle pane moves the right pane. If the user moves the middle
    // pane, then the left and right panes are moved directly.
    static constexpr int scrollbarInfluence[3][2] = {{1, 2}, {0, 2}, {1, 0}};

    for (int k = 0; k < numPanes_ - 1; ++k) {
        const int i = scrollbarInfluence[master][k];
        QScrollBar *bar = textview_[size_t(i)]->verticalScrollBar();

        // Find the chunk, or more commonly the space between chunks, that
        // contains the target line.
        int mbegin = 0;
        int mend = textbuffer_[size_t(master)]->lineCount();
        int obegin = 0;
        int oend = textbuffer_[size_t(i)]->lineCount();
        for (const DiffChunk &chunk : linediffer_->pairChanges(master, i)) {
            if (chunk.start_a >= targetLine) {
                mend = chunk.start_a;
                oend = chunk.start_b;
                break;
            } else if (chunk.end_a >= targetLine) {
                mbegin = chunk.start_a;
                mend = chunk.end_a;
                obegin = chunk.start_b;
                oend = chunk.end_b;
                break;
            } else {
                mbegin = chunk.end_a;
                obegin = chunk.end_b;
            }
        }

        const double fraction = (targetLine - mbegin) / double(std::max(1, mend - mbegin));
        const double otherLine = obegin + fraction * (oend - obegin);

        // At this point, we've identified the line within the corresponding
        // chunk that we want to sync to.
        const QTextDocument *otherDoc = textview_[size_t(i)]->document();
        const int otherIndex = int(otherLine);
        QTextBlock block = otherDoc->findBlockByNumber(otherIndex);
        bool isEnd = false;
        if (!block.isValid()) {
            block = otherDoc->lastBlock();
            isEnd = true;
        } else if (!block.next().isValid() && block.length() <= 1) {
            isEnd = true;
        }
        double value = block.firstLineNumber();
        // Special case line-height adjustment for EOF
        const double lineFactor = isEnd ? 1.0 : otherLine - otherIndex;
        value += lineFactor * std::max(1, block.lineCount());
        if (syncpoint > 0.5) {
            // If we're in the last half page, gradually factor in the
            // overscroll margin.
            const double overscrollScale = (syncpoint - 0.5) / 0.5;
            value += 5 * overscrollScale;
        }
        value -= bar->pageStep() * syncpoint;
        value = std::clamp(value, double(bar->minimum()), double(bar->maximum()));
        bar->setValue(int(std::floor(value + 0.5)));

        // If we just changed the central bar, make it the master
        if (i == 1) {
            master = 1;
            targetLine = otherLine;
        }
    }

    syncVScrollLock_ = false;
    for (LinkMap *linkmap : linkmap_)
        linkmap->update();
    for (ActionGutter *gutter : actiongutter_)
        gutter->update();
}

void FileDiff::setNumPanes(int n)
{
    if (n == numPanes_ || n < 1 || n > 3)
        return;
    numPanes_ = n;

    for (int i = 0; i < 3; ++i) {
        const bool visible = i < n;
        vbox_[size_t(i)]->setVisible(visible);
        fileToolbar_[size_t(i)]->setVisible(visible);
        statusbar_[size_t(i)]->parentWidget()->setVisible(visible);
        chunkmap_[size_t(i)]->setVisible(visible);
    }
    for (int i = 0; i < 2; ++i) {
        linkmap_[size_t(i)]->setVisible(i < n - 1);
        dummyStatusbar_[size_t(i)]->setVisible(i < n - 1);
    }
    for (int i = 0; i < 4; ++i)
        actiongutter_[size_t(i)]->setVisible(i < (n - 1) * 2);

    // Collapse the grid columns of hidden panes
    static constexpr int paneColumns[3] = {0, 4, 8};
    for (int i = 0; i < 3; ++i)
        grid_->setColumnStretch(paneColumns[i], i < n ? 1 : 0);

    setActionEnabled(QStringLiteral("format-as-patch"), n > 1);

    linkmap_[0]->associate(this, textview_[0], textview_[1], 0, 1);
    linkmap_[1]->associate(this, textview_[size_t(std::max(0, n - 2))], textview_[size_t(std::max(1, n - 1))],
                           std::max(0, n - 2), std::max(1, n - 1));

    QList<QTextDocument *> documents;
    for (int pane = 0; pane < n; ++pane)
        documents.append(textbuffer_[size_t(pane)]->document());
    syncpoints_.setDocuments(documents);

    for (int i = 0; i < n; ++i)
        fileSaveButton_[size_t(i)]->setEnabled(textbuffer_[size_t(i)]->isModified());
    queueDraw();
    recomputeLabel();
}

} // namespace meld
