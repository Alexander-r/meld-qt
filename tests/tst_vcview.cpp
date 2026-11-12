// Tests for the version control view (vcview.py), its dialogs and its
// integration with file comparisons and the main window, on real git
// repositories. The original has no tests for its version control view.

#include "core/translations.h"
#include "core/vc/vc.h"
#include "filediff.h"
#include "meldapp.h"
#include "meldwindow.h"
#include "newdifftab.h"
#include "recent.h"
#include "settings.h"
#include "vcdialogs.h"
#include "vcview.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QMenu>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeView>

using namespace meld;

namespace {

void writeFile(const QString &path, const QByteArray &contents)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
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

void replaceText(MeldBuffer *buffer, const QString &text)
{
    QTextCursor cursor(buffer->document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(text);
}

template<typename T>
T *visibleDialog()
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (auto *dialog = qobject_cast<T *>(w); dialog && dialog->isVisible())
            return dialog;
    }
    return nullptr;
}

//! Click a dialog's button with the given label
bool clickButton(QWidget *dialog, const QString &label)
{
    for (QAbstractButton *button : dialog->findChildren<QAbstractButton *>()) {
        if (button->text().remove(QLatin1Char('&')) == label) {
            button->click();
            return true;
        }
    }
    return false;
}

//! Click the button of a message box with the given label
bool answer(const QString &label)
{
    auto *box = visibleDialog<QMessageBox>();
    if (!box)
        return false;
    for (QAbstractButton *button : box->buttons()) {
        if (button->text().remove(QLatin1Char('&')) == label) {
            button->click();
            return true;
        }
    }
    return false;
}

} // namespace

class TestVcView : public QObject {
    Q_OBJECT

private:
    QTemporaryDir home_;
    std::unique_ptr<MeldWindow> window_;

    static QString git(const QStringList &args, const QString &cwd, int *exitCode = nullptr)
    {
        QProcess process;
        process.setWorkingDirectory(cwd);
        process.start(QStringLiteral("git"), args);
        process.waitForFinished(30000);
        if (exitCode)
            *exitCode = process.exitCode();
        return QString::fromUtf8(process.readAllStandardOutput());
    }

    //! A repository with committed files, then changes of each kind
    static QString makeRepo(const QTemporaryDir &dir)
    {
        const QString root = dir.path();
        git({"init", "-q"}, root);
        writeFile(root + "/a.txt", "a\n");
        writeFile(root + "/b.txt", "b\n");
        writeFile(root + "/clean.txt", "clean\n");
        writeFile(root + "/dir/c.txt", "c\n");
        writeFile(root + "/dir/sub/d.txt", "d\n");
        writeFile(root + "/.gitignore", "*.log\n");
        git({"add", "."}, root);
        git({"commit", "-q", "-m", "Initial"}, root);

        writeFile(root + "/a.txt", "a changed\n");
        writeFile(root + "/dir/sub/d.txt", "d changed\n");
        writeFile(root + "/new.txt", "new\n");
        git({"add", "new.txt"}, root);
        QFile::remove(root + "/b.txt");
        writeFile(root + "/untracked.txt", "?\n");
        writeFile(root + "/debug.log", "ignored\n");
        QDir().mkpath(root + "/empty");
        return root;
    }

