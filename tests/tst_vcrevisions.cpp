// Tests for comparing revisions in the version control view: any commit, tag
// or tree with another, or with the working copy, on real git repositories.
// Revisions' files are read into memory, never written to temporary files.

#include "core/meldbuffer.h"
#include "core/vc/vc.h"
#include "filediff.h"
#include "meldwindow.h"
#include "msgarea.h"
#include "settings.h"
#include "vcview.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeView>

using namespace meld;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("Couldn't write %s", qPrintable(path));
    file.write(content);
}

} // namespace

class TestVcRevisions : public QObject {
    Q_OBJECT

private:
    QTemporaryDir home_;
    // Temporary files would be made here (TMPDIR)
    QTemporaryDir temp_;
    std::unique_ptr<MeldWindow> window_;

    static QString git(const QStringList &args, const QString &cwd)
    {
        QProcess process;
        process.setWorkingDirectory(cwd);
        process.start(QStringLiteral("git"), args);
        process.waitForFinished(30000);
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }

    /*!
     * A repository with two tagged commits, and changes in the working copy:
     *  v1: a.txt, b.txt, dir/c.txt, gone/x.txt
     *  v2: a.txt changed, b.txt deleted, dir/new.txt added, gone/ deleted
     *  working copy: a.txt changed again, u.txt untracked
     */
    static QString makeHistory(const QTemporaryDir &dir)
    {
        const QString root = dir.path();
        git({"init", "-q"}, root);
        writeFile(root + "/a.txt", "a1\n");
        writeFile(root + "/b.txt", "b1\n");
        writeFile(root + "/dir/c.txt", "c1\n");
        writeFile(root + "/gone/x.txt", "x\n");
        git({"add", "."}, root);
        git({"commit", "-q", "-m", "First"}, root);
        git({"tag", "v1"}, root);

        writeFile(root + "/a.txt", "a2\n");
        QFile::remove(root + "/b.txt");
        writeFile(root + "/dir/new.txt", "new\n");
        QDir(root + "/gone").removeRecursively();
        git({"add", "-A"}, root);
        git({"commit", "-q", "-m", "Second"}, root);
        git({"tag", "v2"}, root);

        writeFile(root + "/a.txt", "a working\n");
        writeFile(root + "/u.txt", "untracked\n");
        return root;
    }

    void settle()
    {
        int idle = 0;
        for (int i = 0; i < 2000 && idle < 3; ++i) {
            window_->runPendingTasks();
            QTest::qWait(5);
            idle = window_->scheduler().tasksPending() ? 0 : idle + 1;
        }
    }

    VcView *openView(const QString &location)
    {
        VcView *view = window_->appendVcview(location);
        window_->tabWidget()->setCurrentWidget(view);
        settle();
        return view;
    }

    static TreeNode *row(VcView *view, const QString &name)
    {
        VcTreeModel *model = view->model();
        for (TreeNode *n = model->firstRoot(); n; n = model->inorderNext(n)) {
            if (n->panes[0].text == name)
                return n;
        }
        return nullptr;
    }

    //! Rows below the root, with their states' labels
    static QStringList rows(VcView *view)
    {
        QStringList names;
        VcTreeModel *model = view->model();
        for (TreeNode *n = model->firstRoot(); n; n = model->inorderNext(n)) {
            if (n == model->firstRoot())
                continue;
            names.append(QStringLiteral("%1 %2").arg(n->panes[0].text, model->text(n, VcTreeModel::ColStatus)).trimmed());
        }
        return names;
    }

    static bool enabled(VcView *view, const char *action)
    {
        return view->action(QLatin1String(action))->isEnabled();
    }

    FileDiff *compare(VcView *view, const QString &name)
    {
        TreeNode *n = row(view, name);
        if (!n)
            return nullptr;
        view->treeView()->setCurrentIndex(view->model()->indexOf(n));
        const int before = int(window_->docs().size());
        view->runDiff(view->model()->filePath(n));
        settle();
        if (int(window_->docs().size()) == before)
            return nullptr;
        return qobject_cast<FileDiff *>(window_->docs().last());
    }

