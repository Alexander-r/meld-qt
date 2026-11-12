#include "vcview.h"

#include "conflictcomparison.h"
#include "core/iohelpers.h"
#include "dirdiff.h"
#include "externalhelpers.h"
#include "icons.h"
#include "msgarea.h"
#include "pathlabel.h"
#include "settings.h"
#include "trash.h"
#include "vcdialogs.h"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <algorithm>

namespace meld {

Q_LOGGING_CATEGORY(lcVcView, "meld.vcview")

namespace {

enum ConsoleKind {
    ConsoleCommand,
    ConsoleOutput,
    ConsoleError,
};

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::VcView)
};

//! Temporary files of version control comparisons, removed when Meld exits
struct TempFiles {
    QStringList files;

    ~TempFiles() { cleanup(); }

    void cleanup()
    {
        // The strings below will probably end up as debug log, and are
        // deliberately not marked for translation.
        const QString tempLocation = QDir::tempPath();
        for (const QString &f : std::as_const(files)) {
            const QFileInfo info(f);
            if (!info.exists() || !info.isAbsolute() || info.absolutePath() != tempLocation) {
                qCWarning(lcVcView) << "File" << f << "not removed: not a temporary file";
                continue;
            }
            QFile::setPermissions(f, QFile::permissions(f) | QFile::WriteOwner);
            if (!QFile::remove(f))
                qCWarning(lcVcView) << "File" << f << "not removed";
        }
        files.clear();
    }
};

TempFiles &tempFiles()
{
    static TempFiles files;
    return files;
}

void registerTempFile(const QString &path)
{
    QFile::setPermissions(path, QFile::ReadOwner | QFile::ReadGroup | QFile::ReadOther);
    tempFiles().files.append(path);
}

QString shellJoin(const QStringList &command)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    QStringList tokens;
    for (const QString &token : command) {
        const bool quote = token.split(whitespace, Qt::SkipEmptyParts).size() > 1;
        tokens.append(quote ? QLatin1Char('"') + token + QLatin1Char('"') : token);
    }
    return tokens.join(QLatin1Char(' '));
}

QString decodeOutput(const QByteArray &data)
{
    QString text = QString::fromLocal8Bit(data);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

//! Run version control commands immediately, without any output
void runCommandsQuietly(const QList<vc::Command> &commands)
{
    for (const vc::Command &command : commands) {
        QStringList args = command.command.mid(1);
        for (const QString &f : command.files)
            args.append(vc::relpath(f, command.workingDir));
        QProcess process;
        process.setWorkingDirectory(command.workingDir);
        process.setStandardInputFile(QProcess::nullDevice());
        process.start(command.command.first(), args);
        process.waitForFinished(-1);
    }
}

int defaultVc(const std::vector<VcChoice> &choices, const QString &targetName)
{
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].enabled && !targetName.isEmpty() && choices[i].label == targetName)
            return int(i);
    }

    qsizetype targetDepth = 0;
    for (const VcChoice &choice : choices) {
        if (choice.vc)
            targetDepth = std::max(targetDepth, choice.vc->root().size());
    }
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].enabled && targetDepth && choices[i].vc->root().size() == targetDepth)
            return int(i);
    }
    return 0;
}

QList<int> treePathOf(const TreeNode *node)
{
    QList<int> rows;
    for (const TreeNode *n = node; n && n->parent; n = n->parent)
        rows.prepend(n->row());
    return rows;
}

void expandToIndex(QTreeView *view, const QModelIndex &index)
{
    std::vector<QModelIndex> chain;
    for (QModelIndex i = index; i.isValid(); i = i.parent())
        chain.push_back(i);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        view->expand(*it);
}

using EntryFilter = bool (*)(const vc::Entry &);

struct StateAction {
    //! The name in the vc-status-filters setting
    const char *filter;
    const char *action;
    //! Entries shown by this filter; flattening doesn't filter entries
    EntryFilter matches;
    const char *label;
};

//! The view's status filters, in the order of the original's state_actions
const std::vector<StateAction> &stateActions()
{
    static const std::vector<StateAction> actions{
        {"flatten", "vc-flatten", nullptr, QT_TRANSLATE_NOOP("meld::VcView", "Flatten Folders")},
        {"modified", "vc-status-modified", &vc::Entry::isModified, QT_TRANSLATE_NOOP("meld::VcView", "Modified")},
        {"normal", "vc-status-normal", &vc::Entry::isNormal, QT_TRANSLATE_NOOP("meld::VcView", "Normal")},
        {"unknown", "vc-status-unknown", &vc::Entry::isNonVc, QT_TRANSLATE_NOOP("meld::VcView", "Unversioned")},
        {"ignored", "vc-status-ignored", &vc::Entry::isIgnored, QT_TRANSLATE_NOOP("meld::VcView", "Ignored")},
    };
    return actions;
}

/*!
 * An action bar with start, center and end widgets, like GtkActionBar
 *
 * The center widget is centered in the bar if there's room, and otherwise
 * moved aside so that it doesn't overlap the other widgets.
 */
class ActionBar : public QWidget {
public:
    ActionBar(QWidget *start, QWidget *center, QWidget *end, QWidget *parent)
        : QWidget(parent)
        , start_(start)
        , center_(center)
        , end_(end)
    {
        for (QWidget *w : {start_, center_, end_}) {
            w->setParent(this);
            w->show();
        }
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        const QSize start = start_->sizeHint();
        const QSize center = center_->sizeHint();
        const QSize end = end_->sizeHint();
        return {start.width() + center.width() + end.width() + 2 * Spacing + Margins.left() + Margins.right(),
                std::max({start.height(), center.height(), end.height()}) + Margins.top() + Margins.bottom()};
    }

    QSize minimumSizeHint() const override
    {
        const QSize hint = sizeHint();
        return {start_->sizeHint().width() + end_->sizeHint().width() + center_->minimumSizeHint().width() + 2 * Spacing
                    + Margins.left() + Margins.right(),
                hint.height()};
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::LayoutRequest) {
            updateGeometry();
            arrange();
        }
        return QWidget::event(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        arrange();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawLine(0, height() - 1, width(), height() - 1);
    }

private:
    static constexpr int Spacing = 6;
    static constexpr QMargins Margins{6, 4, 6, 5};

    void arrange()
    {
        const QRect area = rect().marginsRemoved(Margins);
        const auto place = [&area](QWidget *w, int x, int width) {
            const int height = std::min(w->sizeHint().height(), area.height());
            w->setGeometry(x, area.top() + (area.height() - height) / 2, width, height);
        };
        const int startWidth = start_->sizeHint().width();
        const int endWidth = end_->sizeHint().width();
        place(start_, area.left(), startWidth);
        place(end_, area.right() + 1 - endWidth, endWidth);

        const int available = area.width() - startWidth - endWidth - 2 * Spacing;
        const int centerWidth = std::max(center_->minimumSizeHint().width(),
                                         std::min(center_->sizeHint().width(), available));
        int x = area.center().x() - centerWidth / 2;
        x = std::min(x, area.right() + 1 - endWidth - Spacing - centerWidth);
        x = std::max(x, area.left() + startWidth + Spacing);
        place(center_, x, centerWidth);
    }

    QWidget *start_;
    QWidget *center_;
    QWidget *end_;
};

//! Buttons joined into a group, as GTK's "linked" style
QWidget *linkedGroup(const QList<QAction *> &actions, QWidget *parent)
{
    auto *group = new QWidget(parent);
    auto *layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    for (QAction *action : actions) {
        auto *button = new QToolButton(group);
        button->setDefaultAction(action);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
    }
    return group;
}

} // namespace

