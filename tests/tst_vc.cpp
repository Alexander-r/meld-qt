// Tests for the version control library (src/core/vc)
//
// The original has no tests for meld/vc, so these are new: differential
// tests against the Python plugins run on canned command output (see
// tools/generate_vc_reference_data.py), and end-to-end tests of the git
// plugin on real repositories.

#include "core/misc.h"
#include "core/translations.h"
#include "core/vc/vc.h"
#include "core/vc/vcutil.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;
using namespace meld::vc;

namespace {

const QJsonObject &vcReference()
{
    static const QJsonObject data = [] {
        QFile file(QStringLiteral(MELD_TEST_DATA_DIR "/vc_reference.json"));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Couldn't open reference data %s", qPrintable(file.fileName()));
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}

QStringList toStringList(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &v : value.toArray())
        result.append(v.toString());
    return result;
}

const char *actionName(Action action)
{
    switch (action) {
    case Action::Compare:
        return "compare";
    case Action::Update:
        return "update";
    case Action::Push:
        return "push";
    case Action::Commit:
        return "commit";
    case Action::Add:
        return "add";
    case Action::Resolve:
        return "resolve";
    case Action::Remove:
        return "remove";
    case Action::Revert:
        return "revert";
    case Action::Unstage:
        return "unstage";
    }
    return "?";
}

QStringList actionNames(Actions actions)
{
    QStringList names;
    for (Action action : {Action::Compare, Action::Update, Action::Push, Action::Commit, Action::Add, Action::Resolve,
                          Action::Remove, Action::Revert, Action::Unstage}) {
        if (actions.testFlag(action))
            names.append(QLatin1String(actionName(action)));
    }
    names.sort();
    return names;
}

void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    file.write(contents);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

//! Restores the default command runner when going out of scope
struct RunnerGuard {
    ~RunnerGuard() { setCommandRunner({}); }
};

//! Runs version control commands to completion, as the view's sync runner
int runCommands(const QList<Command> &commands, QString *output = nullptr)
{
    int status = 0;
    for (const Command &c : commands) {
        QStringList files;
        for (const QString &f : c.files)
            files.append(relpath(f, c.workingDir));
        QProcess process;
        process.setWorkingDirectory(c.workingDir);
        process.start(c.command.first(), c.command.mid(1) + files);
        process.waitForFinished(30000);
        if (output)
            *output += QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
        status = status ? status : process.exitCode();
    }
    return status;
}

} // namespace

class TestVc : public QObject {
    Q_OBJECT

private:
    QTemporaryDir home_;
    bool haveGit_ = false;

    QString git(const QStringList &args, const QString &cwd, int *exitCode = nullptr)
    {
        QProcess process;
        process.setWorkingDirectory(cwd);
        process.start(QStringLiteral("git"), args);
        process.waitForFinished(30000);
        if (exitCode)
            *exitCode = process.exitCode();
        else if (process.exitCode() != 0)
            qWarning() << "git" << args << "failed:" << process.readAllStandardError();
        return QString::fromUtf8(process.readAllStandardOutput());
    }

    //! A repository with one commit of a.txt, b.txt and dir/c.txt
    QString makeRepo(const QTemporaryDir &dir)
    {
        const QString root = dir.path();
        git({QStringLiteral("init"), QStringLiteral("-q")}, root);
        writeFile(root + "/a.txt", "a\n");
        writeFile(root + "/b.txt", "b\n");
        writeFile(root + "/dir/c.txt", "c\n");
        writeFile(root + "/.gitignore", "*.log\nbuild/\n");
        git({QStringLiteral("add"), QStringLiteral(".")}, root);
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("Initial")}, root);
        return root;
    }

    std::shared_ptr<Vc> gitVc(const QString &path)
    {
        auto vc = gitPlugin().create(path);
        vc->refreshVcState();
        return vc;
    }

    static const Plugin &gitPlugin() { return *pluginByName(QStringLiteral("Git")); }

    static QHash<QString, int> states(const Vc &vc, const QString &folder)
    {
        QHash<QString, int> result;
        for (const Entry &e : vc.entries(folder))
            result.insert(e.name, e.state);
        return result;
    }