    QStringList temporaryFiles() const
    {
        return QDir(temp_.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed");
        QVERIFY(home_.isValid());
        QVERIFY(temp_.isValid());
        const QString config = home_.filePath(QStringLiteral("gitconfig"));
        writeFile(config, "[user]\n\tname = Meld Test\n\temail = meld@example.com\n"
                          "[init]\n\tdefaultBranch = main\n[commit]\n\tgpgsign = false\n[tag]\n\tgpgsign = false\n");
        qputenv("GIT_CONFIG_GLOBAL", config.toLocal8Bit());
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("HOME", home_.path().toLocal8Bit());
        qputenv("LC_ALL", "C");
        MeldSettings::useSettingsFile(home_.filePath(QStringLiteral("settings.ini")));
    }

    void init()
    {
        for (const char *key : {"vc-status-filters", "vc-left-is-local"})
            MeldSettings::instance()->resetValue(QLatin1String(key));
        window_ = std::make_unique<MeldWindow>();
        window_->resize(1000, 700);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
        qputenv("TMPDIR", temp_.path().toLocal8Bit());
    }

    void cleanup()
    {
        for (MeldDoc *doc : window_->docs()) {
            if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                for (int pane = 0; pane < filediff->numPanes(); ++pane)
                    filediff->textBuffer(pane)->setModified(false);
            }
        }
        while (QWidget *dialog = QApplication::activeModalWidget())
            dialog->close();
        window_.reset();
        qunsetenv("TMPDIR");
        cleanupVcTempFiles();
    }

    // -----------------------------------------------------------------------
    // Git

    void gitRevisions()
    {
        QTemporaryDir dir(home_.filePath(QStringLiteral("repo-XXXXXX")));
        const QString root = makeHistory(dir);
        const std::shared_ptr<vc::Vc> vc = vc::pluginByName(QStringLiteral("Git"))->create(root);
        QVERIFY(vc->hasRevisionSupport());

        QStringList names;
        for (const vc::Revision &revision : vc->revisions())
            names.append(revision.name);
        QCOMPARE(names.first(), QStringLiteral("HEAD"));
        QVERIFY(names.contains(QStringLiteral("main")));
        QVERIFY(names.contains(QStringLiteral("v1")));
        QVERIFY(names.contains(QStringLiteral("v2")));
        const QString first = git({"rev-parse", "--short", "v1"}, root);
        QVERIFY2(names.contains(first), qPrintable(names.join(u',')));

        QString error;
        QVERIFY(vc->verifyRevision(QStringLiteral("v1"), &error));
        QVERIFY(vc->verifyRevision(QStringLiteral("HEAD~1"), &error));
        // Trees, as well as commits and tags
        QVERIFY(vc->verifyRevision(git({"rev-parse", "v1^{tree}"}, root), &error));
        QVERIFY(!vc->verifyRevision(QStringLiteral("no-such-thing"), &error));
        QVERIFY2(error.contains(QLatin1String("no-such-thing")), qPrintable(error));
        QVERIFY(!vc->verifyRevision(QStringLiteral("--all"), &error));

        QCOMPARE(vc->fileAtRevision(QStringLiteral("v1"), root + "/a.txt", &error), std::optional<QByteArray>("a1\n"));
        QCOMPARE(vc->fileAtRevision(QString(), root + "/a.txt", &error), std::optional<QByteArray>("a2\n"));
        QVERIFY(!vc->fileAtRevision(QStringLiteral("v2"), root + "/b.txt", &error));
        QVERIFY(!vc->fileAtRevision(QStringLiteral("v1"), QStringLiteral("/elsewhere/a.txt"), &error));
    }

    void gitStatesBetweenRevisions()
    {
        QTemporaryDir dir(home_.filePath(QStringLiteral("repo-XXXXXX")));
        const QString root = makeHistory(dir);
        const std::shared_ptr<vc::Vc> vc = vc::pluginByName(QStringLiteral("Git"))->create(root);
        vc->setRevisions(QStringLiteral("v1"), QStringLiteral("v2"));
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/a.txt").state, int(StateModified));
        QCOMPARE(vc->entry(root + "/b.txt").state, int(StateRemoved));
        QCOMPARE(vc->entry(root + "/dir/new.txt").state, int(StateNew));
        QCOMPARE(vc->entry(root + "/gone/x.txt").state, int(StateRemoved));
        QCOMPARE(vc->entry(root + "/dir/c.txt").state, int(StateNormal));
        // Folders that only a revision has
        QVERIFY(vc->entry(root + "/gone").isDir);

        QStringList names;
        for (const vc::Entry &e : vc->entries(root))
            names.append(e.name);
        names.sort();
        // The working copy's untracked file isn't in either revision
        QCOMPARE(names, (QStringList{"a.txt", "b.txt", "dir", "gone"}));

        // The working copy, compared with v1
        vc->setRevisions(QStringLiteral("v1"), QString());
        vc->refreshVcState();
        QCOMPARE(vc->entry(root + "/a.txt").state, int(StateModified));
        QCOMPARE(vc->entry(root + "/b.txt").state, int(StateRemoved));
        QCOMPARE(vc->entry(root + "/dir/new.txt").state, int(StateNew));
        QCOMPARE(vc->entry(root + "/u.txt").state, int(StateNone));
        // Staging is relative to the current commit, so isn't shown
        QVERIFY(!vc->entry(root + "/dir/new.txt").options.contains(QLatin1String("taged")));
    }

