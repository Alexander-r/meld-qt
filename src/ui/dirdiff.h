#pragma once

#include "core/filecompare.h"
#include "core/filters.h"
#include "dirdifftree.h"
#include "melddoc.h"

#include <QPersistentModelIndex>
#include <QPointer>
#include <QSet>
#include <QTreeView>

#include <array>
#include <map>
#include <memory>
#include <optional>

class QGridLayout;
class QLineEdit;

namespace meld {

class FileButton;
class MsgAreaController;
class PathLabel;
class ScrollLinkMap;
class TreeViewChunkMap;

struct ComparisonOptions {
    bool ignoreCase = false;
    bool normalizeEncoding = false;
};

//! Multi-pane lists with canonicalised matching and error detection
class CanonicalListing {
public:
    CanonicalListing(int n, ComparisonOptions options);

    void add(int pane, const QString &item);
    //! Sorted name tuples, with missing entries filled from other panes
    std::vector<QStringList> get() const;

    struct Error {
        int pane;
        QString item;
        QString existing;
    };
    std::vector<Error> errors;
    std::vector<std::pair<int, QString>> whitespace;

private:
    int n_;
    ComparisonOptions options_;
    std::map<QString, QStringList> items_;
    QHash<QString, QString> strippedItems_;
};

//! A tree view for one pane of a folder comparison
class DirTreeView : public QTreeView {
    Q_OBJECT

public:
    explicit DirTreeView(QWidget *parent = nullptr);

    //! Meld's search: basenames (or full paths if the key contains a
    //! separator), case-insensitively if the key is lower-case
    static bool searchMatches(const QStringList &paths, const QString &key);

Q_SIGNALS:
    void focusedIn();
    void paneSwitchRequested(bool right);
    void contextMenuRequested(const QPoint &globalPos);
    void compareRequested();
    void deleteRequested();

protected:
    void focusInEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
};

namespace patch {
class PatchedFolder;
}

class DirDiff : public MeldDoc {
    Q_OBJECT

public:
    explicit DirDiff(int numPanes, QWidget *parent = nullptr);
    ~DirDiff() override;

    //! Set the folders to compare; empty paths are allowed for blank panes
    void setFolders(const QStringList &folders);
    QStringList folders() const { return folders_; }
    void setLocations();
    void setLabels(const QStringList &labels) override;
    std::optional<Comparison> comparison() const override;

    FilterVisibility filterVisibility() const override { return {false, true, false}; }
    ToolbarKind toolbarKind() const override { return ToolbarKind::DirDiff; }
    QList<QPair<QString, QString>> fileFilterActions() const override;
    void onFileChanged(const QString &path) override;
    void requestClose() override;
    QWidget *focusPane() const override;

    //! Open comparisons for all modified rows (the --auto-compare option)
    void autoCompare();
    void refresh();

    /*!
     * Compare a folder with itself as a patch would change it. The folders
     * are the folder and the patch file, whose pane shows the patched
     * folder; which files the patch changes is known from the patch, and
     * their patched text is made when they're compared.
     */
    void setPatch(std::shared_ptr<patch::PatchedFolder> patched);
    std::shared_ptr<patch::PatchedFolder> patchedFolder() const { return patched_; }