//! The version control tree view (MeldTreeView)
class VcTreeView : public QTreeView {
public:
    explicit VcTreeView(QWidget *parent)
        : QTreeView(parent)
    {
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setUniformRowHeights(true);
        setExpandsOnDoubleClick(false);
        setAllColumnsShowFocus(true);
        setEditTriggers(QAbstractItemView::NoEditTriggers);
        header()->setStretchLastSection(true);
        header()->setSectionsMovable(false);
    }

    std::function<void(const QPoint &)> contextMenuRequested;

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        setFocus(Qt::MouseFocusReason);
        const QModelIndex index = indexAt(event->pos());
        if (!index.isValid())
            return;
        if (!selectionModel()->isSelected(index)) {
            selectionModel()->clearSelection();
            setCurrentIndex(index);
        }
        if (contextMenuRequested)
            contextMenuRequested(event->globalPos());
        event->accept();
    }
};

// ---------------------------------------------------------------------------
// VcTreeModel

VcTreeModel::VcTreeModel(QObject *parent)
    : DirDiffTreeModel(1, parent)
{
}

int VcTreeModel::columnCount(const QModelIndex &) const
{
    return ColumnCount;
}

QVariant VcTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    if (index.column() == ColName)
        return DirDiffTreeModel::data(index, role);

    const TreeNode *n = node(index);
    const QString &value = n->extra[size_t(index.column() - 1)];
    switch (role) {
    case Qt::DisplayRole:
        return value;
    case Qt::ToolTipRole:
        return value.isEmpty() ? QVariant() : QVariant(value);
    case Qt::FontRole:
        // Conflicts are the only status shown in bold
        if (index.column() == ColStatus && state(n, 0) == StateConflict) {
            QFont font = QApplication::font();
            font.setBold(true);
            return font;
        }
        return {};
    default:
        return {};
    }
}

QVariant VcTreeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColName:
        return tr("Name");
    case ColLocation:
        return tr("Location");
    case ColStatus:
        return tr("Status");
    case ColOptions:
        return tr("Extra");
    default:
        return {};
    }
}

QString VcTreeModel::text(const TreeNode *n, Column column) const
{
    return column == ColName ? n->panes[0].text : n->extra[size_t(column - 1)];
}

void VcTreeModel::setText(TreeNode *n, Column column, const QString &text)
{
    if (column == ColName)
        return;
    n->extra[size_t(column - 1)] = text;
    nodeChanged(n);
}

// ---------------------------------------------------------------------------
// Version control systems and file comparisons

VcChoices vcChoicesForLocation(QString location, const QString &preferredName)
{
    VcChoices result;

    // Version control systems can be executed at the directory level, so
    // make sure we're checking for support there instead of on a specific
    // file or on a deleted/unexisting path inside a repository
    location = vc::abspath(location.isEmpty() ? QStringLiteral(".") : location);
    bool haveFolder = true;
    while (!QFileInfo(location).isDir()) {
        const QString parentLocation = vc::pathDirname(location);
        if (parentLocation.size() >= location.size()) {
            // no existing parent: for example unexisting drive on Windows
            haveFolder = false;
            break;
        }
        location = parentLocation;
    }

    if (haveFolder) {
        for (const vc::AvailableVc &available : vc::getVcs(location)) {
            const vc::Plugin &plugin = *available.plugin;
            QString error;
            if (!available.enabled) {
                // Translators: This error message is shown when no
                // repository of this type is found.
                error = Tr::tr("%1 (not found)").arg(plugin.name());
            } else if (!plugin.isInstalled()) {
                // Translators: This error message is shown when a version
                // control binary isn't installed.
                error = Tr::tr("%1 (%2 not installed)").arg(plugin.name(), plugin.command());
            } else if (!plugin.validRepo(location)) {
                // Translators: This error message is shown when a version
                // controlled repository is invalid.
                error = Tr::tr("%1 (invalid repository)").arg(plugin.name());
            }
            if (!error.isEmpty()) {
                result.choices.push_back({error, nullptr, false});
                continue;
            }
            try {
                result.choices.push_back({plugin.name(), plugin.create(location), true});
            } catch (const std::exception &e) {
                qCWarning(lcVcView) << "Couldn't use" << plugin.name() << "for" << location << ":" << e.what();
                result.choices.push_back({Tr::tr("%1 (invalid repository)").arg(plugin.name()), nullptr, false});
            }
        }
    }

    result.defaultIndex = defaultVc(result.choices, preferredName);

    const bool anyEnabled =
        std::any_of(result.choices.begin(), result.choices.end(), [](const VcChoice &c) { return c.enabled; });
    if (!anyEnabled) {
        // If we didn't get any valid vcs then fallback to null
        std::shared_ptr<vc::Vc> null = vc::nullPlugin().create(location);
        result.choices.insert(result.choices.begin(), {null->name(), null, true});
        result.tooltip = Tr::tr("No valid version control system found in this folder");
    } else {
        result.tooltip = Tr::tr("Choose which version control system to use");
    }
    return result;
}

VcFileComparison vcFileComparison(const std::shared_ptr<vc::Vc> &vc, const QString &path, const QPointer<VcView> &view)
{
    MeldSettings *settings = MeldSettings::instance();
    VcFileComparison result;
    ComparisonMeta meta;
    QStringList temps;
    const QString basename = vc::pathBasename(path);

    // Other revisions are read into memory, rather than temporary files
    if (vc->hasRevisionSupport() && !vc->comparesWorkingCopy()) {
        const auto revisionFile = [vc, path, basename](const QString &revision) {
            const QString name = revisionName(revision);
            VirtualFile file;
            file.label = Tr::tr("%1 — %2").arg(basename, name);
            file.load = [vc, path, revision, name]() {
                VirtualFile::Content content;
                QString error;
                content.data = vc->fileAtRevision(revision, path, &error);
                if (!content.data) {
                    // Added or deleted files are compared with nothing
                    content.data = QByteArray();
                    content.noteTitle = Tr::tr("The file isn’t in %1.").arg(name);
                    content.notes = {error};
                }
                return content;
            };
            return file;
        };
        const QString baseName = revisionName(vc->baseRevision());
        if (vc->comparesRevisions()) {
            // Neither is the working copy, so both are read-only
            result.paths = {QString(), QString()};
            meta.virtualFiles.insert(0, revisionFile(vc->baseRevision()));
            meta.virtualFiles.insert(1, revisionFile(vc->targetRevision()));
            meta.tabLabel =
                Tr::tr("%1 (%2, %3)").arg(basename, baseName, revisionName(vc->targetRevision(), true));
        } else if (settings->boolValue(QStringLiteral("vc-left-is-local"))) {
            result.paths = {path, QString()};
            meta.virtualFiles.insert(1, revisionFile(vc->baseRevision()));
            meta.tabLabel = Tr::tr("%1 (working, %2)").arg(basename, baseName);
        } else {
            result.paths = {QString(), path};
            meta.virtualFiles.insert(0, revisionFile(vc->baseRevision()));
            meta.tabLabel = Tr::tr("%1 (%2, working)").arg(basename, baseName);
        }
        result.options.meta = meta;
        return result;
    }

    // May have removed directories in list.
    const vc::Entry entry = vc->entry(path);
    // Systems that can't give the sides of conflicts leave them in the file
    if (entry.state == StateConflict && !vc->hasConflictSupport() && conflictsInFile(path) > 0) {
        result.paths = {QString(), QString(), QString()};
        result.options = conflictResolution(path, vcResolver(vc, view));
        return result;
    }
    if (entry.state == StateConflict && vc->hasConflictSupport()) {
        const QString localLabel = Tr::tr("%1 — local").arg(basename);
        const QString remoteLabel = Tr::tr("%1 — remote").arg(basename);

        // We create new temp files for other, base and this, and then set
        // the output to the current file.
        std::array<vc::Conflict, 3> conflicts{};
        if (settings->stringValue(QStringLiteral("vc-merge-file-order")) == QLatin1String("local-merge-remote")) {
            conflicts = {vc::ConflictThis, vc::ConflictMerged, vc::ConflictOther};
            meta.labels = {localLabel, QString(), remoteLabel};
            meta.tabLabel = Tr::tr("%1 (local, merge, remote)").arg(basename);
        } else {
            conflicts = {vc::ConflictOther, vc::ConflictMerged, vc::ConflictThis};
            meta.labels = {remoteLabel, QString(), localLabel};
            meta.tabLabel = Tr::tr("%1 (remote, merge, local)").arg(basename);
        }
        for (vc::Conflict conflict : conflicts) {
            const auto [file, isTemp] = vc->pathForConflict(path, conflict);
            result.paths.append(file);
            if (isTemp)
                temps.append(file);
        }
        result.options.autoMerge = false;
        result.options.mergeOutput = path;
        meta.promptResolve = true;
        meta.resolve = vcResolver(vc, view);
    } else {
        const QString remoteLabel = Tr::tr("%1 — repository").arg(basename);
        const QString repositoryFile = vc->pathForRepoFile(path);
        temps.append(repositoryFile);
        if (settings->boolValue(QStringLiteral("vc-left-is-local"))) {
            result.paths = {path, repositoryFile};
            meta.labels = {QString(), remoteLabel};
            meta.tabLabel = Tr::tr("%1 (working, repository)").arg(basename);
        } else {
            result.paths = {repositoryFile, path};
            meta.labels = {remoteLabel, QString()};
            meta.tabLabel = Tr::tr("%1 (repository, working)").arg(basename);
        }
    }

    for (const QString &temp : std::as_const(temps))
        registerTempFile(temp);
    result.options.meta = meta;
    return result;
}