    //! A repository with a merge conflict in a.txt
    static QString makeConflict(const QTemporaryDir &dir)
    {
        const QString root = dir.path();
        git({"init", "-q"}, root);
        writeFile(root + "/a.txt", "1\nbase\n3\n");
        git({"add", "."}, root);
        git({"commit", "-q", "-m", "Base"}, root);
        git({"checkout", "-q", "-b", "theirs"}, root);
        writeFile(root + "/a.txt", "1\ntheirs\n3\n");
        git({"commit", "-q", "-am", "Theirs"}, root);
        git({"checkout", "-q", "main"}, root);
        writeFile(root + "/a.txt", "1\nours\n3\n");
        git({"commit", "-q", "-am", "Ours"}, root);
        int exitCode = 0;
        git({"merge", "theirs"}, root, &exitCode);
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

    //! Rows below the root, indented by depth
    static QStringList rows(VcView *view)
    {
        QStringList names;
        VcTreeModel *model = view->model();
        for (TreeNode *n = model->firstRoot(); n; n = model->inorderNext(n)) {
            int depth = 0;
            for (TreeNode *p = n->parent; p && p != model->invisibleRoot(); p = p->parent)
                ++depth;
            if (depth > 0)
                names.append(QString((depth - 1) * 2, QLatin1Char(' ')) + n->panes[0].text);
        }
        return names;
    }

    static void select(VcView *view, const QStringList &names)
    {
        QTreeView *tree = view->treeView();
        tree->selectionModel()->clearSelection();
        for (const QString &name : names) {
            TreeNode *n = row(view, name);
            QVERIFY2(n, qPrintable(name));
            const QModelIndex index = view->model()->indexOf(n);
            tree->selectionModel()->setCurrentIndex(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }

    static bool enabled(VcView *view, const char *action)
    {
        return view->action(QLatin1String(action))->isEnabled();
    }

    template<typename T>
    T *docAt(int index) const
    {
        return qobject_cast<T *>(window_->tabWidget()->widget(index));
    }

private Q_SLOTS:
    void initTestCase()
    {
        // The English plural forms, as the program has them
        translations::install(QLocale(QLocale::English));
        if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty())
            QSKIP("git is not installed");
        QVERIFY(home_.isValid());
        const QString config = home_.filePath(QStringLiteral("gitconfig"));
        writeFile(config, "[user]\n\tname = Meld Test\n\temail = meld@example.com\n"
                          "[init]\n\tdefaultBranch = main\n[commit]\n\tgpgsign = false\n"
                          "[merge]\n\tconflictstyle = merge\n");
        qputenv("GIT_CONFIG_GLOBAL", config.toLocal8Bit());
        qputenv("GIT_CONFIG_NOSYSTEM", "1");
        qputenv("HOME", home_.path().toLocal8Bit());
        qputenv("LC_ALL", "C");
        // Keep the trash of deleted files in the test folder; Qt doesn't
        // create the trash if this folder doesn't exist
        qputenv("XDG_DATA_HOME", home_.filePath(QStringLiteral("data")).toLocal8Bit());
        QVERIFY(QDir().mkpath(home_.filePath(QStringLiteral("data"))));
        MeldSettings::useSettingsFile(home_.filePath(QStringLiteral("settings.ini")));
    }

    void init()
    {
        const QStringList keys{"vc-status-filters", "vc-console-visible", "vc-left-is-local", "vc-merge-file-order",
                               "vc-show-commit-margin", "vc-commit-margin", "vc-break-commit-message",
                               "commit-message-history"};
        for (const QString &key : keys)
            MeldSettings::instance()->resetValue(key);
        window_ = std::make_unique<MeldWindow>();
        window_->resize(1000, 700);
        window_->show();
        window_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));
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
        cleanupVcTempFiles();
    }

    // -----------------------------------------------------------------------
    // Showing version control state

