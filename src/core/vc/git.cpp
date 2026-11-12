#include "core/vc/plugins.h"
#include "core/vc/vc.h"
#include "core/vc/vcutil.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace meld::vc {

namespace {

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::vc)
};

const QString NullSha = QStringLiteral("0000000000000000000000000000000000000000");

//! The stage numbers of conflict sides, as arguments for git-show.
//! ConflictMerged has no git-show argument unfortunately.
int conflictStage(Conflict conflict)
{
    switch (conflict) {
    case ConflictBase:
        return 1;
    case ConflictLocal:
        return 2;
    case ConflictRemote:
        return 3;
    default:
        return 0;
    }
}

int stateForKey(const QString &key)
{
    static const QHash<QString, int> stateMap{
        {QStringLiteral("X"), StateNone},     // Unknown
        {QStringLiteral("A"), StateNew},      // New
        {QStringLiteral("D"), StateRemoved},  // Deleted
        {QStringLiteral("M"), StateModified}, // Modified
        {QStringLiteral("T"), StateModified}, // Type-changed
        {QStringLiteral("U"), StateConflict}, // Unmerged
    };
    return stateMap.value(key, StateNone);
}

const QRegularExpression &diffRe()
{
    static const QRegularExpression re(QStringLiteral(R"(:(\d+) (\d+) ([a-z0-9]+) ([a-z0-9]+) ([XADMTU])\t(.*))"));
    return re;
}

class GitVc : public Vc {
public:
    using Vc::Vc;

    Actions supportedActions() const override
    {
        return Action::Commit | Action::Update | Action::Push | Action::Add | Action::Remove | Action::Revert
            | Action::Resolve | Action::Unstage;
    }

    std::optional<QString> commitsToPushSummary() override
    {
        const QHash<QString, QStringList> branchRefs = commitsToPush();
        qsizetype unpushedBranches = 0;
        qsizetype unpushedCommits = 0;
        for (const QStringList &revisions : branchRefs) {
            if (!revisions.isEmpty())
                ++unpushedBranches;
            unpushedCommits += revisions.size();
        }
        if (!unpushedCommits)
            return QString();
        const QString commits = Tr::tr("%n unpushed commits", nullptr, int(unpushedCommits));
        if (unpushedBranches > 1) {
            const QString branches = Tr::tr("%n branches", nullptr, int(unpushedBranches));
            //: The first is replaced by the translated "%n unpushed commits", the
            //: second by the translated "%n branches"
            return Tr::tr("%1 in %2").arg(commits, branches);
        }
        return commits;
    }

    Actions validActions(const QHash<QString, int> &pathStates) const override
    {
        Actions valid = Vc::validActions(pathStates);
        const bool staged = std::all_of(pathStates.cbegin(), pathStates.cend(),
                                        [](int s) { return s == StateModified || s == StateNew; });
        if (staged)
            valid |= Action::Unstage;
        return valid;
    }

    std::optional<QStringList> filesToCommit(const QStringList &paths) override
    {
        QSet<QString> files;
        for (const QString &p : paths) {
            if (QFileInfo(p).isDir()) {
                const auto [cachedEntries, entries] = modifiedFiles(p);
                for (const QString &e : cachedEntries + entries) {
                    const QRegularExpressionMatch match = diffRe().match(e);
                    if (match.hasMatch())
                        files.insert(match.captured(6));
                }
            } else {
                files.insert(relpath(p, root()));
            }
        }
        QStringList sorted(files.begin(), files.end());
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    }

    QString commitMessagePrefill() const override
    {
        QFile file(pathJoin(root(), QStringLiteral(".git/MERGE_MSG")));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        // If I have to deal with non-ascii, non-UTF8 pregenerated commit
        // messages, I'm taking up pig farming.
        QStringList lines;
        for (const QString &line : splitLines(QString::fromUtf8(file.readAll()))) {
            if (!line.startsWith(QLatin1Char('#')))
                lines.append(line);
        }
        return lines.join(QLatin1Char('\n'));
    }

    QList<Command> commit(const QStringList &files, const QString &message) const override
    {
        return {rootCommand({command(), QStringLiteral("commit"), QStringLiteral("-m"), message}, files)};
    }

    QList<Command> update() const override { return {rootCommand({command(), QStringLiteral("pull")})}; }

    QList<Command> push() const override { return {rootCommand({command(), QStringLiteral("push")})}; }