    // -----------------------------------------------------------------------
    // The view

    void revisionSelectors()
    {
        QTemporaryDir dir(home_.filePath(QStringLiteral("repo-XXXXXX")));
        const QString root = makeHistory(dir);
        VcView *view = openView(root);
        QVERIFY(view->revisionBar()->isVisible());
        QCOMPARE(view->baseRevisionSelector()->currentText(), QStringLiteral("HEAD"));
        QCOMPARE(view->targetRevisionSelector()->currentText(), QStringLiteral("working copy"));
        QVERIFY(view->vc()->comparesWorkingCopy());
        QVERIFY(view->baseRevisionSelector()->findText(QStringLiteral("v1")) >= 0);
        QVERIFY(view->targetRevisionSelector()->findText(QStringLiteral("v2")) >= 0);

        // Revisions can be typed
        view->baseRevisionSelector()->setEditText(QStringLiteral("HEAD~1"));
        Q_EMIT view->baseRevisionSelector()->lineEdit()->returnPressed();
        settle();
        QCOMPARE(view->baseRevision(), QStringLiteral("HEAD~1"));
        QCOMPARE(view->vc()->baseRevision(), QStringLiteral("HEAD~1"));
        QVERIFY(view->tabTitle().contains(QStringLiteral("HEAD~1 → working copy")));

        // ...or chosen
        view->targetRevisionSelector()->setCurrentIndex(view->targetRevisionSelector()->findText(QStringLiteral("v2")));
        Q_EMIT view->targetRevisionSelector()->activated(view->targetRevisionSelector()->currentIndex());
        settle();
        QCOMPARE(view->targetRevision(), QStringLiteral("v2"));
        QVERIFY(view->vc()->comparesRevisions());

        // Unknown revisions are refused
        view->baseRevisionSelector()->setEditText(QStringLiteral("nonsense"));
        Q_EMIT view->baseRevisionSelector()->lineEdit()->returnPressed();
        settle();
        QCOMPARE(view->baseRevision(), QStringLiteral("HEAD~1"));
        QVERIFY(view->messageArea()->hasMessage());

        // Refreshing keeps them
        view->action(QStringLiteral("refresh"))->trigger();
        settle();
        QCOMPARE(view->vc()->baseRevision(), QStringLiteral("HEAD~1"));
        QCOMPARE(view->vc()->targetRevision(), QStringLiteral("v2"));

        // Back to the working copy and the current commit
        QVERIFY(view->setRevisions(QString(), QString()));
        settle();
        QVERIFY(view->vc()->comparesWorkingCopy());
        QCOMPARE(view->tabTitle(), QFileInfo(root).fileName());
    }

