#pragma once

#include <QPointer>
#include <QWidget>

#include <optional>

namespace meld {

class Differ;
class SourceView;

//! The comparison state needed by the link map and gutters
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual const Differ *differ() const = 0;
    virtual std::optional<int> currentChunk() const = 0;
};

//! Draws the curves linking chunks between two adjacent panes
class LinkMap : public QWidget {
    Q_OBJECT

public:
    explicit LinkMap(QWidget *parent = nullptr);

    void associate(ChunkSource *source, SourceView *left, SourceView *right, int leftPane, int rightPane);

Q_SIGNALS:
    //! Scrolling over the link map moves between changes
    void scrollRequested(bool down);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    ChunkSource *source_ = nullptr;
    QPointer<SourceView> views_[2];
    int panes_[2] = {0, 1};
    int wheelAccumulator_ = 0;
};

//! A plain spacer between folder comparison panes that handles scrolling
class ScrollLinkMap : public QWidget {
    Q_OBJECT

public:
    explicit ScrollLinkMap(QWidget *parent = nullptr);

Q_SIGNALS:
    void scrollRequested(bool down);

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    int wheelAccumulator_ = 0;
};

//! Turn wheel deltas into discrete steps; returns +1/-1 or 0
int wheelSteps(int &accumulator, int delta);

} // namespace meld