    QList<Command> add(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("add")}, files)};
    }

    QList<Command> remove(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("rm"), QStringLiteral("-r")}, files)};
    }

    QList<Command> revert(const QStringList &files) const override
    {
        QStringList exists;
        QStringList missing;
        for (const QString &f : files)
            (QFileInfo::exists(f) ? exists : missing).append(f);
        QList<Command> commands;
        if (!exists.isEmpty())
            commands.append(rootCommand({command(), QStringLiteral("checkout")}, exists));
        if (!missing.isEmpty())
            commands.append(rootCommand({command(), QStringLiteral("checkout"), QStringLiteral("HEAD")}, missing));
        return commands;
    }

    QList<Command> resolve(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("add")}, files)};
    }

    QList<Command> unstage(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("restore"), QStringLiteral("--staged")}, files)};
    }

    bool hasRevisionSupport() const override { return true; }

    std::vector<Revision> revisions(int maxCommits) const override
    {
        std::vector<Revision> result;
        result.push_back({Revision::Kind::Head, QStringLiteral("HEAD"), Tr::tr("The current commit")});
        // Branches, remote branches and tags, newest first
        const QStringList refs = outputLines(
            run({QStringLiteral("for-each-ref"), QStringLiteral("--sort=-creatordate"),
                 QStringLiteral("--format=%(refname)%09%(refname:short)%09%(subject)"), QStringLiteral("refs/heads"),
                 QStringLiteral("refs/remotes"), QStringLiteral("refs/tags")})
                .text());
        for (const QString &line : refs) {
            const QStringList parts = line.split(QLatin1Char('\t'));
            if (parts.size() < 3 || parts.at(0).endsWith(QLatin1String("/HEAD")))
                continue;
            Revision::Kind kind = Revision::Kind::Tag;
            if (parts.at(0).startsWith(QLatin1String("refs/heads/")))
                kind = Revision::Kind::Branch;
            else if (parts.at(0).startsWith(QLatin1String("refs/remotes/")))
                kind = Revision::Kind::RemoteBranch;
            result.push_back({kind, parts.at(1), parts.mid(2).join(QLatin1Char('\t'))});
        }
        if (maxCommits > 0) {
            const QStringList commits = outputLines(run({QStringLiteral("log"), QStringLiteral("-n"),
                                                         QString::number(maxCommits), QStringLiteral("--format=%h%x09%s"),
                                                         QStringLiteral("HEAD"), QStringLiteral("--")})
                                                        .text());
            for (const QString &line : commits) {
                const qsizetype tab = line.indexOf(QLatin1Char('\t'));
                if (tab > 0)
                    result.push_back({Revision::Kind::Commit, line.left(tab), line.mid(tab + 1)});
            }
        }
        return result;
    }

    std::optional<QString> verifyRevision(const QString &revision, QString *error) const override
    {
        // Commits, tags and trees all have trees
        const ProcessResult result =
            run({QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("--quiet"),
                 QStringLiteral("--end-of-options"), revision + QStringLiteral("^{tree}")});
        const QString id = result.text().trimmed();
        if (result.exitCode != 0 || id.isEmpty() || revision.startsWith(QLatin1Char('-'))) {
            if (error)
                *error = Tr::tr("“%1” isn’t a commit, tag or tree in this repository.").arg(revision);
            return std::nullopt;
        }
        return id;
    }

    std::optional<QByteArray> fileAtRevision(const QString &revision, const QString &path,
                                             QString *error) const override
    {
        QString repoPath;
        try {
            repoPath = repoRelativePath(abspath(path));
        } catch (const InvalidPath &) {
            if (error)
                *error = Tr::tr("%1 isn’t in the repository.").arg(path);
            return std::nullopt;
        }
        const ProcessResult result =
            runCommand({command(), QStringLiteral("cat-file"), QStringLiteral("blob"),
                        QStringLiteral("%1:%2").arg(revision.isEmpty() ? QStringLiteral("HEAD") : revision, repoPath)},
                       root());
        if (!result.started || result.exitCode != 0) {
            if (error)
                *error = Tr::tr("%1 isn’t in %2.").arg(repoPath, revision.isEmpty() ? QStringLiteral("HEAD") : revision);
            return std::nullopt;
        }
        return result.output;
    }

    bool hasConflictSupport() const override { return true; }

    std::pair<QString, bool> pathForConflict(const QString &path, Conflict conflict) override
    {
        if (conflict == ConflictMerged) {
            // Special case: no way to get merged result from git directly
            const QString local = pathForConflict(path, ConflictLocal).first;
            const QString base = pathForConflict(path, ConflictBase).first;
            const QString remote = pathForConflict(path, ConflictRemote).first;
            const auto [filename, isTemp] = remergeWithAncestor(local, base, remote, pathExtension(path));
            for (const QString &tempFile : {local, base, remote}) {
                QFile::setPermissions(tempFile, QFile::permissions(tempFile) | QFile::WriteOwner);
                QFile::remove(tempFile);
            }
            return {filename, isTemp};
        }

        const QString repoPath = repoRelativePath(path);
        const QStringList args{QStringLiteral("git"), QStringLiteral("show"),
                               QStringLiteral(":%1:%2").arg(conflictStage(conflict)).arg(repoPath)};
        return {callTempOutput(args, location(), conflictName(conflict), pathExtension(repoPath)), true};
    }

    QString pathForRepoFile(const QString &path) override
    {
        const QString repoPath = repoRelativePath(path);
        const QString object = QStringLiteral("HEAD:") + repoPath;
        return callTempOutput({command(), QStringLiteral("cat-file"), QStringLiteral("blob"), object}, root(), QString(),
                              pathExtension(repoPath));
    }