std::function<void(const QString &)> vcResolver(const std::shared_ptr<vc::Vc> &vc, const QPointer<VcView> &view)
{
    if (!vc->supportedActions().testFlag(vc::Action::Resolve))
        return {};
    return [vc, view](const QString &file) {
        if (view)
            view->command(QStringLiteral("resolve"), {file}, true);
        else
            runCommandsQuietly(vc->resolve({file}));
    };
}

QString revisionName(const QString &revision, bool target)
{
    if (revision.isEmpty())
        return target ? Tr::tr("working copy") : QStringLiteral("HEAD");
    return revision;
}

void cleanupVcTempFiles()
{
    tempFiles().cleanup();
}

// ---------------------------------------------------------------------------
// VcView construction

VcView::VcView(QWidget *parent)
    : MeldDoc(parent)
{
    MeldSettings *settings = MeldSettings::instance();
    statusFilters_ = settings->stringListValue(QStringLiteral("vc-status-filters"));
    model_ = new VcTreeModel(this);

    createActions();
    buildLayout();
    setConsoleVisible(actionState(QStringLiteral("vc-console-visible")));

    connect(settings, &MeldSettings::changed, this, [this](const QString &key) {
        MeldSettings *settings = MeldSettings::instance();
        if (key == QLatin1String("vc-console-visible")) {
            setActionState(key, settings->boolValue(key));
        } else if (key == QLatin1String("vc-status-filters")) {
            // Filters changed in another view apply on the next refresh
            statusFilters_ = settings->stringListValue(key);
            for (const StateAction &state : stateActions()) {
                QAction *a = action(QLatin1String(state.action));
                const QSignalBlocker blocker(a);
                a->setChecked(statusFilters_.contains(QLatin1String(state.filter)));
            }
        }
    });
}

VcView::~VcView()
{
    // Running commands write to the console as they're stopped
    scheduler_.removeAllTasks();
}

void VcView::createActions()
{
    const auto add = [this](const char *name, const QString &text, const QString &icon, const QString &tooltip,
                            std::function<void()> callback) {
        QAction *a = addAction(QLatin1String(name), std::move(callback));
        a->setText(text);
        if (!icon.isEmpty())
            a->setIcon(icons::icon(icon));
        if (!tooltip.isEmpty())
            a->setToolTip(tooltip);
        return a;
    };
    const auto runVcAction = [this](QList<vc::Command> (vc::Vc::*method)(const QStringList &) const) {
        return [this, method]() {
            if (vc_)
                runCommands((vc_.get()->*method)(selectedFiles()));
        };
    };

    QAction *console = addToggleAction(QStringLiteral("vc-console-visible"),
                                       MeldSettings::instance()->boolValue(QStringLiteral("vc-console-visible")),
                                       [this](bool visible) { setConsoleVisible(visible); });
    console->setText(tr("Version Control Console"));

    add("clear-console", tr("&Clear"), {}, {}, [this]() { consoleview_->clear(); });
    add("compare", tr("&Compare"), {}, {}, [this]() { actionDiff(); });
    add("find", tr("&Find…"), {}, {}, [this]() { startSearch(); });
    add("next-change", tr("Next Change"), {}, {}, [this]() { nextDiff(true); });
    add("next-change-shortcut", tr("Next Change"), {}, {}, [this]() { nextDiff(true); });
    add("open-external", tr("&Open Externally"), {}, {}, [this]() { actionOpenExternal(); });
    add("previous-change", tr("Previous Change"), {}, {}, [this]() { nextDiff(false); });
    add("previous-change-shortcut", tr("Previous Change"), {}, {}, [this]() { nextDiff(false); });
    add("refresh", tr("Refresh"), {}, {}, [this]() { setLocation(location_); });
    add("vc-add", tr("&Add"), QStringLiteral("list-add-symbolic"), tr("Add to version control"),
        runVcAction(&vc::Vc::add));
    add("vc-unstage", tr("&Unstage"), QStringLiteral("edit-undo-symbolic"), tr("Remove staged changes"),
        runVcAction(&vc::Vc::unstage));
    QAction *commit = add("vc-commit", tr("Commit…"), {}, tr("Commit changes to version control"),
                          [this]() { actionCommit(); });
    // Tool buttons drop ellipses from their text unless it's set explicitly
    commit->setIconText(commit->text());
    add("vc-delete-locally", tr("Delete"), {}, {}, [this]() { actionDelete(); });
    add("vc-push", tr("Push"), QStringLiteral("vc-push-symbolic"), tr("Push local changes to remote"),
        [this]() { actionPush(); });
    add("vc-remove", tr("&Remove"), QStringLiteral("list-remove-symbolic"), tr("Remove from version control"),
        [this]() { actionRemove(); });
    add("vc-resolve", tr("Resolve Conflict"), QStringLiteral("emblem-ok-symbolic"),
        tr("Mark as resolved in version control"), runVcAction(&vc::Vc::resolve));
    add("vc-revert", tr("Re&vert"), QStringLiteral("document-revert-symbolic"),
        tr("Revert working copy to original state"), runVcAction(&vc::Vc::revert));
    add("vc-update", tr("Pull"), QStringLiteral("vc-pull-symbolic"), tr("Pull remote changes to the local repository"),
        [this]() {
            if (vc_)
                runCommands(vc_->update());
        });

    addToggleAction(QStringLiteral("vc-filter"), false, {});
    for (const StateAction &state : stateActions()) {
        QAction *a = addToggleAction(QLatin1String(state.action), statusFilters_.contains(QLatin1String(state.filter)),
                                     [this](bool) { actionFilterStateChange(); });
        a->setText(tr(state.label));
    }

    // Nothing is selected until a location is set
    for (const char *name : {"compare", "open-external", "vc-add", "vc-unstage", "vc-commit", "vc-delete-locally",
                             "vc-push", "vc-remove", "vc-resolve", "vc-revert", "vc-update", "previous-change",
                             "next-change"}) {
        setActionEnabled(QLatin1String(name), false);
    }
}

