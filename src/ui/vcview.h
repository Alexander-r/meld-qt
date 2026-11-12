#pragma once

#include "core/vc/vc.h"
#include "dirdifftree.h"
#include "melddoc.h"

#include <QPersistentModelIndex>
#include <QPointer>

#include <memory>
#include <vector>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QSplitter;
class QTreeView;

namespace meld {

class FileButton;
class MsgAreaController;
class PathLabel;

//! The version control tree: a single pane, plus location, status and extra
//! columns (VcTreeStore)
class VcTreeModel : public DirDiffTreeModel {
    Q_OBJECT

public:
    enum Column {
        ColName,
        ColLocation,
        ColStatus,
        ColOptions,
        ColumnCount,
    };

    explicit VcTreeModel(QObject *parent = nullptr);

    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    QString filePath(const TreeNode *node) const { return valuePath(node, 0); }
    QString text(const TreeNode *node, Column column) const;
    void setText(TreeNode *node, Column column, const QString &text);
};

//! A version control system that can handle a location, for the view's
//! selector; systems that can't are listed with the reason
struct VcChoice {
    QString label;
    std::shared_ptr<vc::Vc> vc;
    bool enabled = false;
};

struct VcChoices {
    std::vector<VcChoice> choices;
    int defaultIndex = 0;
    QString tooltip;
};

/*!
 * Find the version control systems that can handle `location`
 *
 * The default is the system named `preferredName` if it's available, and
 * otherwise the one with the deepest repository root. If there are none,
 * the "None" system is used.
 */
VcChoices vcChoicesForLocation(QString location, const QString &preferredName = QString());

//! Files for comparing a version-controlled file: the repository version
//! and the working copy, or the three sides of a conflict
struct VcFileComparison {
    QStringList paths;
    DiffOptions options;
};

/*!
 * The comparison for the version-controlled file `path`
 *
 * Temporary files are made read-only and removed when Meld exits. The
 * resolve callback of conflict comparisons runs the version control
 * system's resolve command through `view` if it still exists, or directly
 * otherwise. Throws vc::InvalidPath or std::runtime_error.
 */
VcFileComparison vcFileComparison(const std::shared_ptr<vc::Vc> &vc, const QString &path,
                                  const QPointer<class VcView> &view = {});

//! How a revision is shown: empty revisions are the current one (HEAD) as
//! a base, and the working copy as a target
QString revisionName(const QString &revision, bool target = false);

//! Marks files as resolved with `vc`, through `view` if it still exists
std::function<void(const QString &)> vcResolver(const std::shared_ptr<vc::Vc> &vc, const QPointer<class VcView> &view = {});

//! Delete the temporary files of version control comparisons
void cleanupVcTempFiles();

class VcView : public MeldDoc {
    Q_OBJECT

public:
    explicit VcView(QWidget *parent = nullptr);
    ~VcView() override;

    //! Show the version control state of `location`
    void setLocation(const QString &location);
    QString location() const { return location_; }
    const std::shared_ptr<vc::Vc> &vc() const { return vc_; }

    std::optional<Comparison> comparison() const override;
    void setLabels(const QStringList &labels) override;
    FilterVisibility filterVisibility() const override { return {false, false, true}; }
    void onFileChanged(const QString &path) override;
    void onContainerSwitchIn() override;
    QWidget *focusPane() const override;

    /*!
     * Compare `base` with `target` rather than the working copy with the
     * current revision; empty revisions are those defaults. Any commit, tag
     * or tree can be compared with another, or with the working copy.
     * Returns false, and shows why, if a revision isn't in the repository.
     */
    bool setRevisions(const QString &base, const QString &target);
    QString baseRevision() const { return baseRevision_; }
    QString targetRevision() const { return targetRevision_; }

    //! Compare the given file with its repository version, or its conflict
    void runDiff(const QString &path);
    //! Open comparisons for all modified rows (the --auto-compare option)
    void autoCompare();
    void refresh();

    /*!
     * Run a command against this view's version control system
     *
     * This is the intended way for things outside of the view to call in
     * to version control methods, e.g., to mark a conflict as resolved
     * from a file comparison. The only command is "resolve". If `sync` is
     * true, the command is run immediately instead of as a task.
     */
    bool hasCommand(const QString &command) const;
    void command(const QString &command, const QStringList &files, bool sync = false);

    // Widgets and state, for tests
    QTreeView *treeView() const;
    VcTreeModel *model() const { return model_; }
    QComboBox *vcSelector() const { return comboVcs_; }
    QComboBox *baseRevisionSelector() const { return baseCombo_; }
    QComboBox *targetRevisionSelector() const { return targetCombo_; }
    QWidget *revisionBar() const { return revisionBar_; }
    MsgAreaController *messageArea() const { return msgarea_; }
    QWidget *consolePane() const { return consoleBox_; }
    QString consoleText() const;
    QStringList statusFilters() const { return statusFilters_; }
    //! The selected rows' paths, without trailing separators
    QStringList selectedFiles() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void buildLayout();
    void createActions();
    void populateVcsForLocation(const QString &location);
    //! Offer the version control system's revisions in the selectors
    void populateRevisions();
    void onRevisionChosen();
    //! Whether a path is a folder, on disk or in the compared revisions
    bool isFolder(const QString &path) const;
    void onVcChange();
    void setVcLocation(const QString &location);
    void recomputeLabel();
    TaskGenerator searchRecursivelyIter(QList<int> startPath, bool replace);
    TreeNode *nodeAt(const QList<int> &treePath) const;
    void updateItemState(TreeNode *node, const vc::Entry &entry);
    TreeNode *findNodeByName(const QString &name) const;
    void refreshPartial(const QString &where);

    void onRowActivated(const QModelIndex &index);
    void onSelectionChanged();
    void onCursorChanged(bool force);
    void nextDiff(bool down);
    void showContextMenu(const QPoint &globalPos);
    void startSearch();
    void actionFilterStateChange();
    void setConsoleVisible(bool visible);
    void applyConsolePosition();

    // Commands
    void runCommands(const QList<vc::Command> &commands, bool sync = false);
    TaskGenerator commandIter(vc::Command command);
    void consoleWrite(const QString &message, int kind);

    // Actions
    void actionCommit();
    void actionPush();
    void actionRemove();
    void actionDelete();
    void actionDiff();
    void actionOpenExternal();

    VcTreeModel *model_ = nullptr;
    std::shared_ptr<vc::Vc> vc_;
    std::vector<VcChoice> vcChoices_;
    QString location_;
    QStringList statusFilters_;
    QPersistentModelIndex currentPath_;
    QPersistentModelIndex prevPath_;
    QPersistentModelIndex nextPath_;

    class VcTreeView *treeview_ = nullptr;
    QComboBox *comboVcs_ = nullptr;
    QString baseRevision_;
    QString targetRevision_;
    QWidget *revisionBar_ = nullptr;
    QComboBox *baseCombo_ = nullptr;
    QComboBox *targetCombo_ = nullptr;
    MsgAreaController *msgarea_ = nullptr;
    FileButton *fileButton_ = nullptr;
    PathLabel *filelabel_ = nullptr;
    QSplitter *splitter_ = nullptr;
    QWidget *consoleBox_ = nullptr;
    QPlainTextEdit *consoleview_ = nullptr;
    QLineEdit *searchEntry_ = nullptr;
    bool consolePositioned_ = false;
};

} // namespace meld