protected:
    void updateTreeStateCache(const QString &path) override
    {
        if (comparesRevisions()) {
            updateRevisionsCache(path);
            return;
        }

        const auto [cachedEntries, entries] = modifiedFiles(path);

        // Identify ignored files and folders
        const QStringList ignoredEntries = outputLines(
            run({QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--ignored"),
                 QStringLiteral("--exclude-standard"), QStringLiteral("--directory"), path})
                .text());

        // Identify unversioned files
        const QStringList unversionedEntries = outputLines(
            run({QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--exclude-standard"), path})
                .text());

        if (cachedEntries.isEmpty() && entries.isEmpty() && QFileInfo(path).isFile()) {
            // If we're just updating a single file there's a chance that it
            // was previously modified, and now has been edited so that it is
            // un-modified. This will result in empty entries, and the cache
            // will still contain stale data, so we force the file to normal.
            treeCache_[realPath(path)] = StateNormal;
        } else {
            QHash<QString, QStringList> treeMetaCache;
            QSet<QString> staged;
            QSet<QString> unstaged;

            // We iterate over both cached entries and entries, accumulating
            // metadata from both, but using the state from entries.
            for (const QString &entry : cachedEntries + entries) {
                const QRegularExpressionMatch match = diffRe().match(entry);
                if (!match.hasMatch())
                    continue;
                const QString oldMode = match.captured(1);
                const QString newMode = match.captured(2);
                const QString newSha = match.captured(4);
                const int state = stateForKey(match.captured(5).trimmed());
                const QString entryPath = realPath(match.captured(6));
                treeCache_[entryPath] = state;
                // Git entries can't be MISSING; that's just an unstaged REMOVED
                addMissingCacheEntry(entryPath, state);
                if (oldMode != newMode)
                    treeMetaCache[entryPath].append(Tr::tr("Mode changed from %1 to %2").arg(oldMode, newMode));
                (newSha == NullSha ? unstaged : staged).insert(entryPath);
            }

            // Staging is relative to the current commit
            if (comparesWorkingCopy()) {
                for (const QString &p : std::as_const(staged))
                    treeMetaCache[p].append(unstaged.contains(p) ? Tr::tr("Partially staged") : Tr::tr("Staged"));
            }

            for (auto it = treeMetaCache.cbegin(); it != treeMetaCache.cend(); ++it)
                treeMetaCache_[it.key()] = it.value().join(QStringLiteral("; "));
        }

        // The original only does these for folders and modified files, so an
        // untracked file that was saved in a comparison became normal.
        for (const QString &p : ignoredEntries)
            treeCache_[realPath(p)] = StateIgnored;
        for (const QString &p : unversionedEntries)
            treeCache_[realPath(p)] = StateNone;
    }

private:
    QString base() const { return baseRevision().isEmpty() ? QStringLiteral("HEAD") : baseRevision(); }

    //! The states of two revisions' files, and the target revision's files
    void updateRevisionsCache(const QString &path)
    {
        // NUL-separated output has no quoted paths
        const QList<QByteArray> tree =
            run({QStringLiteral("ls-tree"), QStringLiteral("-r"), QStringLiteral("-t"), QStringLiteral("-z"),
                 targetRevision(), QStringLiteral("--"), path})
                .output.split('\0');
        for (const QByteArray &record : tree) {
            // "<mode> <type> <id>\t<path>"
            const qsizetype tab = record.indexOf('\t');
            if (tab < 0)
                continue;
            const QString name = QString::fromUtf8(record.mid(tab + 1));
            if (name == QLatin1String(".") || name == QLatin1String("./"))
                continue;
            const bool isDir = record.left(tab).split(' ').value(1) == "tree";
            addRevisionTreeEntry(abspath(pathJoin(location(), name)), isDir);
        }

        const QList<QByteArray> changes =
            run({QStringLiteral("diff"), QStringLiteral("--relative"), QStringLiteral("--name-status"),
                 QStringLiteral("--no-renames"), QStringLiteral("-z"), base(), targetRevision(), QStringLiteral("--"),
                 path})
                .output.split('\0');
        for (qsizetype i = 0; i + 1 < changes.size(); i += 2) {
            const int state = stateForKey(QString::fromLatin1(changes.at(i).left(1)));
            const QString entryPath = abspath(pathJoin(location(), QString::fromUtf8(changes.at(i + 1))));
            treeCache_[entryPath] = state;
            // Files that only the base has are shown too
            if (state == StateRemoved)
                addRevisionTreeEntry(entryPath, false);
        }
    }

    QHash<QString, QStringList> commitsToPush() const
    {
        QHash<QString, QStringList> branchRevisions;
        const QStringList branchRemotes = outputLines(
            run({QStringLiteral("for-each-ref"),
                 QStringLiteral("--format=%(refname:short) %(upstream:short) %(upstream:trackshort)"),
                 QStringLiteral("refs/heads")})
                .text());
        static const QRegularExpression whitespace(QStringLiteral("\\s+"));
        for (const QString &line : branchRemotes) {
            const QStringList parts = line.split(whitespace, Qt::SkipEmptyParts);
            if (parts.size() != 3)
                continue;
            const QString &branch = parts.at(0);
            const QString &remote = parts.at(1);
            branchRevisions[branch] = outputLines(
                run({QStringLiteral("rev-list"), branch, QLatin1Char('^') + remote, QStringLiteral("--")}).text());
        }
        return branchRevisions;
    }

    std::pair<QStringList, QStringList> modifiedFiles(const QString &path) const
    {
        // Update the index to avoid reading stale status information
        run({QStringLiteral("update-index"), QStringLiteral("--refresh")}, 5000);

        // Compared with another revision, the working copy's differences
        // from it are what matter, staged or not
        if (!comparesWorkingCopy()) {
            const QStringList entries = outputLines(
                run({QStringLiteral("diff-index"), base(), QStringLiteral("--relative"), path}).text());
            return {QStringList(), entries};
        }

        // Get status differences between the index and the repo HEAD
        const QStringList cachedEntries = outputLines(
            run({QStringLiteral("diff-index"), QStringLiteral("--cached"), QStringLiteral("HEAD"),
                 QStringLiteral("--relative"), path})
                .text());

        // Get status differences between the index and files-on-disk
        const QStringList entries = outputLines(
            run({QStringLiteral("diff-files"), QStringLiteral("-0"), QStringLiteral("--relative"), path}).text());

        // Files can show up in both lists, e.g., if a file is modified,
        // added to the index and changed again. This is okay, and in fact
        // the calling logic requires it for staging feedback.
        return {cachedEntries, entries};
    }

    std::pair<QString, bool> remergeWithAncestor(const QString &local, const QString &base, const QString &remote,
                                                 const QString &suffix) const
    {
        // Reconstruct a mixed merge-plus-base file: this re-merges a given
        // file to get diff3-style conflicts which we can then use to get a
        // file that contains the pre-merged result everywhere that has no
        // conflict, and the common ancestor anywhere there *is* a conflict.
        const ProcessResult result = run(
            {QStringLiteral("merge-file"), QStringLiteral("-p"), QStringLiteral("--diff3"), local, base, remote});
        const QString prefix = QStringLiteral("meld-tmp-%1-").arg(int(ConflictMerged));
        return {writeTempFile(baseFromDiff3(result.output), prefix, suffix), true};
    }


    QString realPath(QString name) const
    {
        name = name.trimmed();
        // Unicode file names and file names containing quotes are returned
        // by git as quoted strings
        if (name.startsWith(QLatin1Char('"')) && name.size() >= 2)
            name = QString::fromUtf8(unescapeCString(QStringView(name).sliced(1, name.size() - 2)));
        return abspath(pathJoin(location(), name));
    }
};

class GitPlugin : public Plugin {
public:
    QString name() const override { return QStringLiteral("Git"); }
    QString command() const override { return QStringLiteral("git"); }
    QString vcDir() const override { return QStringLiteral(".git"); }

    bool isInstalled() const override
    {
        return runCommand({command(), QStringLiteral("--version")}).text().startsWith(QLatin1String("git version"));
    }

    bool checkRepoRoot(const QString &location) const override
    {
        // Check exists instead of isdir, since .git might be a git-file
        return QFileInfo::exists(pathJoin(location, vcDir()));
    }

    bool validRepo(const QString &path) const override
    {
        // TODO: On Windows, this exit code is wrong under the normal shell;
        // it appears to be correct under the default git bash shell however.
        return call({command(), QStringLiteral("branch")}, path) == 0;
    }

    std::shared_ptr<Vc> create(const QString &path) const override { return std::make_shared<GitVc>(*this, path); }
};

} // namespace

const Plugin &gitPlugin()
{
    static const GitPlugin plugin;
    return plugin;
}

} // namespace meld::vc