void VcView::buildLayout()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The action bar: version control actions, the location and the
    // version control system selector
    auto *start = new QWidget;
    auto *startLayout = new QHBoxLayout(start);
    startLayout->setContentsMargins(0, 0, 0, 0);
    startLayout->setSpacing(6);
    auto *commit = new QToolButton(start);
    commit->setDefaultAction(action(QStringLiteral("vc-commit")));
    commit->setToolButtonStyle(Qt::ToolButtonTextOnly);
    commit->setAutoRaise(true);
    commit->setFocusPolicy(Qt::NoFocus);
    startLayout->addWidget(commit);
    startLayout->addWidget(linkedGroup({action(QStringLiteral("vc-update")), action(QStringLiteral("vc-push"))}, start));
    startLayout->addWidget(linkedGroup({action(QStringLiteral("vc-add")), action(QStringLiteral("vc-unstage")),
                                        action(QStringLiteral("vc-remove")), action(QStringLiteral("vc-revert")),
                                        action(QStringLiteral("vc-resolve"))},
                                       start));

    auto *center = new QWidget;
    auto *centerLayout = new QHBoxLayout(center);
    centerLayout->setContentsMargins(0, 0, 0, 0);
    centerLayout->setSpacing(0);
    fileButton_ = new FileButton(0, FileButton::Mode::SelectFolder, tr("Select Folder"), center);
    fileButton_->setToolTip(tr("Open version controlled folder in this pane"));
    connect(fileButton_, &FileButton::fileSelected, this, [this](int, const QString &folder) {
        // Revisions are of the repository
        baseRevision_.clear();
        targetRevision_.clear();
        setLocation(folder);
    });
    filelabel_ = new PathLabel(center);
    filelabel_->setEmptyLabel(tr("Unknown folder"));
    centerLayout->addWidget(fileButton_);
    centerLayout->addWidget(filelabel_, 1);

    comboVcs_ = new QComboBox;
    comboVcs_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    comboVcs_->setMinimumContentsLength(12);
    comboVcs_->setFocusPolicy(Qt::NoFocus);
    connect(comboVcs_, &QComboBox::currentIndexChanged, this, [this]() { onVcChange(); });

    layout->addWidget(new ActionBar(start, center, comboVcs_, this));

    // The revisions to compare: by default, the working copy's changes from
    // the current revision
    revisionBar_ = new QWidget(this);
    auto *revisionLayout = new QHBoxLayout(revisionBar_);
    revisionLayout->setContentsMargins(6, 0, 6, 4);
    revisionLayout->setSpacing(6);
    const auto makeCombo = [this](const QString &tooltip) {
        auto *combo = new QComboBox(revisionBar_);
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(18);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->setToolTip(tooltip);
        connect(combo, &QComboBox::activated, this, [this]() { onRevisionChosen(); });
        connect(combo->lineEdit(), &QLineEdit::returnPressed, this, [this]() { onRevisionChosen(); });
        return combo;
    };
    baseCombo_ = makeCombo(tr("The commit, tag or tree that changes are compared with"));
    targetCombo_ = makeCombo(tr("The working copy, or a commit, tag or tree, whose changes are shown"));
    auto *swap = new QToolButton(revisionBar_);
    swap->setText(tr("Swap"));
    swap->setToolTip(tr("Swap the revisions"));
    swap->setAutoRaise(true);
    connect(swap, &QToolButton::clicked, this, [this]() {
        // The working copy can only be the target
        if (targetRevision_.isEmpty())
            errorBell();
        else
            setRevisions(targetRevision_, baseRevision_.isEmpty() ? QStringLiteral("HEAD") : baseRevision_);
    });
    auto *reset = new QToolButton(revisionBar_);
    reset->setText(tr("Reset"));
    reset->setToolTip(tr("Compare the working copy with the current commit"));
    reset->setAutoRaise(true);
    connect(reset, &QToolButton::clicked, this, [this]() { setRevisions(QString(), QString()); });
    revisionLayout->addWidget(new QLabel(tr("Changes from"), revisionBar_));
    revisionLayout->addWidget(baseCombo_, 1);
    revisionLayout->addWidget(new QLabel(tr("to"), revisionBar_));
    revisionLayout->addWidget(targetCombo_, 1);
    revisionLayout->addWidget(swap);
    revisionLayout->addWidget(reset);
    revisionBar_->hide();
    layout->addWidget(revisionBar_);

    msgarea_ = new MsgAreaController(this);
    layout->addWidget(msgarea_);

    // The tree, with the console below it
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setChildrenCollapsible(false);
    treeview_ = new VcTreeView(splitter_);
    treeview_->setModel(model_);
    treeview_->setColumnWidth(VcTreeModel::ColName, 320);
    treeview_->setColumnWidth(VcTreeModel::ColLocation, 160);
    treeview_->setColumnWidth(VcTreeModel::ColStatus, 120);
    treeview_->contextMenuRequested = [this](const QPoint &pos) { showContextMenu(pos); };
    connect(treeview_, &QTreeView::activated, this, &VcView::onRowActivated);
    connect(treeview_->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { onSelectionChanged(); });
    connect(treeview_->selectionModel(), &QItemSelectionModel::currentChanged, this, [this]() { onCursorChanged(false); });

    consoleBox_ = new QWidget(splitter_);
    auto *consoleLayout = new QVBoxLayout(consoleBox_);
    consoleLayout->setContentsMargins(6, 0, 6, 0);
    consoleLayout->setSpacing(2);
    auto *consoleLabel = new QLabel(tr("Console output"), consoleBox_);
    QFont bold = consoleLabel->font();
    bold.setBold(true);
    consoleLabel->setFont(bold);
    consoleLayout->addWidget(consoleLabel);
    consoleview_ = new QPlainTextEdit(consoleBox_);
    consoleview_->setReadOnly(true);
    consoleview_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    consoleview_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(consoleview_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = consoleview_->createStandardContextMenu(pos);
        menu->addSeparator();
        menu->addAction(action(QStringLiteral("clear-console")));
        menu->exec(consoleview_->viewport()->mapToGlobal(pos));
        delete menu;
    });
    consoleLayout->addWidget(consoleview_, 1);
    splitter_->addWidget(treeview_);
    splitter_->addWidget(consoleBox_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 0);
    connect(splitter_, &QSplitter::splitterMoved, this, [this](int pos) {
        (void)this;
        MeldSettings::instance()->setValue(QStringLiteral("vc-console-pane-position"), pos);
    });
    layout->addWidget(splitter_, 1);

    searchEntry_ = new QLineEdit(this);
    searchEntry_->setPlaceholderText(tr("Search"));
    searchEntry_->setClearButtonEnabled(true);
    searchEntry_->hide();
    searchEntry_->installEventFilter(this);
    layout->addWidget(searchEntry_);
    const auto search = [this](bool next) {
        const QString text = searchEntry_->text();
        if (text.isEmpty())
            return;
        TreeNode *start = treeview_->currentIndex().isValid() ? model_->node(treeview_->currentIndex())
                                                              : model_->firstRoot();
        if (next && start)
            start = model_->inorderNext(start);
        for (TreeNode *n = start; n; n = model_->inorderNext(n)) {
            if (DirTreeView::searchMatches(model_->valuePaths(n), text)) {
                const QModelIndex index = model_->indexOf(n);
                expandToIndex(treeview_, index.parent());
                treeview_->setCurrentIndex(index);
                treeview_->scrollTo(index);
                return;
            }
        }
        if (next)
            errorBell();
    };
    connect(searchEntry_, &QLineEdit::textChanged, this, [search]() { search(false); });
    connect(searchEntry_, &QLineEdit::returnPressed, this, [search]() { search(true); });
}