    void comparesTwoRevisions()
    {
        QTemporaryDir dir(home_.filePath(QStringLiteral("repo-XXXXXX")));
        const QString root = makeHistory(dir);
        VcView *view = openView(root);
        QVERIFY(view->setRevisions(QStringLiteral("v1"), QStringLiteral("v2")));
        settle();

        // The default filters show changes, with folders flattened
        QStringList shown = rows(view);
        QVERIFY2(shown.contains(QStringLiteral("a.txt Modified")), qPrintable(shown.join(u'\n')));
        QVERIFY2(shown.contains(QStringLiteral("b.txt Removed")), qPrintable(shown.join(u'\n')));
        QVERIFY2(shown.contains(QStringLiteral("new.txt Newly added")), qPrintable(shown.join(u'\n')));
        QVERIFY2(shown.contains(QStringLiteral("x.txt Removed")), qPrintable(shown.join(u'\n')));
        QVERIFY(!row(view, QStringLiteral("u.txt")));
        QVERIFY(!row(view, QStringLiteral("c.txt")));

        // Revisions' files can't be changed with version control
        view->treeView()->setCurrentIndex(view->model()->indexOf(row(view, QStringLiteral("a.txt"))));
        QCoreApplication::processEvents();
        QVERIFY(enabled(view, "compare"));
        QVERIFY(!enabled(view, "vc-commit"));
        QVERIFY(!enabled(view, "vc-revert"));
        QVERIFY(!enabled(view, "vc-add"));
        QVERIFY(!enabled(view, "open-external"));
        QVERIFY(!enabled(view, "vc-delete-locally"));

        FileDiff *filediff = compare(view, QStringLiteral("a.txt"));
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a1\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("a2\n"));
        // Both are only viewed
        for (int pane = 0; pane < 2; ++pane) {
            QVERIFY(filediff->textBuffer(pane)->data.file.isEmpty());
            QVERIFY(!filediff->textBuffer(pane)->data.writable());
        }
        QCOMPARE(filediff->tabTitle(), QStringLiteral("a.txt (v1, v2)"));

        // A deleted file is compared with nothing
        filediff = compare(view, QStringLiteral("x.txt"));
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("x\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QString());

        // Nothing was written to temporary files
        QVERIFY2(temporaryFiles().isEmpty(), qPrintable(temporaryFiles().join(u',')));
    }

    void comparesARevisionWithTheWorkingCopy()
    {
        QTemporaryDir dir(home_.filePath(QStringLiteral("repo-XXXXXX")));
        const QString root = makeHistory(dir);
        VcView *view = openView(root);
        QVERIFY(view->setRevisions(QStringLiteral("v1"), QString()));
        settle();

        QStringList shown = rows(view);
        QVERIFY2(shown.contains(QStringLiteral("a.txt Modified")), qPrintable(shown.join(u'\n')));
        QVERIFY2(shown.contains(QStringLiteral("b.txt Removed")), qPrintable(shown.join(u'\n')));
        QVERIFY2(shown.contains(QStringLiteral("new.txt Newly added")), qPrintable(shown.join(u'\n')));

        // Version control actions are for changes from the current commit
        view->treeView()->setCurrentIndex(view->model()->indexOf(row(view, QStringLiteral("a.txt"))));
        QCoreApplication::processEvents();
        QVERIFY(!enabled(view, "vc-commit"));
        QVERIFY(enabled(view, "open-external"));

        FileDiff *filediff = compare(view, QStringLiteral("a.txt"));
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a1\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("a working\n"));
        // The revision's file is only viewed; the working file can be edited
        QVERIFY(!filediff->textBuffer(0)->data.writable());
        QCOMPARE(filediff->textBuffer(1)->data.file, root + "/a.txt");
        QVERIFY(filediff->textBuffer(1)->data.writable());
        QCOMPARE(filediff->tabTitle(), QStringLiteral("a.txt (v1, working)"));
        QVERIFY2(temporaryFiles().isEmpty(), qPrintable(temporaryFiles().join(u',')));

        // The working copy is on the left, if preferred
        MeldSettings::instance()->setValue(QStringLiteral("vc-left-is-local"), true);
        filediff = compare(view, QStringLiteral("a.txt"));
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->data.file, root + "/a.txt");
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("a1\n"));
    }

    void swapsRevisions()
    {
        QTemporaryDir dir(home_.filePath(QStringLiteral("repo-XXXXXX")));
        const QString root = makeHistory(dir);
        VcView *view = openView(root);
        QVERIFY(view->setRevisions(QStringLiteral("v1"), QStringLiteral("v2")));
        settle();
        QToolButton *swap = nullptr;
        for (QToolButton *button : view->revisionBar()->findChildren<QToolButton *>()) {
            if (button->text() == QLatin1String("Swap"))
                swap = button;
        }
        QVERIFY(swap);
        swap->click();
        settle();
        QCOMPARE(view->baseRevision(), QStringLiteral("v2"));
        QCOMPARE(view->targetRevision(), QStringLiteral("v1"));
        QVERIFY(rows(view).contains(QStringLiteral("b.txt Newly added")));
    }
};

QTEST_MAIN(TestVcRevisions)
#include "tst_vcrevisions.moc"
