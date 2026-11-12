#include "core/vc/plugins.h"
#include "core/vc/vc.h"
#include "core/vc/vcutil.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace meld::vc {

namespace {

int stateForKey(const QString &key)
{
    static const QHash<QString, int> stateMap{
        {QStringLiteral("a"), StateNone},      {QStringLiteral("A"), StateNew},
        {QStringLiteral("M"), StateModified},  {QStringLiteral("M!"), StateConflict},
        {QStringLiteral("R"), StateRemoved},
        {QStringLiteral("F"), StateNonExist}, // previous name of file
        {QStringLiteral("T"), StateRenamed},  // new name of file
    };
    return stateMap.value(key, StateNone);
}

// Requires Darcs version >= 2.10.3
// TODO implement commitsToPushSummary using `darcs push --dry-run`
// Currently `darcs whatsnew` (as of v2.10.3) does not report conflicts
// see http://bugs.darcs.net/issue2138
class DarcsVc : public Vc {
public:
    using Vc::Vc;

    Actions supportedActions() const override
    {
        return Action::Commit | Action::Update | Action::Push | Action::Add | Action::Remove | Action::Revert;
    }

    QList<Command> commit(const QStringList &, const QString &message) const override
    {
        return {rootCommand({command(), QStringLiteral("record"), QStringLiteral("-a"), QStringLiteral("-m"), message})};
    }

    QList<Command> update() const override
    {
        return {rootCommand({command(), QStringLiteral("pull"), QStringLiteral("-a")})};
    }

    QList<Command> push() const override
    {
        return {rootCommand({command(), QStringLiteral("push"), QStringLiteral("-a")})};
    }

    QList<Command> add(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("add"), QStringLiteral("-r")}, files)};
    }

    QList<Command> remove(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("remove"), QStringLiteral("-r")}, files)};
    }

    QList<Command> revert(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("revert"), QStringLiteral("-a")}, files)};
    }

    QString pathForRepoFile(const QString &path) override
    {
        repoRelativePath(path);

        // `darcs show contents` needs the path before rename
        const QString original = reverseRenameCache_.value(path, path);
        const QString repoPath = original.mid(root().size() + 1);
        const ProcessResult result =
            runCommand({command(), QStringLiteral("show"), QStringLiteral("contents"), repoPath}, root());
        return writeTempFile(result.output, QStringLiteral("meld-tmp"), pathExtension(repoPath));
    }

protected:
    void updateTreeStateCache(const QString &path) override
    {
        // FIXME: currently ignoring 'path' due to darcs's bad behaviour
        // (= fails) when given "" argument
        const QStringList lines = outputLines(
            runCommand({command(), QStringLiteral("whatsnew"), QStringLiteral("-sl"), QStringLiteral("--machine-readable")},
                       location())
                .text());

        // Files can appear twice in the list if were modified and renamed at
        // once. Darcs first show file moves then modifications.
        if (lines.isEmpty() && QFileInfo(path).isFile()) {
            // If we're just updating a single file there's a chance that it
            // was previously modified, and now has been edited so that it is
            // un-modified. This will result in empty entries, and the cache
            // will still contain stale data, so we force the file to normal.
            treeCache_[path] = StateNormal;
            return;
        }

        QHash<QString, int> treeCache;
        QHash<QString, QString> treeMetaCache;
        QList<std::pair<QString, QString>> renameCache;
        reverseRenameCache_.clear();
        QString oldName;
        bool haveOldName = false;
        for (const QString &line : lines) {
            // skip empty lines and line starting with "What's new in foo"
            if (line.trimmed().isEmpty() || line.startsWith(QLatin1String("What")))
                continue;
            const qsizetype space = line.indexOf(QLatin1Char(' '));
            if (space < 0)
                continue;
            const QString statekey = line.left(space);
            const QString entryName = normpath(line.mid(space + 1));
            if (statekey == QLatin1String("F")) {
                oldName = entryName;
                haveOldName = true;
            }

            const QString entryPath = pathJoin(location(), entryName);

            if (statekey == QLatin1String("T") && haveOldName) {
                const QString oldPath = pathJoin(location(), oldName);
                bool replaced = false;
                for (auto &rename : renameCache) {
                    if (rename.first == oldPath) {
                        rename.second = entryPath;
                        replaced = true;
                    }
                }
                if (!replaced)
                    renameCache.append({oldPath, entryPath});
                haveOldName = false;
            }

            treeCache[entryPath] = stateForKey(statekey.trimmed());
        }

        for (const auto &[oldPath, newPath] : std::as_const(renameCache)) {
            reverseRenameCache_.insert(newPath, oldPath);
            const QString oldRelative = oldPath.mid(root().size() + 1);
            const QString newRelative = newPath.mid(root().size() + 1);
            treeMetaCache[newPath] = QStringLiteral("%1 ➡ %2").arg(oldRelative, newRelative);
        }

        treeCache_.insert(treeCache);
        treeMetaCache_ = treeMetaCache;
    }

private:
    QHash<QString, QString> reverseRenameCache_;
};

class DarcsPlugin : public Plugin {
public:
    QString name() const override { return QStringLiteral("Darcs"); }
    QString command() const override { return QStringLiteral("darcs"); }
    QString vcDir() const override { return QStringLiteral("_darcs"); }

    bool isInstalled() const override
    {
        // check that version >= 2.10.3
        const ProcessResult result = runCommand({command(), QStringLiteral("--version")});
        if (!result.started)
            return false;
        const QStringList parts = result.text().section(QLatin1Char(' '), 0, 0).split(QLatin1Char('.'));
        if (parts.size() < 3)
            return false;
        QList<int> version;
        for (int i = 0; i < 3; ++i) {
            static const QRegularExpression leadingDigits(QStringLiteral("^\\d+"));
            const QRegularExpressionMatch match = leadingDigits.match(parts.at(i));
            if (!match.hasMatch())
                return false;
            version.append(match.captured().toInt());
        }
        return version >= QList<int>{2, 10, 3};
    }

    bool validRepo(const QString &path) const override
    {
        return call({command(), QStringLiteral("show"), QStringLiteral("repo"), QStringLiteral("--no-files")}, path) == 0;
    }

    std::shared_ptr<Vc> create(const QString &path) const override { return std::make_shared<DarcsVc>(*this, path); }
};

} // namespace

const Plugin &darcsPlugin()
{
    static const DarcsPlugin plugin;
    return plugin;
}

} // namespace meld::vc
