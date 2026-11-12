#pragma once

#include "actiongutter.h"
#include "core/diffutil.h"
#include "core/filters.h"
#include "core/meldbuffer.h"
#include "linkmap.h"
#include "melddoc.h"
#include "syncpoints.h"

#include <QFileSystemWatcher>
#include <QPointer>

#include <array>
#include <memory>
#include <optional>

class QGridLayout;
class QToolButton;
class QWidget;

namespace meld {

class CachedSequenceMatcher;
class FileButton;
class FindBar;
class MsgAreaController;
class PathLabel;
class SourceView;
class StatusBar;
class TextViewChunkMap;

enum class FileComparisonMode {
    Compare,
    AutoMerge,
};

//! Two or three way comparison of text files
class FileDiff : public MeldDoc, public ChunkSource {
    Q_OBJECT

public:
    explicit FileDiff(int numPanes, FileComparisonMode mode = FileComparisonMode::Compare,
                      QWidget *parent = nullptr);
    ~FileDiff() override;

    // ChunkSource
    const Differ *differ() const override { return linediffer_.get(); }
    std::optional<int> currentChunk() const override { return cursor_.chunk; }

    //! Load the given files; an empty path leaves a pane blank
    /*!
     * Make panes' text in memory, rather than reading it from files: those
     * panes are given empty files in setFiles(). They're unnamed, so their
     * text can be saved elsewhere.
     */
    void setVirtualFiles(const QMap<int, VirtualFile> &files);
    void setFiles(const QStringList &files, const QList<QByteArray> &encodings = {});
    void setFile(int pane, const QString &file, const QByteArray &encoding = QByteArray());
    void setMergeOutputFile(const QString &file);
    void setMeta(const ComparisonMeta &meta);
    void setLabels(const QStringList &labels) override;
    std::optional<Comparison> comparison() const override;

    FilterVisibility filterVisibility() const override { return {true, false, false}; }
    bool conflictVisibility() const override { return numPanes_ == 3; }
    ToolbarKind toolbarKind() const override { return ToolbarKind::FileDiff; }
    QList<QPair<QString, QString>> textFilterActions() const override;
    void requestClose() override;
    QWidget *focusPane() const override;
    void onContainerSwitchIn() override;

    FileComparisonMode comparisonMode() const { return comparisonMode_; }
    SourceView *textView(int pane) const { return textview_[size_t(pane)]; }
    MeldBuffer *textBuffer(int pane) const { return textbuffer_[size_t(pane)]; }
    StatusBar *statusBar(int pane) const { return statusbar_[size_t(pane)]; }
    MsgAreaController *messageArea(int pane) const { return msgarea_[size_t(pane)]; }
    PathLabel *fileLabel(int pane) const { return filelabel_[size_t(pane)]; }

    //! Whether a comparison has completed and live updates are connected
    bool comparisonReady() const { return buffersConnected_; }

    // Chunk actions on explicit panes, as used by the action gutters
    void replaceChunk(int src, int dst, const DiffChunk &chunk);
    void copyChunk(int src, int dst, const DiffChunk &chunk, bool copyUp);
    void deleteChunk(int src, const DiffChunk &chunk);

    void refreshComparison();
    bool saveFile(int pane, bool saveas = false, bool forceOverwrite = false);
    void goToChunk(std::optional<int> target, std::optional<int> pane = std::nullopt, bool centered = false);
    void moveCursor(int pane, int line, bool focus = true);

    void setActionMode(ActionMode mode);
    ActionMode actionMode() const { return actionMode_; }

Q_SIGNALS:
    void nextConflictChanged(bool havePrevious, bool haveNext);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct CursorDetails {
        std::optional<int> chunk;
        std::optional<int> line;
        std::optional<int> next;
        std::optional<int> nextConflict;
        std::optional<int> pane;
        std::optional<int> pos;
        std::optional<int> prev;
        std::optional<int> prevConflict;
    };

    // Construction (filediff_layout.cpp)
    void buildLayout();
    void createActions();
    QWidget *buildFileToolbar(int pane);

    // Comparison management
    void setNumPanes(int numPanes);
    bool createTextFilters();
    void disconnectBufferHandlers();
    void connectBufferHandlers();
    void preComparisonInit();
    void loadFileInPane(int pane, const QString &file, const QByteArray &encoding);
    //! Reset a pane's buffer for loading `file`; done synchronously, so that
    //! e.g. a merge output file set afterwards isn't reset by the load
    void prepareLoadInPane(int pane, const QString &file);
    void readFileInPane(int pane, const QString &file, const QByteArray &encoding);
    TaskGenerator mergeFiles();
    TaskGenerator diffFiles(bool refresh);
    TaskGenerator compareFilesInternal();
    void onLinesChanged(int pane, int startLine, int linesRemoved, int linesAdded);
    QString filterText(int pane, const QString &text, int start, int end);
    void onDiffsChanged(const ChunkChanges &changes);
    void applyInlineHighlights(const ChunkChanges &changes);

    // Cursor and sensitivity handling
    void onCursorPositionChanged(int pane, bool force = false);
    void onCurrentDiffChanged();
    void updateTextActionsSensitivity();
    void setSaveActionSensitivity();
    void setMergeActionSensitivity();
    void setExternalActionSensitivity();
    void setFocusedTextview(SourceView *view);
    int focusedPane(bool useLastFocusedPane = false) const;
    void updateStatusbarCursor(int pane);
    void setKeymask(int mask);