QTreeView *VcView::treeView() const
{
    return treeview_;
}

QWidget *VcView::focusPane() const
{
    return treeview_;
}

QString VcView::consoleText() const
{
    return consoleview_->toPlainText();
}

// ---------------------------------------------------------------------------
// Locations

void VcView::setLocation(const QString &location)
{
    populateVcsForLocation(location);
}

void VcView::populateVcsForLocation(const QString &location)
{
    VcChoices choices = vcChoicesForLocation(location, vc_ ? vc_->name() : QString());
    vcChoices_ = std::move(choices.choices);
    {
        const QSignalBlocker blocker(comboVcs_);
        comboVcs_->clear();
        auto *items = qobject_cast<QStandardItemModel *>(comboVcs_->model());
        for (const VcChoice &choice : vcChoices_) {
            comboVcs_->addItem(choice.label);
            if (items)
                items->item(comboVcs_->count() - 1)->setEnabled(choice.enabled);
        }
        comboVcs_->setToolTip(choices.tooltip);
        comboVcs_->setCurrentIndex(choices.defaultIndex);
    }
    onVcChange();
}

void VcView::onVcChange()
{
    const int index = comboVcs_->currentIndex();
    if (index < 0 || size_t(index) >= vcChoices_.size() || !vcChoices_[size_t(index)].vc)
        return;
    vc_ = vcChoices_[size_t(index)].vc;

    // The chosen revisions are kept, e.g., on refreshing, if they're there
    revisionBar_->setVisible(vc_->hasRevisionSupport());
    if (vc_->hasRevisionSupport()) {
        for (QString *revision : {&baseRevision_, &targetRevision_}) {
            if (!revision->isEmpty() && !vc_->verifyRevision(*revision, nullptr))
                revision->clear();
        }
        vc_->setRevisions(baseRevision_, targetRevision_);
        populateRevisions();
    } else {
        baseRevision_.clear();
        targetRevision_.clear();
    }
    setVcLocation(vc_->location());
}

void VcView::populateRevisions()
{
    const std::vector<vc::Revision> revisions = vc_->revisions();
    for (QComboBox *combo : {baseCombo_, targetCombo_}) {
        const QSignalBlocker blocker(combo);
        combo->clear();
        if (combo == targetCombo_) {
            combo->addItem(revisionName(QString(), true), QString());
            combo->setItemData(0, tr("The files as they are now, which can be changed"), Qt::ToolTipRole);
        }
        for (const vc::Revision &revision : revisions) {
            QString text = revision.name;
            if (revision.kind == vc::Revision::Kind::Commit)
                text = QStringLiteral("%1 %2").arg(revision.name, revision.description);
            // The current revision is the default base
            const QString value =
                revision.kind == vc::Revision::Kind::Head && combo == baseCombo_ ? QString() : revision.name;
            combo->addItem(text, value);
            combo->setItemData(combo->count() - 1, revision.description, Qt::ToolTipRole);
        }
    }
    const auto select = [](QComboBox *combo, const QString &revision, bool target) {
        const QSignalBlocker blocker(combo);
        const int index = combo->findData(revision);
        if (index >= 0)
            combo->setCurrentIndex(index);
        else
            combo->setEditText(revision.isEmpty() ? revisionName(revision, target) : revision);
    };
    select(baseCombo_, baseRevision_, false);
    select(targetCombo_, targetRevision_, true);
}

void VcView::onRevisionChosen()
{
    // Listed revisions are chosen by their text; others are typed
    const auto revisionOf = [](QComboBox *combo) {
        const QString text = combo->currentText().trimmed();
        const int index = combo->findText(text);
        if (index >= 0)
            return combo->itemData(index).toString();
        if (text.isEmpty() || text == QLatin1String("HEAD"))
            return QString();
        return text;
    };
    const QString base = revisionOf(baseCombo_);
    const QString target = revisionOf(targetCombo_);
    if (base != baseRevision_ || target != targetRevision_)
        setRevisions(base, target);
}

bool VcView::setRevisions(const QString &base, const QString &target)
{
    if (!vc_ || !vc_->hasRevisionSupport())
        return false;
    // The current revision as the base is the default
    const QString chosenBase = base == QLatin1String("HEAD") && target.isEmpty() ? QString() : base;
    for (const QString &revision : {chosenBase, target}) {
        QString error;
        if (!revision.isEmpty() && !vc_->verifyRevision(revision, &error)) {
            msgarea_->addDismissableMsg(QStringLiteral("dialog-error-symbolic"), tr("Unknown revision"), error);
            populateRevisions();
            return false;
        }
    }
    msgarea_->clear();
    baseRevision_ = chosenBase;
    targetRevision_ = target;
    vc_->setRevisions(baseRevision_, targetRevision_);
    populateRevisions();
    setVcLocation(vc_->location());
    return true;
}

bool VcView::isFolder(const QString &path) const
{
    if (vc_ && vc_->comparesRevisions())
        return vc_->entry(path).isDir;
    return QFileInfo(path).isDir();
}

