#pragma once

#include "diffchunk.h"
#include "linesource.h"
#include "textfile.h"

#include <QDateTime>
#include <QObject>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocument>

#include <functional>
#include <optional>
#include <vector>

namespace meld {

namespace syntax {
class Language;
class SyntaxHighlighter;
} // namespace syntax

/*!
 * A set of non-overlapping text ranges that move with document edits, in
 * the same way that GtkTextTag ranges do. This is used for inline change
 * highlighting and for dimming filtered text, without modifying the
 * document's own formatting (and so its undo stack).
 */
class RangeSet {
public:
    explicit RangeSet(QTextDocument *document);

    void add(int start, int end);
    //! Remove (or trim) ranges intersecting [start, end)
    void remove(int start, int end);
    void clear();
    bool isEmpty() const { return ranges_.empty(); }

    //! All current ranges, ordered by position
    std::vector<std::pair<int, int>> ranges() const;
    //! Ranges intersecting [start, end), clipped to that interval
    std::vector<std::pair<int, int>> rangesIn(int start, int end) const;

private:
    size_t firstEndingAfter(int position) const;

    QTextDocument *document_;
    std::vector<QTextCursor> ranges_;
};

enum class BufferState {
    Empty,
    Loading,
    LoadFinished,
    LoadError,
};

//! File-related information for a buffer (MeldBufferData)
struct BufferData {
    //! The file shown in this buffer, if any
    QString file;
    //! Alternate target for saving (e.g., a merge output file)
    QString savefile;
    QString customLabel;
    BufferState state = BufferState::Empty;
    TextFileFormat format;
    std::optional<QDateTime> diskMtime;
    std::optional<QDateTime> mtime;

    QString label() const;
    QString target() const { return savefile.isEmpty() ? file : savefile; }
    bool writable() const;
    bool currentOnDisk() const { return mtime == diskMtime; }
    void reset(const QString &newFile, BufferState newState);
    void updateMtime();
    static std::optional<QDateTime> queryMtime(const QString &path);
};

class MeldBuffer : public QObject {
    Q_OBJECT

public:
    explicit MeldBuffer(QObject *parent = nullptr);
    ~MeldBuffer() override;

    QTextDocument *document() const { return document_; }

    int lineCount() const { return document_->blockCount(); }
    QString lineText(int line) const;
    //! The whole buffer contents, with '\n' line separators
    QString text() const;
    QString textBetween(int start, int end) const;
    //! Replace the contents without recording undo information
    void setText(const QString &text);
    //! Replace the contents as a single undoable action
    void replaceAllText(const QString &text);

    int endPosition() const { return document_->characterCount() - 1; }
    //! Position at the start of `line`, or the end of the buffer
    int positionAtLineOrEof(int line) const;
    //! Gtk get_iter_at_line(): start of `line`, or start of the last line
    int positionAtLine(int line) const;
    int lineAtPosition(int position) const;

    void insertText(int position, const QString &text);
    void deleteRange(int start, int end);
    /*!
     * Insert text at the given line, or the end of the buffer. If asked to
     * insert past the last line, a line break is added before the inserted
     * text. Returns the position after the inserted text.
     */
    int insertAtLine(int line, const QString &text);

    //! Group edits into a single undo action (GtkTextBuffer user actions)
    void beginUserAction();
    void endUserAction();

    bool isModified() const { return document_->isModified(); }
    void setModified(bool modified);

    RangeSet &inlineHighlights() { return inline_; }
    RangeSet &dimmedRanges() { return dimmed_; }

    //! Syntax highlighting (GtkSourceBuffer's language, highlight-syntax
    //! and style-scheme)
    syntax::SyntaxHighlighter *highlighter() const { return highlighter_; }
    //! The highlighted language, or nullptr for plain text
    const syntax::Language *language() const;
    void setLanguage(const syntax::Language *language);

