#pragma once

#include "core/diffchunk.h"

#include <QElapsedTimer>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTimer>

#include <functional>
#include <optional>
#include <vector>

namespace meld {

class MeldBuffer;
class SourceView;

//! Line numbers with chunk backgrounds (GutterRendererChunkLines)
class LineNumberGutter : public QWidget {
    Q_OBJECT

public:
    explicit LineNumberGutter(SourceView *view);
    int preferredWidth() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    SourceView *view_;
};

//! Search settings used to highlight all matches in a view
struct SearchHighlight {
    QString text;
    bool caseSensitive = false;
    bool wholeWord = false;
    bool regex = false;

    bool isEmpty() const { return text.isEmpty(); }
};

class SourceView : public QPlainTextEdit {
    Q_OBJECT

public:
    enum class AnimationType {
        Fill,
        Stroke,
    };

    struct VisibleLine {
        int number;
        //! Top of the line (block) in viewport coordinates
        qreal top;
        qreal height;
    };

    explicit SourceView(QWidget *parent = nullptr);
    ~SourceView() override;

    void setBuffer(MeldBuffer *buffer);
    MeldBuffer *buffer() const { return buffer_; }

    //! Chunks to paint for a range of lines (FileDiff.single_changes)
    std::function<std::vector<DiffChunk>(int, int)> chunkIter;
    //! Whether the given chunk is the comparison's current chunk
    std::function<bool(const DiffChunk &)> currentChunkCheck;
    //! The chunk shown in the line number gutter for a line
    std::function<std::optional<DiffChunk>(int)> gutterChunkAt;
    //! Lines that have synchronisation points in this pane
    std::function<std::vector<int>()> syncpointLines;

    std::vector<VisibleLine> visibleLines() const;
    //! Top of `line` in viewport coordinates; for lines past the end of
    //! the buffer, the bottom of the last line. Lines outside the visible
    //! area are extrapolated.
    qreal yForLine(int line) const;
    //! The line at viewport y-coordinate `y`, clamped to the buffer
    int lineAtY(qreal y) const;
    qreal lineHeight() const;
    //! Visual column (with tab expansion) of a document position
    int visualColumn(int position) const;

    bool editable() const { return !isReadOnly(); }
    void setEditable(bool editable);

    bool showLineNumbers() const { return showLineNumbers_; }
    void setShowLineNumbers(bool show);
    bool drawSpaces() const { return drawSpaces_; }
    void setDrawSpaces(bool draw);
    bool highlightCurrentLine() const { return highlightCurrentLine_; }
    void setHighlightCurrentLine(bool highlight);
    bool wrapText() const { return wrapText_; }
    void setWrapText(bool wrap);
    void setOverscrollLines(int lines);
    void applyFontSettings();
    //! Use the current style scheme's colours and syntax highlighting
    void applyStyleScheme();

    void setSearchHighlight(const SearchHighlight &highlight);

    void addFadingHighlight(int start, int end, const QColor &color, int durationMs,
                            AnimationType type = AnimationType::Fill, double startingAlpha = 1.0);

    //! Scroll so that `line` is centred in the view
    void scrollToLineCentered(int line);
    //! Place the cursor at the start of `line`
    void placeCursorAtLine(int line);

Q_SIGNALS:
    void focusChanged(bool focused);
    void contextMenuRequested(const QPoint &globalPos);
    void undoRequested();
    void redoRequested();
    void escapePressed();
    void filesDropped(const QStringList &paths);
    void viewportChanged();
    void editableChanged(bool editable);
    void displayOptionsChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool canInsertFromMimeData(const QMimeData *source) const override;
    void insertFromMimeData(const QMimeData *source) override;
    void scrollContentsBy(int dx, int dy) override;
    void changeEvent(QEvent *event) override;

private:
    friend class LineNumberGutter;

    struct Animation {
        QTextCursor start;
        QTextCursor end;
        QColor startColor;
        QColor endColor;
        QElapsedTimer timer;
        int duration;
        AnimationType type;
    };

    void ensureVisibleHighlighted();
    void paintLayers(QPainter &painter);
    void paintTextBlocks(QPainter &painter, const QRect &exposed);
    //! Draw markers for whitespace, where it's shown or in `forced` ranges
    void paintWhitespace(QPainter &painter, const QTextBlock &block, const QPointF &offset,
                         const std::vector<std::pair<int, int>> &forced);
    void paintRanges(QPainter &painter, const std::vector<std::pair<int, int>> &ranges, const QColor &color,
                     const std::vector<VisibleLine> &lines);
    std::vector<std::pair<int, int>> searchMatchesIn(int startLine, int endLine) const;
    void updateGutterGeometry();
    void updateScrollRange();
    void extendScrollRange(int min, int max);

    MeldBuffer *buffer_ = nullptr;
    LineNumberGutter *gutter_;
    bool showLineNumbers_ = false;
    bool drawSpaces_ = false;
    bool highlightCurrentLine_ = false;
    bool wrapText_ = false;
    int overscrollLines_ = 5;
    int extendedMaximum_ = -1;
    bool adjustingRange_ = false;
    SearchHighlight search_;
    QRegularExpression searchRegex_;
    std::optional<QColor> cursorColor_;
    std::optional<QColor> spacesColor_;
    std::vector<Animation> animations_;
    QTimer animationTimer_;
};

} // namespace meld
