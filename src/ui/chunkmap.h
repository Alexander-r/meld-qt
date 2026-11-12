#pragma once

#include "core/diffchunk.h"

#include <QList>
#include <QModelIndex>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>
#include <vector>

class QScrollBar;
class QTreeView;

namespace meld {

class SourceView;

/*!
 * An overview map showing the location of chunks in a whole document,
 * together with a handle showing the visible area. Clicking or dragging
 * scrolls the mapped view.
 */
class ChunkMap : public QWidget {
    Q_OBJECT

public:
    explicit ChunkMap(QScrollBar *scrollBar, QWidget *parent = nullptr);

    void invalidate();

protected:
    using TaggedCoords = QMap<ChunkTag, std::vector<std::pair<double, double>>>;

    //! Map chunks to fractional document offsets for drawing, by tag
    virtual TaggedCoords chunkCoordsByTag() const = 0;
    virtual double heightScale() const { return 1.0; }
    virtual void scrollToLocation(double fraction) = 0;

    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;

    QPointer<QScrollBar> scrollBar_;

private:
    void scrollFraction(double position);

    std::optional<QPixmap> cachedMap_;
    bool grabbing_ = false;
    static constexpr int OverdrawPadding = 2;
};

//! Chunk map for a text view
class TextViewChunkMap : public ChunkMap {
    Q_OBJECT

public:
    TextViewChunkMap(SourceView *view, QWidget *parent = nullptr);

    void setChunks(std::vector<DiffChunk> chunks);
    void setPairedViews(const QList<SourceView *> &views);

protected:
    TaggedCoords chunkCoordsByTag() const override;
    double heightScale() const override;
    void scrollToLocation(double fraction) override;

private:
    QPointer<SourceView> view_;
    QList<QPointer<SourceView>> paired_;
    std::vector<DiffChunk> chunks_;
};

//! Chunk map for a folder comparison tree view
class TreeViewChunkMap : public ChunkMap {
    Q_OBJECT

public:
    //! `tagFor` maps a row to the chunk type shown for it, if any. Error
    //! states are reported as ChunkTag::Equal, which is drawn in the error
    //! colour.
    TreeViewChunkMap(QTreeView *view, std::function<std::optional<ChunkTag>(const QModelIndex &)> tagFor,
                     QWidget *parent = nullptr);

protected:
    TaggedCoords chunkCoordsByTag() const override;
    void scrollToLocation(double fraction) override;

private:
    QPointer<QTreeView> view_;
    std::function<std::optional<ChunkTag>(const QModelIndex &)> tagFor_;
};

} // namespace meld
