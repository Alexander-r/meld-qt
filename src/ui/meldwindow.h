#pragma once

#include "core/task.h"
#include "melddoc.h"

#include <QAction>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

class QMenu;
class QTabWidget;
class QToolBar;
class QToolButton;

namespace meld {

class DirDiff;
class FileDiff;
class NewDiffTab;
class Spinner;
class VcView;

//! A window action that forwards to the current tab's action of the same
//! name, like GTK's "view." action group being swapped on tab changes.
class ProxyAction : public QAction {
    Q_OBJECT

public:
    ProxyAction(const QString &name, QObject *parent);

    QString name() const { return name_; }
    void bind(QAction *target);
    QAction *target() const { return target_; }

private:
    QString name_;
    QPointer<QAction> target_;
    QList<QKeySequence> shortcuts_;
};

class MeldWindow : public QMainWindow {
    Q_OBJECT

public:
    MeldWindow();
    ~MeldWindow() override;

    MeldDoc *currentDoc() const;
    QList<MeldDoc *> docs() const;
    bool hasPages() const;
    QTabWidget *tabWidget() const { return tabs_; }
    LifoScheduler &scheduler() { return scheduler_; }

    NewDiffTab *appendNewComparison();
    DirDiff *appendDirdiff(const QStringList &folders, bool autoCompare = false);
    FileDiff *appendFilediff(const QStringList &files, const QList<QByteArray> &encodings = {},
                             const QString &mergeOutput = QString(),
                             const std::optional<ComparisonMeta> &meta = std::nullopt);
    FileDiff *appendFilemerge(const QStringList &files, const QString &mergeOutput = QString());
    //! Open a comparison of files or folders; throws std::invalid_argument
    MeldDoc *appendDiff(const QStringList &paths, bool autoCompare = false, bool autoMerge = false,
                        const QString &mergeOutput = QString(),
                        const std::optional<ComparisonMeta> &meta = std::nullopt);
    VcView *appendVcview(const QString &location, bool autoCompare = false);
    /*!
     * Compare a file or folder with what a patch would make of it; throws
     * std::invalid_argument if the patch can't be read, or doesn't change
     * the file
     */
    MeldDoc *appendPatchDiff(const QString &path, const QString &patchFile);
    //! Compare a single version-controlled file with its repository version;
    //! throws std::exception if that's not possible
    MeldDoc *singleFileOpen(const QString &path);
    MeldDoc *appendRecent(const Comparison &comparison);
    //! Open paths as given on the command line; returns nullptr and sets
    //! `error` if the comparison couldn't be created.
    MeldDoc *openPaths(const QStringList &paths, bool autoCompare, bool autoMerge, bool focus, QString *error);
    //! Open a comparison with a patch as given on the command line (or an
    //! archive, which is extracted); returns nullptr and sets `error` if it
    //! couldn't be created
    MeldDoc *openPatch(const QString &path, const QString &patchFile, bool autoCompare, bool focus, QString *error);

    QAction *windowAction(const QString &name) const { return windowActions_.value(name); }
    ProxyAction *viewAction(const QString &name) const { return proxies_.value(name); }

    //! Run all pending tasks (for tests)
    void runPendingTasks();

Q_SIGNALS:
    void preferencesRequested();
    void aboutRequested();
    void quitRequested();

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void buildHeaderBar();
    QMenu *buildGearMenu();
    ProxyAction *proxy(const QString &name, const QString &text = QString(), const QString &icon = QString());
    QAction *addWindowAction(const QString &name, const QString &text, std::function<void()> callback);
    QToolButton *toolButton(QAction *action);

    void appendPage(MeldDoc *doc);
    void onCurrentTabChanged();
    void onTabLabelChanged(MeldDoc *doc);
    void pageRemoved(MeldDoc *doc);
    void closeWindowAsync();
    void onSchedulerRunnable();
    void onIdle();
    void updateFilterMenus();
    void rebuildRecentMenu();
    void showKeyboardShortcuts();

    void restoreWindowState();
    void saveWindowState();

    /*!
     * Extract the archives among paths, replacing them with the extracted
     * folders. Returns false if one couldn't be extracted, which is shown and
     * given in `error`, or if it was cancelled, leaving `error` empty.
     */
    bool extractArchives(QStringList &paths, QString *error);

    QToolBar *headerBar_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    Spinner *spinner_ = nullptr;
    QHash<QString, ProxyAction *> proxies_;
    QHash<QString, QAction *> windowActions_;
    QList<QAction *> fileDiffToolbar_;
    QList<QAction *> dirDiffToolbar_;
    QList<QAction *> conflictActions_;
    QAction *textFilterButtonAction_ = nullptr;
    QAction *folderFilterButtonAction_ = nullptr;
    QAction *vcFilterButtonAction_ = nullptr;
    QToolButton *textFilterButton_ = nullptr;
    QToolButton *folderFilterButton_ = nullptr;
    QToolButton *vcFilterButton_ = nullptr;
    QToolButton *gearButton_ = nullptr;
    QMenu *textFilterMenu_ = nullptr;
    QMenu *folderFilterMenu_ = nullptr;
    QMenu *vcFilterMenu_ = nullptr;
    QMenu *recentMenu_ = nullptr;
    QPointer<MeldDoc> boundDoc_;
    QMetaObject::Connection filterConnection_;

    LifoScheduler scheduler_;
    QTimer idleTimer_;
    bool shouldClose_ = false;
};

} // namespace meld
