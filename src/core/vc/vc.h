#pragma once

#include "core/vc/states.h"

#include <QByteArray>
#include <QFlags>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace meld::vc {

//! Sides of a version control conflict
enum Conflict {
    ConflictMerged,
    ConflictBase,
    ConflictLocal,
    ConflictRemote,
    ConflictMax,
};
// These names are used by BZR, and are logically identical.
constexpr Conflict ConflictOther = ConflictRemote;
constexpr Conflict ConflictThis = ConflictLocal;

//! "Merged", "Base", "Local" or "Remote"
QString conflictName(Conflict conflict);

struct Entry {
    QString path;
    QString name;
    int state = StateNormal;
    bool isDir = false;
    QString options;

    //! Label for a state of a version controlled file; not all states have
    //! a label, to avoid visual clutter.
    static QString stateName(int state);
    QString status() const { return stateName(state); }

    //! Should this Entry actually be present on the file system
    bool isPresent() const { return state != StateRemoved && state != StateMissing; }

    static bool isModified(const Entry &entry);
    static bool isNormal(const Entry &entry);
    static bool isNonVc(const Entry &entry);
    static bool isIgnored(const Entry &entry);
};

//! A revision to offer for comparing: a branch, tag or commit
struct Revision {
    enum class Kind {
        Head,
        Branch,
        RemoteBranch,
        Tag,
        Commit,
    };
    Kind kind = Kind::Commit;
    //! What identifies the revision to the version control system, e.g., a
    //! branch name or a commit id
    QString name;
    //! E.g., a commit's subject
    QString description;
};

//! Operations offered by the version control view
enum class Action {
    Compare = 1 << 0,
    Update = 1 << 1,
    Push = 1 << 2,
    Commit = 1 << 3,
    Add = 1 << 4,
    Resolve = 1 << 5,
    Remove = 1 << 6,
    Revert = 1 << 7,
    Unstage = 1 << 8,
};
Q_DECLARE_FLAGS(Actions, Action)

/*!
 * A version control command, as the original's runner() arguments
 *
 * The command is run in `workingDir` with `files` appended, after making
 * them relative to the working directory.
 */
struct Command {
    QStringList command;
    QStringList files;
    bool refresh = true;
    QString workingDir;

    friend bool operator==(const Command &, const Command &) = default;
};

struct ProcessResult {
    //! False if the program couldn't be started
    bool started = false;
    //! The exit code, or -1 if the process crashed or wasn't started
    int exitCode = -1;
    QByteArray output;
    QByteArray error;

    //! Standard output decoded with the locale encoding and universal
    //! newlines, as the original's text-mode pipes
    QString text() const;
};

//! Runs a command synchronously, with an optional timeout in milliseconds
using CommandRunner = std::function<ProcessResult(const QStringList &command, const QString &workingDir, int timeout)>;

//! Run a command synchronously and collect its output; the working
//! directory defaults to the current one.
ProcessResult runCommand(const QStringList &command, const QString &workingDir = QString(), int timeout = -1);
//! Replace the function used by runCommand(), e.g., with canned output for
//! tests. An empty function restores the default.
void setCommandRunner(CommandRunner runner);

//! Return the exit code of a given command, or -1 if it couldn't be run
int call(const QStringList &command, const QString &workingDir = QString());

/*!
 * Call `command` in `workingDir` and write the output to a temporary file
 *
 * This returns the name of the temporary file used. It is the caller's
 * responsibility to delete this file. If `fileId` is provided, it is used
 * as part of the temporary file's name, for ease of identification. If
 * `suffix` is provided, it is used as the extension of the temporary
 * file's name.
 */
QString callTempOutput(const QStringList &command, const QString &workingDir, const QString &fileId = QString(),
                       const QString &suffix = QString());
//! Write `contents` to a new temporary file named with `prefix`
QString writeTempFile(const QByteArray &contents, const QString &prefix, const QString &suffix = QString());

//! Replace diff3-style conflicts in a merged file with their base text,
//! marked with "==== BASE ====" lines
QByteArray baseFromDiff3(const QByteArray &merged);

// Path helpers with the semantics of Python's os.path, which the plugins'
// cache keys depend on. On Windows, they're ntpath's: paths may have drives
// ("C:" or "//server/share"), backslashes are separators, and paths are
// compared without case. Paths are returned with '/' separators, as Qt's.

//! Which rules paths follow; tests can use the other platform's
enum class PathRules {
    Posix,
    Windows,
};
#ifdef Q_OS_WIN
constexpr PathRules NativePathRules = PathRules::Windows;
#else
constexpr PathRules NativePathRules = PathRules::Posix;
#endif