private Q_SLOTS:
    void initTestCase()
    {
        // The English plural forms, as the program has them
        translations::install(QLocale(QLocale::English));
        QVERIFY(home_.isValid());
        haveGit_ = !QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty();
        // Isolate git from the user's configuration
        const QString config = home_.filePath(QStringLiteral("gitconfig"));
        writeFile(config, "[user]\n\tname = Meld Test\n\temail = meld@example.com\n"
                          "[init]\n\tdefaultBranch = main\n[commit]\n\tgpgsign = false\n"
                          "[merge]\n\tconflictstyle = merge\n");
        qputenv("GIT_CONFIG_GLOBAL", config.toLocal8Bit());
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("HOME", home_.path().toLocal8Bit());
        qputenv("LC_ALL", "C");
    }

    // -----------------------------------------------------------------------
    // Differential tests against the original

    void entryPredicates()
    {
        for (const QJsonValue &value : vcReference().value(QStringLiteral("entries")).toArray()) {
            const QJsonObject c = value.toObject();
            Entry e;
            e.state = c.value(QStringLiteral("state")).toInt();
            e.isDir = c.value(QStringLiteral("isdir")).toBool();
            const QByteArray where = QByteArray::number(e.state) + (e.isDir ? " dir" : " file");
            QVERIFY2(Entry::isModified(e) == c.value(QStringLiteral("modified")).toBool(), where.constData());
            QVERIFY2(Entry::isNormal(e) == c.value(QStringLiteral("normal")).toBool(), where.constData());
            QVERIFY2(Entry::isNonVc(e) == c.value(QStringLiteral("nonvc")).toBool(), where.constData());
            QVERIFY2(Entry::isIgnored(e) == c.value(QStringLiteral("ignored")).toBool(), where.constData());
            QVERIFY2(e.isPresent() == c.value(QStringLiteral("present")).toBool(), where.constData());
            QCOMPARE(e.status(), c.value(QStringLiteral("status")).toString());
        }
    }

    void validActions()
    {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral(".git"))));
        const std::shared_ptr<Vc> base = nullPlugin().create(dir.path());
        const std::shared_ptr<Vc> gitVc = gitPlugin().create(dir.path());
        QCOMPARE(base->root(), dir.path());
        QCOMPARE(gitVc->root(), dir.path());

        int count = 0;
        for (const QJsonValue &value : vcReference().value(QStringLiteral("valid_actions")).toArray()) {
            const QJsonObject c = value.toObject();
            QHash<QString, int> pathStates;
            for (const QJsonValue &pair : c.value(QStringLiteral("path_states")).toArray()) {
                QString path = pair.toArray().at(0).toString();
                path.replace(QLatin1String("/repo"), dir.path());
                pathStates.insert(path, pair.toArray().at(1).toInt());
            }
            const Vc &vc = c.value(QStringLiteral("plugin")).toString() == QLatin1String("Git") ? *gitVc : *base;
            QCOMPARE(actionNames(vc.validActions(pathStates)), toStringList(c.value(QStringLiteral("actions"))));
            ++count;
        }
        QVERIFY(count > 100);
    }

    void baseFromDiff3()
    {
        for (const QJsonValue &value : vcReference().value(QStringLiteral("base_from_diff3")).toArray()) {
            const QJsonObject c = value.toObject();
            const QByteArray input = c.value(QStringLiteral("input")).toString().toLatin1();
            const QByteArray expected = c.value(QStringLiteral("output")).toString().toLatin1();
            QCOMPARE(vc::baseFromDiff3(input), expected);
        }
    }

    void textWrap()
    {
        int count = 0;
        for (const QJsonValue &value : vcReference().value(QStringLiteral("textwrap")).toArray()) {
            const QJsonObject c = value.toObject();
            const QString text = c.value(QStringLiteral("text")).toString();
            const int width = c.value(QStringLiteral("width")).toInt();
            const QString filled = textWrapFill(text, width);
            if (filled != c.value(QStringLiteral("fill")).toString())
                qWarning().noquote() << "Wrapping" << text << "at" << width;
            QCOMPARE(filled, c.value(QStringLiteral("fill")).toString());
            ++count;
        }
        QVERIFY(count > 100);
    }

    void status_data()
    {
        QTest::addColumn<QJsonObject>("testCase");
        for (const QJsonValue &value : vcReference().value(QStringLiteral("status")).toArray()) {
            const QJsonObject c = value.toObject();
            QTest::newRow(qPrintable(c.value(QStringLiteral("name")).toString())) << c;
        }
    }

    void status()
    {
        QFETCH(QJsonObject, testCase);
        QTemporaryDir dir;
        const QString root = dir.path();
        const auto subst = [&root](QString text) { return text.replace(QLatin1String("@ROOT@"), root); };
        const auto unroot = [&root](QString text) { return text.replace(root, QLatin1String("@ROOT@")); };

        for (const QString &d : toStringList(testCase.value(QStringLiteral("dirs"))))
            QVERIFY(QDir().mkpath(dir.filePath(d)));
        for (const QString &f : toStringList(testCase.value(QStringLiteral("files"))))
            writeFile(dir.filePath(f), "content\n");

        const QJsonArray outputs = testCase.value(QStringLiteral("outputs")).toArray();
        QJsonArray commands;
        RunnerGuard guard;
        setCommandRunner([&](const QStringList &command, const QString &workingDir, int) {
            QJsonArray recorded;
            for (const QString &c : command)
                recorded.append(unroot(c));
            commands.append(QJsonObject{{QStringLiteral("command"), recorded}, {QStringLiteral("cwd"), unroot(workingDir)}});
            ProcessResult result;
            result.started = true;
            result.exitCode = 0;
            for (const QJsonValue &o : outputs) {
                QStringList expected = toStringList(o.toObject().value(QStringLiteral("command")));
                for (QString &e : expected)
                    e = subst(e);
                if (expected == command) {
                    result.output = subst(o.toObject().value(QStringLiteral("output")).toString()).toUtf8();
                    break;
                }
            }
            return result;
        });

        const Plugin *plugin = pluginByName(testCase.value(QStringLiteral("plugin")).toString());
        QVERIFY(plugin);
        const QString location = subst(testCase.value(QStringLiteral("location")).toString());
        const std::shared_ptr<Vc> vc = plugin->create(location);
        const QJsonValue refresh = testCase.value(QStringLiteral("refresh"));
        vc->refreshVcState(refresh.isString() ? subst(refresh.toString()) : QString());

        const QJsonObject expected = testCase.value(QStringLiteral("expected")).toObject();
        QCOMPARE(commands, expected.value(QStringLiteral("commands")).toArray());

        QJsonObject treeCache;
        for (auto it = vc->treeCache().cbegin(); it != vc->treeCache().cend(); ++it)
            treeCache.insert(unroot(it.key()), it.value());
        QCOMPARE(treeCache, expected.value(QStringLiteral("tree_cache")).toObject());

        QJsonObject metaCache;
        for (auto it = vc->treeMetaCache().cbegin(); it != vc->treeMetaCache().cend(); ++it)
            metaCache.insert(unroot(it.key()), it.value());
        QCOMPARE(metaCache, expected.value(QStringLiteral("meta_cache")).toObject());

        QJsonObject missingCache;
        for (auto it = vc->treeMissingCache().cbegin(); it != vc->treeMissingCache().cend(); ++it) {
            if (it.value().isEmpty())
                continue;
            QStringList names(it.value().begin(), it.value().end());
            names.sort();
            missingCache.insert(unroot(it.key()), QJsonArray::fromStringList(names));
        }
        QCOMPARE(missingCache, expected.value(QStringLiteral("missing_cache")).toObject());

        QList<QJsonArray> entries;
        for (const Entry &e : vc->entries(location))
            entries.append(QJsonArray{unroot(e.name), e.state, e.isDir, e.options});
        std::sort(entries.begin(), entries.end(), [](const QJsonArray &a, const QJsonArray &b) {
            return a.at(0).toString() < b.at(0).toString();
        });
        QJsonArray entriesJson;
        for (const QJsonArray &e : entries)
            entriesJson.append(e);
        QCOMPARE(entriesJson, expected.value(QStringLiteral("entries")).toArray());
    }

    // -----------------------------------------------------------------------
    // Helpers with the semantics of Python's os.path

    void windowsPathHelpers_data()
    {
        // Expected results are Python's ntpath's, with '/' separators (and
        // drive letters in upper case)
        QTest::addColumn<QString>("function");
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<QString>("expected");
        const auto row = [](const char *function, const QStringList &arguments, const QString &expected) {
            QTest::addRow("%s(%s)", function, qPrintable(arguments.join(QStringLiteral(", "))))
                << QString::fromLatin1(function) << arguments << expected;
        };
        row("normpath", {"C:/a/b/../c"}, "C:/a/c");
        row("normpath", {"C:\\a\\.\\b\\"}, "C:/a/b");
        row("normpath", {"C:/../a"}, "C:/a");
        row("normpath", {"C:"}, "C:");
        row("normpath", {"C:a/../.."}, "C:..");
        row("normpath", {"c:/x"}, "C:/x");
        row("normpath", {"//server/share/x/../y"}, "//server/share/y");
        row("normpath", {"//server/share"}, "//server/share");
        row("normpath", {"a/../../b"}, "../b");
        row("normpath", {"/a/b"}, "/a/b");
        row("pathJoin", {"C:/a", "b"}, "C:/a/b");
        row("pathJoin", {"C:/a/", "b"}, "C:/a/b");
        row("pathJoin", {"C:/a", "/b"}, "C:/b");
        row("pathJoin", {"C:/a", "D:/b"}, "D:/b");
        row("pathJoin", {"C:/a", "D:b"}, "D:b");
        row("pathJoin", {"C:", "b"}, "C:b");
        row("pathJoin", {"C:/a", "C:b"}, "C:/a/b");
        row("pathJoin", {"//srv/share", "x"}, "//srv/share/x");
        row("pathDirname", {"C:/a/b"}, "C:/a");
        row("pathDirname", {"C:/a"}, "C:/");
        row("pathDirname", {"C:/"}, "C:/");
        row("pathDirname", {"//srv/share/x"}, "//srv/share/");
        row("pathDirname", {"C:a"}, "C:");
        row("pathDirname", {"a\\b"}, "a");
        row("pathBasename", {"C:/a/b.txt"}, "b.txt");
        row("pathBasename", {"C:\\a\\b"}, "b");
        row("pathBasename", {"C:"}, "");
        row("isAbsolutePath", {"C:/a"}, "true");
        row("isAbsolutePath", {"C:\\a"}, "true");
        row("isAbsolutePath", {"//srv/share"}, "true");
        row("isAbsolutePath", {"C:a"}, "false");
        row("isAbsolutePath", {"/a"}, "false");
        row("isAbsolutePath", {"a"}, "false");
        row("abspath", {"C:/a/./b"}, "C:/a/b");
        row("abspath", {"C:\\x\\..\\y"}, "C:/y");
        row("relpath", {"C:/a/b/c", "C:/a"}, "b/c");
        row("relpath", {"c:/A/B", "C:/a"}, "B");
        row("relpath", {"C:/a", "C:/a/c/d"}, "../..");
        row("relpath", {"C:/a", "C:/a"}, ".");
        // ntpath fails; there's no relative path to another drive
        row("relpath", {"D:/x", "C:/a"}, "D:/x");
        row("pathIsInFolder", {"C:/Repo/a", "C:/repo"}, "true");
        row("pathIsInFolder", {"C:/repo", "C:/repo"}, "true");
        row("pathIsInFolder", {"C:/x", "C:/"}, "true");
        row("pathIsInFolder", {"C:/repo2", "C:/repo"}, "false");
        row("pathIsInFolder", {"D:/repo/a", "C:/repo"}, "false");
    }

    void windowsPathHelpers()
    {
        QFETCH(QString, function);
        QFETCH(QStringList, arguments);
        QFETCH(QString, expected);
        constexpr PathRules w = PathRules::Windows;
        const QString a = arguments.value(0);
        const QString b = arguments.value(1);
        const auto boolean = [](bool value) { return value ? QStringLiteral("true") : QStringLiteral("false"); };
        QString actual;
        if (function == QLatin1String("normpath"))
            actual = normpath(a, w);
        else if (function == QLatin1String("pathJoin"))
            actual = pathJoin(a, b, w);
        else if (function == QLatin1String("pathDirname"))
            actual = pathDirname(a, w);
        else if (function == QLatin1String("pathBasename"))
            actual = pathBasename(a, w);
        else if (function == QLatin1String("isAbsolutePath"))
            actual = boolean(isAbsolutePath(a, w));
        else if (function == QLatin1String("abspath"))
            actual = abspath(a, w);
        else if (function == QLatin1String("relpath"))
            actual = relpath(a, b, w);
        else if (function == QLatin1String("pathIsInFolder"))
            actual = boolean(pathIsInFolder(a, b, w));
        QCOMPARE(actual, expected);
    }

    void windowsRepositoryRoots()
    {
        // Walking up from a folder ends at its drive's root
        QString location = QStringLiteral("C:/work/repo/sub");
        QStringList seen;
        for (int i = 0; i < 10; ++i) {
            seen.append(location);
            const QString parent = pathDirname(location, PathRules::Windows);
            if (parent == location)
                break;
            location = parent;
        }
        QCOMPARE(seen, (QStringList{"C:/work/repo/sub", "C:/work/repo", "C:/work", "C:/"}));
    }

    void pathHelpers()
    {
        QVERIFY(isAbsolutePath(QStringLiteral("/a"), PathRules::Posix));
        QVERIFY(!isAbsolutePath(QStringLiteral("C:/a"), PathRules::Posix));
        QVERIFY(pathIsInFolder(QStringLiteral("/a/b"), QStringLiteral("/a"), PathRules::Posix));
        QVERIFY(pathIsInFolder(QStringLiteral("/x"), QStringLiteral("/"), PathRules::Posix));
        QVERIFY(!pathIsInFolder(QStringLiteral("/ab"), QStringLiteral("/a"), PathRules::Posix));
        QVERIFY(!pathIsInFolder(QStringLiteral("/A/b"), QStringLiteral("/a"), PathRules::Posix));
        QCOMPARE(normpath(QString()), QStringLiteral("."));
        QCOMPARE(normpath(QStringLiteral("./a/../b//c/")), QStringLiteral("b/c"));
        QCOMPARE(normpath(QStringLiteral("/../a")), QStringLiteral("/a"));
        QCOMPARE(normpath(QStringLiteral("../a")), QStringLiteral("../a"));
        QCOMPARE(normpath(QStringLiteral("//a")), QStringLiteral("//a"));
        QCOMPARE(normpath(QStringLiteral("///a")), QStringLiteral("/a"));
        QCOMPARE(pathDirname(QStringLiteral("/a/b")), QStringLiteral("/a"));
        QCOMPARE(pathDirname(QStringLiteral("/a")), QStringLiteral("/"));
        QCOMPARE(pathDirname(QStringLiteral("a")), QString());
        QCOMPARE(pathDirname(QStringLiteral("a//b")), QStringLiteral("a"));
        QCOMPARE(pathJoin(QStringLiteral("/a"), QStringLiteral("b")), QStringLiteral("/a/b"));
        QCOMPARE(pathJoin(QStringLiteral("/a/"), QStringLiteral("b")), QStringLiteral("/a/b"));
        QCOMPARE(pathJoin(QStringLiteral("/a"), QStringLiteral("/b")), QStringLiteral("/b"));
        QCOMPARE(relpath(QStringLiteral("/a/b/c"), QStringLiteral("/a")), QStringLiteral("b/c"));
        QCOMPARE(relpath(QStringLiteral("/a/b"), QStringLiteral("/a/c/d")), QStringLiteral("../../b"));
        QCOMPARE(relpath(QStringLiteral("/a"), QStringLiteral("/a")), QStringLiteral("."));
        QCOMPARE(pathExtension(QStringLiteral("/a/b.txt")), QStringLiteral(".txt"));
        QCOMPARE(pathExtension(QStringLiteral("/a/.bashrc")), QString());
        QCOMPARE(pathExtension(QStringLiteral("/a/..b.c")), QStringLiteral(".c"));
        QCOMPARE(pathExtension(QStringLiteral("/a.d/b")), QString());
        QCOMPARE(commonPrefix({QStringLiteral("/a/bc"), QStringLiteral("/a/bd")}), QStringLiteral("/a/b"));
        QCOMPARE(commonPrefix({}), QString());
        QCOMPARE(unescapeCString(u"caf\\303\\251 \\\"q\\\"\\t\\\\"), QByteArray("caf\xc3\xa9 \"q\"\t\\"));
        QCOMPARE(splitLines(QStringLiteral("a\r\nb\rc\n\nd")),
                 (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QString(),
                              QStringLiteral("d")}));
        QCOMPARE(outputLines(QStringLiteral("a\nb\n")), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(outputLines(QStringLiteral("a\nb")), QStringList{QStringLiteral("a")});
    }

    void repositoryDetection()
    {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("repo/.hg"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("repo/sub/.git"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("repo/sub/deeper"))));
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("repo/sub/deeper/CVS"))));
        const QString deeper = dir.filePath(QStringLiteral("repo/sub/deeper"));

        QStringList enabled;
        for (const AvailableVc &available : getVcs(deeper)) {
            if (available.enabled)
                enabled.append(available.plugin->name());
        }
        QCOMPARE(enabled, (QStringList{QStringLiteral("Git"), QStringLiteral("Mercurial"), QStringLiteral("CVS")}));

        const auto [gitRoot, gitLocation] = gitPlugin().isInRepo(deeper);
        QCOMPARE(gitRoot, dir.filePath(QStringLiteral("repo/sub")));
        QCOMPARE(gitLocation, deeper);
        QCOMPARE(pluginByName(QStringLiteral("Mercurial"))->isInRepo(deeper).first, dir.filePath(QStringLiteral("repo")));
        // CVS doesn't walk up to find the root
        QCOMPARE(pluginByName(QStringLiteral("CVS"))->isInRepo(deeper).first, deeper);
        QCOMPARE(pluginByName(QStringLiteral("CVS"))->isInRepo(dir.filePath(QStringLiteral("repo/sub"))).first,
                 QString());

        // Files are compared at their folder
        writeFile(deeper + "/file.txt", "x");
        QCOMPARE(gitPlugin().isInRepo(deeper + "/file.txt").second, deeper);

        // .git can be a file, as in worktrees and submodules
        writeFile(dir.filePath(QStringLiteral("worktree/.git")), "gitdir: elsewhere\n");
        QCOMPARE(gitPlugin().isInRepo(dir.filePath(QStringLiteral("worktree"))).first,
                 dir.filePath(QStringLiteral("worktree")));

        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, pluginByName(QStringLiteral("Darcs"))->create(deeper));

        // The fallback treats every folder as its own repository
        const std::shared_ptr<Vc> null = nullPlugin().create(deeper);
        QCOMPARE(null->root(), deeper);
        QCOMPARE(null->supportedActions(), Actions());
        const QString copy = null->pathForRepoFile(deeper + "/file.txt");
        QVERIFY(copy.endsWith(QLatin1String(".txt")));
        QCOMPARE(readFile(copy), QByteArray("x"));
        QFile::remove(copy);
    }

    void pluginCommands()
    {
        QTemporaryDir dir;
        const QString root = dir.path();
        for (const char *vcDir : {".git", ".hg", ".svn", ".bzr", "_darcs", "CVS"})
            QVERIFY(QDir().mkpath(root + QLatin1Char('/') + QLatin1String(vcDir)));
        writeFile(root + "/a.txt", "a");
        writeFile(root + "/sub/b.txt", "b");
        const QString a = root + "/a.txt";
        const QString b = root + "/sub/b.txt";
        const QString gone = root + "/gone.txt";
        const auto vcFor = [&root](const char *name) { return pluginByName(QLatin1String(name))->create(root); };
        const auto cmd = [&root](QStringList command, QStringList files = {}, QString cwd = QString()) {
            return Command{command, files, true, cwd.isEmpty() ? root : cwd};
        };

        const auto git = vcFor("Git");
        QCOMPARE(git->revert({a, gone}), (QList<Command>{cmd({"git", "checkout"}, {a}),
                                                         cmd({"git", "checkout", "HEAD"}, {gone})}));
        QCOMPARE(git->unstage({a}), QList<Command>{cmd({"git", "restore", "--staged"}, {a})});
        QCOMPARE(git->remove({a}), QList<Command>{cmd({"git", "rm", "-r"}, {a})});
        QCOMPARE(git->commit({a}, "msg"), QList<Command>{cmd({"git", "commit", "-m", "msg"}, {a})});

        const auto hg = vcFor("Mercurial");
        QCOMPARE(hg->update(), QList<Command>{cmd({"hg", "pull", "-u"})});
        QVERIFY(!hg->supportedActions().testFlag(Action::Push));
        QVERIFY(hg->push().isEmpty());

        const auto svn = vcFor("Subversion");
        QCOMPARE(svn->add({root + "/sub", a}), (QList<Command>{cmd({"svn", "add"}, {root + "/sub"}, root),
                                                              cmd({"svn", "add"}, {a})}));
        QCOMPARE(svn->resolve({a}), QList<Command>{cmd({"svn", "resolve", "--accept=working"}, {a})});

        // Bazaar passes files as part of the command, so they stay absolute
        const auto bzr = vcFor("Bazaar");
        QCOMPARE(bzr->revert({a}), QList<Command>{cmd({"bzr", "--no-aliases", "--no-plugins", "revert", a})});
        QCOMPARE(bzr->add({a}), QList<Command>{cmd({"bzr", "--no-aliases", "--no-plugins", "add"}, {a})});
        QCOMPARE(bzr->commit({a}, "m"), QList<Command>{cmd({"bzr", "--no-aliases", "--no-plugins", "commit", "-m", "m"})});

        const auto darcs = vcFor("Darcs");
        QCOMPARE(darcs->commit({a}, "m"), QList<Command>{cmd({"darcs", "record", "-a", "-m", "m"})});

        // CVS adds files with all their parent folders
        const auto cvs = vcFor("CVS");
        QCOMPARE(cvs->add({b, root + "/sub"}), QList<Command>{cmd({"cvs", "add"}, {root + "/sub", b})});
        QVERIFY(cvs->resolve({a}).isEmpty());

        // Repository files are fetched with each system's cat command
        QStringList commands;
        RunnerGuard guard;
        setCommandRunner([&](const QStringList &command, const QString &workingDir, int) {
            commands.append(command.join(QLatin1Char(' ')) + QStringLiteral(" (in ") + workingDir + QLatin1Char(')'));
            ProcessResult result;
            result.started = true;
            result.exitCode = 0;
            result.output = "repository contents";
            return result;
        });
        for (const char *name : {"Git", "Mercurial", "Subversion", "Bazaar", "Darcs", "CVS"}) {
            const QString temp = vcFor(name)->pathForRepoFile(b);
            QVERIFY(temp.endsWith(QLatin1String(".txt")));
            QCOMPARE(readFile(temp), QByteArray("repository contents"));
            QFile::remove(temp);
        }
        const QString in = QStringLiteral(" (in ") + root + QLatin1Char(')');
        QCOMPARE(commands, (QStringList{
                               "git cat-file blob HEAD:sub/b.txt" + in,
                               "hg cat sub/b.txt" + in,
                               "svn cat -r BASE sub/b.txt" + in,
                               "bzr cat sub/b.txt" + in,
                               "darcs show contents sub/b.txt" + in,
                               "cvs -q update -p sub/b.txt" + in,
                           }));
        QVERIFY_THROWS_EXCEPTION(InvalidPath, vcFor("Git")->pathForRepoFile(QStringLiteral("/elsewhere/x.txt")));

        // Conflict sides
        QCOMPARE(bzr->pathForConflict(a, ConflictBase), std::make_pair(a + ".BASE", false));
        QCOMPARE(bzr->pathForConflict(a, ConflictMerged), std::make_pair(a, false));
        writeFile(a + ".mine", "mine");
        writeFile(a + ".r12", "new");
        writeFile(a + ".r9", "old");
        QCOMPARE(svn->pathForConflict(a, ConflictMerged), std::make_pair(a, false));
        QCOMPARE(svn->pathForConflict(a, ConflictBase), std::make_pair(a + ".mine", false));
        QCOMPARE(svn->pathForConflict(a, ConflictThis), std::make_pair(a + ".r9", false));
        QCOMPARE(svn->pathForConflict(a, ConflictOther), std::make_pair(a + ".r12", false));
        QVERIFY_THROWS_EXCEPTION(InvalidPath, svn->pathForConflict(b, ConflictBase));
        QVERIFY(!hg->hasConflictSupport());
    }

    // -----------------------------------------------------------------------
    // The git plugin on real repositories

    void gitStates()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);

        writeFile(root + "/a.txt", "a changed\n");
        writeFile(root + "/new.txt", "new\n");
        git({QStringLiteral("add"), QStringLiteral("new.txt")}, root);
        QFile::remove(root + "/b.txt");
        writeFile(root + "/untracked.txt", "?\n");
        writeFile(root + "/debug.log", "ignored\n");
        writeFile(root + "/build/out.o", "ignored\n");
        writeFile(root + "/dir/c.txt", "c staged\n");
        git({QStringLiteral("add"), QStringLiteral("dir/c.txt")}, root);
        writeFile(root + "/dir/c.txt", "c staged and changed\n");

        const std::shared_ptr<Vc> vc = gitVc(root);
        QCOMPARE(vc->name(), QStringLiteral("Git"));
        const QHash<QString, int> top = states(*vc, root);
        QCOMPARE(top.value("a.txt"), int(StateModified));
        QCOMPARE(top.value("new.txt"), int(StateNew));
        QCOMPARE(top.value("b.txt"), int(StateRemoved));
        QCOMPARE(top.value("untracked.txt"), int(StateNone));
        QCOMPARE(top.value("debug.log"), int(StateIgnored));
        QCOMPARE(top.value("build"), int(StateIgnored));
        QCOMPARE(top.value(".gitignore"), int(StateNormal));
        QCOMPARE(top.value("dir"), int(StateNormal));
        QVERIFY(!top.contains(".git"));
        QCOMPARE(states(*vc, root + "/dir").value("c.txt"), int(StateModified));

        // The removed file is only known to git
        const Entry removed = vc->entry(root + "/b.txt");
        QCOMPARE(removed.state, int(StateRemoved));
        QVERIFY(!removed.isPresent());
        QCOMPARE(vc->entry(root + "/new.txt").options, QStringLiteral("Mode changed from 000000 to 100644; Staged"));
        QCOMPARE(vc->entry(root + "/dir/c.txt").options, QStringLiteral("Partially staged"));
        QCOMPARE(vc->entry(root + "/a.txt").options, QString());

        // Files to commit list changed files in folders
        QCOMPARE(*vc->filesToCommit({root}),
                 (QStringList{QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("dir/c.txt"),
                              QStringLiteral("new.txt")}));
        QCOMPARE(*vc->filesToCommit({root + "/a.txt"}), QStringList{QStringLiteral("a.txt")});

        // The repository version of a modified file
        const QString repoFile = vc->pathForRepoFile(root + "/a.txt");
        QCOMPARE(readFile(repoFile), QByteArray("a\n"));
        QFile::remove(repoFile);

        // Valid actions for selections
        QVERIFY(vc->validActions({{root + "/new.txt", StateNew}}).testFlag(Action::Unstage));
        QVERIFY(!vc->validActions({{root + "/untracked.txt", StateNone}}).testFlag(Action::Commit));
        QVERIFY(vc->validActions({{root + "/untracked.txt", StateNone}}).testFlag(Action::Add));
        QVERIFY(!vc->validActions({{root, StateNormal}}).testFlag(Action::Remove));
    }

    void gitPartialRefresh()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        writeFile(root + "/a.txt", "a changed\n");
        writeFile(root + "/dir/c.txt", "c changed\n");
        git({QStringLiteral("add"), QStringLiteral("dir/c.txt")}, root);
        const std::shared_ptr<Vc> vc = gitVc(root);
        QCOMPARE(vc->entry(root + "/a.txt").state, int(StateModified));
        QCOMPARE(vc->entry(root + "/dir/c.txt").options, QStringLiteral("Staged"));

        // Reverting a file outside of the view, then refreshing it alone
        writeFile(root + "/a.txt", "a\n");
        vc->refreshVcState(root + "/a.txt");
        QCOMPARE(vc->entry(root + "/a.txt").state, int(StateNormal));

        // Unstaging and refreshing a folder clears the stale staged state
        git({QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("dir/c.txt")}, root);
        vc->refreshVcState(root + "/dir");
        QCOMPARE(vc->entry(root + "/dir/c.txt").state, int(StateModified));
        QCOMPARE(vc->entry(root + "/dir/c.txt").options, QString());

        // Committing everything, then refreshing the root without clearing
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("More")}, root);
        vc->refreshVcState(root);
        QCOMPARE(vc->entry(root + "/dir/c.txt").state, int(StateNormal));

        // A file that becomes untracked, refreshed on its own
        writeFile(root + "/fresh.txt", "new\n");
        vc->refreshVcState(root + "/fresh.txt");
        QCOMPARE(vc->entry(root + "/fresh.txt").state, int(StateNone));
    }

    void gitSubfolder()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        writeFile(root + "/dir/c.txt", "c changed\n");
        writeFile(root + "/a.txt", "a changed\n");

        // A location inside the repository limits the comparison
        const std::shared_ptr<Vc> vc = gitVc(root + "/dir");
        QCOMPARE(vc->root(), root);
        QCOMPARE(vc->location(), root + "/dir");
        QCOMPARE(states(*vc, root + "/dir").value("c.txt"), int(StateModified));
        QVERIFY(!vc->treeCache().contains(root + "/a.txt"));
        const QString repoFile = vc->pathForRepoFile(root + "/dir/c.txt");
        QCOMPARE(readFile(repoFile), QByteArray("c\n"));
        QFile::remove(repoFile);
    }

    void gitUnicodeNames()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        const QString name = QStringLiteral("café \"quoted\".txt");
        writeFile(root + QLatin1Char('/') + name, "1\n");
        git({QStringLiteral("add"), QStringLiteral(".")}, root);
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("Unicode")}, root);
        writeFile(root + QLatin1Char('/') + name, "2\n");
        writeFile(root + QStringLiteral("/ünïcode untracked.txt"), "?\n");

        const std::shared_ptr<Vc> vc = gitVc(root);
        const QHash<QString, int> top = states(*vc, root);
        QCOMPARE(top.value(name), int(StateModified));
        QCOMPARE(top.value(QStringLiteral("ünïcode untracked.txt")), int(StateNone));
    }

    void gitCommands()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        std::shared_ptr<Vc> vc = gitVc(root);

        writeFile(root + "/new.txt", "new\n");
        QCOMPARE(runCommands(vc->add({root + "/new.txt"})), 0);
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/new.txt").state, int(StateNew));

        QCOMPARE(runCommands(vc->unstage({root + "/new.txt"})), 0);
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/new.txt").state, int(StateNone));

        writeFile(root + "/a.txt", "a changed\n");
        QFile::remove(root + "/b.txt");
        QCOMPARE(runCommands(vc->revert({root + "/a.txt", root + "/b.txt"})), 0);
        QCOMPARE(readFile(root + "/a.txt"), QByteArray("a\n"));
        QCOMPARE(readFile(root + "/b.txt"), QByteArray("b\n"));

        QCOMPARE(runCommands(vc->remove({root + "/dir"})), 0);
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/dir/c.txt").state, int(StateRemoved));
        QVERIFY(vc->treeMissingCache().value(root + "/dir").contains(QStringLiteral("c.txt")));

        QCOMPARE(runCommands(vc->add({root + "/new.txt"})), 0);
        QCOMPARE(runCommands(vc->commit({root + "/new.txt", root + "/dir"}, QStringLiteral("Second commit"))), 0);
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/new.txt").state, int(StateNormal));
        QVERIFY(!QFileInfo::exists(root + "/dir"));
        QCOMPARE(git({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%s")}, root),
                 QStringLiteral("Second commit\n"));
    }

    void gitPushSummary()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        std::shared_ptr<Vc> vc = gitVc(root);
        // No upstream configured
        QCOMPARE(vc->commitsToPushSummary(), std::optional<QString>(QString()));

        const QString remote = dir.filePath(QStringLiteral("remote.git"));
        git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("--bare"), remote}, root);
        git({QStringLiteral("remote"), QStringLiteral("add"), QStringLiteral("origin"), remote}, root);
        git({QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("-u"), QStringLiteral("origin"),
             QStringLiteral("main")},
            root);
        QCOMPARE(vc->commitsToPushSummary(), std::optional<QString>(QString()));

        writeFile(root + "/a.txt", "1\n");
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("One")}, root);
        QCOMPARE(vc->commitsToPushSummary(), std::optional<QString>(QStringLiteral("1 unpushed commit")));
        writeFile(root + "/a.txt", "2\n");
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("Two")}, root);
        QCOMPARE(vc->commitsToPushSummary(), std::optional<QString>(QStringLiteral("2 unpushed commits")));

        git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("other")}, root);
        git({QStringLiteral("push"), QStringLiteral("-q"), QStringLiteral("-u"), QStringLiteral("origin"),
             QStringLiteral("other")},
            root);
        writeFile(root + "/a.txt", "3\n");
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("Three")}, root);
        QCOMPARE(vc->commitsToPushSummary(), std::optional<QString>(QStringLiteral("3 unpushed commits in 2 branches")));
    }

    void gitConflicts()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        writeFile(root + "/a.txt", "1\nbase\n3\nshared\n");
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("Base")}, root);
        git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("theirs")}, root);
        writeFile(root + "/a.txt", "1\ntheirs\n3\nshared\n");
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("Theirs")}, root);
        git({QStringLiteral("checkout"), QStringLiteral("-q"), QStringLiteral("main")}, root);
        writeFile(root + "/a.txt", "1\nours\n3\nshared\n");
        git({QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-am"), QStringLiteral("Ours")}, root);
        int exitCode = 0;
        git({QStringLiteral("merge"), QStringLiteral("theirs")}, root, &exitCode);
        QVERIFY(exitCode != 0);

        const std::shared_ptr<Vc> vc = gitVc(root);
        QCOMPARE(vc->entry(root + "/a.txt").state, int(StateConflict));
        QVERIFY(vc->validActions({{root + "/a.txt", StateConflict}}).testFlag(Action::Resolve));
        QVERIFY(vc->hasConflictSupport());

        const auto [local, localTemp] = vc->pathForConflict(root + "/a.txt", ConflictLocal);
        const auto [base, baseTemp] = vc->pathForConflict(root + "/a.txt", ConflictBase);
        const auto [remote, remoteTemp] = vc->pathForConflict(root + "/a.txt", ConflictRemote);
        QVERIFY(localTemp && baseTemp && remoteTemp);
        QVERIFY(QFileInfo(local).fileName().startsWith(QLatin1String("meld-tmp-Local")));
        QCOMPARE(readFile(local), QByteArray("1\nours\n3\nshared\n"));
        QCOMPARE(readFile(base), QByteArray("1\nbase\n3\nshared\n"));
        QCOMPARE(readFile(remote), QByteArray("1\ntheirs\n3\nshared\n"));

        // The merged file has the base text where there are conflicts
        const auto [merged, mergedTemp] = vc->pathForConflict(root + "/a.txt", ConflictMerged);
        QVERIFY(mergedTemp);
        QVERIFY(QFileInfo(merged).fileName().startsWith(QLatin1String("meld-tmp-0-")));
        QVERIFY(merged.endsWith(QLatin1String(".txt")));
        QCOMPARE(readFile(merged), QByteArray("1\n==== BASE ====\nbase\n==== BASE ====\n3\nshared\n"));

        // The commit message is prefilled from the merge, without comments
        // (the blank line before git's comments is kept, as in the original)
        QCOMPARE(vc->commitMessagePrefill(), QStringLiteral("Merge branch 'theirs'\n"));

        // Resolving marks the conflict as resolved
        writeFile(root + "/a.txt", "1\nresolved\n3\nshared\n");
        QCOMPARE(runCommands(vc->resolve({root + "/a.txt"})), 0);
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/a.txt").state, int(StateModified));
        for (const QString &temp : {local, base, remote, merged})
            QFile::remove(temp);
    }

    void gitValidRepository()
    {
        if (!haveGit_)
            QSKIP("git is not installed");
        QVERIFY(gitPlugin().isInstalled());
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        QVERIFY(gitPlugin().validRepo(root));
        QTemporaryDir broken;
        QVERIFY(QDir().mkpath(broken.filePath(QStringLiteral(".git"))));
        QVERIFY(!gitPlugin().validRepo(broken.path()));
    }
};

QTEST_GUILESS_MAIN(TestVc)
#include "tst_vc.moc"
