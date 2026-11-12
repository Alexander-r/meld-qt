#include "core/vc/plugins.h"
#include "core/vc/vc.h"
#include "core/vc/vcutil.h"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace meld::vc {

namespace {

const QStringList CommandArgs{QStringLiteral("--no-aliases"), QStringLiteral("--no-plugins")};

// We use std::nullopt here to indicate flags that we don't deal with or
// care about
std::optional<int> state1(QChar c)
{
    switch (c.unicode()) {
    case 'R':
        return StateRenamed; // File renamed
    case '?':
        return StateNone; // File unknown
    case 'C':
        return StateConflict; // File has conflicts
    default:
        // ' ': first status column empty, '+': file versioned, '-': file
        // unversioned, 'X': file nonexistent (and unknown to bzr), 'P':
        // entry for a pending merge (not a file)
        return std::nullopt;
    }
}

std::optional<int> state2(QChar c)
{
    switch (c.unicode()) {
    case ' ':
        return StateNormal; // Second status column empty
    case 'N':
        return StateNew; // File created
    case 'D':
        return StateRemoved; // File deleted
    case 'M':
        return StateModified; // File modified
    default:
        // 'K': file kind changed
        return std::nullopt;
    }
}

std::optional<int> state3(QChar c)
{
    switch (c.unicode()) {
    case '*':
    case '/':
    case '@':
        return StateModified;
    default:
        return std::nullopt;
    }
}

const QRegularExpression &validStatusRe()
{
    // Built from the status maps' keys, as in the original; note that "+-R"
    // in the first class is a character range.
    static const QRegularExpression re(QStringLiteral(R"(\A[ +-R?XCP][ NDKM][ */@]\s*)"));
    return re;
}

constexpr auto CommitStatuses = {StateModified, StateRenamed, StateNew, StateRemoved};

QString conflictSuffix(Conflict conflict)
{
    switch (conflict) {
    case ConflictBase:
        return QStringLiteral(".BASE");
    case ConflictOther:
        return QStringLiteral(".OTHER");
    case ConflictThis:
        return QStringLiteral(".THIS");
    default:
        return {};
    }
}

class BazaarVc : public Vc {
public:
    using Vc::Vc;

    Actions supportedActions() const override
    {
        return Action::Commit | Action::Update | Action::Push | Action::Add | Action::Remove | Action::Revert
            | Action::Resolve;
    }

    QList<Command> add(const QStringList &files) const override
    {
        return {rootCommand(fullCommand({QStringLiteral("add")}), files)};
    }

    QList<Command> commit(const QStringList &, const QString &message) const override
    {
        return {rootCommand(fullCommand({QStringLiteral("commit"), QStringLiteral("-m"), message}))};
    }

    QList<Command> revert(const QStringList &files) const override
    {
        return {rootCommand(fullCommand(QStringList{QStringLiteral("revert")} + files))};
    }

    QList<Command> push() const override { return {rootCommand(fullCommand({QStringLiteral("push")}))}; }

    QList<Command> update() const override
    {
        // TODO: Handle checkouts/bound branches by calling update instead of
        // pull. For now we've replicated existing functionality, as update
        // will not work for unbound branches.
        return {rootCommand(fullCommand({QStringLiteral("pull")}))};
    }

    QList<Command> resolve(const QStringList &files) const override
    {
        return {rootCommand(fullCommand(QStringList{QStringLiteral("resolve")} + files))};
    }

    QList<Command> remove(const QStringList &files) const override
    {
        return {rootCommand(fullCommand(QStringList{QStringLiteral("rm")} + files))};
    }

    std::optional<QStringList> filesToCommit(const QStringList &paths) override
    {
        std::set<QString> files;
        for (const QString &p : paths) {
            if (QFileInfo(p).isDir()) {
                for (auto it = treeCache_.cbegin(); it != treeCache_.cend(); ++it) {
                    const bool committable = std::ranges::find(CommitStatuses, it.value()) != CommitStatuses.end();
                    if (committable && it.key().startsWith(p))
                        files.insert(relpath(it.key(), root()));
                }
            } else {
                files.insert(relpath(p, root()));
            }
        }
        return QStringList(files.begin(), files.end());
    }

    QString pathForRepoFile(const QString &path) override
    {
        const QString repoPath = repoRelativePath(path);
        return callTempOutput({command(), QStringLiteral("cat"), repoPath}, root(), QString(), pathExtension(repoPath));
    }

    bool hasConflictSupport() const override { return true; }