void VcView::setVcLocation(const QString &location)
{
    scheduler_.removeAllTasks();
    location_ = location;
    currentPath_ = QPersistentModelIndex();
    prevPath_ = QPersistentModelIndex();
    nextPath_ = QPersistentModelIndex();
    model_->clear();
    filelabel_->setFile(location);
    fileButton_->setFile(location);
    TreeNode *root = model_->addEntries(nullptr, {location});
    model_->setPathState(root, 0, StateNormal, true);
    treeview_->selectionModel()->select(model_->indexOf(root),
                                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    recomputeLabel();

    // If the user is just diffing a file (i.e., not a directory), there's
    // no need to scan the rest of the repository.
    if (!QFileInfo(vc_->location()).isDir())
        return;

    if (const std::optional<QString> summary = vc_->commitsToPushSummary())
        model_->setText(root, VcTreeModel::ColOptions, *summary);

    std::shared_ptr<vc::Vc> vc = vc_;
    scheduler_.addOneShot([vc]() { vc->refreshVcState(); });
    scheduler_.addTask(searchRecursivelyIter({0}, false));
    scheduler_.addOneShot([this]() { onSelectionChanged(); });
    scheduler_.addOneShot([this]() { onCursorChanged(true); });
}

std::optional<Comparison> VcView::comparison() const
{
    Comparison c;
    c.type = RecentType::VersionControl;
    if (!location_.isEmpty())
        c.paths = {location_};
    return c;
}

void VcView::recomputeLabel()
{
    //: This is the name of the version control system being used, e.g.,
    //: "Git" or "Subversion"
    const QString title = tr("%1 comparison:").arg(vc_ ? vc_->name() : QString());
    QString label = path::basename(location_);
    QString tooltip = title + QLatin1Char('\n') + location_;
    if (vc_ && !vc_->comparesWorkingCopy()) {
        const QString revisions =
            tr("%1 → %2").arg(revisionName(baseRevision_), revisionName(targetRevision_, true));
        label = tr("%1 (%2)").arg(label, revisions);
        tooltip += QLatin1Char('\n') + revisions;
    }
    setTabLabel(label, tooltip);
}

void VcView::setLabels(const QStringList &labels)
{
    if (!labels.isEmpty())
        filelabel_->setCustomLabel(labels.first());
    recomputeLabel();
}

TreeNode *VcView::nodeAt(const QList<int> &treePath) const
{
    TreeNode *n = model_->invisibleRoot();
    for (int row : treePath) {
        if (row < 0 || size_t(row) >= n->children.size())
            return nullptr;
        n = n->children[size_t(row)].get();
    }
    return n == model_->invisibleRoot() ? nullptr : n;
}

TaskGenerator VcView::searchRecursivelyIter(QList<int> startPath, bool replace)
{
    // Initial yield so when we add this to our tasks, we don't create
    // references that may be invalidated.
    co_yield tr("Scanning repository");

    TreeNode *start = nodeAt(startPath);
    if (!start || !vc_)
        co_return;

    if (replace) {
        // Replace the row with a new, empty row ready to be filled.
        treeview_->collapse(model_->indexOf(start));
        model_->removeChildren(start);
        start->extra = {};
        model_->setPathState(start, 0, StateNormal, true);
    }

    const QString rootname = model_->filePath(start);
    const qsizetype displayPrefix = rootname.size() + 1;
    struct Todo {
        QList<int> treePath;
        QString path;

        bool operator<(const Todo &other) const
        {
            return std::tie(treePath, path) < std::tie(other.treePath, other.path);
        }
    };
    std::vector<Todo> todo{{treePathOf(start), rootname}};

    const bool flattened = statusFilters_.contains(QStringLiteral("flatten"));
    std::vector<EntryFilter> filters;
    for (const StateAction &state : stateActions()) {
        if (state.matches && statusFilters_.contains(QLatin1String(state.filter)))
            filters.push_back(state.matches);
    }

    while (!todo.empty()) {
        // This needs to happen sorted and depth-first in order for our row
        // references to remain valid while we traverse.
        std::sort(todo.begin(), todo.end());
        const Todo current = todo.front();
        todo.erase(todo.begin());
        co_yield tr("Scanning %1").arg(current.path.mid(displayPrefix));

        TreeNode *it = nodeAt(current.treePath);
        if (!it)
            continue;

        std::vector<vc::Entry> entries;
        for (const vc::Entry &e : vc_->entries(current.path)) {
            if (std::any_of(filters.begin(), filters.end(), [&e](EntryFilter f) { return f(e); }))
                entries.push_back(e);
        }
        std::sort(entries.begin(), entries.end(), [](const vc::Entry &a, const vc::Entry &b) {
            return std::make_pair(!a.isDir, a.name) < std::make_pair(!b.isDir, b.name);
        });

        for (const vc::Entry &e : entries) {
            if (e.isDir && e.isPresent()) {
                const QFileInfo info(e.path);
                // Revisions' folders needn't be on disk
                if (!vc_->comparesRevisions() && !info.exists() && !info.isSymLink()) {
                    model_->addError(it, QStringLiteral("'%1': %2").arg(e.path, tr("No such file or directory")), 0);
                    continue;
                }

                if (flattened) {
                    if (e.state != StateIgnored) {
                        // If directory state is changed, render it in
                        // flattened mode.
                        if (e.state != StateNormal) {
                            TreeNode *child = model_->addEntries(it, {e.path});
                            updateItemState(child, e);
                        }
                        todo.push_back({{0}, e.path});
                    }
                    continue;
                }
            }

            TreeNode *child = model_->addEntries(it, {e.path});
            if (e.isDir && e.state != StateIgnored)
                todo.push_back({treePathOf(child), e.path});
            updateItemState(child, e);
        }

        if (!flattened) {
            if (entries.empty()) {
                model_->addEmpty(it, tr("(Empty)"));
            } else if (std::any_of(entries.begin(), entries.end(),
                                   [](const vc::Entry &e) { return e.state != StateNormal; })) {
                expandToIndex(treeview_, model_->indexOf(it));
            }
        }
    }

    if (TreeNode *first = model_->firstRoot()) {
        const QModelIndex index = model_->indexOf(first);
        treeview_->expand(index);
        treeview_->setCurrentIndex(index);
    }
}

void VcView::updateItemState(TreeNode *node, const vc::Entry &entry)
{
    model_->setPathState(node, 0, entry.state, entry.isDir);
    const std::optional<QString> location = path::relativePath(vc_->location(), vc::pathDirname(entry.path));
    model_->setText(node, VcTreeModel::ColLocation, location.value_or(QString()));
    model_->setText(node, VcTreeModel::ColStatus, entry.status());
    model_->setText(node, VcTreeModel::ColOptions, entry.options);
}

TreeNode *VcView::findNodeByName(const QString &name) const
{
    const auto isWithin = [&name](const QString &folder) {
        return !folder.isEmpty() && name.size() > folder.size() && vc::pathIsInFolder(name, folder);
    };
    TreeNode *it = model_->firstRoot();
    while (it) {
        const QString path = model_->filePath(it);
        if (name == path)
            return it;
        if (!isWithin(path))
            break;
        TreeNode *next = nullptr;
        for (const auto &child : it->children) {
            const QString childPath = model_->filePath(child.get());
            if (name == childPath)
                return child.get();
            if (isWithin(childPath)) {
                next = child.get();
                break;
            }
        }
        it = next;
    }
    return nullptr;
}

void VcView::refresh()
{
    if (TreeNode *root = model_->firstRoot())
        setLocation(model_->filePath(root));
}

void VcView::refreshPartial(const QString &where)
{
    if (!vc_)
        return;
    if (actionState(QStringLiteral("vc-flatten"))) {
        // The original refreshes everything in flattened mode, too
        refresh();
        return;
    }
    TreeNode *node = findNodeByName(where);
    if (!node)
        return;
    treeview_->setFocus();
    vc_->refreshVcState(where);
    scheduler_.addTask(searchRecursivelyIter(treePathOf(node), true));
    scheduler_.addOneShot([this]() { onSelectionChanged(); });
    scheduler_.addOneShot([this]() { onCursorChanged(true); });
}

void VcView::onFileChanged(const QString &filename)
{
    // Revisions don't change as files are saved
    if (!vc_ || vc_->comparesRevisions())
        return;
    if (TreeNode *node = findNodeByName(filename)) {
        const QString path = model_->filePath(node);
        vc_->refreshVcState(path);
        updateItemState(node, vc_->entry(path));
    }
}

void VcView::onContainerSwitchIn()
{
    MeldDoc::onContainerSwitchIn();
    onSelectionChanged();
    scheduler_.addOneShot([this]() { onCursorChanged(true); });
}

// ---------------------------------------------------------------------------
// Tree

QStringList VcView::selectedFiles() const
{
    QList<TreeNode *> nodes;
    for (const QModelIndex &index : treeview_->selectionModel()->selectedRows(0))
        nodes.append(model_->node(index));
    // Keep tree order, as GtkTreeSelection does
    std::sort(nodes.begin(), nodes.end(),
              [](const TreeNode *a, const TreeNode *b) { return treePathOf(a) < treePathOf(b); });
    QStringList files;
    for (const TreeNode *n : std::as_const(nodes)) {
        QString path = model_->filePath(n);
        if (path.isEmpty())
            continue;
        if (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        files.append(path);
    }
    return files;
}

void VcView::onRowActivated(const QModelIndex &index)
{
    const QModelIndex row = index.siblingAtColumn(0);
    TreeNode *node = model_->node(row);
    if (!node->children.empty()) {
        treeview_->setExpanded(row, !treeview_->isExpanded(row));
        return;
    }
    // A folder may no longer exist, and is only tracked by version control
    const QString path = model_->filePath(node);
    if (!path.isEmpty() && !isFolder(path))
        runDiff(path);
}

void VcView::onSelectionChanged()
{
    QHash<QString, int> pathStates;
    QStringList paths;
    for (const QModelIndex &index : treeview_->selectionModel()->selectedRows(0)) {
        const TreeNode *n = model_->node(index);
        paths.append(model_->filePath(n));
        pathStates.insert(model_->filePath(n), model_->state(n, 0));
    }

    // Unlike the original, actions that the version control system doesn't
    // implement are disabled
    vc::Actions valid;
    if (vc_)
        valid = vc_->validActions(pathStates) & (vc_->supportedActions() | vc::Action::Compare);
    setActionEnabled(QStringLiteral("compare"), valid.testFlag(vc::Action::Compare));
    setActionEnabled(QStringLiteral("open-external"), !paths.isEmpty());
    setActionEnabled(QStringLiteral("vc-add"), valid.testFlag(vc::Action::Add));
    setActionEnabled(QStringLiteral("vc-unstage"), valid.testFlag(vc::Action::Unstage));
    setActionEnabled(QStringLiteral("vc-commit"), valid.testFlag(vc::Action::Commit));
    setActionEnabled(QStringLiteral("vc-delete-locally"), vc_ && !paths.isEmpty() && !paths.contains(vc_->root()));
    setActionEnabled(QStringLiteral("vc-push"), valid.testFlag(vc::Action::Push));
    setActionEnabled(QStringLiteral("vc-remove"), valid.testFlag(vc::Action::Remove));
    setActionEnabled(QStringLiteral("vc-resolve"), valid.testFlag(vc::Action::Resolve));
    setActionEnabled(QStringLiteral("vc-revert"), valid.testFlag(vc::Action::Revert));
    setActionEnabled(QStringLiteral("vc-update"), valid.testFlag(vc::Action::Update));

    // Compared with other revisions, the files' states aren't what version
    // control actions would change; revisions' files are only viewed
    if (vc_ && !vc_->comparesWorkingCopy()) {
        for (const char *name : {"vc-add", "vc-unstage", "vc-commit", "vc-remove", "vc-resolve", "vc-revert"})
            setActionEnabled(QLatin1String(name), false);
        if (vc_->comparesRevisions()) {
            setActionEnabled(QStringLiteral("vc-delete-locally"), false);
            setActionEnabled(QStringLiteral("open-external"), false);
        }
    }
}

void VcView::onCursorChanged(bool force)
{
    const QModelIndex cursor = treeview_->currentIndex().isValid() ? treeview_->currentIndex().siblingAtColumn(0)
                                                                   : QModelIndex();
    if (!cursor.isValid()) {
        setActionEnabled(QStringLiteral("previous-change"), false);
        setActionEnabled(QStringLiteral("next-change"), false);
        currentPath_ = QPersistentModelIndex();
        return;
    }

    // If invoked directly rather than through a signal, we always check
    bool skip = false;
    if (!force && currentPath_.isValid()) {
        // We can skip recalculation if the new cursor is between the
        // previous/next bounds, and we weren't on a changed row
        const int state = model_->state(model_->node(currentPath_), 0);
        if (state == StateNormal || state == StateEmpty) {
            const QList<int> cursorPath = treePathOf(model_->node(cursor));
            const auto pathOf = [this](const QPersistentModelIndex &index) { return treePathOf(model_->node(index)); };
            if (!prevPath_.isValid() && !nextPath_.isValid())
                skip = true;
            else if (!prevPath_.isValid())
                skip = cursorPath < pathOf(nextPath_);
            else if (!nextPath_.isValid())
                skip = pathOf(prevPath_) < cursorPath;
            else
                skip = pathOf(prevPath_) < cursorPath && cursorPath < pathOf(nextPath_);
        }
    }

    if (!skip) {
        const auto [prev, next] = model_->previousNextPaths(cursor, [](TreeNode *n) {
            const int s = n->panes[0].state;
            return s != StateNormal && s != StateNoChange && s != StateEmpty;
        });
        prevPath_ = prev;
        nextPath_ = next;
        setActionEnabled(QStringLiteral("previous-change"), prev.isValid());
        setActionEnabled(QStringLiteral("next-change"), next.isValid());
    }
    currentPath_ = cursor;
}

void VcView::nextDiff(bool down)
{
    const QPersistentModelIndex path = down ? nextPath_ : prevPath_;
    if (!path.isValid()) {
        errorBell();
        return;
    }
    expandToIndex(treeview_, path);
    treeview_->setCurrentIndex(path);
    treeview_->scrollTo(path);
}

void VcView::showContextMenu(const QPoint &globalPos)
{
    onSelectionChanged();
    QMenu menu(this);
    const auto add = [&](const char *name, const QString &label) {
        QAction *a = action(QLatin1String(name));
        QAction *item = menu.addAction(label);
        item->setEnabled(a->isEnabled());
        connect(item, &QAction::triggered, a, &QAction::trigger);
    };
    add("compare", tr("&Compare"));
    add("vc-commit", tr("Co&mmit…"));
    menu.addSeparator();
    add("vc-add", tr("&Add"));
    add("vc-unstage", tr("&Unstage"));
    add("vc-remove", tr("&Remove"));
    add("vc-revert", tr("Re&vert"));
    add("vc-delete-locally", tr("Delete"));
    menu.addSeparator();
    add("vc-resolve", tr("Resolve Conflict"));
    menu.exec(globalPos);
}

bool VcView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == searchEntry_) {
        if (event->type() == QEvent::ShortcutOverride) {
            // Escape and Return belong to the search entry while it has
            // focus, rather than to the stop shortcut
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
                event->accept();
                return false;
            }
        }
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            searchEntry_->hide();
            treeview_->setFocus();
            return true;
        }
        if (event->type() == QEvent::FocusOut)
            searchEntry_->hide();
    }
    return MeldDoc::eventFilter(watched, event);
}

