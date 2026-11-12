// Integration tests for folder comparisons: scanning, row states, filtering,
// copying and opening file comparisons.

#include "dirdiff.h"
#include "dirdifftree.h"
#include "filediff.h"
#include "meldwindow.h"
#include "msgarea.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QPointer>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;

class TestDirDiff : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;
    std::unique_ptr<MeldWindow> window_;
    int treeCounter_ = 0;

    void writeFile(const QString &path, const QByteArray &content)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Couldn't write test file");
        file.write(content);
    }

    //! Create a pair of folders with all the basic kinds of differences
    QStringList makeFolders()
    {
        const QString base = dir_.filePath(QStringLiteral("tree%1").arg(++treeCounter_));
        const QString a = base + QStringLiteral("/dirA");
        const QString b = base + QStringLiteral("/dirB");
        writeFile(a + QStringLiteral("/same.txt"), "same\n");
        writeFile(b + QStringLiteral("/same.txt"), "same\n");
        writeFile(a + QStringLiteral("/changed.txt"), "left\n");
        writeFile(b + QStringLiteral("/changed.txt"), "right\n");
        writeFile(a + QStringLiteral("/a_only.txt"), "only in A\n");
        writeFile(b + QStringLiteral("/b_only.txt"), "only in B\n");
        writeFile(a + QStringLiteral("/sub/inner.txt"), "x\n");
        writeFile(b + QStringLiteral("/sub/inner.txt"), "y\n");
        writeFile(b + QStringLiteral("/newdir/file.txt"), "z\n");
        // Hidden by the default "Backups" filename filter
        writeFile(a + QStringLiteral("/notes.txt~"), "backup\n");
        return {a, b};
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

    DirDiff *open(const QStringList &folders, bool autoCompare = false)
    {
        DirDiff *doc = window_->appendDirdiff(folders, autoCompare);
        window_->tabWidget()->setCurrentWidget(doc);
        settle();
        return doc;
    }

    //! Find the row for a path relative to the compared folders
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

    void select(DirDiff *doc, int pane, const QString &relative)
    {
        TreeNode *node = find(doc, relative);
        QVERIFY2(node, qPrintable(relative));
        DirTreeView *view = doc->treeView(pane);
        view->setFocus();
        view->setCurrentIndex(doc->model()->indexOf(node));
        QCoreApplication::processEvents();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        MeldSettings::useSettingsFile(dir_.filePath(QStringLiteral("settings.ini")));
    }

    void init()
    {
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
        window_->activateWindow();
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

    void rowStates()
    {
        DirDiff *doc = open(makeFolders());
        QVERIFY(!doc->scanInProgress());

        QCOMPARE(states(doc, QStringLiteral("same.txt")), (QList<int>{StateNormal, StateNormal}));
        QCOMPARE(states(doc, QStringLiteral("changed.txt")), (QList<int>{StateModified, StateModified}));
        QCOMPARE(states(doc, QStringLiteral("a_only.txt")), (QList<int>{StateNew, StateNonExist}));
        QCOMPARE(states(doc, QStringLiteral("b_only.txt")), (QList<int>{StateNonExist, StateNew}));
        QCOMPARE(states(doc, QStringLiteral("sub")), (QList<int>{StateNormal, StateNormal}));
        QCOMPARE(states(doc, QStringLiteral("sub/inner.txt")), (QList<int>{StateModified, StateModified}));
        QCOMPARE(states(doc, QStringLiteral("newdir")), (QList<int>{StateNonExist, StateNew}));
        QCOMPARE(states(doc, QStringLiteral("newdir/file.txt")), (QList<int>{StateNonExist, StateNew}));
        QVERIFY(!find(doc, QStringLiteral("notes.txt~")));

        const std::optional<Comparison> comparison = doc->comparison();
        QVERIFY(comparison);
        QCOMPARE(comparison->type, RecentType::Folder);
        QCOMPARE(comparison->paths.size(), 2);
    }

    void folderRowsAreExpandedWhenTheyContainDifferences()
    {
        DirDiff *doc = open(makeFolders());
        const QModelIndex sub = doc->model()->indexOf(find(doc, QStringLiteral("sub")));
        QVERIFY(doc->treeView(0)->isExpanded(sub));
        QVERIFY(doc->treeView(1)->isExpanded(sub));
    }

    void statusFilters()
    {
        DirDiff *doc = open(makeFolders());
        QVERIFY(find(doc, QStringLiteral("same.txt")));

        doc->action(QStringLiteral("folder-status-same"))->trigger();
        settle();
        QVERIFY(!find(doc, QStringLiteral("same.txt")));
        QVERIFY(find(doc, QStringLiteral("changed.txt")));

        doc->action(QStringLiteral("folder-status-new"))->trigger();
        settle();
        QVERIFY(!find(doc, QStringLiteral("a_only.txt")));
        QVERIFY(find(doc, QStringLiteral("changed.txt")));

        doc->action(QStringLiteral("folder-status-same"))->trigger();
        doc->action(QStringLiteral("folder-status-new"))->trigger();
        settle();
        QVERIFY(find(doc, QStringLiteral("same.txt")));
        QVERIFY(find(doc, QStringLiteral("a_only.txt")));
    }

    void filenameFilters()
    {
        DirDiff *doc = open(makeFolders());
        QVERIFY(!find(doc, QStringLiteral("notes.txt~")));

        // Disable the "Backups" filter, which is the first default filter
        QAction *backups = doc->action(QStringLiteral("folder-custom-filter-0"));
        QVERIFY(backups);
        QVERIFY(backups->isChecked());
        backups->trigger();
        settle();
        QCOMPARE(states(doc, QStringLiteral("notes.txt~")), (QList<int>{StateNew, StateNonExist}));
    }

    void copyFileToTheOtherSide()
    {
        const QStringList folders = makeFolders();
        DirDiff *doc = open(folders);
        select(doc, 0, QStringLiteral("a_only.txt"));
        QVERIFY(doc->action(QStringLiteral("folder-copy-right"))->isEnabled());
        doc->action(QStringLiteral("folder-copy-right"))->trigger();
        settle();

        QFile copied(folders[1] + QStringLiteral("/a_only.txt"));
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("only in A\n"));
        QCOMPARE(states(doc, QStringLiteral("a_only.txt")), (QList<int>{StateNormal, StateNormal}));
    }

    void copyModifiedFileOverwrites()
    {
        const QStringList folders = makeFolders();
        DirDiff *doc = open(folders);
        select(doc, 1, QStringLiteral("changed.txt"));
        doc->action(QStringLiteral("folder-copy-left"))->trigger();
        settle();

        QFile copied(folders[0] + QStringLiteral("/changed.txt"));
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll(), QByteArray("right\n"));
        QCOMPARE(states(doc, QStringLiteral("changed.txt")), (QList<int>{StateNormal, StateNormal}));
    }

    void compareSelectedFiles()
    {
        const QStringList folders = makeFolders();
        DirDiff *doc = open(folders);
        select(doc, 0, QStringLiteral("changed.txt"));
        doc->action(QStringLiteral("folder-compare"))->trigger();
        settle();

        QCOMPARE(window_->docs().size(), 2);
        auto *filediff = qobject_cast<FileDiff *>(window_->docs().last());
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("left\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("right\n"));
    }

    void autoCompareOpensModifiedFiles()
    {
        open(makeFolders(), true);
        QStringList compared;
        for (MeldDoc *doc : window_->docs()) {
            if (auto *filediff = qobject_cast<FileDiff *>(doc))
                compared.append(QFileInfo(filediff->textBuffer(0)->data.file).fileName());
        }
        compared.sort();
        QCOMPARE(compared, (QStringList{QStringLiteral("changed.txt"), QStringLiteral("inner.txt")}));
    }

    void identicalFolders()
    {
        const QString base = dir_.filePath(QStringLiteral("identical"));
        writeFile(base + QStringLiteral("/a/file.txt"), "same\n");
        writeFile(base + QStringLiteral("/b/file.txt"), "same\n");
        DirDiff *doc = open({base + QStringLiteral("/a"), base + QStringLiteral("/b")});
        QCOMPARE(states(doc, QStringLiteral("file.txt")), (QList<int>{StateNormal, StateNormal}));
        QVERIFY(doc->messageArea(0)->hasMessage());
    }

    void threeWayComparison()
    {
        const QString base = dir_.filePath(QStringLiteral("threeway"));
        const QStringList folders{base + QStringLiteral("/a"), base + QStringLiteral("/b"), base + QStringLiteral("/c")};
        writeFile(folders[0] + QStringLiteral("/all.txt"), "1\n");
        writeFile(folders[1] + QStringLiteral("/all.txt"), "1\n");
        writeFile(folders[2] + QStringLiteral("/all.txt"), "1\n");
        writeFile(folders[0] + QStringLiteral("/two.txt"), "1\n");
        writeFile(folders[2] + QStringLiteral("/two.txt"), "2\n");
        writeFile(folders[1] + QStringLiteral("/middle.txt"), "1\n");

        DirDiff *doc = open(folders);
        QCOMPARE(doc->model()->ntree(), 3);
        QCOMPARE(states(doc, QStringLiteral("all.txt")), (QList<int>{StateNormal, StateNormal, StateNormal}));
        QCOMPARE(states(doc, QStringLiteral("two.txt")), (QList<int>{StateModified, StateNonExist, StateModified}));
        QCOMPARE(states(doc, QStringLiteral("middle.txt")), (QList<int>{StateNonExist, StateNew, StateNonExist}));
    }

    void blankPane()
    {
        const QStringList folders = makeFolders();
        DirDiff *doc = open({folders[0], QString()});
        QCOMPARE(states(doc, QStringLiteral("same.txt")), (QList<int>{StateNew, StateNonExist}));
    }

    void closingWhileScanning()
    {
        const QString base = dir_.filePath(QStringLiteral("large"));
        for (int d = 0; d < 10; ++d) {
            for (int f = 0; f < 20; ++f) {
                const QString name = QStringLiteral("/d%1/f%2.txt").arg(d).arg(f);
                writeFile(base + QStringLiteral("/a") + name, "same\n");
                writeFile(base + QStringLiteral("/b") + name, f % 3 ? "same\n" : "different\n");
            }
        }
        for (int iterations : {0, 1, 3, 10, 50}) {
            DirDiff *doc = window_->appendDirdiff({base + QStringLiteral("/a"), base + QStringLiteral("/b")});
            for (int i = 0; i < iterations && window_->scheduler().tasksPending(); ++i)
                window_->scheduler().iteration();
            QPointer<DirDiff> guard(doc);
            doc->requestClose();
            QTRY_VERIFY(!guard);
        }
        settle();
        QVERIFY(!window_->hasPages());
    }

    void navigateChanges()
    {
        DirDiff *doc = open(makeFolders());
        doc->treeView(0)->setFocus();
        doc->treeView(0)->setCurrentIndex(doc->model()->indexOf(doc->model()->firstRoot()));
        QCoreApplication::processEvents();

        doc->action(QStringLiteral("next-change"))->trigger();
        settle();
        const TreeNode *current = doc->model()->node(doc->treeView(0)->currentIndex());
        QVERIFY(current);
        QVERIFY(doc->model()->state(current, 0) != StateNormal || doc->model()->state(current, 1) != StateNormal);
    }
};

QTEST_MAIN(TestDirDiff)
#include "tst_dirdiff.moc"
