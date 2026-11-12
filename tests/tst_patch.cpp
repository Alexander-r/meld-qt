// Tests for comparing files and folders with patches applied: reading
// patches, applying them in memory, patched folders, and the comparisons.

#include "core/archive.h"
#include "core/patch.h"
#include "core/translations.h"
#include "dirdiff.h"
#include "dirdifftree.h"
#include "filediff.h"
#include "meldapp.h"
#include "meldwindow.h"
#include "msgarea.h"
#include "newdifftab.h"
#include "pathlabel.h"
#include "settings.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;
using namespace meld::patch;

namespace {

const QByteArray Original = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n";

//! A unified diff of Original, changing "five" to "FIVE"
const QByteArray FivePatch = "--- a/numbers.txt\t2024-01-01 00:00:00.000000000 +0000\n"
                             "+++ b/numbers.txt\t2024-01-02 00:00:00.000000000 +0000\n"
                             "@@ -2,7 +2,7 @@\n"
                             " two\n"
                             " three\n"
                             " four\n"
                             "-five\n"
                             "+FIVE\n"
                             " six\n"
                             " seven\n"
                             " eight\n";

FilePatch parseOne(const QByteArray &data)
{
    QString error;
    const std::optional<Patch> patch = parse(data, &error);
    if (!patch || patch->files.size() != 1)
        qFatal("Couldn't parse test patch: %s", qPrintable(error));
    return patch->files.front();
}

QByteArray applied(const QByteArray &original, const QByteArray &patchText, QString *error = nullptr,
                   QStringList *notes = nullptr)
{
    const ApplyResult result = apply(original, parseOne(patchText));
    if (error)
        *error = result.error;
    if (notes)
        *notes = result.notes;
    return result.text.value_or(QByteArray("<failed>"));
}

} // namespace