    void showsModifiedFiles()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);

        QCOMPARE(view->vc()->name(), QStringLiteral("Git"));
        QCOMPARE(view->location(), root);
        QCOMPARE(view->tabTitle(), QFileInfo(root).fileName());
        QCOMPARE(view->tabTooltip(), QStringLiteral("Git comparison:\n") + root);
        QCOMPARE(window_->windowTitle(), QFileInfo(root).fileName() + QStringLiteral(" - Meld Qt"));

        // The default filters show modified files, with folders flattened
        QCOMPARE(rows(view), (QStringList{"a.txt", "b.txt", "new.txt", "d.txt"}));
        VcTreeModel *model = view->model();
        TreeNode *d = row(view, "d.txt");
        QCOMPARE(model->text(d, VcTreeModel::ColLocation), QStringLiteral("dir/sub"));
        QCOMPARE(model->text(d, VcTreeModel::ColStatus), QStringLiteral("Modified"));
        QCOMPARE(model->text(row(view, "b.txt"), VcTreeModel::ColStatus), QStringLiteral("Removed"));
        QCOMPARE(model->state(row(view, "b.txt"), 0), int(StateRemoved));
        QCOMPARE(model->text(row(view, "new.txt"), VcTreeModel::ColStatus), QStringLiteral("Newly added"));
        QCOMPARE(model->text(row(view, "new.txt"), VcTreeModel::ColOptions),
                 QStringLiteral("Mode changed from 000000 to 100644; Staged"));
        QCOMPARE(model->text(row(view, "a.txt"), VcTreeModel::ColLocation), QString());
        // No upstream, so nothing to push
        QCOMPARE(model->text(model->firstRoot(), VcTreeModel::ColOptions), QString());

        // The root is the cursor, and the header bar shows the version
        // filters instead of the file filters
        QCOMPARE(view->treeView()->currentIndex().row(), 0);
        QVERIFY(!view->treeView()->currentIndex().parent().isValid());
        QVERIFY(window_->viewAction(QStringLiteral("next-change"))->isEnabled());
        QVERIFY(!window_->viewAction(QStringLiteral("previous-change"))->isEnabled());
    }

    void versionControlSelector()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        QComboBox *selector = view->vcSelector();
        QCOMPARE(selector->currentText(), QStringLiteral("Git"));
        QCOMPARE(selector->toolTip(), QStringLiteral("Choose which version control system to use"));
        QStringList labels;
        auto *items = qobject_cast<QStandardItemModel *>(selector->model());
        for (int i = 0; i < selector->count(); ++i) {
            labels.append(selector->itemText(i));
            QCOMPARE(items->item(i)->isEnabled(), i == 0);
        }
        QCOMPARE(labels, (QStringList{"Git", "Mercurial (not found)", "Bazaar (not found)", "Subversion (not found)",
                                      "Darcs (not found)", "CVS (not found)"}));

        // Without a repository, the view falls back to no version control
        QTemporaryDir plain;
        writeFile(plain.filePath(QStringLiteral("file.txt")), "x\n");
        view->setLocation(plain.path());
        settle();
        QCOMPARE(selector->itemText(0), QStringLiteral("None"));
        QCOMPARE(selector->currentIndex(), 0);
        QCOMPARE(view->vc()->name(), QStringLiteral("None"));
        QCOMPARE(selector->toolTip(), QStringLiteral("No valid version control system found in this folder"));
        QVERIFY(rows(view).isEmpty());
        view->action(QStringLiteral("vc-status-normal"))->trigger();
        settle();
        QCOMPARE(rows(view), QStringList{"file.txt"});
        // Nothing can be done without version control
        select(view, {"file.txt"});
        QVERIFY(!enabled(view, "vc-commit"));
        QVERIFY(!enabled(view, "vc-update"));
        QVERIFY(enabled(view, "compare"));
    }

    void statusFilters()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        QCOMPARE(view->statusFilters(), (QStringList{"flatten", "modified"}));

        view->action(QStringLiteral("vc-status-unknown"))->trigger();
        settle();
        QVERIFY(rows(view).contains(QStringLiteral("untracked.txt")));
        QVERIFY(!rows(view).contains(QStringLiteral("debug.log")));
        QCOMPARE(MeldSettings::instance()->stringListValue(QStringLiteral("vc-status-filters")),
                 (QStringList{"flatten", "modified", "unknown"}));

        view->action(QStringLiteral("vc-status-ignored"))->trigger();
        settle();
        QVERIFY(rows(view).contains(QStringLiteral("debug.log")));

        // Without flattening, folders are shown with their contents, and
        // folders containing changes are expanded
        view->action(QStringLiteral("vc-status-unknown"))->trigger();
        view->action(QStringLiteral("vc-status-ignored"))->trigger();
        view->action(QStringLiteral("vc-status-normal"))->trigger();
        view->action(QStringLiteral("vc-flatten"))->trigger();
        settle();
        QCOMPARE(view->statusFilters(), (QStringList{"modified", "normal"}));
        QCOMPARE(rows(view), (QStringList{"dir", "  sub", "    d.txt", "  c.txt", "empty", "  (Empty)", ".gitignore",
                                          "a.txt", "b.txt", "clean.txt", "new.txt"}));
        QTreeView *tree = view->treeView();
        QVERIFY(tree->isExpanded(view->model()->indexOf(row(view, "sub"))));
        QCOMPARE(view->model()->state(row(view, "(Empty)"), 0), int(StateEmpty));

        // Other version control views use the changed filters when they
        // next refresh
        VcView *other = openView(root);
        QCOMPARE(other->statusFilters(), (QStringList{"modified", "normal"}));
        QVERIFY(!other->action(QStringLiteral("vc-flatten"))->isChecked());
    }

    void navigateChanges()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->action(QStringLiteral("vc-status-normal"))->trigger();
        view->action(QStringLiteral("vc-flatten"))->trigger();
        settle();

        QAction *next = view->action(QStringLiteral("next-change"));
        QAction *previous = view->action(QStringLiteral("previous-change"));
        const auto current = [view]() { return view->model()->node(view->treeView()->currentIndex())->panes[0].text; };
        QCOMPARE(current(), QFileInfo(root).fileName());
        QVERIFY(!previous->isEnabled());
        next->trigger();
        // Changes are in tree order, with folders first
        QCOMPARE(current(), QStringLiteral("d.txt"));
        QVERIFY(!previous->isEnabled());
        next->trigger();
        QCOMPARE(current(), QStringLiteral("a.txt"));
        QVERIFY(previous->isEnabled());
        next->trigger();
        next->trigger();
        QCOMPARE(current(), QStringLiteral("new.txt"));
        QVERIFY(!next->isEnabled());
        previous->trigger();
        QCOMPARE(current(), QStringLiteral("b.txt"));
    }

    void actionSensitivity()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->action(QStringLiteral("vc-status-unknown"))->trigger();
        settle();

        select(view, {QFileInfo(root).fileName()});
        QVERIFY(enabled(view, "vc-commit"));
        QVERIFY(enabled(view, "vc-update"));
        QVERIFY(enabled(view, "vc-push"));
        QVERIFY(!enabled(view, "vc-add"));
        QVERIFY(!enabled(view, "vc-remove"));
        QVERIFY(!enabled(view, "vc-revert"));
        QVERIFY(!enabled(view, "vc-delete-locally"));

        select(view, {"a.txt"});
        QVERIFY(enabled(view, "vc-revert"));
        QVERIFY(enabled(view, "vc-remove"));
        QVERIFY(enabled(view, "vc-unstage"));
        QVERIFY(enabled(view, "vc-add"));
        QVERIFY(enabled(view, "vc-delete-locally"));
        QVERIFY(!enabled(view, "vc-resolve"));

        select(view, {"untracked.txt"});
        QVERIFY(enabled(view, "vc-add"));
        QVERIFY(!enabled(view, "vc-commit"));
        QVERIFY(!enabled(view, "vc-revert"));
        QVERIFY(!enabled(view, "vc-unstage"));

        // Actions must be valid for all of the selection
        select(view, {"a.txt", "untracked.txt"});
        QVERIFY(enabled(view, "vc-add"));
        QVERIFY(!enabled(view, "vc-commit"));
        QCOMPARE(view->selectedFiles(), (QStringList{root + "/a.txt", root + "/untracked.txt"}));
    }

    // -----------------------------------------------------------------------
    // Comparisons

    void compareWithRepositoryVersion()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        const QModelIndex index = view->model()->indexOf(row(view, "a.txt"));
        view->treeView()->setCurrentIndex(index);
        Q_EMIT view->treeView()->activated(index);
        settle();

        // Comparisons from a version control view become the current tab
        auto *filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 2);
        QCOMPARE(filediff->tabTitle(), QStringLiteral("a.txt (repository, working)"));
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("a changed\n"));
        QCOMPARE(filediff->textBuffer(1)->data.file, root + "/a.txt");
        // The repository version is a read-only temporary file
        const QString temp = filediff->textBuffer(0)->data.file;
        QVERIFY(temp.startsWith(QDir::tempPath()));
        QVERIFY(!(QFile::permissions(temp) & QFile::WriteOwner));
        QVERIFY(!filediff->textBuffer(0)->data.writable());

        // Version control comparisons can't be swapped
        QVERIFY(filediff->action(QStringLiteral("swap-2-panes")));

        // The order of panes is a preference
        MeldSettings::instance()->setValue(QStringLiteral("vc-left-is-local"), true);
        window_->tabWidget()->setCurrentWidget(view);
        view->runDiff(root + "/a.txt");
        settle();
        filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QCOMPARE(filediff->tabTitle(), QStringLiteral("a.txt (working, repository)"));
        QCOMPARE(filediff->textBuffer(0)->data.file, root + "/a.txt");

        // Temporary files are removed when Meld exits
        cleanupVcTempFiles();
        QVERIFY(!QFileInfo::exists(temp));
    }

    void autoCompare()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        window_->appendVcview(root, true);
        settle();
        QStringList titles;
        for (MeldDoc *doc : window_->docs()) {
            if (qobject_cast<FileDiff *>(doc))
                titles.append(doc->tabTitle());
        }
        // Modified files that exist; removed and added files aren't compared
        QCOMPARE(titles, (QStringList{"a.txt (repository, working)", "d.txt (repository, working)"}));
    }

    void conflictMerge_data()
    {
        QTest::addColumn<QString>("order");
        QTest::addColumn<QString>("title");
        QTest::addColumn<QString>("left");
        QTest::addColumn<QString>("right");
        QTest::newRow("remote, merge, local")
            << "remote-merge-local" << "a.txt (remote, merge, local)" << "1\ntheirs\n3\n" << "1\nours\n3\n";
        QTest::newRow("local, merge, remote")
            << "local-merge-remote" << "a.txt (local, merge, remote)" << "1\nours\n3\n" << "1\ntheirs\n3\n";
    }

    void conflictMerge()
    {
        QFETCH(QString, order);
        QFETCH(QString, title);
        QFETCH(QString, left);
        QFETCH(QString, right);
        MeldSettings::instance()->setValue(QStringLiteral("vc-merge-file-order"), order);

        QTemporaryDir dir;
        const QString root = makeConflict(dir);
        VcView *view = openView(root);
        QCOMPARE(view->model()->text(row(view, "a.txt"), VcTreeModel::ColStatus), QStringLiteral("Conflict"));
        select(view, {"a.txt"});
        QVERIFY(enabled(view, "vc-resolve"));

        view->action(QStringLiteral("compare"))->trigger();
        settle();
        auto *filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 3);
        QCOMPARE(filediff->tabTitle(), title);
        QCOMPARE(filediff->textBuffer(0)->text(), left);
        QCOMPARE(filediff->textBuffer(2)->text(), right);
        // The merged file has the base text in conflicts, and is saved to
        // the conflicted file
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("1\n==== BASE ====\nbase\n==== BASE ====\n3\n"));
        QCOMPARE(filediff->textBuffer(1)->data.savefile, root + "/a.txt");

        // Saving the merge and closing offers to mark it as resolved
        replaceText(filediff->textBuffer(1), QStringLiteral("1\nresolved\n3\n"));
        filediff->setFocus();
        filediff->textView(1)->setFocus();
        filediff->action(QStringLiteral("save"))->trigger();
        settle();
        QCOMPARE(readFile(root + "/a.txt"), QByteArray("1\nresolved\n3\n"));
        QSignalSpy closed(filediff, &MeldDoc::closed);
        filediff->requestClose();
        QTRY_VERIFY(visibleDialog<QMessageBox>());
        QCOMPARE(visibleDialog<QMessageBox>()->text(), QStringLiteral("Mark conflict as resolved?"));
        QVERIFY(answer(QStringLiteral("Mark Resolved")));
        QTRY_COMPARE(closed.size(), 1);
        settle();

        QVERIFY(view->consoleText().contains(QStringLiteral("git add a.txt (in %1)").arg(root)));
        QCOMPARE(git({"diff", "--name-only", "--diff-filter=U"}, root), QString());
        // The view was refreshed after the command
        QCOMPARE(view->model()->text(row(view, "a.txt"), VcTreeModel::ColStatus), QStringLiteral("Modified"));
    }

    void closingUnsavedConflictDoesntResolve()
    {
        QTemporaryDir dir;
        const QString root = makeConflict(dir);
        VcView *view = openView(root);
        view->runDiff(root + "/a.txt");
        settle();
        auto *filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QVERIFY(filediff && filediff->numPanes() == 3);
        // The merge is saved to a different file than it was loaded from,
        // so it starts out modified, as in the original
        QVERIFY(filediff->textBuffer(1)->isModified());
        QVERIFY(!filediff->textBuffer(0)->isModified());

        QSignalSpy closed(filediff, &MeldDoc::closed);
        filediff->requestClose();
        QDialog *prompt = nullptr;
        QTRY_VERIFY((prompt = visibleDialog<QDialog>()));
        QVERIFY(clickButton(prompt, QStringLiteral("Discard")));
        QTRY_COMPARE(closed.size(), 1);
        QVERIFY(!visibleDialog<QMessageBox>());
        QVERIFY(!git({"diff", "--name-only", "--diff-filter=U"}, root).isEmpty());
    }

    void compareFolder()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->runDiff(root + "/dir");
        settle();
        // As in the original, a folder opens in a single-pane folder view
        QVERIFY(window_->currentDoc() != view);
        QCOMPARE(window_->currentDoc()->numPanes(), 1);
    }

    // -----------------------------------------------------------------------
    // Version control commands

    void addAndUnstage()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->action(QStringLiteral("vc-status-unknown"))->trigger();
        settle();

        select(view, {"untracked.txt"});
        view->action(QStringLiteral("vc-add"))->trigger();
        settle();
        QVERIFY(view->consoleText().contains(QStringLiteral("git add untracked.txt (in %1)\n").arg(root)));
        QCOMPARE(view->model()->text(row(view, "untracked.txt"), VcTreeModel::ColStatus),
                 QStringLiteral("Newly added"));

        select(view, {"untracked.txt"});
        view->action(QStringLiteral("vc-unstage"))->trigger();
        settle();
        QVERIFY(view->consoleText().contains(QStringLiteral("git restore --staged untracked.txt")));
        QCOMPARE(view->model()->text(row(view, "untracked.txt"), VcTreeModel::ColStatus),
                 QStringLiteral("Unversioned"));
    }

    void revertAndRemove()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);

        select(view, {"a.txt", "b.txt", "d.txt"});
        view->action(QStringLiteral("vc-revert"))->trigger();
        settle();
        QVERIFY(view->consoleText().contains(QStringLiteral("git checkout a.txt dir/sub/d.txt (in %1)").arg(root)));
        QVERIFY(view->consoleText().contains(QStringLiteral("git checkout HEAD b.txt (in %1)").arg(root)));
        QCOMPARE(readFile(root + "/a.txt"), QByteArray("a\n"));
        QCOMPARE(readFile(root + "/b.txt"), QByteArray("b\n"));
        QCOMPARE(readFile(root + "/dir/sub/d.txt"), QByteArray("d\n"));
        QCOMPARE(rows(view), QStringList{"new.txt"});

        // Removing files doesn't need confirmation
        view->action(QStringLiteral("vc-status-normal"))->trigger();
        settle();
        select(view, {"clean.txt"});
        view->action(QStringLiteral("vc-remove"))->trigger();
        settle();
        QVERIFY(!QFileInfo::exists(root + "/clean.txt"));
        QCOMPARE(view->model()->state(row(view, "clean.txt"), 0), int(StateRemoved));

        // Removing folders does
        view->action(QStringLiteral("vc-flatten"))->trigger();
        settle();
        select(view, {"dir"});
        view->action(QStringLiteral("vc-remove"))->trigger();
        QTRY_VERIFY(visibleDialog<QMessageBox>());
        QCOMPARE(visibleDialog<QMessageBox>()->text(), QStringLiteral("Remove folder and all its files?"));
        QVERIFY(answer(QStringLiteral("Cancel")));
        settle();
        QVERIFY(QFileInfo::exists(root + "/dir/c.txt"));
        select(view, {"dir"});
        view->action(QStringLiteral("vc-remove"))->trigger();
        QTRY_VERIFY(visibleDialog<QMessageBox>());
        QVERIFY(answer(QStringLiteral("Remove")));
        settle();
        QVERIFY(view->consoleText().contains(QStringLiteral("git rm -r dir")));
        QVERIFY(!QFileInfo::exists(root + "/dir/c.txt"));
    }

    void commit()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        MeldSettings::instance()->setValue(QStringLiteral("vc-break-commit-message"), true);
        MeldSettings::instance()->setValue(QStringLiteral("vc-commit-margin"), 70);
        VcView *view = openView(root);
        select(view, {QFileInfo(root).fileName()});
        // Ctrl+M commits
        QTest::keyClick(view->treeView(), Qt::Key_M, Qt::ControlModifier);
        CommitDialog *dialog = nullptr;
        QTRY_VERIFY((dialog = visibleDialog<CommitDialog>()));
        QCOMPARE(dialog->changedFiles(),
                 (QStringList{"a.txt", "b.txt", "dir/sub/d.txt", "new.txt"}));
        QVERIFY(dialog->messageEdit()->showRightMargin());
        QCOMPARE(dialog->messageEdit()->rightMarginPosition(), 70);

        const QString subject = QStringLiteral("Commit from the version control view");
        const QString body = QString(QStringLiteral("word ")).repeated(20).trimmed();
        dialog->messageEdit()->setPlainText(subject + QStringLiteral("\n\n") + body);
        dialog->accept();
        settle();

        // The message was broken at the margin, and remembered
        QString wrappedBody = body;
        wrappedBody[69] = QLatin1Char('\n');
        QCOMPARE(git({"log", "-1", "--format=%B"}, root).trimmed(), subject + QStringLiteral("\n\n") + wrappedBody);
        QCOMPARE(git({"status", "--porcelain"}, root), QStringLiteral("?? untracked.txt\n"));
        QVERIFY(view->consoleText().contains(QStringLiteral("git commit -m")));
        QVERIFY(rows(view).isEmpty());
        QCOMPARE(MeldSettings::instance()->stringListValue(QStringLiteral("commit-message-history")),
                 QStringList{subject + QStringLiteral("\n\n") + wrappedBody});

        // Previous messages can be reused
        select(view, {QFileInfo(root).fileName()});
        view->action(QStringLiteral("vc-commit"))->trigger();
        QTRY_VERIFY((dialog = visibleDialog<CommitDialog>()));
        QCOMPARE(dialog->previousLogs()->count(), 1);
        QCOMPARE(dialog->previousLogs()->itemText(0), subject);
        QCOMPARE(dialog->changedFiles(), QStringList{"No files will be committed"});
        Q_EMIT dialog->previousLogs()->activated(0);
        QCOMPARE(dialog->messageEdit()->toPlainText(), subject + QStringLiteral("\n\n") + wrappedBody);
        dialog->reject();
    }

    void failedCommandShowsTheConsole()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        QVERIFY(!view->consolePane()->isVisible());
        QVERIFY(!window_->viewAction(QStringLiteral("vc-console-visible"))->isChecked());

        // There's no remote to push to
        view->action(QStringLiteral("vc-push"))->trigger();
        QTRY_VERIFY(visibleDialog<QMessageBox>());
        QVERIFY(answer(QStringLiteral("Push commits")));
        settle();
        QVERIFY(view->consolePane()->isVisible());
        QVERIFY(window_->viewAction(QStringLiteral("vc-console-visible"))->isChecked());
        QVERIFY(MeldSettings::instance()->boolValue(QStringLiteral("vc-console-visible")));
        QVERIFY(view->consoleText().contains(QStringLiteral("git push (in %1)\n").arg(root)));
        QVERIFY(view->consoleText().contains(QStringLiteral("Exit code: 128")));

        // F9 toggles the console, and the clear action empties it
        window_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));
        QTest::keyClick(view->treeView(), Qt::Key_F9);
        QVERIFY(!view->consolePane()->isVisible());
        QVERIFY(!MeldSettings::instance()->boolValue(QStringLiteral("vc-console-visible")));
        view->action(QStringLiteral("clear-console"))->trigger();
        QCOMPARE(view->consoleText(), QString());
    }

    void deleteLocally()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->action(QStringLiteral("vc-status-unknown"))->trigger();
        settle();
        select(view, {"untracked.txt"});
        view->action(QStringLiteral("vc-delete-locally"))->trigger();
        settle();
        QVERIFY(!QFileInfo::exists(root + "/untracked.txt"));
        QVERIFY(!rows(view).contains(QStringLiteral("untracked.txt")));
    }

    void pushSummary()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        QTemporaryDir remote;
        git({"init", "-q", "--bare", remote.path()}, root);
        git({"remote", "add", "origin", remote.path()}, root);
        git({"push", "-q", "-u", "origin", "main"}, root);
        git({"commit", "-q", "-m", "Unpushed"}, root);
        VcView *view = openView(root);
        QCOMPARE(view->model()->text(view->model()->firstRoot(), VcTreeModel::ColOptions),
                 QStringLiteral("1 unpushed commit"));

        view->action(QStringLiteral("vc-push"))->trigger();
        QTRY_VERIFY(visibleDialog<QMessageBox>());
        QVERIFY(answer(QStringLiteral("Push commits")));
        settle();
        QCOMPARE(view->model()->text(view->model()->firstRoot(), VcTreeModel::ColOptions), QString());
    }

    void savingRefreshesTheView()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->action(QStringLiteral("vc-status-normal"))->trigger();
        view->action(QStringLiteral("vc-flatten"))->trigger();
        settle();
        QCOMPARE(view->model()->state(row(view, "clean.txt"), 0), int(StateNormal));

        // Saving a file in a comparison updates its state
        writeFile(root + "/clean.txt", "changed\n");
        view->onFileChanged(root + "/clean.txt");
        QCOMPARE(view->model()->state(row(view, "clean.txt"), 0), int(StateModified));
        QCOMPARE(view->model()->text(row(view, "clean.txt"), VcTreeModel::ColStatus), QStringLiteral("Modified"));
        writeFile(root + "/dir/c.txt", "changed\n");
        view->onFileChanged(root + "/dir/c.txt");
        QCOMPARE(view->model()->state(row(view, "c.txt"), 0), int(StateModified));
        // Similarly named files aren't confused
        QVERIFY(view->model()->state(row(view, "clean.txt"), 0) == int(StateModified));
    }

    void search()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        view->action(QStringLiteral("find"))->trigger();
        auto *entry = view->findChild<QLineEdit *>();
        QVERIFY(entry && entry->isVisible());
        QTest::keyClicks(entry, QStringLiteral("d.t"));
        QCOMPARE(view->model()->node(view->treeView()->currentIndex())->panes[0].text, QStringLiteral("d.txt"));
        QTest::keyClick(entry, Qt::Key_Escape);
        QVERIFY(!entry->isVisible());
    }

    void subfolderLocation()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root + "/dir");
        QCOMPARE(view->vc()->root(), root);
        QCOMPARE(view->location(), root + "/dir");
        QCOMPARE(rows(view), QStringList{"d.txt"});
        QCOMPARE(view->model()->text(row(view, "d.txt"), VcTreeModel::ColLocation), QStringLiteral("sub"));
    }

    // -----------------------------------------------------------------------
    // Window integration

    void openSingleFiles()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        QString error;
        // A single file is compared with its repository version
        QCOMPARE(window_->openPaths({root + "/a.txt"}, false, false, true, &error), nullptr);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        settle();
        auto *filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QVERIFY(filediff);
        QCOMPARE(filediff->tabTitle(), QStringLiteral("a.txt (repository, working)"));
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a\n"));

        // A conflicted file opens as a merge, and resolving works without a
        // version control view
        QTemporaryDir conflictDir;
        const QString conflict = makeConflict(conflictDir);
        MeldDoc *merge = window_->singleFileOpen(conflict + "/a.txt");
        settle();
        QCOMPARE(merge->numPanes(), 3);
        auto *mergeDiff = qobject_cast<FileDiff *>(merge);
        replaceText(mergeDiff->textBuffer(1), QStringLiteral("resolved\n"));
        mergeDiff->textView(1)->setFocus();
        mergeDiff->action(QStringLiteral("save"))->trigger();
        settle();
        mergeDiff->requestClose();
        QTRY_VERIFY(visibleDialog<QMessageBox>());
        QVERIFY(answer(QStringLiteral("Mark Resolved")));
        settle();
        QCOMPARE(git({"diff", "--name-only", "--diff-filter=U"}, conflict), QString());

        // Unversioned files are compared with a copy of themselves
        QTemporaryDir plain;
        writeFile(plain.filePath(QStringLiteral("plain.txt")), "plain\n");
        QCOMPARE(window_->openPaths({plain.filePath(QStringLiteral("plain.txt"))}, false, false, true, &error), nullptr);
        settle();
        filediff = qobject_cast<FileDiff *>(window_->docs().last());
        QCOMPARE(filediff->tabTitle(), QStringLiteral("plain.txt (repository, working)"));
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("plain\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("plain\n"));
    }

    void commandLine()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        window_.reset();
        MeldApp app;
        QCOMPARE(app.handleCommandLine({"meld-qt", "--auto-compare", "-L", "Label", root}), -1);
        MeldWindow *window = app.window();
        for (int i = 0; i < 200 && window->scheduler().tasksPending(); ++i) {
            window->runPendingTasks();
            QTest::qWait(5);
        }
        auto *view = qobject_cast<VcView *>(window->tabWidget()->widget(0));
        QVERIFY(view);
        QCOMPARE(view->location(), root);
        // Labels are only shown in the location bar
        QCOMPARE(view->tabTitle(), QFileInfo(root).fileName());
        // Auto-compare opened the modified files
        QCOMPARE(window->tabWidget()->count(), 3);
        for (MeldDoc *doc : window->docs()) {
            if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                for (int pane = 0; pane < filediff->numPanes(); ++pane)
                    filediff->textBuffer(pane)->setModified(false);
            }
        }
        window_ = std::make_unique<MeldWindow>();
    }

    void newComparisonTab()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        NewDiffTab *tab = window_->appendNewComparison();
        Q_EMIT tab->diffRequested(NewDiffTab::DiffType::Version, {root});
        QCoreApplication::processEvents();
        settle();
        QCOMPARE(window_->docs().size(), 1);
        auto *view = qobject_cast<VcView *>(window_->currentDoc());
        QVERIFY(view);
        QCOMPARE(view->location(), root);
    }

    void recentComparisons()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        const std::optional<Comparison> comparison = view->comparison();
        QVERIFY(comparison);
        QCOMPARE(comparison->type, RecentType::VersionControl);
        QCOMPARE(comparison->paths, QStringList{root});
        QCOMPARE(RecentComparisons::displayName(*comparison).left(16), QStringLiteral("Version control:"));

        auto *reopened = qobject_cast<VcView *>(window_->appendRecent(*comparison));
        QVERIFY(reopened);
        QCOMPARE(window_->currentDoc(), reopened);
        settle();
        QCOMPARE(reopened->location(), root);
    }

    void versionFilterMenu()
    {
        QTemporaryDir dir;
        const QString root = makeRepo(dir);
        VcView *view = openView(root);
        QToolButton *button = nullptr;
        for (QToolButton *b : window_->findChildren<QToolButton *>()) {
            if (b->text() == QLatin1String("Version Filters"))
                button = b;
        }
        QVERIFY(button);
        QVERIFY(button->isVisible());
        QStringList labels;
        for (QAction *a : button->menu()->actions())
            labels.append(a->isSeparator() ? QStringLiteral("-") : a->text());
        QCOMPARE(labels, (QStringList{"Modified", "Normal", "Unversioned", "Ignored", "-", "Flatten Folders"}));
        QCOMPARE(button->menu()->actions().first(), view->action(QStringLiteral("vc-status-modified")));

        // Other tabs show their own filters
        QTemporaryDir other;
        writeFile(other.filePath(QStringLiteral("x.txt")), "x\n");
        writeFile(other.filePath(QStringLiteral("y.txt")), "y\n");
        QString error;
        window_->openPaths({other.filePath(QStringLiteral("x.txt")), other.filePath(QStringLiteral("y.txt"))}, false,
                           false, true, &error);
        settle();
        QVERIFY(!button->isVisible());
    }
};

QTEST_MAIN(TestVcView)
#include "tst_vcview.moc"
