#pragma once

#include "core/diffchunk.h"
#include "sourceview.h"

#include <QPointer>
#include <QWidget>

#include <optional>
#include <utility>
#include <vector>

namespace meld {


//! Action mode for chunk change actions
enum class ActionMode {
    Replace = 0,
    Delete = 1,
    Insert = 2,
};

enum class ChunkAction {
    Delete,
    Replace,
    CopyDown,
    CopyUp,
};

//! Draws chunk action buttons between a text view and a link map
class ActionGutter : public QWidget {
    Q_OBJECT

public:
    ActionGutter(Qt::LayoutDirection iconDirection, QWidget *parent = nullptr);

    void setViews(SourceView *source, SourceView *target);
    SourceView *sourceView() const { return source_; }
    SourceView *targetView() const { return target_; }

    void setChunks(std::vector<DiffChunk> chunks);
    const std::vector<DiffChunk> &chunks() const { return chunks_; }

    ActionMode actionMode() const { return actionMode_; }
    void setActionMode(ActionMode mode);

    /*!
     * Classify possible actions for the given change
     *
     * Returns the action that can be performed given the content and
     * context of the change.
     */
    static std::optional<ActionMode> classifyChangeActions(const DiffChunk &change, ActionMode mode,
                                                           bool sourceEditable, bool targetEditable);

    //! The top and height of the button of a chunk starting at `line`, in
    //! the coordinates of the view's viewport
    static std::pair<qreal, qreal> buttonExtent(const SourceView *view, int line);
    /*!
     * The top and bottom of the band around the button of a chunk with no
     * lines in this view, which the band widens to in the gutter, and which
     * the link map continues from, in the coordinates of the view's viewport
     */
    static std::pair<qreal, qreal> emptyChunkBand(const SourceView *view, int line);
    /*!
     * The last line that can have a chunk showing at this height of the view:
     * the one there, or the one after the last line, whose chunk's band is on
     * the last line
     */
    static int lastChunkLineAtY(const SourceView *view, qreal y);

Q_SIGNALS:
    void chunkActionActivated(meld::ChunkAction action, meld::SourceView *from, meld::SourceView *to,
                              const meld::DiffChunk &chunk);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Button {
        QRectF rect;
        DiffChunk chunk;
    };

    std::vector<DiffChunk> chunkRange(qreal startY, qreal endY) const;
    void updatePointerChunk(const QPointF &position);
    void activate(const DiffChunk &chunk, const QPointF &position);
    QString iconName(ActionMode mode) const;

    Qt::LayoutDirection iconDirection_;
    QPointer<SourceView> source_;
    QPointer<SourceView> target_;
    std::vector<DiffChunk> chunks_;
    std::vector<int> chunkStarts_;
    ActionMode actionMode_ = ActionMode::Replace;
    std::vector<Button> buttons_;
    std::optional<DiffChunk> pointerChunk_;
    std::optional<DiffChunk> pressedChunk_;
};

} // namespace meld