class TestPatch : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_{QDir::currentPath() + QStringLiteral("/tst_patch-XXXXXX")};
    std::unique_ptr<MeldWindow> window_;
    int counter_ = 0;

    QString write(const QString &relative, const QByteArray &content)
    {
        const QString path = dir_.filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Couldn't write %s", qPrintable(path));
        file.write(content);
        return path;
    }

    //! A folder for patching: changed.txt, same.txt, gone.txt, old.txt and
    //! sub/inner.txt
    QString makeFolder()
    {
        const QString name = QStringLiteral("folder%1").arg(++counter_);
        write(name + QStringLiteral("/changed.txt"), Original);
        write(name + QStringLiteral("/same.txt"), "same\n");
        write(name + QStringLiteral("/gone.txt"), "gone\n");
        write(name + QStringLiteral("/old.txt"), "a\nb\nc\n");
        write(name + QStringLiteral("/sub/inner.txt"), "inner\n");
        return dir_.filePath(name);
    }

    //! A git patch for makeFolder()'s folders
    static QByteArray folderPatch()
    {
        return "From 1234 Mon Sep 17 00:00:00 2001\n"
               "Subject: [PATCH] Change things\n"
               "\n"
               "A commit message.\n"
               "---\n"
               "diff --git a/changed.txt b/changed.txt\n"
               "index 1111111..2222222 100644\n"
               + QByteArray(FivePatch).replace("numbers.txt", "changed.txt") +
               "diff --git a/gone.txt b/gone.txt\n"
               "deleted file mode 100644\n"
               "index 3333333..0000000\n"
               "--- a/gone.txt\n"
               "+++ /dev/null\n"
               "@@ -1 +0,0 @@\n"
               "-gone\n"
               "diff --git a/new/added.txt b/new/added.txt\n"
               "new file mode 100644\n"
               "index 0000000..4444444\n"
               "--- /dev/null\n"
               "+++ b/new/added.txt\n"
               "@@ -0,0 +1,2 @@\n"
               "+added\n"
               "+lines\n"
               "diff --git a/old.txt b/renamed.txt\n"
               "similarity index 80%\n"
               "rename from old.txt\n"
               "rename to renamed.txt\n"
               "index 5555555..6666666 100644\n"
               "--- a/old.txt\n"
               "+++ b/renamed.txt\n"
               "@@ -1,3 +1,3 @@\n"
               " a\n"
               "-b\n"
               "+B\n"
               " c\n"
               "diff --git a/missing.txt b/missing.txt\n"
               "--- a/missing.txt\n"
               "+++ b/missing.txt\n"
               "@@ -1 +1 @@\n"
               "-x\n"
               "+y\n"
               "-- \n"
               "2.45.0\n";
    }

    void settle()
    {
        for (int i = 0; i < 100; ++i) {
            window_->runPendingTasks();
            QCoreApplication::processEvents();
            if (!window_->scheduler().tasksPending())
                break;
        }
    }

    static TreeNode *find(const DirDiff *doc, const QString &relative)
    {
        const DirDiffTreeModel *model = doc->model();
        const QStringList roots = model->valuePaths(model->firstRoot());
        for (TreeNode *node = model->firstRoot(); node; node = model->inorderNext(node)) {
            for (int pane = 0; pane < model->ntree(); ++pane) {
                if (model->valuePath(node, pane) == roots.value(pane) + QLatin1Char('/') + relative)
                    return node;
            }
        }
        return nullptr;
    }

    static QList<int> states(const DirDiff *doc, const QString &relative)
    {
        QList<int> result;
        if (TreeNode *node = find(doc, relative)) {
            for (int pane = 0; pane < doc->model()->ntree(); ++pane)
                result.append(doc->model()->state(node, pane));
        }
        return result;
    }

    //! The texts of the rows in a pane
    static QStringList paneTexts(const DirDiff *doc, int pane)
    {
        QStringList texts;
        const DirDiffTreeModel *model = doc->model();
        for (TreeNode *node = model->firstRoot(); node; node = model->inorderNext(node))
            texts.append(node->panes[size_t(pane)].text);
        return texts;
    }

    static QString messages(MsgAreaController *area)
    {
        QStringList texts;
        for (const QLabel *label : area->findChildren<QLabel *>())
            texts.append(label->text());
        return texts.join(QLatin1Char('\n'));
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
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
    }

    void cleanup()
    {
        window_.reset();
    }

    // -----------------------------------------------------------------------
    // Reading

    void parsesUnifiedDiffs()
    {
        const FilePatch file = parseOne(FivePatch);
        QCOMPARE(file.oldPath, QStringLiteral("a/numbers.txt"));
        QCOMPARE(file.newPath, QStringLiteral("b/numbers.txt"));
        QVERIFY(!file.newFile);
        QVERIFY(!file.deletedFile);
        QCOMPARE(file.hunks.size(), size_t(1));
        const Hunk &hunk = file.hunks.front();
        QCOMPARE(hunk.oldStart, 2);
        QCOMPARE(hunk.oldCount, 7);
        QCOMPARE(hunk.newStart, 2);
        QCOMPARE(hunk.newCount, 7);
        QCOMPARE(hunk.lines.size(), size_t(8));
        QCOMPARE(hunk.lines[3].kind, '-');
        QCOMPARE(hunk.lines[3].text, QByteArray("five\n"));
        QCOMPARE(hunk.lines[4].kind, '+');
    }

    void parsesGitPatches()
    {
        QString error;
        const std::optional<Patch> patch = parse(folderPatch(), &error);
        QVERIFY2(patch, qPrintable(error));
        QCOMPARE(patch->files.size(), size_t(5));
        QCOMPARE(patch->files[0].targetPath(), QStringLiteral("b/changed.txt"));
        QVERIFY(patch->files[1].deletedFile);
        QCOMPARE(patch->files[1].oldPath, QStringLiteral("a/gone.txt"));
        QVERIFY(patch->files[1].newPath.isEmpty());
        QVERIFY(patch->files[2].newFile);
        QCOMPARE(patch->files[2].newPath, QStringLiteral("b/new/added.txt"));
        QVERIFY(patch->files[3].rename);
        QCOMPARE(patch->files[3].oldPath, QStringLiteral("a/old.txt"));
        QCOMPARE(patch->files[3].newPath, QStringLiteral("b/renamed.txt"));
        // The signature after the last hunk isn't part of it
        QCOMPARE(patch->files[4].hunks.front().lines.size(), size_t(2));
    }

    void parsesGitHeadersWithoutHunks()
    {
        QString error;
        const std::optional<Patch> patch = parse("diff --git a/x.txt b/y.txt\n"
                                                 "similarity index 100%\n"
                                                 "rename from x.txt\n"
                                                 "rename to y.txt\n"
                                                 "diff --git a/mode.sh b/mode.sh\n"
                                                 "old mode 100644\n"
                                                 "new mode 100755\n"
                                                 "diff --git a/image.png b/image.png\n"
                                                 "index 1..2 100644\n"
                                                 "Binary files a/image.png and b/image.png differ\n"
                                                 "diff --git \"a/caf\\303\\251 x.txt\" \"b/caf\\303\\251 x.txt\"\n"
                                                 "new file mode 100644\n"
                                                 "--- /dev/null\n"
                                                 "+++ \"b/caf\\303\\251 x.txt\"\n"
                                                 "@@ -0,0 +1 @@\n"
                                                 "+x\n",
                                                 &error);
        QVERIFY2(patch, qPrintable(error));
        // Mode changes change nothing here
        QCOMPARE(patch->files.size(), size_t(3));
        QVERIFY(patch->files[0].rename);
        QCOMPARE(patch->files[0].oldPath, QStringLiteral("a/x.txt"));
        QCOMPARE(patch->files[0].newPath, QStringLiteral("b/y.txt"));
        QVERIFY(patch->files[1].binary);
        QCOMPARE(patch->files[2].newPath, QStringLiteral("b/café x.txt"));
    }

    void parsesNoNewlineMarkers()
    {
        const FilePatch file = parseOne("--- a\n+++ b\n@@ -1,2 +1,2 @@\n x\n-y\n\\ No newline at end of file\n+z\n"
                                        "\\ No newline at end of file\n");
        const Hunk &hunk = file.hunks.front();
        QCOMPARE(hunk.lines.size(), size_t(3));
        QCOMPARE(hunk.lines[1].text, QByteArray("y"));
        QCOMPARE(hunk.lines[2].text, QByteArray("z"));
    }

    void parsesContextLinesWithoutTheirSpace()
    {
        // Editors that strip trailing whitespace leave empty context lines
        const FilePatch file = parseOne("--- a\n+++ b\n@@ -1,3 +1,3 @@\n x\n\n-y\n+z\n");
        QCOMPARE(file.hunks.front().lines[1].kind, ' ');
        QCOMPARE(file.hunks.front().lines[1].text, QByteArray("\n"));
    }

    void rejectsWhatIsntAPatch_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<QString>("error");
        QTest::newRow("text") << QByteArray("Just some text\n") << QStringLiteral("No changes were found");
        QTest::newRow("context diff") << QByteArray("*** a\n--- b\n***************\n*** 1 ****\n! x\n--- 1 ----\n! y\n")
                                      << QStringLiteral("context diff");
        QTest::newRow("truncated") << QByteArray("--- a\n+++ b\n@@ -1,3 +1,3 @@\n x\n-y\n")
                                   << QStringLiteral("Line 3 of the patch: the patch ends in the middle of a hunk");
        QTest::newRow("bad hunk line") << QByteArray("--- a\n+++ b\n@@ -1,2 +1,2 @@\n x\n?y\n")
                                       << QStringLiteral("Line 5 of the patch: unexpected line");
        QTest::newRow("bad header") << QByteArray("--- a\n+++ b\n@@ -x +y @@\n")
                                    << QStringLiteral("malformed hunk header");
    }

    void rejectsWhatIsntAPatch()
    {
        QFETCH(QByteArray, data);
        QFETCH(QString, error);
        QString message;
        QVERIFY(!parse(data, &message));
        QVERIFY2(message.contains(error), qPrintable(message));
    }

    // -----------------------------------------------------------------------
    // Applying

    void appliesExactly()
    {
        QStringList notes;
        QCOMPARE(applied(Original, FivePatch, nullptr, &notes), QByteArray(Original).replace("five", "FIVE"));
        QCOMPARE(notes, QStringList());
    }

    void appliesAtAnOffset()
    {
        const QByteArray original = "zero\nzero\nzero\n" + Original;
        QStringList notes;
        QCOMPARE(applied(original, FivePatch, nullptr, &notes), QByteArray(original).replace("five", "FIVE"));
        QCOMPARE(notes, QStringList{QStringLiteral("Hunk #1 applied at line 5 (offset 3 lines).")});
    }

    void appliesWithFuzz()
    {
        // The first and last context lines don't match
        const QByteArray original = QByteArray(Original).replace("two", "TWO").replace("eight", "EIGHT");
        QStringList notes;
        QString error;
        QCOMPARE(applied(original, FivePatch, &error, &notes), QByteArray(original).replace("five", "FIVE"));
        QCOMPARE(notes, QStringList{QStringLiteral("Hunk #1 applied at line 2 with fuzz 1.")});
    }

    void reportsHunksThatDontApply()
    {
        const QByteArray original = QByteArray(Original).replace("five", "5").replace("four", "4");
        QString error;
        QCOMPARE(applied(original, FivePatch, &error), QByteArray("<failed>"));
        QVERIFY2(error.contains(QStringLiteral("1 of 1 hunk can’t be applied")), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("Hunk #1 doesn’t match the file at line 2")), qPrintable(error));
    }

    void reportsPatchesThatAreAppliedAlready()
    {
        QString error;
        QCOMPARE(applied(QByteArray(Original).replace("five", "FIVE"), FivePatch, &error), QByteArray("<failed>"));
        QVERIFY2(error.contains(QLatin1String("applied already")), qPrintable(error));
    }

    void appliesSeveralHunksInOrder()
    {
        const QByteArray patch = "--- a\n+++ b\n"
                                 "@@ -1,2 +1,3 @@\n one\n+one and a half\n two\n"
                                 "@@ -9,2 +10,1 @@\n nine\n-ten\n";
        QCOMPARE(applied(Original, patch), QByteArray("one\none and a half\ntwo\nthree\nfour\nfive\nsix\nseven\n"
                                                      "eight\nnine\n"));
    }

    void appliesHunksWithoutContext()
    {
        // diff -U0
        const QByteArray patch = "--- a\n+++ b\n@@ -3,0 +4 @@\n+inserted\n@@ -5 +6 @@\n-five\n+FIVE\n";
        QCOMPARE(applied(Original, patch), QByteArray("one\ntwo\nthree\ninserted\nfour\nFIVE\nsix\nseven\neight\n"
                                                      "nine\nten\n"));
    }

    void keepsTheFilesLineEndings()
    {
        // A patch with LF line endings, for a file with CRLF ones
        const QByteArray original = QByteArray(Original).replace("\n", "\r\n");
        QCOMPARE(applied(original, FivePatch), QByteArray(original).replace("five", "FIVE"));
        // ...and the other way around
        QCOMPARE(applied(Original, QByteArray(FivePatch).replace("\n", "\r\n")),
                 QByteArray(Original).replace("five", "FIVE"));
    }

    void changesLineEndingsThatThePatchChanges()
    {
        const QByteArray patch = "--- a\n+++ b\n@@ -1,2 +1,2 @@\n-x\r\n-y\r\n+x\n+y\n";
        QCOMPARE(applied("x\r\ny\r\n", patch), QByteArray("x\ny\n"));
    }

    void handlesMissingNewlines()
    {
        QCOMPARE(applied("x\ny", "--- a\n+++ b\n@@ -1,2 +1,2 @@\n x\n-y\n\\ No newline at end of file\n+y\n"),
                 QByteArray("x\ny\n"));
        QCOMPARE(applied("x\ny\n", "--- a\n+++ b\n@@ -1,2 +1,2 @@\n x\n-y\n+z\n\\ No newline at end of file\n"),
                 QByteArray("x\nz"));
    }

    void createsAndDeletesFiles()
    {
        const QByteArray create = "--- /dev/null\n+++ b/x\n@@ -0,0 +1,2 @@\n+a\n+b\n";
        QCOMPARE(applied(QByteArray(), create), QByteArray("a\nb\n"));
        QString error;
        QCOMPARE(applied("other\n", create, &error), QByteArray("<failed>"));
        QVERIFY2(error.contains(QLatin1String("already exists")), qPrintable(error));

        const ApplyResult deleted = apply("a\nb\n", parseOne("--- a/x\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-a\n-b\n"));
        QVERIFY(deleted.text);
        QVERIFY(deleted.deleted);
        QCOMPARE(*deleted.text, QByteArray());
        QVERIFY(!apply("a\nb\nc\n", parseOne("--- a/x\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-a\n-b\n")).text);
    }

    void doesntApplyBinaryChanges()
    {
        QString error;
        const std::optional<Patch> patch =
            parse("diff --git a/i.png b/i.png\nBinary files a/i.png and b/i.png differ\n", &error);
        QVERIFY(patch);
        const ApplyResult result = apply("x", patch->files.front());
        QVERIFY(!result.text);
        QVERIFY(result.error.contains(QLatin1String("Binary")));
    }

    void readsWindowsPaths()
    {
        // Paths from Windows diff tools, with backslashes
        const FilePatch windows = parseOne("--- old\\src\\numbers.txt\r\n+++ new\\src\\numbers.txt\r\n"
                                           "@@ -1 +1 @@\r\n-one\r\n+ONE\r\n");
        QCOMPARE(windows.oldPath, QStringLiteral("old/src/numbers.txt"));
        QCOMPARE(windows.newPath, QStringLiteral("new/src/numbers.txt"));

        // Elsewhere, a backslash can be part of a name, as in git's paths
        const FilePatch named = parseOne("--- a/back\\slash.txt\n+++ b/back\\slash.txt\n@@ -1 +1 @@\n-x\n+y\n");
        QCOMPARE(named.oldPath, QStringLiteral("a/back\\slash.txt"));

        // A folder is patched with them
        write(QStringLiteral("windows/src/numbers.txt"), Original);
        const QString patchFile = write(QStringLiteral("windows.patch"), "--- old\\src\\numbers.txt\n"
                                                                         "+++ new\\src\\numbers.txt\n"
                                                                         "@@ -1 +1 @@\n-one\n+ONE\n");
        QString error;
        const std::shared_ptr<PatchedFolder> patched =
            PatchedFolder::load(dir_.filePath(QStringLiteral("windows")), patchFile, &error);
        QVERIFY2(patched, qPrintable(error));
        QCOMPARE(patched->stripLevel(), 1);
        QCOMPARE(patched->change(QStringLiteral("src/numbers.txt")), PatchedFolder::Change::Modified);
        QVERIFY(patched->content(QStringLiteral("src/numbers.txt")).text->startsWith("ONE\n"));
    }

    void stripsPaths()
    {
        QCOMPARE(stripPath(QStringLiteral("a/src/x.c"), 0), std::optional<QString>(QStringLiteral("a/src/x.c")));
        QCOMPARE(stripPath(QStringLiteral("a/src/x.c"), 1), std::optional<QString>(QStringLiteral("src/x.c")));
        QCOMPARE(stripPath(QStringLiteral("./src//x.c"), 0), std::optional<QString>(QStringLiteral("src/x.c")));
        QCOMPARE(stripPath(QStringLiteral("x.c"), 1), std::optional<QString>());
    }

    void findsAFilesChanges()
    {
        QString error;
        const std::optional<Patch> patch = parse(folderPatch(), &error);
        QVERIFY(patch);
        const FilePatch *changed = findFilePatch(*patch, QStringLiteral("/somewhere/changed.txt"), &error);
        QVERIFY(changed);
        QCOMPARE(changed->targetPath(), QStringLiteral("b/changed.txt"));
        QVERIFY(!findFilePatch(*patch, QStringLiteral("/somewhere/unknown.txt"), &error));
        QVERIFY2(error.contains(QStringLiteral("doesn’t change unknown.txt")), qPrintable(error));
        // A patch of one file applies to any file
        const std::optional<Patch> single = parse(FivePatch, &error);
        QCOMPARE(findFilePatch(*single, QStringLiteral("/x/other.txt"), &error), &single->files.front());
    }

    // -----------------------------------------------------------------------
    // Patched folders

    void patchedFolders()
    {
        const QString folder = makeFolder();
        const QString patchFile = write(QStringLiteral("folder.patch"), folderPatch());
        QString error;
        const std::shared_ptr<PatchedFolder> patched = PatchedFolder::load(folder, patchFile, &error);
        QVERIFY2(patched, qPrintable(error));
        QCOMPARE(patched->stripLevel(), 1);

        using Change = PatchedFolder::Change;
        QCOMPARE(patched->change(QStringLiteral("changed.txt")), Change::Modified);
        QCOMPARE(patched->change(QStringLiteral("same.txt")), Change::None);
        QCOMPARE(patched->change(QStringLiteral("gone.txt")), Change::Deleted);
        QCOMPARE(patched->change(QStringLiteral("new/added.txt")), Change::Added);
        QCOMPARE(patched->change(QStringLiteral("old.txt")), Change::Deleted);
        QCOMPARE(patched->change(QStringLiteral("renamed.txt")), Change::Added);
        QCOMPARE(patched->source(QStringLiteral("renamed.txt")), QStringLiteral("old.txt"));

        QVERIFY(patched->exists(QString()));
        QVERIFY(patched->isDir(QStringLiteral("new")));
        QVERIFY(patched->isDir(QStringLiteral("sub")));
        QVERIFY(patched->isFile(QStringLiteral("sub/inner.txt")));
        QVERIFY(!patched->exists(QStringLiteral("gone.txt")));
        QVERIFY(!patched->exists(QStringLiteral("missing.txt")));

        QStringList entries = patched->entries(QString());
        entries.sort();
        QCOMPARE(entries, (QStringList{QStringLiteral("changed.txt"), QStringLiteral("new"),
                                       QStringLiteral("renamed.txt"), QStringLiteral("same.txt"),
                                       QStringLiteral("sub")}));
        QCOMPARE(patched->entries(QStringLiteral("new")), QStringList{QStringLiteral("added.txt")});
        const auto errors = patched->errors(QString());
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().first, QStringLiteral("missing.txt"));

        QCOMPARE(patched->content(QStringLiteral("changed.txt")).text,
                 std::optional<QByteArray>(QByteArray(Original).replace("five", "FIVE")));
        QCOMPARE(patched->content(QStringLiteral("same.txt")).text, std::optional<QByteArray>("same\n"));
        QCOMPARE(patched->content(QStringLiteral("new/added.txt")).text, std::optional<QByteArray>("added\nlines\n"));
        QCOMPARE(patched->content(QStringLiteral("renamed.txt")).text, std::optional<QByteArray>("a\nB\nc\n"));
        QVERIFY(patched->content(QStringLiteral("gone.txt")).deleted);

        // Files are only read as they're asked for
        write(QFileInfo(folder).fileName() + QStringLiteral("/changed.txt"), "something else\n");
        QVERIFY(!patched->content(QStringLiteral("changed.txt")).text);
    }

    void patchedFoldersFindTheirStripLevel()
    {
        const QString folder = makeFolder();
        // diff -ru between differently named folders
        const QString patchFile = write(QStringLiteral("strip.patch"),
                                        "diff -ru old-version/sub/inner.txt new-version/sub/inner.txt\n"
                                        "--- old-version/sub/inner.txt\n"
                                        "+++ new-version/sub/inner.txt\n"
                                        "@@ -1 +1 @@\n"
                                        "-inner\n"
                                        "+INNER\n"
                                        "Only in new-version: extra\n");
        QString error;
        const std::shared_ptr<PatchedFolder> patched = PatchedFolder::load(folder, patchFile, &error);
        QVERIFY2(patched, qPrintable(error));
        QCOMPARE(patched->stripLevel(), 1);
        QCOMPARE(patched->content(QStringLiteral("sub/inner.txt")).text, std::optional<QByteArray>("INNER\n"));
    }

    void patchedFoldersStayInTheirFolder()
    {
        const QString folder = makeFolder();
        const QString patchFile = write(QStringLiteral("unsafe.patch"), "--- /dev/null\n"
                                                                        "+++ ../escaped.txt\n"
                                                                        "@@ -0,0 +1 @@\n"
                                                                        "+x\n");
        QString error;
        const std::shared_ptr<PatchedFolder> patched = PatchedFolder::load(folder, patchFile, &error);
        QVERIFY2(patched, qPrintable(error));
        QVERIFY(!patched->exists(QStringLiteral("../escaped.txt")));
        QVERIFY(!patched->entries(QString()).contains(QStringLiteral("escaped.txt")));
        QCOMPARE(patched->errors(QString()).size(), 1);
        QVERIFY(!patched->content(QStringLiteral("../escaped.txt")).text);
    }

    // -----------------------------------------------------------------------
    // Comparisons

    void comparesAFileWithItsPatchedText()
    {
        const QString file = write(QStringLiteral("numbers.txt"), Original);
        const QString patchFile = write(QStringLiteral("numbers.patch"), FivePatch);
        auto *filediff = qobject_cast<FileDiff *>(window_->appendPatchDiff(file, patchFile));
        QVERIFY(filediff);
        settle();
        QCOMPARE(filediff->textBuffer(0)->text(), QString::fromUtf8(Original));
        QCOMPARE(filediff->textBuffer(1)->text(), QString::fromUtf8(QByteArray(Original).replace("five", "FIVE")));

        // The patched text is in memory: it has no file, and is read-only
        QVERIFY(filediff->textBuffer(1)->data.file.isEmpty());
        QVERIFY(!filediff->textBuffer(1)->data.writable());
        QVERIFY(filediff->textBuffer(0)->data.writable());
        QCOMPARE(filediff->tabTitle(), QStringLiteral("numbers.txt — numbers.patch"));
        QStringList labels;
        for (const PathLabel *label : filediff->findChildren<PathLabel *>())
            labels.append(label->displayLabel());
        QVERIFY2(labels.contains(QStringLiteral("numbers.txt with numbers.patch applied")),
                 qPrintable(labels.join(u',')));
        QVERIFY(!filediff->comparison() || filediff->comparison()->paths.contains(QString()));

        // Reverting makes it again, from the file as it is now
        write(QStringLiteral("numbers.txt"), QByteArray("zero\n") + Original);
        filediff->setFile(0, file);
        filediff->setFile(1, QString());
        settle();
        QCOMPARE(filediff->textBuffer(1)->text(),
                 QString::fromUtf8("zero\n" + QByteArray(Original).replace("five", "FIVE")));
        QVERIFY(messages(filediff->messageArea(1)).contains(QLatin1String("offset 1 line)")));
    }

    void showsPatchesThatCantBeApplied()
    {
        const QString file = write(QStringLiteral("other-numbers.txt"), "nothing alike\n");
        const QString patchFile = write(QStringLiteral("other.patch"), FivePatch);
        auto *filediff = qobject_cast<FileDiff *>(window_->appendPatchDiff(file, patchFile));
        QVERIFY(filediff);
        settle();
        QCOMPARE(filediff->textBuffer(1)->text(), QString());
        const QString shown = messages(filediff->messageArea(1));
        QVERIFY2(shown.contains(QStringLiteral("can’t be applied")), qPrintable(shown));
        QVERIFY2(shown.contains(QLatin1String("Hunk #1")), qPrintable(shown));
    }

    void refusesPatchesForOtherFiles()
    {
        const QString file = write(QStringLiteral("unrelated.txt"), Original);
        const QString patchFile = write(QStringLiteral("folder-for-file.patch"), folderPatch());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, window_->appendPatchDiff(file, patchFile));
        const QString notAPatch = write(QStringLiteral("not.patch"), "hello\n");
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, window_->appendPatchDiff(file, notAPatch));
        QVERIFY(!window_->hasPages());
    }

    void comparesAFolderWithItsPatchedFolder()
    {
        const QString folder = makeFolder();
        const QString patchFile = write(QStringLiteral("folder-compare.patch"), folderPatch());
        auto *dirdiff = qobject_cast<DirDiff *>(window_->appendPatchDiff(folder, patchFile));
        QVERIFY(dirdiff);
        window_->tabWidget()->setCurrentWidget(dirdiff);
        settle();
        QVERIFY(dirdiff->patchedFolder());
        QVERIFY(!dirdiff->comparison());

        QCOMPARE(states(dirdiff, QStringLiteral("changed.txt")), (QList<int>{StateModified, StateModified}));
        QCOMPARE(states(dirdiff, QStringLiteral("same.txt")), (QList<int>{StateNormal, StateNormal}));
        QCOMPARE(states(dirdiff, QStringLiteral("gone.txt")), (QList<int>{StateNew, StateNonExist}));
        QCOMPARE(states(dirdiff, QStringLiteral("old.txt")), (QList<int>{StateNew, StateNonExist}));
        QCOMPARE(states(dirdiff, QStringLiteral("renamed.txt")), (QList<int>{StateNonExist, StateNew}));
        QCOMPARE(states(dirdiff, QStringLiteral("new")), (QList<int>{StateNonExist, StateNew}));
        QCOMPARE(states(dirdiff, QStringLiteral("new/added.txt")), (QList<int>{StateNonExist, StateNew}));
        QCOMPARE(states(dirdiff, QStringLiteral("sub/inner.txt")), (QList<int>{StateNormal, StateNormal}));
        const QStringList patchedTexts = paneTexts(dirdiff, 1);
        QVERIFY2(patchedTexts.contains(QStringLiteral("missing.txt: The patch changes this file, but it isn’t in the "
                                                      "folder.")),
                 qPrintable(patchedTexts.join(u'\n')));
        QCOMPARE(patchedTexts.first(), patchFile);

        // Nothing is made until a file is compared
        TreeNode *changed = find(dirdiff, QStringLiteral("changed.txt"));
        QCOMPARE(changed->panes[1].size, qint64(-1));

        // The patched folder can't be changed
        DirTreeView *view = dirdiff->treeView(1);
        view->setFocus();
        view->setCurrentIndex(dirdiff->model()->indexOf(changed));
        QCoreApplication::processEvents();
        QVERIFY(!dirdiff->action(QStringLiteral("folder-copy-left"))->isEnabled());
        QVERIFY(!dirdiff->action(QStringLiteral("folder-delete"))->isEnabled());

        dirdiff->action(QStringLiteral("folder-compare"))->trigger();
        settle();
        auto *filediff = qobject_cast<FileDiff *>(window_->docs().last());
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QString::fromUtf8(Original));
        QCOMPARE(filediff->textBuffer(1)->text(), QString::fromUtf8(QByteArray(Original).replace("five", "FIVE")));
        QVERIFY(filediff->textBuffer(1)->data.file.isEmpty());
        QCOMPARE(filediff->tabTitle(), QStringLiteral("changed.txt — folder-compare.patch"));

        // A new file is compared with a blank pane
        window_->tabWidget()->setCurrentWidget(dirdiff);
        view->setCurrentIndex(dirdiff->model()->indexOf(find(dirdiff, QStringLiteral("new/added.txt"))));
        dirdiff->action(QStringLiteral("folder-compare"))->trigger();
        settle();
        filediff = qobject_cast<FileDiff *>(window_->docs().last());
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QString());
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("added\nlines\n"));
    }

    void commandLinePatches()
    {
        const QString file = write(QStringLiteral("cli/numbers.txt"), Original);
        const QString patchFile = write(QStringLiteral("cli/numbers.patch"), FivePatch);
        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "--patch", patchFile, file}), -1);
            auto *filediff = qobject_cast<FileDiff *>(app.window()->currentDoc());
            QVERIFY(filediff);
            QTRY_COMPARE(filediff->textBuffer(1)->text(),
                         QString::fromUtf8(QByteArray(Original).replace("five", "FIVE")));
            QCOMPARE(filediff->tabTitle(), QStringLiteral("numbers.txt — numbers.patch"));
        }
        {
            // Relative to the invocation's folder, with labels
            MeldApp app;
            const QString previous = QDir::currentPath();
            QDir::setCurrent(dir_.filePath(QStringLiteral("cli")));
            const int status =
                app.handleCommandLine({"meld-qt", "-L", "Mine", "-L", "Patched", "--patch=numbers.patch", "numbers.txt"});
            QDir::setCurrent(previous);
            QCOMPARE(status, -1);
            auto *filediff = qobject_cast<FileDiff *>(app.window()->currentDoc());
            QVERIFY(filediff);
            QStringList labels;
            for (const PathLabel *label : filediff->findChildren<PathLabel *>())
                labels.append(label->displayLabel());
            QVERIFY2(labels.contains(QStringLiteral("Patched")), qPrintable(labels.join(u',')));
        }
        {
            // --auto-compare opens the changed files' comparisons
            const QString folder = makeFolder();
            const QString folderPatchFile = write(QStringLiteral("cli/folder.patch"), folderPatch());
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "-a", "--patch", folderPatchFile, folder}), -1);
            auto *dirdiff = qobject_cast<DirDiff *>(app.window()->currentDoc());
            QVERIFY(dirdiff);
            QVERIFY(dirdiff->patchedFolder());
            QTRY_VERIFY(app.window()->docs().size() == 2);
            auto *filediff = qobject_cast<FileDiff *>(app.window()->docs().last());
            QVERIFY(filediff);
            QCOMPARE(filediff->tabTitle(), QStringLiteral("changed.txt — folder.patch"));
        }
        {
            // Archives are extracted, and the patch applies to their files
            const QString archivePatch = write(QStringLiteral("cli/archive.patch"), "--- a/dir/changed.txt\n"
                                                                                      "+++ b/dir/changed.txt\n"
                                                                                      "@@ -1 +1 @@\n"
                                                                                      "-left\n"
                                                                                      "+patched\n");
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "--patch", archivePatch,
                                            QStringLiteral(MELD_TEST_DATA_DIR "/archives/left.tar.gz")}),
                     -1);
            auto *dirdiff = qobject_cast<DirDiff *>(app.window()->currentDoc());
            QVERIFY(dirdiff);
            QVERIFY(archive::isExtractedArchiveRoot(dirdiff->folders().first()));
            QCOMPARE(dirdiff->patchedFolder()->content(QStringLiteral("dir/changed.txt")).text,
                     std::optional<QByteArray>("patched\n"));
            dirdiff->requestClose();
            QVERIFY(!archive::haveExtractedArchives());
        }
    }

    void commandLinePatchErrors_data()
    {
        QTest::addColumn<QString>("patch");
        QTest::addColumn<QString>("target");
        QTest::addColumn<QString>("error");
        QTest::newRow("missing patch") << QStringLiteral("missing.patch") << QStringLiteral("numbers.txt")
                                       << QStringLiteral("doesn’t exist");
        QTest::newRow("missing file") << QStringLiteral("numbers.patch") << QStringLiteral("missing.txt")
                                      << QStringLiteral("Cannot compare a non-existent file");
        QTest::newRow("not a patch") << QStringLiteral("numbers.txt") << QStringLiteral("numbers.txt")
                                     << QStringLiteral("No changes were found in the patch");
        QTest::newRow("other file") << QStringLiteral("folder.patch") << QStringLiteral("numbers.txt")
                                    << QStringLiteral("doesn’t change numbers.txt");
    }

    void commandLinePatchErrors()
    {
        QFETCH(QString, patch);
        QFETCH(QString, target);
        QFETCH(QString, error);
        write(QStringLiteral("errors/numbers.txt"), Original);
        write(QStringLiteral("errors/numbers.patch"), FivePatch);
        write(QStringLiteral("errors/folder.patch"), folderPatch());
        const QDir errors(dir_.filePath(QStringLiteral("errors")));

        QString message;
        QVERIFY(!window_->openPatch(errors.filePath(target), errors.filePath(patch), false, true, &message));
        QVERIFY2(message.contains(error), qPrintable(message));
        QVERIFY(!window_->hasPages());

        // ...which end the invocation
        MeldApp app;
        QCOMPARE(app.handleCommandLine({"meld-qt", "--patch", errors.filePath(patch), errors.filePath(target)}), 2);
    }

    void newComparisonsCanBeWithPatches()
    {
        NewDiffTab *tab = window_->appendNewComparison();
        // Without opening the comparison, which fails without paths
        disconnect(tab, &NewDiffTab::patchDiffRequested, window_.get(), nullptr);
        QSignalSpy spy(tab, &NewDiffTab::patchDiffRequested);
        const QList<QToolButton *> types = tab->findChildren<QToolButton *>();
        QToolButton *folderType = nullptr;
        for (QToolButton *button : types) {
            if (button->text() == QLatin1String("Folder"))
                folderType = button;
        }
        QVERIFY(folderType);
        folderType->click();

        QCheckBox *patchCheck = nullptr;
        QCheckBox *threeWay = nullptr;
        for (QCheckBox *check : tab->findChildren<QCheckBox *>()) {
            if (!check->isVisibleTo(tab))
                continue;
            if (check->text() == QLatin1String("Compare with a &patch"))
                patchCheck = check;
            if (check->text() == QLatin1String("&3-way comparison"))
                threeWay = check;
        }
        QVERIFY(patchCheck);
        QVERIFY(threeWay);
        threeWay->setChecked(true);
        patchCheck->setChecked(true);
        QVERIFY(!threeWay->isChecked());
        QVERIFY(!threeWay->isEnabled());
        bool hasPatchChooser = false;
        for (QPushButton *button : tab->findChildren<QPushButton *>())
            hasPatchChooser = hasPatchChooser || (button->isVisibleTo(tab) && button->text() == QLatin1String("Select Patch File"));
        QVERIFY(hasPatchChooser);

        for (QPushButton *button : tab->findChildren<QPushButton *>()) {
            if (button->text() == QLatin1String("C&ompare"))
                button->click();
        }
        QCOMPARE(spy.size(), 1);
    }
};

QTEST_MAIN(TestPatch)
#include "tst_patch.moc"
