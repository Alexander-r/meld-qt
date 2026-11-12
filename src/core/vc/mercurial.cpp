#include "core/vc/plugins.h"
#include "core/vc/vc.h"
#include "core/vc/vcutil.h"

#include <QFileInfo>

namespace meld::vc {

namespace {

int stateForKey(const QString &key)
{
    static const QHash<QString, int> stateMap{
        {QStringLiteral("?"), StateNone},     {QStringLiteral("A"), StateNew},
        {QStringLiteral("C"), StateNormal},   {QStringLiteral("!"), StateMissing},
        {QStringLiteral("I"), StateIgnored},  {QStringLiteral("M"), StateModified},
        {QStringLiteral("R"), StateRemoved},
    };
    return stateMap.value(key, StateNone);
}

class MercurialVc : public Vc {
public:
    using Vc::Vc;

    Actions supportedActions() const override
    {
        return Action::Commit | Action::Update | Action::Add | Action::Remove | Action::Revert;
    }

    QList<Command> commit(const QStringList &files, const QString &message) const override
    {
        return {rootCommand({command(), QStringLiteral("commit"), QStringLiteral("-m"), message}, files)};
    }

    QList<Command> update() const override
    {
        return {rootCommand({command(), QStringLiteral("pull"), QStringLiteral("-u")})};
    }

    QList<Command> add(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("add")}, files)};
    }

    QList<Command> remove(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("rm")}, files)};
    }

    QList<Command> revert(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("revert")}, files)};
    }

    QString pathForRepoFile(const QString &path) override
    {
        const QString repoPath = repoRelativePath(path);
        return callTempOutput({command(), QStringLiteral("cat"), repoPath}, root(), QString(), pathExtension(repoPath));
    }

protected:
    void updateTreeStateCache(const QString &path) override
    {
        // Get the status of modified files
        QStringList entries = outputLines(
            runCommand({command(), QStringLiteral("status"), QStringLiteral("-A"), path}, location()).text());
        // The following removes duplicate file entries. Just in case.
        entries.removeDuplicates();

        if (entries.isEmpty() && QFileInfo(path).isFile()) {
            // If we're just updating a single file there's a chance that it
            // was previously modified, and now has been edited so that it is
            // un-modified. This will result in empty entries, and the cache
            // will still contain stale data, so we force the file to normal.
            treeCache_[path] = StateNormal;
            return;
        }

        // There are 1 or more modified files, parse their state
        for (const QString &entry : std::as_const(entries)) {
            // we might have a space in file name, it should be ignored
            const qsizetype space = entry.indexOf(QLatin1Char(' '));
            if (space < 0)
                continue;
            const QString entryPath = pathJoin(location(), entry.mid(space + 1).trimmed());
            const int state = stateForKey(entry.left(space).trimmed());
            treeCache_[entryPath] = state;
            addMissingCacheEntry(entryPath, state);
        }
    }
};

class MercurialPlugin : public Plugin {
public:
    QString name() const override { return QStringLiteral("Mercurial"); }
    QString command() const override { return QStringLiteral("hg"); }
    QString vcDir() const override { return QStringLiteral(".hg"); }

    bool validRepo(const QString &path) const override { return call({command(), QStringLiteral("root")}, path) == 0; }

    std::shared_ptr<Vc> create(const QString &path) const override
    {
        return std::make_shared<MercurialVc>(*this, path);
    }
};

} // namespace

const Plugin &mercurialPlugin()
{
    static const MercurialPlugin plugin;
    return plugin;
}

} // namespace meld::vc
