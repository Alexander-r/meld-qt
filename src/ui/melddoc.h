#pragma once

#include "core/task.h"

#include <QAction>
#include <QHash>
#include <QMap>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <optional>

namespace meld {

enum class RecentType {
    File,
    Folder,
    VersionControl,
    Merge,
};

QString recentTypeName(RecentType type);
std::optional<RecentType> recentTypeFromName(const QString &name);

struct Comparison {
    RecentType type = RecentType::File;
    QStringList paths;
};

/*!
 * A file comparison pane's text that isn't read from a file, but made in
 * memory when the pane is loaded (e.g., a file with a patch applied)
 */
struct VirtualFile {
    struct Content {
        //! The file's bytes, which are decoded as files' are; nullopt if
        //! they couldn't be made
        std::optional<QByteArray> data;
        //! Why not, with a heading
        QString errorTitle;
        QString error;
        //! Shown with the text, e.g., how a patch was applied
        QString noteTitle;
        QStringList notes;
    };
    //! The pane's label
    QString label;
    std::function<Content()> load;
};

//! Details of file comparisons started from a version control view
struct ComparisonMeta {
    //! Labels for each pane; empty entries keep the default label
    QStringList labels;
    QString tabLabel;
    //! Panes whose text is made in memory, rather than read from files
    QMap<int, VirtualFile> virtualFiles;
    //! Offer to mark the conflict as resolved when the comparison is closed
    //! after saving the merged file
    bool promptResolve = false;
    //! Marks the given conflicted file as resolved in version control
    std::function<void(const QString &path)> resolve;
    //! Ask before saving the merge (the middle pane) with unresolved
    //! conflicts, which are dropped otherwise
    bool warnUnresolved = false;
};

//! Options for creating a new comparison from an existing tab
struct DiffOptions {
    bool autoCompare = false;
    bool autoMerge = false;
    QString mergeOutput;
    std::optional<ComparisonMeta> meta;
};

//! Which per-view filter menus the window should show
struct FilterVisibility {
    bool text = false;
    bool folder = false;
    bool vc = false;
};

enum class ToolbarKind {
    None,
    FileDiff,
    DirDiff,
};

//! Base class for documents (comparison tabs) in the meld application
class MeldDoc : public QWidget {
    Q_OBJECT

public:
    enum class State {
        Normal = 0,
        Closing = 1,
        SavingError = 2,
    };

    explicit MeldDoc(QWidget *parent = nullptr);
    ~MeldDoc() override;

    FifoScheduler &scheduler() { return scheduler_; }

    QString tabTitle() const { return tabTitle_; }
    QString tabTooltip() const { return tabTooltip_; }

    //! The named actions of this document ("view" actions in GTK)
    QAction *action(const QString &name) const;
    QStringList actionNames() const { return actions_.keys(); }
    void setActionEnabled(const QString &name, bool enabled);
    bool actionState(const QString &name) const;
    void setActionState(const QString &name, bool state);

    State state() const { return state_; }
    void setState(State state);
    int numPanes() const { return numPanes_; }

    virtual FilterVisibility filterVisibility() const { return {}; }
    virtual bool conflictVisibility() const { return false; }
    virtual ToolbarKind toolbarKind() const { return ToolbarKind::None; }
    //! Filter menu entries: (action name, label) pairs
    virtual QList<QPair<QString, QString>> textFilterActions() const { return {}; }
    virtual QList<QPair<QString, QString>> fileFilterActions() const { return {}; }

    virtual std::optional<Comparison> comparison() const { return std::nullopt; }
    virtual void setLabels(const QStringList &labels) { Q_UNUSED(labels); }
    virtual void onFileChanged(const QString &path) { Q_UNUSED(path); }
    //! Called when the container app switches to this tab
    virtual void onContainerSwitchIn();
    //! Called when the docs container is about to close
    virtual void requestClose();
    void actionStop();

    //! The widget that should get focus when this tab is activated
    virtual QWidget *focusPane() const { return nullptr; }

Q_SIGNALS:
    void tabLabelChanged();
    void closed(int status);
    void createDiff(const QStringList &paths, const meld::DiffOptions &options);
    void fileChanged(const QString &path);
    void stateChanged(meld::MeldDoc::State oldState, meld::MeldDoc::State newState);
    //! The set of filter actions changed, so menus should be rebuilt
    void filterActionsChanged();
    void statusMessage(const QString &message);

protected:
    QAction *addAction(const QString &name, std::function<void()> callback);
    QAction *addToggleAction(const QString &name, bool initial, std::function<void(bool)> callback);
    void removeAction(const QString &name);
    void setTabLabel(const QString &title, const QString &tooltip);
    void errorBell();

    FifoScheduler scheduler_;
    QHash<QString, QAction *> actions_;
    int numPanes_ = 0;

private:
    QString tabTitle_;
    QString tabTooltip_;
    State state_ = State::Normal;
};

} // namespace meld

Q_DECLARE_METATYPE(meld::DiffOptions)