    std::pair<QString, bool> pathForConflict(const QString &path, Conflict conflict) override
    {
        QString conflictPath = path;
        if (conflict != ConflictMerged && reverseRenameCache_.contains(path))
            conflictPath = reverseRenameCache_.value(path);
        repoRelativePath(conflictPath);
        // bzr paths are all temporary files
        return {conflictPath + conflictSuffix(conflict), false};
    }

protected:
    void updateTreeStateCache(const QString &path) override
    {
        // FIXME: This actually clears out state information, because the
        // current API doesn't have any state outside of the tree cache.
        QString branchRoot =
            runCommand(fullCommand({QStringLiteral("root"), path}), location()).text();
        while (branchRoot.endsWith(QLatin1Char('\n')))
            branchRoot.chop(1);
        const QStringList entries = outputLines(
            runCommand(fullCommand({QStringLiteral("status"), QStringLiteral("-S"), QStringLiteral("--no-pending"),
                                    branchRoot}))
                .text());

        static const QRegularExpression conflictRe(QStringLiteral("conflict in (.*)$"));
        static const QRegularExpression renamedRe(QStringLiteral("^(.*) => (.*)$"));
        static const QRegularExpression patchIndexRe(QStringLiteral("^=== modified file '(.*)' (.*)$"));

        std::map<QString, std::set<int>> treeCache;
        std::map<QString, QStringList> treeMetaCache;
        // Renames are handled in the order they're reported, as with the
        // original's dict
        QList<std::pair<QString, QString>> renameCache;
        const auto addRename = [&renameCache](const QString &oldPath, const QString &newPath) {
            for (auto &rename : renameCache) {
                if (rename.first == oldPath) {
                    rename.second = newPath;
                    return;
                }
            }
            renameCache.append({oldPath, newPath});
        };
        reverseRenameCache_.clear();

        // Files can appear twice in the list if they conflict and were
        // renamed at once.
        for (const QString &entry : entries) {
            QStringList meta;
            QString oldName;
            const QString stateString = entry.left(3);
            QString entryName = entry.mid(4).trimmed();
            if (!validStatusRe().match(stateString).hasMatch())
                continue;

            const std::optional<int> s1 = state1(stateString.at(0));
            const std::optional<int> s2 = state2(stateString.at(1));
            const std::optional<int> s3 = state3(stateString.at(2));
            std::set<int> states;
            for (const std::optional<int> &s : {s1, s2, s3}) {
                if (s)
                    states.insert(*s);
            }

            if (states.contains(StateConflict)) {
                const QRegularExpressionMatch match = conflictRe.match(entryName);
                if (match.hasMatch())
                    entryName = match.captured(1);
            }

            if (states.contains(StateRenamed)) {
                const QRegularExpressionMatch match = renamedRe.match(entryName);
                if (match.hasMatch()) {
                    oldName = match.captured(1);
                    entryName = match.captured(2);
                    meta.append(QStringLiteral("%1 ➡ %2").arg(oldName, entryName));
                }
            }

            QString entryPath = pathJoin(branchRoot, entryName);
            if (!oldName.isEmpty())
                addRename(pathJoin(branchRoot, oldName), entryPath);

            if (s3 == StateModified) {
                const QString line = splitLines(runCommand({QStringLiteral("bzr"), QStringLiteral("diff"), entryPath})
                                                    .text())
                                         .value(0);
                const QRegularExpressionMatch match = patchIndexRe.match(line);
                if (match.hasMatch())
                    meta.append(match.captured(2));
            }

            if (entryPath.endsWith(QLatin1Char('/')))
                entryPath.chop(1);
            treeCache[entryPath].insert(states.begin(), states.end());
            treeMetaCache[entryPath].append(meta);
            // Bazaar entries will only be REMOVED in the second state column
            addMissingCacheEntry(entryPath, s2.value_or(-1));
        }

        // Handle any renames now
        for (const auto &[oldPath, newPath] : renameCache) {
            if (auto it = treeCache.find(oldPath); it != treeCache.end()) {
                treeCache[newPath].insert(it->second.begin(), it->second.end());
                treeMetaCache[newPath].append(treeMetaCache[oldPath]);
                treeCache.erase(oldPath);
                treeMetaCache.erase(oldPath);
            }
            reverseRenameCache_.insert(newPath, oldPath);
        }

        for (const auto &[entryPath, states] : treeCache) {
            // The original fails on entries with no known states
            if (!states.empty())
                treeCache_[entryPath] = *states.rbegin();
        }
        treeMetaCache_.clear();
        for (const auto &[entryPath, meta] : treeMetaCache)
            treeMetaCache_[entryPath] = meta.join(QLatin1Char(','));
    }

private:
    QStringList fullCommand(const QStringList &args) const { return QStringList{command()} + CommandArgs + args; }

    QHash<QString, QString> reverseRenameCache_;
};

class BazaarPlugin : public Plugin {
public:
    QString name() const override { return QStringLiteral("Bazaar"); }
    QString command() const override { return QStringLiteral("bzr"); }
    QString vcDir() const override { return QStringLiteral(".bzr"); }

    bool validRepo(const QString &path) const override { return call({command(), QStringLiteral("root")}, path) == 0; }

    std::shared_ptr<Vc> create(const QString &path) const override { return std::make_shared<BazaarVc>(*this, path); }
};

} // namespace

const Plugin &bazaarPlugin()
{
    static const BazaarPlugin plugin;
    return plugin;
}

} // namespace meld::vc
