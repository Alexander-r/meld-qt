// Synchronisation points are kept as QTextCursors, which move with edits
// in the same way as GtkSourceMarks.

#pragma once

#include <QList>
#include <QString>
#include <QTextCursor>

#include <vector>

class QTextDocument;

namespace meld {

enum class SyncpointAction {
    //! A dangling syncpoint can be moved to the line
    Move,
    //! A dangling syncpoint sits can be removed from this line
    Delete,
    //! A syncpoint can be added to this line to match existing ones in
    //! other panes
    Match,
    //! A new, dangling syncpoint can be added to this line
    Add,
    //! No syncpoint-related action can be taken on this line
    Disabled,
};

QString syncpointActionName(SyncpointAction action);

class Syncpoints {
public:
    enum class PaneState {
        //! The state of a pane with all its syncpoints matched
        Matched,
        //! The state of a pane waiting to be matched to existing
        //! syncpoints in other panes
        Short,
        //! The state of a pane with a dangling syncpoint, not yet matched
        //! across all panes
        Dangling,
    };

    struct Mark {
        int id;
        QTextCursor cursor;
    };

    void setDocuments(const QList<QTextDocument *> &documents);

    //! Add a syncpoint at the start of the line containing `cursorPosition`
    void add(int pane, int cursorPosition);
    //! Move an unmatched mark (i.e., not part of a valid pairing) to the cursor
    void move(int pane, int cursorPosition);
    void remove(int pane, int cursorPosition);
    void clear();

    //! Matched syncpoints, as the line of each pane's mark
    std::vector<std::vector<int>> validPoints() const;
    SyncpointAction action(int pane, int cursorPosition) const;
    //! Lines of all of the syncpoints in a pane, for display
    std::vector<int> lines(int pane) const;

private:
    int lineStart(int pane, int cursorPosition) const;
    int markAtCursor(int pane, int cursorPosition) const;
    void sortMarks(int pane);
    void updateSyncpointState();
    int lineOf(int pane, const Mark &mark) const;

    QList<QTextDocument *> documents_;
    const Mark *findMark(int pane, int id) const;

    std::vector<std::vector<Mark>> marks_;
    //! Most recent calculated set of pairings (the mark IDs of each
    //! syncpoint). These are recalculated only when all panes have the
    //! same number of syncpoints.
    std::vector<std::vector<int>> cachedPoints_;
    std::vector<PaneState> paneStates_;
    int nextId_ = 0;
};

} // namespace meld