QString pathJoin(const QString &a, const QString &b, PathRules rules = NativePathRules);
QString pathDirname(const QString &path, PathRules rules = NativePathRules);
QString pathBasename(const QString &path, PathRules rules = NativePathRules);
QString normpath(const QString &path, PathRules rules = NativePathRules);
//! os.path.isabs
bool isAbsolutePath(const QString &path, PathRules rules = NativePathRules);
//! os.path.abspath: normalised, relative to the current directory
QString abspath(const QString &path, PathRules rules = NativePathRules);
//! os.path.relpath; on Windows, a path on another drive than `start` has
//! no relative path, and is returned absolute
QString relpath(const QString &path, const QString &start, PathRules rules = NativePathRules);
//! Whether `path` is in `folder` (or is it), both absolute and normalised
bool pathIsInFolder(const QString &path, const QString &folder, PathRules rules = NativePathRules);
//! The extension of a path, as os.path.splitext()[1]
QString pathExtension(const QString &path);
//! os.path.commonprefix: the longest common leading string
QString commonPrefix(const QStringList &paths);

//! Raised when a version control module is passed an invalid (or not
//! present) path
class InvalidPath : public std::runtime_error {
public:
    InvalidPath(const QString &vcName, const QString &path, const QString &error);

    QString path() const { return path_; }

private:
    QString path_;
};

class Vc;

/*!
 * A version control system
 *
 * This holds the class-level parts of the original's plugins: their names,
 * how to find their repositories, and how to create a Vc for a location.
 */
class Plugin {
public:
    virtual ~Plugin() = default;

    //! The displayed name of the version control system
    virtual QString name() const = 0;
    //! The command line program
    virtual QString command() const = 0;
    //! The name of the repository folder, which is never listed
    virtual QString vcDir() const = 0;
    //! Whether to walk the current location's parents to find a repository
    //! root. Only used in legacy version control systems (e.g., old SVN,
    //! CVS, RCS).
    virtual bool rootWalk() const { return true; }

    virtual bool isInstalled() const;
    virtual bool checkRepoRoot(const QString &location) const;
    //! Determine if a directory is a valid repository for this system
    virtual bool validRepo(const QString &path) const = 0;
    //! Throws std::invalid_argument if `path` isn't in a repository
    virtual std::shared_ptr<Vc> create(const QString &path) const = 0;

    //! The repository root (empty if there is none) and the location
    std::pair<QString, QString> isInRepo(const QString &path) const;
    QString findRepoRoot(QString location) const;
};

/*!
 * A version-controlled location
 *
 * The location may be a sub-directory of the repository being compared,
 * and limits the comparison to that location. If a file is given, the
 * location is its containing directory.
 */
class Vc {
public:
    Vc(const Plugin &plugin, const QString &path);
    virtual ~Vc();

    Vc(const Vc &) = delete;
    Vc &operator=(const Vc &) = delete;

    const Plugin &plugin() const { return plugin_; }
    QString name() const { return plugin_.name(); }
    QString command() const { return plugin_.command(); }
    QString root() const { return root_; }
    QString location() const { return location_; }

    //! Run the version control program with `args` at the location (not
    //! the root)
    ProcessResult run(const QStringList &args, int timeout = -1) const;

    /*!
     * Get a list of files that will be committed from paths
     *
     * paths is a list of paths under the version control system root,
     * which may include directories. The result is the file paths that
     * would actually be committed given the paths; specifically this
     * excludes unchanged files and recursively lists files in directories.
     * Systems that can't tell return std::nullopt.
     */
    virtual std::optional<QStringList> filesToCommit(const QStringList &paths);

    /*!
     * Get a version-control defined pre-filled commit message
     *
     * This returns a message in situations where the version control
     * system has a (possibly partial) pre-filled message, most commonly in
     * merging, revert or cherry-picking scenarios, or an empty string.
     */
    virtual QString commitMessagePrefill() const { return {}; }

    /*!
     * A one-line readable description of unpushed commits
     *
     * This describes what would be pushed by the push action, e.g., "2
     * unpushed commits in 3 branches". Systems that always only push the
     * current branch shouldn't show branch information. Systems that can't
     * tell return std::nullopt.
     */
    virtual std::optional<QString> commitsToPushSummary() { return std::nullopt; }

    /*!
     * Get the set of valid actions for paths with version states
     *
     * This returns all actions that could reasonably be taken on *all* of
     * the paths. Empty paths are the placeholder rows of the view.
     */
    virtual Actions validActions(const QHash<QString, int> &pathStates) const;
    //! The actions this system implements
    virtual Actions supportedActions() const = 0;

    /*!
     * A file path for the repository version of the file at `path`
     *
     * Even if the system keeps an on-disk copy, a temporary file is
     * created, to avoid destructive editing. Temporary files aren't
     * deleted by the plugin. Throws InvalidPath.
     */
    virtual QString pathForRepoFile(const QString &path) = 0;

    // Comparing revisions
    //
    // By default, the working copy is compared with the repository's
    // current revision. Systems with revision support can compare it with
    // another revision (the base), or compare two revisions (the base and
    // the target), e.g., any two commits, tags or trees in git's history.
    // Files in revisions are read into memory, never written to files.