    DirDiffTreeModel *model() const { return model_; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

public:
    DirTreeView *treeView(int pane) const { return treeview_[size_t(pane)]; }
    MsgAreaController *messageArea(int pane) const { return msgarea_[size_t(pane)]; }
    bool scanInProgress() const { return scanInProgress_ > 0; }

private:
    void buildLayout();
    void createActions();
    void setNumPanes(int numPanes);
    bool createNameFilters();
    bool createTextFilters();
    void updateComparator();
    void updateTreeviewColumns();
    QList<QRegularExpression> activeByteFilters() const;
    FileCompareResult fileCompare(const QStringList &files) const;

    // Paths in the patched folder's pane, in patch comparisons
    //! The path in the patched folder of a path in its pane, if it's one
    std::optional<QString> patchedPath(const QString &path) const;
    bool pathExists(const QString &path) const;
    bool pathIsDir(const QString &path) const;
    bool pathIsFile(const QString &path) const;
    //! The state of a row in a patch comparison
    int patchedState(const QStringList &files) const;
    bool updatePatchedItemState(TreeNode *node);
    void reloadPatch(const QString &folder);

    void recursivelyUpdate(const QModelIndex &index);
    void markInProgressRow(TreeNode *node);
    TaskGenerator searchRecursivelyIter(QPersistentModelIndex root);
    std::vector<QStringList> filterOnState(const QStringList &roots, const std::vector<QStringList> &fileslist);
    bool updateItemState(TreeNode *node);
    void showIdenticalStatus();
    void showDuplicateDirectory(const QString &directory);
    void showTreeWideErrors(const std::vector<std::pair<int, QString>> &invalid,
                            const std::vector<std::tuple<int, QString, QString, QString>> &shadowed,
                            const std::vector<std::pair<int, QString>> &whitespace);
    void recomputeLabel();

    int focusedPane() const;
    QList<TreeNode *> selectedNodes(int pane) const;
    void updateActionSensitivity();
    void onCursorChanged(int pane);
    void changeFocusedTree(int oldPane, int newPane);
    void onRowActivated(int pane, const QModelIndex &index);
    void onRowExpanded(int pane, const QModelIndex &index);
    void onRowCollapsed(int pane, const QModelIndex &index);
    void onFocusIn(int pane);
    void runDiffFromNode(TreeNode *node);
    void nextDiff(bool down);
    void showContextMenu(int pane, const QPoint &globalPos);
    void startSearch();

    // Actions
    void actionDiff();
    void actionMark();
    void actionDiffMarked();
    void actionCollapse();
    void actionExpand();
    void copySelected(int direction);
    void deleteSelected();
    void fileCreated(TreeNode *node);
    void fileDeleted(TreeNode *node, int pane);
    void actionOpenExternal();
    void actionCopyFilePaths();
    void actionFilterStateChange();

    void syncVScroll(int master);
    void syncHScroll(int master);

    DirDiffTreeModel *model_ = nullptr;
    QStringList folders_;
    std::shared_ptr<patch::PatchedFolder> patched_;
    QStringList customLabels_;
    FilterList nameFilters_;
    FilterList textFilters_;
    ComparisonArgs comparisonArgs_;
    std::vector<int> stateFilters_;
    int scanInProgress_ = 0;
    bool doToOthersLock_ = false;
    bool forceCursorRecalculate_ = false;
    bool ignoreSymlinks_ = false;
    bool showOverviewMap_ = true;
    QPersistentModelIndex currentPath_;
    QPersistentModelIndex prevPath_;
    QPersistentModelIndex nextPath_;
    QPointer<DirTreeView> focusPane_;
    struct Marker {
        int pane;
        QPersistentModelIndex row;
    };
    std::optional<Marker> marked_;

    std::array<DirTreeView *, 3> treeview_{};
    std::array<QWidget *, 3> vbox_{};
    std::array<QWidget *, 3> paneActionbar_{};
    std::array<FileButton *, 3> folderOpenButton_{};
    std::array<PathLabel *, 3> folderLabel_{};
    std::array<MsgAreaController *, 3> msgarea_{};
    std::array<TreeViewChunkMap *, 3> chunkmap_{};
    std::array<ScrollLinkMap *, 2> linkmap_{};
    std::array<QWidget *, 2> dummyToolbarLinkmap_{};
    QGridLayout *grid_ = nullptr;
    QWidget *chunkmapBox_ = nullptr;
    QWidget *overviewToolbarSpacer_ = nullptr;
    QLineEdit *searchEntry_ = nullptr;
};

} // namespace meld
