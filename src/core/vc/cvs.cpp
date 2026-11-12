#include "core/vc/plugins.h"
#include "core/vc/vc.h"
#include "core/vc/vcutil.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>

namespace meld::vc {

Q_DECLARE_LOGGING_CATEGORY(lcVc)

namespace {

// According to the output of the 'status' command
int stateForKey(const QString &key)
{
    static const QHash<QString, int> stateMap{
        {QStringLiteral("Unknown"), StateNone},
        {QStringLiteral("Locally Added"), StateNew},
        {QStringLiteral("Up-to-date"), StateNormal},
        {QStringLiteral("!"), StateMissing},
        {QStringLiteral("I"), StateIgnored},
        {QStringLiteral("Locally Modified"), StateModified},
        {QStringLiteral("Locally Removed"), StateRemoved},
    };
    return stateMap.value(key, StateNone);
}

//! Files below `folder`, walked top-down as os.walk(), with sorted listings
void walkFiles(const QString &folder, const QString &vcDir, QStringList &files)
{
    const QDir dir(folder);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    if (!folder.endsWith(vcDir)) {
        for (const QFileInfo &info : entries) {
            if (!info.isDir())
                files.append(pathJoin(folder, info.fileName()));
        }
    }
    for (const QFileInfo &info : entries) {
        // Symbolic links to folders are listed as folders, but not followed
        if (info.isDir() && !info.isSymLink())
            walkFiles(pathJoin(folder, info.fileName()), vcDir, files);
    }
}

class CvsVc : public Vc {
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

    QList<Command> update() const override { return {rootCommand({command(), QStringLiteral("update")})}; }

    QList<Command> add(const QStringList &paths) const override
    {
        // CVS needs to add files together with all the parents (if those
        // are Unversioned yet)
        QStringList args;
        for (const QString &path : paths) {
            if (!QFileInfo(path).isFile())
                continue;
            const QString relative = relpath(path, root());
            const QString withSeparator = relative + QLatin1Char('/');
            for (qsizetype i = 0; i < withSeparator.size(); ++i) {
                if (withSeparator.at(i) == QLatin1Char('/'))
                    args.append(pathJoin(root(), relative.left(i)));
            }
        }
        return {rootCommand({command(), QStringLiteral("add")}, args)};
    }

    QList<Command> remove(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("remove"), QStringLiteral("-f")}, files)};
    }

    QList<Command> revert(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("update"), QStringLiteral("-C")}, files)};
    }

    QString pathForRepoFile(const QString &path) override
    {
        const QString repoPath = repoRelativePath(path);
        return callTempOutput({command(), QStringLiteral("-q"), QStringLiteral("update"), QStringLiteral("-p"), repoPath},
                              root(), QString(), pathExtension(repoPath));
    }

protected:
    void updateTreeStateCache(const QString &path) override
    {
        // Get the status of files
        const QStringList files = QFileInfo(path).isDir() ? findFiles(path) : QStringList{path};

        // Should suppress stderr here
        QStringList entries;
        const QString output =
            runCommand(QStringList{command(), QStringLiteral("-Q"), QStringLiteral("status")} + files, location()).text();
        for (const QString &line : splitLines(output)) {
            if (line.startsWith(QLatin1String("File:")))
                entries.append(line);
        }

        if (entries.isEmpty() && QFileInfo(path).isFile()) {
            // If we're just updating a single file there's a chance that it
            // was previously modified, and now has been edited so that it is
            // un-modified. This will result in empty entries, and the cache
            // will still contain stale data, so we force the file to normal.
            treeCache_[path] = StateNormal;
            return;
        }

        // There are 1 or more [modified] files, parse their state
        for (qsizetype i = 0; i < std::min(files.size(), entries.size()); ++i) {
            const QString &entry = entries.at(i);
            const QString statekey = entry.section(QLatin1Char(':'), -1).trimmed();
            const QString name = files.at(i).trimmed();
            if (!entry.contains(pathBasename(name))) {
                qCWarning(lcVc) << "CVS status for" << name << "does not match" << entry;
                return;
            }
            const QString entryPath = pathJoin(location(), name);
            const int state = stateForKey(statekey);
            treeCache_[entryPath] = state;
            addMissingCacheEntry(entryPath, state);
        }
    }

private:
    QStringList findFiles(const QString &path) const
    {
        const QString folder = pathJoin(location(), path);
        QStringList files;
        walkFiles(folder, plugin().vcDir(), files);
        for (QString &file : files)
            file = relpath(file, folder);
        return files;
    }
};

class CvsPlugin : public Plugin {
public:
    QString name() const override { return QStringLiteral("CVS"); }

    QString command() const override
    {
        // CVSNT is a drop-in replacement for CVS; if found, it is used instead
        static const QString cmd = QStandardPaths::findExecutable(QStringLiteral("cvsnt")).isEmpty()
            ? QStringLiteral("cvs")
            : QStringLiteral("cvsnt");
        return cmd;
    }

    QString vcDir() const override { return QStringLiteral("CVS"); }
    bool rootWalk() const override { return false; }

    bool validRepo(const QString &path) const override { return call({command(), QStringLiteral("ls")}, path) == 0; }

    std::shared_ptr<Vc> create(const QString &path) const override { return std::make_shared<CvsVc>(*this, path); }
};

} // namespace

const Plugin &cvsPlugin()
{
    static const CvsPlugin plugin;
    return plugin;
}

} // namespace meld::vc