    // Navigation
    void scrollToChunkIndex(int chunkIndex);
    void nextDiff(bool down, bool centered = false, bool useViewport = false);
    bool isChunkInArea(std::optional<int> chunkId, int pane, qreal top, qreal bottom) const;
    DiffChunk synthChunk(int pane0, int pane1, int line) const;
    int correspondingChunkLine(std::optional<int> chunk, int line, int pane, int newPane) const;
    void moveCursorPane(int pane, int newPane);

    // Actions
    std::optional<DiffChunk> actionChunk(int src, int dst) const;
    std::pair<int, int> actionPanes(int direction, bool reverse = false) const;
    void pushChange(int direction);
    void pullChange(int direction);
    void copyChange(int direction, bool copyUp);
    void pullAllNonConflictingChanges(int src, int dst);
    void mergeAllChanges();
    void deleteChange();
    void onChunkActionActivated(ChunkAction action, SourceView *from, SourceView *to, const DiffChunk &chunk);
    void undo();
    void redo();
    void scrollToAffected(QTextDocument *document);
    void showContextMenu(int pane, const QPoint &globalPos);
    void setSyncpointMenuItems(int pane);
    void refreshSyncPoints();
    void swapPanes();
    void openExternal();
    void openFolder();
    void copyFullPath();
    void formatAsPatch();
    void findAction(bool replace);

    // Files
    void recomputeLabel();
    void updateBufferWritable(int pane);
    void setBufferEditable(int pane, bool editable);
    void doSaveFile(int pane, bool forceOverwrite);
    void onFileSaved(int pane);
    void notifyFileChanged(int pane);
    void onWatchedFileChanged(const QString &path);
    void confirmUnsavedChangeAction(std::function<void()> onConfirm, const QList<int> &panes = {});
    void revert();
    void revertPane(int pane);
    void requestCloseResponse(const QString &response, const QList<bool> &trySave);
    bool promptResolveConflict();
    void showSameFilesMessage();
    void promptLongHighlighting();
    void promptLoadLongLines(int pane, const QString &primary, const QString &secondary);
    void dropFiles(int pane, const QStringList &files);

    // Scrolling
    void syncVScroll(int master);
    void syncHScroll(int master);
    void queueDraw();

    FileComparisonMode comparisonMode_;
    std::unique_ptr<Differ> linediffer_;
    std::array<MeldBuffer *, 3> textbuffer_{};
    std::array<SourceView *, 3> textview_{};
    std::array<std::unique_ptr<BufferLines>, 3> bufferTexts_;
    std::array<std::unique_ptr<BufferLines>, 3> bufferFiltered_;
    UndoSequence undosequence_;
    Syncpoints syncpoints_;
    CachedSequenceMatcher *cachedMatch_;
    CursorDetails cursor_;
    std::optional<ComparisonMeta> meta_;
    //! Panes whose text is made in memory (when they have no file)
    std::array<std::optional<VirtualFile>, 3> virtualFiles_;
    //! Panes to save even with unresolved conflicts, as asked
    std::array<bool, 3> saveUnresolved_{};
    bool middleSaved_ = false;
    FilterList textFilters_;
    QPointer<SourceView> focusPane_;
    QFileSystemWatcher watcher_;

    // Widgets
    QGridLayout *grid_ = nullptr;
    std::array<QWidget *, 3> vbox_{};
    std::array<QWidget *, 3> fileToolbar_{};
    std::array<QToolButton *, 3> readonlyToggle_{};
    std::array<QToolButton *, 3> fileSaveButton_{};
    std::array<FileButton *, 3> fileOpenButton_{};
    std::array<PathLabel *, 3> filelabel_{};
    std::array<MsgAreaController *, 3> msgarea_{};
    std::array<StatusBar *, 3> statusbar_{};
    std::array<TextViewChunkMap *, 3> chunkmap_{};
    std::array<LinkMap *, 2> linkmap_{};
    std::array<ActionGutter *, 4> actiongutter_{};
    std::array<QWidget *, 2> dummyStatusbar_{};
    QWidget *chunkmapBox_ = nullptr;
    QWidget *overviewToolbarSpacer_ = nullptr;
    QWidget *overviewStatusSpacer_ = nullptr;
    FindBar *findbar_ = nullptr;
    QList<QAction *> viewLocalActions_;

    int keymask_ = 0;
    ActionMode actionMode_ = ActionMode::Replace;
    bool buffersConnected_ = false;
    bool syncVScrollLock_ = false;
    bool syncHScrollLock_ = false;
    bool lockScrolling_ = false;
    bool forceHighlight_ = false;
    bool forceLoad_ = false;
    bool warnedBadComparison_ = false;
    bool ignoreBlankLines_ = false;
    bool showOverviewMap_ = true;
    //! Lines changed by the most recent edit, used to refresh highlights
    std::optional<std::pair<int, std::pair<int, int>>> lastEdit_;

    static constexpr int MsgSame = 0;
    static constexpr int MsgSlowHighlight = 1;
    static constexpr int MsgSyncpoints = 2;
    static constexpr int MsgLineTooLong = 3;
};

} // namespace meld
