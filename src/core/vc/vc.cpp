#include "core/vc/vc.h"

#include "core/vc/plugins.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <tuple>
#include <QTemporaryFile>

#include <algorithm>

namespace meld::vc {

Q_LOGGING_CATEGORY(lcVc, "meld.vc")

namespace {

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::vc)
};

ProcessResult defaultRunner(const QStringList &command, const QString &workingDir, int timeout)
{
    ProcessResult result;
    if (command.isEmpty())
        return result;
    QProcess process;
    if (!workingDir.isEmpty())
        process.setWorkingDirectory(workingDir);
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(command.first(), command.mid(1));
    if (!process.waitForStarted())
        return result;
    result.started = true;
    if (!process.waitForFinished(timeout)) {
        process.terminate();
        if (!process.waitForFinished(1000))
            process.kill();
        process.waitForFinished();
    }
    result.output = process.readAllStandardOutput();
    result.error = process.readAllStandardError();
    if (process.exitStatus() == QProcess::NormalExit)
        result.exitCode = process.exitCode();
    return result;
}

CommandRunner &commandRunner()
{
    static CommandRunner runner;
    return runner;
}

} // namespace

QString conflictName(Conflict conflict)
{
    switch (conflict) {
    case ConflictMerged:
        return Tr::tr("Merged");
    case ConflictBase:
        return Tr::tr("Base");
    case ConflictLocal:
        return Tr::tr("Local");
    case ConflictRemote:
        return Tr::tr("Remote");
    case ConflictMax:
        break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Entry

QString Entry::stateName(int state)
{
    switch (state) {
    case StateIgnored:
        return Tr::tr("Ignored");
    case StateNone:
        return Tr::tr("Unversioned");
    case StateError:
        return Tr::tr("Error");
    case StateNew:
        return Tr::tr("Newly added");
    case StateModified:
        return Tr::tr("Modified");
    case StateRenamed:
        return Tr::tr("Renamed");
    case StateConflict:
        return Tr::tr("Conflict");
    case StateRemoved:
        return Tr::tr("Removed");
    case StateMissing:
        return Tr::tr("Missing");
    case StateNonExist:
        return Tr::tr("Not present");
    case StateSpinner:
        return Tr::tr("Scanning…");
    default:
        return {};
    }
}

bool Entry::isModified(const Entry &entry)
{
    return entry.state >= StateNew || (entry.isDir && entry.state > StateNone);
}

bool Entry::isNormal(const Entry &entry)
{
    return entry.state == StateNormal;
}

bool Entry::isNonVc(const Entry &entry)
{
    return entry.state == StateNone || (entry.isDir && entry.state > StateIgnored);
}

bool Entry::isIgnored(const Entry &entry)
{
    return entry.state == StateIgnored || entry.isDir;
}

// ---------------------------------------------------------------------------
// Processes

QString ProcessResult::text() const
{
    QString decoded = QString::fromLocal8Bit(output);
    decoded.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    decoded.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return decoded;
}

ProcessResult runCommand(const QStringList &command, const QString &workingDir, int timeout)
{
    if (const CommandRunner &runner = commandRunner())
        return runner(command, workingDir, timeout);
    return defaultRunner(command, workingDir, timeout);
}

void setCommandRunner(CommandRunner runner)
{
    commandRunner() = std::move(runner);
}

int call(const QStringList &command, const QString &workingDir)
{
    const ProcessResult result = runCommand(command, workingDir);
    return result.started ? result.exitCode : -1;
}

QString writeTempFile(const QByteArray &contents, const QString &prefix, const QString &suffix)
{
    QTemporaryFile file(QDir::tempPath() + QLatin1Char('/') + prefix + QStringLiteral("XXXXXX") + suffix);
    file.setAutoRemove(false);
    if (!file.open())
        throw std::runtime_error(QStringLiteral("Couldn't create a temporary file: %1").arg(file.errorString()).toStdString());
    file.write(contents);
    file.close();
    return file.fileName();
}

QString callTempOutput(const QStringList &command, const QString &workingDir, const QString &fileId,
                       const QString &suffix)
{
    // Error handling here involves doing nothing; in most cases, the only
    // sane response is to return an empty temp file.
    const ProcessResult result = runCommand(command, workingDir);
    const QString prefix = QStringLiteral("meld-tmp") + (fileId.isEmpty() ? QString() : QLatin1Char('-') + fileId);
    return writeTempFile(result.output, prefix, suffix);
}

QByteArray baseFromDiff3(const QByteArray &merged)
{
    // The file contents are bytes in an unknown encoding, so they are
    // matched as Latin-1, which maps every byte to one character.
    static const QRegularExpression baseRe(QStringLiteral("(*LF)^<{7}.*?$\\r?\\n(?<local>.*?)"
                                                          "^\\|{7}.*?$\\r?\\n(?<base>.*?)"
                                                          "^={7}.*?$\\r?\\n(?<remote>.*?)"
                                                          "^>{7}.*?$\\r?\\n"),
                                           QRegularExpression::DotMatchesEverythingOption
                                               | QRegularExpression::MultilineOption);
    QString text = QString::fromLatin1(merged);
    text.replace(baseRe, QStringLiteral("==== BASE ====\n\\2==== BASE ====\n"));
    return text.toLatin1();
}

// ---------------------------------------------------------------------------
// os.path equivalents

namespace {

// Windows paths (ntpath), with '/' separators

QString windowsSeparators(const QString &path)
{
    QString result = path;
    result.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return result;
}

/*!
 * ntpath.splitdrive: a drive letter ("C:") or a UNC share
 * ("//server/share"), and the rest of the path. Drive letters are made
 * upper case, so that paths compare as Qt's do.
 */
std::pair<QString, QString> splitDrive(const QString &rawPath)
{
    const QString path = windowsSeparators(rawPath);
    if (path.size() >= 2 && path.at(1) == QLatin1Char(':') && path.at(0).isLetter())
        return {path.left(1).toUpper() + QLatin1Char(':'), path.mid(2)};
    if (path.startsWith(QLatin1String("//")) && !path.startsWith(QLatin1String("///"))) {
        // "//server/share" and the rest; without a share, it's all drive
        const qsizetype server = path.indexOf(QLatin1Char('/'), 2);
        if (server < 0)
            return {path, QString()};
        const qsizetype share = path.indexOf(QLatin1Char('/'), server + 1);
        if (share < 0)
            return {path, QString()};
        return {path.left(share), path.mid(share)};
    }
    return {QString(), path};
}

bool sameDrive(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

QString windowsJoin(const QString &a, const QString &b)
{
    const auto [aDrive, aPath] = splitDrive(a);
    const auto [bDrive, bPath] = splitDrive(b);
    if (bPath.startsWith(QLatin1Char('/'))) {
        // Rooted: on its drive, or on a's
        return bDrive.isEmpty() ? aDrive + bPath : bDrive + bPath;
    }
    if (!bDrive.isEmpty() && !sameDrive(aDrive, bDrive))
        return bDrive + bPath;
    // "C:" + "b" is "C:b", relative to the drive's current folder, but a
    // share is always its root
    QString result = aDrive + aPath;
    const bool share = aDrive.startsWith(QLatin1String("//"));
    if (!result.isEmpty() && !result.endsWith(QLatin1Char('/')) && (!aPath.isEmpty() || share))
        result += QLatin1Char('/');
    return result + bPath;
}

QString windowsDirname(const QString &path)
{
    const auto [drive, rest] = splitDrive(path);
    QString head = rest.left(rest.lastIndexOf(QLatin1Char('/')) + 1);
    if (!head.isEmpty() && head != QString(head.size(), QLatin1Char('/'))) {
        while (head.endsWith(QLatin1Char('/')))
            head.chop(1);
    }
    return drive + head;
}

QString windowsNormpath(const QString &path)
{
    const auto [drive, rest] = splitDrive(path);
    const bool rooted = rest.startsWith(QLatin1Char('/'));
    QStringList components;
    for (const QString &component : rest.split(QLatin1Char('/'))) {
        if (component.isEmpty() || component == QLatin1String("."))
            continue;
        if (component != QLatin1String("..")) {
            components.append(component);
        } else if (!components.isEmpty() && components.last() != QLatin1String("..")) {
            components.removeLast();
        } else if (!rooted) {
            // Above the root is the root
            components.append(component);
        }
    }
    const QString result = drive + (rooted ? QStringLiteral("/") : QString()) + components.join(QLatin1Char('/'));
    return result.isEmpty() ? QStringLiteral(".") : result;
}

} // namespace

QString pathJoin(const QString &a, const QString &b, PathRules rules)
{
    if (rules == PathRules::Windows)
        return windowsJoin(a, b);
    if (b.startsWith(QLatin1Char('/')))
        return b;
    if (a.isEmpty() || a.endsWith(QLatin1Char('/')))
        return a + b;
    return a + QLatin1Char('/') + b;
}

QString pathDirname(const QString &path, PathRules rules)
{
    if (rules == PathRules::Windows)
        return windowsDirname(path);
    const qsizetype i = path.lastIndexOf(QLatin1Char('/')) + 1;
    QString head = path.left(i);
    if (!head.isEmpty() && head != QString(head.size(), QLatin1Char('/'))) {
        while (head.endsWith(QLatin1Char('/')))
            head.chop(1);
    }
    return head;
}

QString pathBasename(const QString &path, PathRules rules)
{
    if (rules == PathRules::Windows) {
        const QString rest = splitDrive(path).second;
        return rest.mid(rest.lastIndexOf(QLatin1Char('/')) + 1);
    }
    return path.mid(path.lastIndexOf(QLatin1Char('/')) + 1);
}

QString normpath(const QString &path, PathRules rules)
{
    if (path.isEmpty())
        return QStringLiteral(".");
    if (rules == PathRules::Windows)
        return windowsNormpath(path);
    int initialSlashes = path.startsWith(QLatin1Char('/')) ? 1 : 0;
    // POSIX allows one or two initial slashes, but treats three or more as
    // a single slash.
    if (initialSlashes && path.startsWith(QLatin1String("//")) && !path.startsWith(QLatin1String("///")))
        initialSlashes = 2;
    QStringList components;
    for (const QString &component : path.split(QLatin1Char('/'))) {
        if (component.isEmpty() || component == QLatin1String("."))
            continue;
        if (component != QLatin1String("..") || (!initialSlashes && components.isEmpty())
            || (!components.isEmpty() && components.last() == QLatin1String(".."))) {
            components.append(component);
        } else if (!components.isEmpty()) {
            components.removeLast();
        }
    }
    QString result = QString(initialSlashes, QLatin1Char('/')) + components.join(QLatin1Char('/'));
    return result.isEmpty() ? QStringLiteral(".") : result;
}

bool isAbsolutePath(const QString &path, PathRules rules)
{
    if (rules == PathRules::Windows) {
        // A drive and a root, or a UNC share
        const auto [drive, rest] = splitDrive(path);
        return !drive.isEmpty() && (drive.startsWith(QLatin1String("//")) || rest.startsWith(QLatin1Char('/')));
    }
    return path.startsWith(QLatin1Char('/'));
}

QString abspath(const QString &path, PathRules rules)
{
    if (isAbsolutePath(path, rules))
        return normpath(path, rules);
    return normpath(pathJoin(QDir::currentPath(), path, rules), rules);
}

QString relpath(const QString &path, const QString &start, PathRules rules)
{
    const Qt::CaseSensitivity sensitivity = rules == PathRules::Windows ? Qt::CaseInsensitive : Qt::CaseSensitive;
    const auto parts = [rules](const QString &p) {
        QString rest = abspath(p, rules);
        QString drive;
        if (rules == PathRules::Windows)
            std::tie(drive, rest) = splitDrive(rest);
        QStringList result;
        for (const QString &part : rest.split(QLatin1Char('/'))) {
            if (!part.isEmpty())
                result.append(part);
        }
        return std::make_pair(drive, result);
    };
    const auto [startDrive, startList] = parts(start.isEmpty() ? QStringLiteral(".") : start);
    const auto [pathDrive, pathList] = parts(path);
    if (!sameDrive(startDrive, pathDrive))
        return abspath(path, rules);
    qsizetype common = 0;
    while (common < startList.size() && common < pathList.size()
           && startList.at(common).compare(pathList.at(common), sensitivity) == 0) {
        ++common;
    }
    QStringList relList;
    for (qsizetype i = common; i < startList.size(); ++i)
        relList.append(QStringLiteral(".."));
    relList.append(pathList.mid(common));
    return relList.isEmpty() ? QStringLiteral(".") : relList.join(QLatin1Char('/'));
}

bool pathIsInFolder(const QString &path, const QString &folder, PathRules rules)
{
    const Qt::CaseSensitivity sensitivity = rules == PathRules::Windows ? Qt::CaseInsensitive : Qt::CaseSensitive;
    if (path.compare(folder, sensitivity) == 0)
        return true;
    const QString prefix = folder.endsWith(QLatin1Char('/')) ? folder : folder + QLatin1Char('/');
    return path.startsWith(prefix, sensitivity);
}

QString pathExtension(const QString &path)
{
    const qsizetype sepIndex = path.lastIndexOf(QLatin1Char('/'));
    const qsizetype dotIndex = path.lastIndexOf(QLatin1Char('.'));
    if (dotIndex > sepIndex) {
        // skip all leading dots
        for (qsizetype filenameIndex = sepIndex + 1; filenameIndex < dotIndex; ++filenameIndex) {
            if (path.at(filenameIndex) != QLatin1Char('.'))
                return path.mid(dotIndex);
        }
    }
    return {};
}

QString commonPrefix(const QStringList &paths)
{
    if (paths.isEmpty())
        return {};
    const QString first = *std::min_element(paths.begin(), paths.end());
    const QString last = *std::max_element(paths.begin(), paths.end());
    for (qsizetype i = 0; i < first.size(); ++i) {
        if (first.at(i) != last.at(i))
            return first.left(i);
    }
    return first;
}

InvalidPath::InvalidPath(const QString &vcName, const QString &path, const QString &error)
    : std::runtime_error(QStringLiteral("%1: Path %2 is invalid or not present\nError: %3\n")
                             .arg(vcName, path, error)
                             .toStdString())
    , path_(path)
{
}

// ---------------------------------------------------------------------------
// Plugin

bool Plugin::isInstalled() const
{
    return !QStandardPaths::findExecutable(command()).isEmpty();
}

bool Plugin::checkRepoRoot(const QString &location) const
{
    return QFileInfo(pathJoin(location, vcDir())).isDir();
}

std::pair<QString, QString> Plugin::isInRepo(const QString &path) const
{
    QString root;
    const QString location = QFileInfo(path).isDir() ? path : pathDirname(path);
    if (rootWalk())
        root = findRepoRoot(location);
    else if (checkRepoRoot(location))
        root = location;
    return {root, location};
}

QString Plugin::findRepoRoot(QString location) const
{
    while (!location.isEmpty()) {
        if (checkRepoRoot(location))
            return location;
        const QString old = location;
        location = pathDirname(location);
        if (location == old)
            break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Vc

Vc::Vc(const Plugin &plugin, const QString &path)
    : plugin_(plugin)
{
    std::tie(root_, location_) = plugin.isInRepo(path);
    if (root_.isEmpty())
        throw std::invalid_argument(QStringLiteral("%1 is not in a %2 repository").arg(path, plugin.name()).toStdString());
}

Vc::~Vc() = default;

ProcessResult Vc::run(const QStringList &args, int timeout) const
{
    return runCommand(QStringList{plugin_.command()} + args, location_, timeout);
}

std::optional<QStringList> Vc::filesToCommit(const QStringList &)
{
    return std::nullopt;
}

Actions Vc::validActions(const QHash<QString, int> &pathStates) const
{
    Actions valid;
    const QList<int> states = pathStates.values();
    const auto all = [&states](auto predicate) { return std::all_of(states.begin(), states.end(), predicate); };

    if (!pathStates.isEmpty())
        valid |= Action::Compare;
    valid |= Action::Update;
    // TODO: We can't do this; this shells out for each selection change...
    valid |= Action::Push;

    // TODO: We can't disable this for NORMAL, because folders don't inherit
    // any state from their children, but committing a folder with modified
    // children is expected behaviour.
    if (all([](int s) { return s != StateNone && s != StateIgnored; }))
        valid |= Action::Commit;
    if (all([](int s) { return s != StateNormal && s != StateRemoved; }))
        valid |= Action::Add;
    if (all([](int s) { return s == StateConflict; }))
        valid |= Action::Resolve;
    if (all([](int s) { return s != StateNone && s != StateIgnored && s != StateRemoved; })
        && !pathStates.contains(root_)) {
        valid |= Action::Remove;
    }
    if (all([](int s) { return s != StateNone && s != StateNormal && s != StateIgnored; }))
        valid |= Action::Revert;
    return valid;
}

std::pair<QString, bool> Vc::pathForConflict(const QString &path, Conflict)
{
    throw InvalidPath(name(), path, QStringLiteral("Conflicts are not supported"));
}

std::vector<Revision> Vc::revisions(int) const
{
    return {};
}

std::optional<QString> Vc::verifyRevision(const QString &, QString *error) const
{
    if (error)
        *error = QStringLiteral("%1 can't compare revisions").arg(name());
    return std::nullopt;
}

std::optional<QByteArray> Vc::fileAtRevision(const QString &, const QString &, QString *error) const
{
    if (error)
        *error = QStringLiteral("%1 can't compare revisions").arg(name());
    return std::nullopt;
}

void Vc::setRevisions(const QString &base, const QString &target)
{
    baseRevision_ = base;
    targetRevision_ = target;
}

void Vc::addRevisionTreeEntry(const QString &path, bool isDir)
{
    // With the folders it's in, up to the location
    QString child = path;
    bool childIsDir = isDir;
    while (child != location_ && pathIsInFolder(child, location_)) {
        const QString parent = pathDirname(child);
        QHash<QString, bool> &names = revisionTree_[parent];
        const QString name = pathBasename(child);
        if (names.contains(name)) {
            // Its folders are there already
            if (childIsDir)
                names[name] = true;
            break;
        }
        names.insert(name, childIsDir);
        child = parent;
        childIsDir = true;
    }
}

void Vc::refreshVcState(const QString &path)
{
    // Revisions don't change, so revision comparisons are always refreshed
    // as a whole
    if (path.isEmpty() || comparesRevisions()) {
        treeCache_.clear();
        treeMetaCache_.clear();
        treeMissingCache_.clear();
        revisionTree_.clear();
        updateTreeStateCache(QStringLiteral("./"));
        return;
    }

    // Forget what we knew about everything being refreshed, as files that
    // are no longer reported by the version control system are normal
    const QString scope = abspath(pathJoin(location_, path));
    const auto inScope = [&scope](const QString &key) {
        return pathIsInFolder(key, scope);
    };
    treeCache_.removeIf([&](const auto &it) { return inScope(it.key()); });
    treeMetaCache_.removeIf([&](const auto &it) { return inScope(it.key()); });
    treeMissingCache_.removeIf([&](const auto &it) { return inScope(it.key()); });
    if (auto it = treeMissingCache_.find(pathDirname(scope)); it != treeMissingCache_.end())
        it->remove(pathBasename(scope));
    updateTreeStateCache(path);
}

std::vector<Entry> Vc::entries(const QString &base) const
{
    std::vector<Entry> result;
    const QString folder = abspath(base);

    // Two revisions are compared: their files aren't on disk
    if (comparesRevisions()) {
        const QHash<QString, bool> names = revisionTree_.value(folder);
        for (auto it = names.cbegin(); it != names.cend(); ++it) {
            Entry e;
            e.path = pathJoin(folder, it.key());
            e.name = it.key();
            e.state = treeCache_.value(e.path, StateNormal);
            e.options = treeMetaCache_.value(e.path);
            e.isDir = it.value();
            result.push_back(e);
        }
        return result;
    }

    const QDir dir(folder);
    if (!dir.exists())
        return result;
    if (!dir.isReadable()) {
        qCWarning(lcVc) << "Failed to scan folder" << base << "; permission denied";
        return result;
    }

    const QString vcDir = plugin_.vcDir();
    for (const QFileInfo &info : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                                   QDir::NoSort)) {
        if (info.fileName() == vcDir)
            continue;
        Entry e;
        e.path = pathJoin(folder, info.fileName());
        e.name = info.fileName();
        e.state = treeCache_.value(e.path, StateNormal);
        e.options = treeMetaCache_.value(e.path);
        // Symbolic links aren't followed
        e.isDir = info.isDir() && !info.isSymLink();
        result.push_back(e);
    }

    // Removed entries are not in the filesystem, so must be added here
    for (const QString &name : treeMissingCache_.value(folder)) {
        Entry e;
        e.path = pathJoin(folder, name);
        e.name = name;
        e.state = treeCache_.value(e.path, StateNormal);
        // TODO: Ideally we'd know whether this was a folder or a file.
        // Since it's gone however, only the VC knows, and may or may not
        // tell us.
        e.isDir = false;
        e.options = treeMetaCache_.value(e.path);
        result.push_back(e);
    }
    return result;
}

Entry Vc::entry(const QString &path) const
{
    Entry e;
    const QFileInfo info(path);
    if (comparesRevisions()) {
        const QString absolute = abspath(path);
        e.name = pathBasename(absolute);
        e.isDir = revisionTree_.value(pathDirname(absolute)).value(e.name);
    } else if (info.exists() || info.isSymLink()) {
        e.name = info.fileName();
        e.isDir = info.isDir() && !info.isSymLink();
    } else {
        // Handling for non-existent files (or other IO errors)
        e.name = path;
        e.isDir = false;
    }
    e.path = abspath(path);
    e.state = treeCache_.value(e.path, StateNormal);
    e.options = treeMetaCache_.value(e.path);
    return e;
}

void Vc::addMissingCacheEntry(const QString &path, int state)
{
    if (state == StateRemoved || state == StateMissing)
        treeMissingCache_[pathDirname(path)].insert(pathBasename(path));
}

QString Vc::repoRelativePath(const QString &path) const
{
    // The root may be "/", or a drive's ("C:/") on Windows
    const QString rootPrefix = root_.endsWith(QLatin1Char('/')) ? root_ : root_ + QLatin1Char('/');
    if (!pathIsInFolder(path, root_) || path.size() <= rootPrefix.size())
        throw InvalidPath(name(), path, QStringLiteral("Path not in repository"));
    return path.mid(rootPrefix.size());
}

Command Vc::rootCommand(const QStringList &command, const QStringList &files) const
{
    return Command{command, files, true, root_};
}

QList<Command> Vc::commit(const QStringList &, const QString &) const
{
    return {};
}

QList<Command> Vc::update() const
{
    return {};
}

QList<Command> Vc::push() const
{
    return {};
}

QList<Command> Vc::add(const QStringList &) const
{
    return {};
}

QList<Command> Vc::remove(const QStringList &) const
{
    return {};
}

QList<Command> Vc::revert(const QStringList &) const
{
    return {};
}

QList<Command> Vc::resolve(const QStringList &) const
{
    return {};
}

QList<Command> Vc::unstage(const QStringList &) const
{
    return {};
}

// ---------------------------------------------------------------------------
// The null version control system, used when none is found

namespace {

class NullVc : public Vc {
public:
    using Vc::Vc;

    Actions supportedActions() const override { return {}; }

    QString pathForRepoFile(const QString &path) override
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw InvalidPath(name(), path, file.errorString());
        return writeTempFile(file.readAll(), QStringLiteral("meld-tmp"), pathExtension(path));
    }

protected:
    void updateTreeStateCache(const QString &) override {}
};

class NullPlugin : public Plugin {
public:
    // Translators: This is the displayed name of a version control system
    // when no version control system is actually found.
    QString name() const override { return Tr::tr("None"); }
    QString command() const override { return {}; }
    // Every folder is its own repository root
    QString vcDir() const override { return QStringLiteral("."); }
    bool isInstalled() const override { return true; }
    bool validRepo(const QString &) const override { return true; }
    std::shared_ptr<Vc> create(const QString &path) const override { return std::make_shared<NullVc>(*this, path); }
};

} // namespace

const Plugin &nullPlugin()
{
    static const NullPlugin plugin;
    return plugin;
}

const std::vector<const Plugin *> &plugins()
{
    static const std::vector<const Plugin *> all{
        &gitPlugin(), &mercurialPlugin(), &bazaarPlugin(), &subversionPlugin(), &darcsPlugin(), &cvsPlugin(),
    };
    return all;
}

const Plugin *pluginByName(const QString &name)
{
    for (const Plugin *plugin : plugins()) {
        if (plugin->name() == name)
            return plugin;
    }
    return nullptr;
}

std::vector<AvailableVc> getVcs(const QString &location)
{
    std::vector<AvailableVc> vcs;
    for (const Plugin *plugin : plugins())
        vcs.push_back({plugin, !plugin->isInRepo(location).first.isEmpty()});
    return vcs;
}

} // namespace meld::vc
