// Tests for resolving files with merge conflict markers: reading the markers
// (diff3 and merge style), and the 3-way merges made from them.

#include "conflictcomparison.h"
#include "core/conflictmarkers.h"
#include "core/meldbuffer.h"
#include "core/translations.h"
#include "filediff.h"
#include "meldwindow.h"
#include "msgarea.h"
#include "pathlabel.h"
#include "settings.h"
#include "vcview.h"

#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;
using namespace meld::conflict;

namespace {

//! A diff3-style conflict, between common lines
const QByteArray Diff3 = "common 1\n"
                         "<<<<<<< HEAD\n"
                         "ours\n"
                         "||||||| merged common ancestors\n"
                         "base\n"
                         "=======\n"
                         "theirs\n"
                         ">>>>>>> feature\n"
                         "common 2\n";

//! A merge-style conflict (git's default), without the common ancestor
const QByteArray MergeStyle = "common 1\n"
                              "<<<<<<< HEAD\n"
                              "ours\n"
                              "=======\n"
                              "theirs\n"
                              "more theirs\n"
                              ">>>>>>> feature\n"
                              "common 2\n";

} // namespace

class TestConflicts : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_{QDir::currentPath() + QStringLiteral("/tst_conflicts-XXXXXX")};
    std::unique_ptr<MeldWindow> window_;
    int counter_ = 0;

    QString write(const QByteArray &content, const QString &name = QString())
    {
        const QString path =
            dir_.filePath(name.isEmpty() ? QStringLiteral("conflict%1.txt").arg(++counter_) : name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Couldn't write %s", qPrintable(path));
        file.write(content);
        return path;
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    void settle()
    {
        for (int i = 0; i < 200; ++i) {
            window_->runPendingTasks();
            QCoreApplication::processEvents();
            if (!window_->scheduler().tasksPending())
                break;
        }
    }

    FileDiff *open(const QString &path)
    {
        QString error;
        window_->openPaths({path}, false, false, true, &error);
        settle();
        return qobject_cast<FileDiff *>(window_->docs().value(window_->docs().size() - 1));
    }

    static QString messages(MsgAreaController *area)
    {
        QStringList texts;
        for (const QLabel *label : area->findChildren<QLabel *>())
            texts.append(label->text());
        return texts.join(QLatin1Char('\n'));
    }

    static QPushButton *button(MsgAreaController *area, const QString &text)
    {
        for (QPushButton *b : area->findChildren<QPushButton *>()) {
            if (b->text() == text)
                return b;
        }
        return nullptr;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        MeldSettings::useSettingsFile(dir_.filePath(QStringLiteral("settings.ini")));
        // The English plural forms, as the program has them
        translations::install(QLocale(QLocale::English));
    }

    void init()
    {
        for (const char *key : {"conflict-show-ancestor", "vc-merge-file-order"})
            MeldSettings::instance()->resetValue(QLatin1String(key));
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    }

    void cleanup()
    {
        for (MeldDoc *doc : window_->docs()) {
            if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                for (int pane = 0; pane < filediff->numPanes(); ++pane)
                    filediff->textBuffer(pane)->setModified(false);
            }
        }
        window_.reset();
    }

    // -----------------------------------------------------------------------
    // Markers

    void readsDiff3Markers()
    {
        QString error;
        const std::optional<Versions> versions = parse(Diff3, true, &error);
        QVERIFY2(versions, qPrintable(error));
        QCOMPARE(versions->conflicts, 1);
        QVERIFY(versions->hasAncestor);
        QVERIFY(versions->ancestorIncluded);
        QCOMPARE(versions->local, QByteArray("common 1\nours\ncommon 2\n"));
        QCOMPARE(versions->remote, QByteArray("common 1\ntheirs\ncommon 2\n"));
        QCOMPARE(versions->merged, QByteArray("common 1\nbase\ncommon 2\n"));
        QCOMPARE(versions->localLabel, QStringLiteral("HEAD"));
        QCOMPARE(versions->ancestorLabel, QStringLiteral("merged common ancestors"));
        QCOMPARE(versions->remoteLabel, QStringLiteral("feature"));

        // The ancestor is optional
        const std::optional<Versions> without = parse(Diff3, false);
        QVERIFY(without->hasAncestor);
        QVERIFY(!without->ancestorIncluded);
        QCOMPARE(without->merged, QByteArray("common 1\ncommon 2\n"));
    }

    void readsMergeStyleMarkers()
    {
        const std::optional<Versions> versions = parse(MergeStyle, true);
        QVERIFY(versions);
        QVERIFY(!versions->hasAncestor);
        QVERIFY(!versions->ancestorIncluded);
        QCOMPARE(versions->local, QByteArray("common 1\nours\ncommon 2\n"));
        QCOMPARE(versions->remote, QByteArray("common 1\ntheirs\nmore theirs\ncommon 2\n"));
        QCOMPARE(versions->merged, QByteArray("common 1\ncommon 2\n"));
    }

    void showsTheAncestorOnlyIfEveryConflictHasIt()
    {
        const std::optional<Versions> versions = parse(Diff3 + MergeStyle, true);
        QVERIFY(versions);
        QCOMPARE(versions->conflicts, 2);
        QVERIFY(!versions->hasAncestor);
        QCOMPARE(versions->merged, QByteArray("common 1\ncommon 2\ncommon 1\ncommon 2\n"));
    }

    void keepsLineEndingsAndMarkerSizes()
    {
        QByteArray crlf = Diff3;
        crlf.replace("\n", "\r\n");
        const std::optional<Versions> versions = parse(crlf, true);
        QVERIFY(versions);
        QCOMPARE(versions->local, QByteArray("common 1\r\nours\r\ncommon 2\r\n"));

        // git's conflict-marker-size: shorter markers are text
        const QByteArray longer = "<<<<<<<<< ours\n"
                                  "=======\n"
                                  "|||||||||\n"
                                  "base\n"
                                  "=========\n"
                                  "theirs\n"
                                  ">>>>>>>>> theirs\n";
        const std::optional<Versions> sized = parse(longer, true);
        QVERIFY(sized);
        QCOMPARE(sized->local, QByteArray("=======\n"));
        QCOMPARE(sized->merged, QByteArray("base\n"));
        QCOMPARE(sized->remote, QByteArray("theirs\n"));

        // A file that doesn't end with a newline
        const std::optional<Versions> unterminated = parse("<<<<<<< a\nx\n=======\ny\n>>>>>>> b", true);
        QVERIFY(unterminated);
        QCOMPARE(unterminated->remote, QByteArray("y\n"));
    }

    void ignoresWhatIsntAConflict_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("no markers") << QByteArray("just text\n");
        QTest::newRow("heading underline") << QByteArray("Title\n=======\n\ntext\n");
        QTest::newRow("marker with text") << QByteArray("<<<<<<<x\n=======\n>>>>>>>\n");
        QTest::newRow("short markers") << QByteArray("<<<<<<\nx\n======\ny\n>>>>>>\n");
    }

    void ignoresWhatIsntAConflict()
    {
        QFETCH(QByteArray, data);
        QString error = QStringLiteral("unset");
        QVERIFY(!parse(data, true, &error));
        QCOMPARE(error, QString());
        QCOMPARE(countConflicts(data), 0);
    }

    void reportsIncompleteMarkers()
    {
        QString error;
        QVERIFY(!parse("a\n<<<<<<< HEAD\nours\n=======\ntheirs\n", true, &error));
        QCOMPARE(error, QStringLiteral("The conflict that starts at line 2 doesn’t end."));
        QVERIFY(!parse("<<<<<<< a\nx\n<<<<<<< b\n", true, &error));
        QVERIFY2(error.contains(QStringLiteral("Line 3")), qPrintable(error));
        QCOMPARE(countConflicts("<<<<<<< a\nx\n"), 0);
        QCOMPARE(countConflicts(Diff3 + Diff3), 2);
    }

    // -----------------------------------------------------------------------
    // Merging

    void opensSingleFilesWithConflictsAsMerges()
    {
        const QString path = write(Diff3);
        FileDiff *filediff = open(path);
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 3);
        // Remote, merge, local by default
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("common 1\ntheirs\ncommon 2\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("common 1\nbase\ncommon 2\n"));
        QCOMPARE(filediff->textBuffer(2)->text(), QStringLiteral("common 1\nours\ncommon 2\n"));
        QCOMPARE(filediff->tabTitle(), QStringLiteral("%1 (remote, merge, local)").arg(QFileInfo(path).fileName()));

        // The sides are only viewed; the merge is saved to the file
        QVERIFY(!filediff->textBuffer(0)->data.writable());
        QVERIFY(!filediff->textBuffer(2)->data.writable());
        QCOMPARE(filediff->textBuffer(1)->data.savefile, path);
        QVERIFY(filediff->textBuffer(1)->data.writable());
        QStringList labels;
        for (const PathLabel *label : filediff->findChildren<PathLabel *>())
            labels.append(label->displayLabel());
        const QString name = QFileInfo(path).fileName();
        QVERIFY2(labels.contains(QStringLiteral("%1 — local (HEAD)").arg(name)), qPrintable(labels.join(u',')));
        QVERIFY2(labels.contains(QStringLiteral("%1 — remote (feature)").arg(name)), qPrintable(labels.join(u',')));
        QVERIFY(messages(filediff->messageArea(1)).contains(QStringLiteral("1 unresolved conflict")));
    }

    void followsTheMergeOrderAndAncestorSettings()
    {
        MeldSettings::instance()->setValue(QStringLiteral("vc-merge-file-order"), QStringLiteral("local-merge-remote"));
        MeldSettings::instance()->setValue(QStringLiteral("conflict-show-ancestor"), false);
        FileDiff *filediff = open(write(Diff3));
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("common 1\nours\ncommon 2\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("common 1\ncommon 2\n"));
        QCOMPARE(filediff->textBuffer(2)->text(), QStringLiteral("common 1\ntheirs\ncommon 2\n"));
    }

    void mergesMergeStyleConflicts()
    {
        FileDiff *filediff = open(write(MergeStyle));
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 3);
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("common 1\ncommon 2\n"));
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("common 1\ntheirs\nmore theirs\ncommon 2\n"));
    }

    void warnsBeforeSavingUnresolvedConflicts()
    {
        const QString path = write(MergeStyle);
        FileDiff *filediff = open(path);
        QVERIFY(filediff);

        filediff->saveFile(1);
        settle();
        QCOMPARE(read(path), MergeStyle);
        QVERIFY2(messages(filediff->messageArea(1)).contains(QStringLiteral("A conflict isn’t resolved")),
                 qPrintable(messages(filediff->messageArea(1))));
        QPushButton *dontSave = button(filediff->messageArea(1), QStringLiteral("Don’t Save"));
        QVERIFY(dontSave);
        dontSave->click();
        settle();
        QCOMPARE(read(path), MergeStyle);

        // Resolved, it's saved without asking
        filediff->textBuffer(1)->replaceAllText(QStringLiteral("common 1\nours\ntheirs\ncommon 2\n"));
        settle();
        filediff->saveFile(1);
        settle();
        QCOMPARE(read(path), QByteArray("common 1\nours\ntheirs\ncommon 2\n"));
    }

    void savesUnresolvedConflictsIfAsked()
    {
        const QString path = write(Diff3);
        FileDiff *filediff = open(path);
        QVERIFY(filediff);
        filediff->saveFile(1);
        settle();
        QPushButton *saveAnyway = button(filediff->messageArea(1), QStringLiteral("Save Anyway"));
        QVERIFY(saveAnyway);
        saveAnyway->click();
        settle();
        QCOMPARE(read(path), QByteArray("common 1\nbase\ncommon 2\n"));
    }

    void offersToMergeFilesWithConflicts()
    {
        const QString conflicted = write(Diff3);
        const QString other = write("common 1\ncommon 2\n");
        QString error;
        auto *filediff = qobject_cast<FileDiff *>(window_->openPaths({conflicted, other}, false, false, true, &error));
        QVERIFY(filediff);
        settle();
        QVERIFY2(messages(filediff->messageArea(0)).contains(QStringLiteral("has an unresolved merge conflict")),
                 qPrintable(messages(filediff->messageArea(0))));
        QVERIFY(!filediff->messageArea(1)->hasMessage());
        QPushButton *resolve = button(filediff->messageArea(0), QStringLiteral("Resolve"));
        QVERIFY(resolve);
        resolve->click();
        settle();
        auto *merge = qobject_cast<FileDiff *>(window_->docs().last());
        QVERIFY(merge && merge != filediff);
        QCOMPARE(merge->numPanes(), 3);
        QCOMPARE(merge->textBuffer(1)->data.savefile, conflicted);
    }

    void countsConflictsInTheOffer()
    {
        const QString conflicted = write(Diff3 + MergeStyle);
        QString error;
        auto *filediff = qobject_cast<FileDiff *>(
            window_->openPaths({conflicted, write("x\n")}, false, false, true, &error));
        QVERIFY(filediff);
        settle();
        const QString expected =
            QStringLiteral("%1 has 2 unresolved merge conflicts.").arg(QFileInfo(conflicted).fileName());
        QVERIFY2(messages(filediff->messageArea(0)).contains(expected), qPrintable(messages(filediff->messageArea(0))));
    }

    void prefersGitsVersionsOfConflicts()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed");
        QTemporaryDir repo;
        const QString root = repo.path();
        const auto git = [&root](const QStringList &args) {
            QProcess process;
            process.setWorkingDirectory(root);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("GIT_CONFIG_NOSYSTEM"), QStringLiteral("1"));
            env.insert(QStringLiteral("GIT_CONFIG_GLOBAL"), QStringLiteral("/dev/null"));
            process.setProcessEnvironment(env);
            process.start(QStringLiteral("git"), QStringList{"-c", "user.name=t", "-c", "user.email=t@t", "-c",
                                                             "commit.gpgsign=false", "-c", "init.defaultBranch=main"}
                                                     + args);
            process.waitForFinished(30000);
        };
        const QString file = root + QStringLiteral("/a.txt");
        const auto writeIn = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        git({"init", "-q"});
        writeIn(file, "1\nbase\n3\n");
        git({"add", "."});
        git({"commit", "-q", "-m", "base"});
        git({"checkout", "-q", "-b", "theirs"});
        writeIn(file, "1\ntheirs\n3\n");
        git({"commit", "-q", "-am", "theirs"});
        git({"checkout", "-q", "main"});
        writeIn(file, "1\nours\n3\n");
        git({"commit", "-q", "-am", "ours"});
        git({"merge", "-q", "theirs"});
        QVERIFY(conflictsInFile(file) > 0);

        // Git has the whole sides of its conflict, which are compared
        FileDiff *filediff = open(file);
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 3);
        QVERIFY(!filediff->textBuffer(0)->data.file.isEmpty());
        QCOMPARE(filediff->textBuffer(1)->data.savefile, file);

        // Once it isn't a conflict for git, the markers are used
        git({"add", "a.txt"});
        filediff = open(file);
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 3);
        QVERIFY(filediff->textBuffer(0)->data.file.isEmpty());
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("1\n3\n"));
        cleanupVcTempFiles();
    }

    void opensResolvedFilesAsTheyAre()
    {
        // After saving the merge, reverting the comparison reads the file
        const QString path = write(Diff3);
        FileDiff *filediff = open(path);
        QVERIFY(filediff);
        write("resolved\n", QFileInfo(path).fileName());
        filediff->setFiles({QString(), QString(), QString()});
        settle();
        for (int pane = 0; pane < 3; ++pane)
            QCOMPARE(filediff->textBuffer(pane)->text(), QStringLiteral("resolved\n"));
    }
};

QTEST_MAIN(TestConflicts)
#include "tst_conflicts.moc"