    //! Whether revisions other than the current one can be compared
    virtual bool hasRevisionSupport() const { return false; }
    //! The revisions to offer: the current one, branches, tags, and up to
    //! `maxCommits` recent commits
    virtual std::vector<Revision> revisions(int maxCommits = 100) const;
    /*!
     * Check that `revision` names a revision (e.g., a commit, tag or tree
     * id, or an expression such as "HEAD~2"); returns its id, or nullopt
     * and an error
     */
    virtual std::optional<QString> verifyRevision(const QString &revision, QString *error) const;
    //! The contents of the file at `path` in `revision`, or nullopt and an
    //! error if it isn't there
    virtual std::optional<QByteArray> fileAtRevision(const QString &revision, const QString &path,
                                                     QString *error) const;

    /*!
     * Compare the working copy with `base`, or `base` with `target`; empty
     * revisions are the current one and the working copy. The state caches
     * are then relative to `base`; refreshVcState() must follow.
     */
    void setRevisions(const QString &base, const QString &target);
    QString baseRevision() const { return baseRevision_; }
    QString targetRevision() const { return targetRevision_; }
    //! Whether two revisions are compared, rather than the working copy
    bool comparesRevisions() const { return !targetRevision_.isEmpty(); }
    //! Whether the working copy is compared with the current revision
    bool comparesWorkingCopy() const { return baseRevision_.isEmpty() && targetRevision_.isEmpty(); }

    //! Whether pathForConflict() is supported
    virtual bool hasConflictSupport() const { return false; }
    /*!
     * A file path for one side of a conflicted repository path
     *
     * The second value is whether the file is a temporary file. Throws
     * InvalidPath.
     */
    virtual std::pair<QString, bool> pathForConflict(const QString &path, Conflict conflict);

    /*!
     * Update cached version control state
     *
     * If a path is given, for example when a file has been modified and
     * saved in a file comparison, only that path is updated. Otherwise,
     * the tree rooted at the location is recursively refreshed.
     */
    void refreshVcState(const QString &path = QString());

    //! The entries in the folder `base`, including removed entries that are
    //! only known to the version control system
    std::vector<Entry> entries(const QString &base) const;

    /*!
     * Return the entry associated with the given path
     *
     * If the path doesn't exist, the entry has the state recorded by the
     * version control system, e.g., removed or missing.
     */
    Entry entry(const QString &path) const;

    // Version control actions, as the commands that perform them
    virtual QList<Command> commit(const QStringList &files, const QString &message) const;
    virtual QList<Command> update() const;
    virtual QList<Command> push() const;
    virtual QList<Command> add(const QStringList &files) const;
    virtual QList<Command> remove(const QStringList &files) const;
    virtual QList<Command> revert(const QStringList &files) const;
    virtual QList<Command> resolve(const QStringList &files) const;
    virtual QList<Command> unstage(const QStringList &files) const;

    // The state caches, keyed by absolute path
    const QHash<QString, int> &treeCache() const { return treeCache_; }
    const QHash<QString, QString> &treeMetaCache() const { return treeMetaCache_; }
    const QHash<QString, QSet<QString>> &treeMissingCache() const { return treeMissingCache_; }

protected:
    //! Update the cached state of the file(s) at `path`
    virtual void updateTreeStateCache(const QString &path) = 0;
    void addMissingCacheEntry(const QString &path, int state);
    //! Add a file or folder of the target revision, in revision comparisons
    void addRevisionTreeEntry(const QString &path, bool isDir);
    //! A path relative to the repository root; throws InvalidPath if it
    //! isn't in the repository
    QString repoRelativePath(const QString &path) const;
    //! A command run at the repository root
    Command rootCommand(const QStringList &command, const QStringList &files = {}) const;

    QHash<QString, int> treeCache_;
    QHash<QString, QString> treeMetaCache_;
    QHash<QString, QSet<QString>> treeMissingCache_;
    //! In revision comparisons, the files and folders of the target
    //! revision (and those the base has that it doesn't), as they're not
    //! on disk: folder → name → whether it's a folder
    QHash<QString, QHash<QString, bool>> revisionTree_;

private:
    QString baseRevision_;
    QString targetRevision_;
    const Plugin &plugin_;
    QString root_;
    QString location_;
};

//! The version control plugins, ordered according to best-guess as to which
//! system a user is likely to want by default in a multiple-VC situation
const std::vector<const Plugin *> &plugins();
//! The fallback used when no version control system is found
const Plugin &nullPlugin();
const Plugin *pluginByName(const QString &name);

struct AvailableVc {
    const Plugin *plugin = nullptr;
    //! Whether `location` is in a repository of this plugin
    bool enabled = false;
};
//! All plugins, with whether they handle `location`
std::vector<AvailableVc> getVcs(const QString &location);

} // namespace meld::vc

Q_DECLARE_OPERATORS_FOR_FLAGS(meld::vc::Actions)