void VcView::startSearch()
{
    searchEntry_->show();
    searchEntry_->setFocus();
    searchEntry_->selectAll();
}

void VcView::actionFilterStateChange()
{
    QStringList active;
    for (const StateAction &state : stateActions()) {
        if (actionState(QLatin1String(state.action)))
            active.append(QLatin1String(state.filter));
    }
    if (QSet<QString>(active.begin(), active.end()) == QSet<QString>(statusFilters_.begin(), statusFilters_.end()))
        return;
    statusFilters_ = active;
    MeldSettings::instance()->setValue(QStringLiteral("vc-status-filters"), active);
    refresh();
}

void VcView::setConsoleVisible(bool visible)
{
    MeldSettings::instance()->setValue(QStringLiteral("vc-console-visible"), visible);
    consoleBox_->setVisible(visible);
    if (visible)
        applyConsolePosition();
}

void VcView::applyConsolePosition()
{
    // The position is the height of the tree, which needs the splitter's
    // real size
    const int height = splitter_->height() - splitter_->handleWidth();
    if (!splitter_->isVisible() || height <= 0)
        return;
    const int position = MeldSettings::instance()->intValue(QStringLiteral("vc-console-pane-position"));
    const int tree = std::clamp(position, 50, std::max(50, height - 50));
    const QSignalBlocker blocker(splitter_);
    splitter_->setSizes({tree, height - tree});
}

void VcView::showEvent(QShowEvent *event)
{
    MeldDoc::showEvent(event);
    if (!consolePositioned_ && consoleBox_->isVisibleTo(this)) {
        consolePositioned_ = true;
        QTimer::singleShot(0, this, [this]() { applyConsolePosition(); });
    }
}

// ---------------------------------------------------------------------------
// Running commands

bool VcView::hasCommand(const QString &command) const
{
    // Map for inter-tab command() calls
    return command == QLatin1String("resolve") && vc_ && vc_->supportedActions().testFlag(vc::Action::Resolve);
}

void VcView::command(const QString &command, const QStringList &files, bool sync)
{
    if (!hasCommand(command)) {
        qCWarning(lcVcView) << "Couldn't understand command" << command;
        return;
    }
    runCommands(vc_->resolve(files), sync);
}

void VcView::runCommands(const QList<vc::Command> &commands, bool sync)
{
    for (const vc::Command &command : commands) {
        if (sync) {
            for (const QString &status : commandIter(command))
                Q_UNUSED(status);
        } else {
            scheduler_.addTask(commandIter(command));
        }
    }
}