    BufferData data;

Q_SIGNALS:
    void languageChanged();
    /*!
     * Emitted after every content change: lines startLine..startLine +
     * linesRemoved of the old text were replaced by lines startLine..
     * startLine + linesAdded.
     */
    void linesChanged(int startLine, int linesRemoved, int linesAdded);

private:
    void onContentsChange(int position, int charsRemoved, int charsAdded);

    QTextDocument *document_;
    QTextCursor editCursor_;
    int lastLineCount_ = 1;
    RangeSet inline_;
    RangeSet dimmed_;
    syntax::SyntaxHighlighter *highlighter_;
};

/*!
 * Buffer shim with line-based access and optional filtering
 *
 * This class allows a buffer to be treated as a list of lines of possibly
 * filtered text. If no filter is given, the raw text is used.
 */
class BufferLines final : public LineSource {
public:
    //! Filter receiving a line's text and its start/end document positions
    using Filter = std::function<QString(const QString &text, int start, int end)>;

    explicit BufferLines(MeldBuffer *buffer, Filter filter = {}, bool cacheDebug = false);
    ~BufferLines() override;

    BufferLines(const BufferLines &) = delete;
    BufferLines &operator=(const BufferLines &) = delete;

    int lineCount() const override { return buffer_->lineCount(); }
    QString line(int index) const override;
    QStringList lines(int lo, int hi = INT_MAX) const override;
    //! Python indexing semantics: throws std::out_of_range past the end
    QString at(int index) const;

    void clearCache();

    //! Cached copy of each line, where nullopt means there is no cached result
    std::vector<std::optional<QString>> &cache() const { return cache_; }

private:
    void onLinesChanged(int startLine, int linesRemoved, int linesAdded);

    MeldBuffer *buffer_;
    Filter filter_;
    bool cacheDebug_;
    mutable std::vector<std::optional<QString>> cache_;
    QMetaObject::Connection connection_;
};

/*!
 * Undo handling across a set of documents
 *
 * Every document keeps its own Qt undo stack; this sequence records the
 * global order of undo steps, so that undo and redo in a comparison work
 * across panes as they do in Meld.
 */
class UndoSequence : public QObject {
    Q_OBJECT

public:
    explicit UndoSequence(QObject *parent = nullptr);

    void addDocument(QTextDocument *document);
    //! Remove all undo and redo actions from this sequence
    void clear();
    bool canUndo() const { return nextRedo_ > 0; }
    bool canRedo() const { return nextRedo_ < int(actions_.size()); }
    QTextDocument *undo();
    QTextDocument *redo();

Q_SIGNALS:
    void canUndoChanged(bool canUndo);
    void canRedoChanged(bool canRedo);

private:
    void onUndoCommandAdded(QTextDocument *document);

    std::vector<QTextDocument *> documents_;
    std::vector<QTextDocument *> actions_;
    int nextRedo_ = 0;
    bool busy_ = false;
};

/*!
 * Apply text filters to a buffer's text (FileDiff._filter_text)
 *
 * Text between `start` and `end` (document positions) is filtered, and
 * the filtered ranges are recorded as dimmed. If a filter would remove a
 * line break, the unfiltered text is returned and `*ok` is set to false.
 */
QString filterBufferText(const QString &text, RangeSet &dimmed, int start, int end,
                         const QList<QRegularExpression> &regexes, bool *ok = nullptr);

//! The edits performed by the chunk actions in a file comparison
namespace chunkops {

struct EditResult {
    //! Inserted (or remaining) range in the target buffer
    int start = 0;
    int end = 0;
};

//! Copy a chunk from `source` above or below the chunk in `target`
EditResult copyChunk(MeldBuffer *source, MeldBuffer *target, const DiffChunk &chunk, bool copyUp);
//! Replace the chunk's range in `target` with the chunk text from `source`
EditResult replaceChunk(MeldBuffer *source, MeldBuffer *target, const DiffChunk &chunk);
//! Delete the chunk's `a` range from `buffer`
EditResult deleteChunk(MeldBuffer *buffer, const DiffChunk &chunk);

} // namespace chunkops

} // namespace meld
