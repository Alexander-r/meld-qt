#include "core/vc/plugins.h"
#include "core/vc/vc.h"

#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QXmlStreamReader>

#include <algorithm>
#include <optional>

namespace meld::vc {

Q_DECLARE_LOGGING_CATEGORY(lcVc)

namespace {

//! Simple enum constants for differentiating conflict cases.
enum class ConflictType {
    Merge,
    Update,
};

int stateForItem(const QString &item)
{
    static const QHash<QString, int> stateMap{
        {QStringLiteral("unversioned"), StateNone},  {QStringLiteral("added"), StateNew},
        {QStringLiteral("normal"), StateNormal},     {QStringLiteral("missing"), StateMissing},
        {QStringLiteral("ignored"), StateIgnored},   {QStringLiteral("modified"), StateModified},
        {QStringLiteral("deleted"), StateRemoved},   {QStringLiteral("conflicted"), StateConflict},
    };
    return stateMap.value(item, StateNone);
}

//! Files matching `path` followed by the wildcard `pattern`, as glob(), but
//! sorted naturally so that lower revisions come first
QStringList globSuffix(const QString &path, const QString &pattern)
{
    const QFileInfo info(path);
    QStringList matches = info.dir().entryList({info.fileName() + pattern}, QDir::Files | QDir::Hidden | QDir::System);
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(matches.begin(), matches.end(), collator);
    for (QString &match : matches)
        match = info.dir().filePath(match);
    return matches;
}

struct StatusEntry {
    QString path;
    //! The "item" and "revision" attributes of each wc-status
    QList<std::pair<QString, std::optional<QString>>> statuses;
};

class SubversionVc : public Vc {
public:
    using Vc::Vc;

    Actions supportedActions() const override
    {
        return Action::Commit | Action::Update | Action::Add | Action::Remove | Action::Revert | Action::Resolve;
    }

    QList<Command> commit(const QStringList &files, const QString &message) const override
    {
        return {rootCommand({command(), QStringLiteral("commit"), QStringLiteral("-m"), message}, files)};
    }

    QList<Command> update() const override { return {rootCommand({command(), QStringLiteral("update")})}; }

    QList<Command> remove(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("rm"), QStringLiteral("--force")}, files)};
    }

    QList<Command> revert(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("revert")}, files)};
    }

    QList<Command> resolve(const QStringList &files) const override
    {
        return {rootCommand({command(), QStringLiteral("resolve"), QStringLiteral("--accept=working")}, files)};
    }

    QList<Command> add(const QStringList &paths) const override
    {
        // SVN < 1.7 needs to add folders from their immediate parent
        QList<Command> commands;
        QStringList files;
        for (const QString &path : paths) {
            const QFileInfo info(path);
            if (info.isDir())
                commands.append(Command{{command(), QStringLiteral("add")}, {path}, true, pathDirname(path)});
            else if (info.isFile())
                files.append(path);
        }
        if (!files.isEmpty())
            commands.append(Command{{command(), QStringLiteral("add")}, files, true, location()});
        return commands;
    }

    QString pathForRepoFile(const QString &path) override
    {
        const QString repoPath = repoRelativePath(path);
        return callTempOutput(
            {command(), QStringLiteral("cat"), QStringLiteral("-r"), QStringLiteral("BASE"), repoPath}, root(),
            QString(), pathExtension(repoPath));
    }

    bool hasConflictSupport() const override { return true; }

    /*!
     * SVN has two types of conflicts:
     * Merge conflicts, which give 3 files:
     *    .left.r* (THIS)
     *    .working (BASE... although this is a bit debatable)
     *    .right.r* (OTHER)
     * Update conflicts which give 3 files:
     *    .mine (THIS)
     *    .r* (lower - BASE)
     *    .r* (higher - OTHER)
     */
    std::pair<QString, bool> pathForConflict(const QString &path, Conflict conflict) override
    {
        repoRelativePath(path);

        // If this is merged, we just return the merged output
        if (conflict == ConflictMerged)
            return {path, false};

        // First find what type of conflict this is by looking at the base
        std::optional<ConflictType> conflictType;
        QString base = path + QStringLiteral(".working");
        if (QFileInfo::exists(base)) {
            // We have a merge conflict
            conflictType = ConflictType::Merge;
        } else {
            base = path + QStringLiteral(".mine");
            if (QFileInfo::exists(base)) {
                // We have an update conflict
                conflictType = ConflictType::Update;
            }
        }
        if (!conflictType)
            throw InvalidPath(name(), path, QStringLiteral("No known conflict type found"));

        QStringList matches;
        if (conflict == ConflictBase)
            return {base, false};
        if (conflict == ConflictThis) {
            matches = *conflictType == ConflictType::Merge ? globSuffix(path, QStringLiteral(".merge-left.r*"))
                                                           : globSuffix(path, QStringLiteral(".r*"));
            if (!matches.isEmpty())
                return {matches.first(), false};
        } else if (conflict == ConflictOther) {
            matches = *conflictType == ConflictType::Merge ? globSuffix(path, QStringLiteral(".merge-right.r*"))
                                                           : globSuffix(path, QStringLiteral(".r*"));
            if (!matches.isEmpty())
                return {matches.last(), false};
        }
        throw InvalidPath(name(), path, QStringLiteral("Conflict file does not exist"));
    }