TaskGenerator VcView::commandIter(vc::Command command)
{
    QStringList files;
    for (const QString &f : std::as_const(command.files))
        files.append(vc::relpath(f, command.workingDir));
    const QStringList args = command.command + files;
    const QString message = shellJoin(args) + QStringLiteral(" (in %1)\n").arg(command.workingDir);
    consoleWrite(message, ConsoleCommand);

    // The process is killed if the task is stopped, e.g., by closing the tab
    struct RunningProcess {
        explicit RunningProcess(VcView *view)
            : view(view)
        {
        }

        ~RunningProcess()
        {
            if (process.state() == QProcess::NotRunning)
                return;
            view->consoleWrite(QStringLiteral("killing '%1'\n").arg(process.program()), ConsoleError);
            process.terminate();
            if (!process.waitForFinished(3000))
                process.kill();
            process.waitForFinished();
            view->consoleWrite(QStringLiteral("killed (status was '%1')\n").arg(process.exitCode()), ConsoleError);
        }

        VcView *view;
        QProcess process;
    };
    RunningProcess running(this);
    QProcess &process = running.process;
    process.setWorkingDirectory(command.workingDir);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(args.first(), args.mid(1));

    int returncode = 0;
    QByteArray output;
    if (!process.waitForStarted()) {
        // This runs in a task, so the error can't use a nested event loop
        auto *box = new QMessageBox(QMessageBox::Critical, tr("Error running command"),
                                    tr("While running '%1'\nError: %2").arg(message, process.errorString()),
                                    QMessageBox::Ok, window());
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
        returncode = 1;
    } else {
        while (process.state() != QProcess::NotRunning) {
            process.waitForFinished(50);
            output += process.readAllStandardOutput();
            consoleWrite(decodeOutput(process.readAllStandardError()), ConsoleError);
            if (process.state() != QProcess::NotRunning)
                co_yield QString();
        }
        output += process.readAllStandardOutput();
        consoleWrite(decodeOutput(process.readAllStandardError()), ConsoleError);
        returncode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
        if (returncode)
            consoleWrite(QStringLiteral("Exit code: %1\n").arg(returncode), ConsoleError);
    }

    consoleWrite(decodeOutput(output) + QLatin1Char('\n'), ConsoleOutput);

    if (returncode)
        setActionState(QStringLiteral("vc-console-visible"), true);

    if (command.refresh) {
        const QString workingDir = command.workingDir;
        QTimer::singleShot(0, this, [this, workingDir]() { refreshPartial(workingDir); });
    }
}

void VcView::consoleWrite(const QString &message, int kind)
{
    if (message.isEmpty())
        return;
    QTextCharFormat format;
    if (kind == ConsoleCommand)
        format.setFontWeight(QFont::Bold);
    else if (kind == ConsoleError)
        format.setForeground(QColor(0xcc, 0x00, 0x00));
    QTextCursor cursor(consoleview_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(message, format);
    consoleview_->verticalScrollBar()->setValue(consoleview_->verticalScrollBar()->maximum());
}

// ---------------------------------------------------------------------------
// Actions

void VcView::actionCommit()
{
    if (!vc_)
        return;
    const QStringList selected = selectedFiles();
    QStringList toCommit;
    if (const std::optional<QStringList> files = vc_->filesToCommit(selected)) {
        toCommit = *files;
        if (toCommit.isEmpty())
            toCommit = {tr("No files will be committed")};
    } else {
        const QString topdir = vc::pathDirname(vc::commonPrefix(selected));
        for (const QString &s : selected)
            toCommit.append(s.mid(topdir.size() + 1));
    }

    auto *dialog = new CommitDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setChangedFiles(toCommit);
    dialog->setMessage(vc_->commitMessagePrefill());
    QPointer<VcView> guard(this);
    connect(dialog, &CommitDialog::commitRequested, this, [guard](const QString &message) {
        if (guard && guard->vc_)
            guard->runCommands(guard->vc_->commit(guard->selectedFiles(), message));
    });
    dialog->open();
}

void VcView::actionPush()
{
    if (!vc_)
        return;
    auto *box = new QMessageBox(QMessageBox::Question, tr("Push local commits to remote?"),
                                tr("Push local commits to remote?"), QMessageBox::NoButton, window());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(tr("The commits to be pushed are determined by your version control system."));
    box->addButton(tr("&Cancel"), QMessageBox::RejectRole);
    QPushButton *push = box->addButton(tr("&Push commits"), QMessageBox::AcceptRole);
    box->setDefaultButton(push);
    QPointer<VcView> guard(this);
    connect(box, &QMessageBox::finished, this, [guard, box, push]() {
        if (guard && guard->vc_ && box->clickedButton() == push)
            guard->runCommands(guard->vc_->push());
    });
    box->open();
}

void VcView::actionRemove()
{
    if (!vc_)
        return;
    const QStringList selected = selectedFiles();

    // Don't prompt for confirmation if we're only removing files from
    // version control
    if (std::none_of(selected.begin(), selected.end(), [](const QString &p) { return QFileInfo(p).isDir(); })) {
        runCommands(vc_->remove(selected));
        return;
    }

    auto *box = new QMessageBox(QMessageBox::Question, tr("Remove folder and all its files?"),
                                tr("Remove folder and all its files?"), QMessageBox::NoButton, window());
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(tr("This will remove all selected files and folders, and all files within any selected "
                               "folders, from version control."));
    QPushButton *cancel = box->addButton(tr("&Cancel"), QMessageBox::RejectRole);
    QPushButton *remove = box->addButton(tr("&Remove"), QMessageBox::DestructiveRole);
    box->setDefaultButton(cancel);
    QPointer<VcView> guard(this);
    connect(box, &QMessageBox::finished, this, [guard, box, remove, selected]() {
        if (guard && guard->vc_ && box->clickedButton() == remove)
            guard->runCommands(guard->vc_->remove(selected));
    });
    box->open();
}

void VcView::actionDelete()
{
    QStringList files = selectedFiles();
    const QString workdir = vc::pathDirname(vc::commonPrefix(files));
    while (!files.isEmpty()) {
        const QString path = files.takeLast();
        QString error;
        const TrashResult result = trashOrConfirm(path, this, &error);
        if (result == TrashResult::Failed) {
            QMessageBox::critical(this, tr("Error deleting %1").arg(QDir::toNativeSeparators(path)), error);
            return;
        }
        if (result == TrashResult::Cancelled)
            break;
    }
    refreshPartial(workdir);
}

void VcView::actionDiff()
{
    // TODO: Review the compare/diff action. It doesn't really add much over
    // activate, since the folder compare doesn't work and hasn't for... a
    // long time.
    for (const QString &f : selectedFiles())
        runDiff(f);
}

void VcView::actionOpenExternal()
{
    openFilesExternal(selectedFiles(), this);
}

void VcView::runDiff(const QString &path)
{
    if (isFolder(path)) {
        // Revisions' folders are only expanded
        if (!vc_ || !vc_->comparesRevisions())
            Q_EMIT createDiff({path}, DiffOptions{});
        return;
    }
    if (!vc_)
        return;
    try {
        const VcFileComparison comparison = vcFileComparison(vc_, path, this);
        Q_EMIT createDiff(comparison.paths, comparison.options);
    } catch (const std::exception &e) {
        // This may run in a task, so the error can't use a nested event loop
        auto *box = new QMessageBox(QMessageBox::Warning, tr("Couldn't open comparison"),
                                    QString::fromUtf8(e.what()), QMessageBox::Ok, window());
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->open();
    }
}

void VcView::autoCompare()
{
    for (TreeNode *n : model_->stateRows({StateModified, StateConflict})) {
        const QString path = model_->filePath(n);
        if (QFileInfo::exists(path) || (vc_ && vc_->comparesRevisions()))
            runDiff(path);
    }
}

} // namespace meld