protected:
    void updateTreeStateCache(const QString &path) override
    {
        // "svn --xml" outputs utf8, even with Windows non-utf8 locale
        const ProcessResult result = runCommand(
            {command(), QStringLiteral("status"), QStringLiteral("-v"), QStringLiteral("--xml"), path}, location());

        QList<StatusEntry> targets;
        QList<StatusEntry> changelists;
        if (!parseStatus(result.output, targets, changelists)) {
            qCWarning(lcVc) << "Couldn't parse Subversion status for" << path;
            return;
        }

        for (const StatusEntry &entry : targets + changelists) {
            QString entryPath = entry.path;
            if (entryPath.isEmpty())
                continue;
            if (!isAbsolutePath(entryPath))
                entryPath = abspath(pathJoin(location(), entryPath));
            for (const auto &[item, revision] : entry.statuses) {
                if (item.isEmpty())
                    continue;
                const int state = stateForItem(item);
                treeCache_[entryPath] = state;
                treeMetaCache_[entryPath] = revision ? QCoreApplication::translate("meld::vc", "Rev %1").arg(*revision)
                                                     : QString();
                addMissingCacheEntry(entryPath, state);
            }
        }
    }

private:
    //! Collect the entries of the status document's targets and changelists
    static bool parseStatus(const QByteArray &output, QList<StatusEntry> &targets, QList<StatusEntry> &changelists)
    {
        QXmlStreamReader xml(output);
        int depth = 0;
        QList<StatusEntry> *container = nullptr;
        int entryDepth = -1;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                ++depth;
                const QStringView name = xml.name();
                if (depth == 2 && (name == QLatin1String("target") || name == QLatin1String("changelist"))) {
                    container = name == QLatin1String("target") ? &targets : &changelists;
                } else if (container && entryDepth < 0 && name == QLatin1String("entry")) {
                    container->append({xml.attributes().value(QLatin1String("path")).toString(), {}});
                    entryDepth = depth;
                } else if (container && entryDepth >= 0 && name == QLatin1String("wc-status")) {
                    const QXmlStreamAttributes attributes = xml.attributes();
                    std::optional<QString> revision;
                    if (attributes.hasAttribute(QLatin1String("revision")))
                        revision = attributes.value(QLatin1String("revision")).toString();
                    container->last().statuses.append(
                        {attributes.value(QLatin1String("item")).toString(), revision});
                }
            } else if (xml.isEndElement()) {
                if (depth == entryDepth)
                    entryDepth = -1;
                if (depth == 2)
                    container = nullptr;
                --depth;
            }
        }
        return !xml.hasError();
    }
};

class SubversionPlugin : public Plugin {
public:
    QString name() const override { return QStringLiteral("Subversion"); }
    QString command() const override { return QStringLiteral("svn"); }
    QString vcDir() const override { return QStringLiteral(".svn"); }

    bool validRepo(const QString &path) const override
    {
        if (call({command(), QStringLiteral("info")}, path) != 0)
            return false;

        const QString vcDirPath = pathJoin(isInRepo(path).first, vcDir());

        // Check for repository version, trusting format file then entries
        // file
        std::optional<int> repoVersion;
        for (const char *filename : {"format", "entries"}) {
            QFile file(pathJoin(vcDirPath, QLatin1String(filename)));
            if (file.exists()) {
                if (file.open(QIODevice::ReadOnly)) {
                    bool ok = false;
                    const int version = QString::fromUtf8(file.readLine()).trimmed().toInt(&ok);
                    if (ok)
                        repoVersion = version;
                }
                break;
            }
        }
        if (!repoVersion && QFileInfo::exists(pathJoin(vcDirPath, QStringLiteral("wc.db"))))
            repoVersion = 12;
        return repoVersion && *repoVersion >= 12;
    }

    std::shared_ptr<Vc> create(const QString &path) const override
    {
        return std::make_shared<SubversionVc>(*this, path);
    }
};

} // namespace

const Plugin &subversionPlugin()
{
    static const SubversionPlugin plugin;
    return plugin;
}

} // namespace meld::vc
