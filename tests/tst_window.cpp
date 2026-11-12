// Tests for the main window (opening comparisons, tab and action handling)
// and the command line handling of meldapp.py.

#include "dirdiff.h"
#include "filediff.h"
#include "meldapp.h"
#include "meldwindow.h"
#include "newdifftab.h"
#include "recent.h"
#include "settings.h"
#ifdef MELD_SINGLE_INSTANCE
#include "singleinstance.h"
#endif
#include "sourceview.h"
#include "vcview.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QToolButton>
#include <QFile>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QSharedMemory>
#include <QStandardPaths>
#include <QThread>

using namespace meld;

class TestWindow : public QObject {
    Q_OBJECT

private:
    // Not in the system temporary folder, as comparisons of temporary files
    // aren't added to the recent comparisons
    QTemporaryDir dir_{QDir::currentPath() + QStringLiteral("/tst_window-XXXXXX")};
    std::unique_ptr<MeldWindow> window_;
    int fileCounter_ = 0;

    QString writeFile(const QByteArray &content)
    {
        const QString path = dir_.filePath(QStringLiteral("file%1.txt").arg(++fileCounter_));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Couldn't write test file");
        file.write(content);
        return path;
    }

    QString makeFolder()
    {
        const QString path = dir_.filePath(QStringLiteral("folder%1").arg(++fileCounter_));
        QDir().mkpath(path);
        QFile file(path + QStringLiteral("/file.txt"));
        if (file.open(QIODevice::WriteOnly))
            file.write("content\n");
        return path;
    }

    void settle(MeldWindow *window)
    {
        for (int i = 0; i < 100; ++i) {
            window->runPendingTasks();
            QCoreApplication::processEvents();
            if (!window->scheduler().tasksPending())
                break;
        }
    }

    static void discardChanges(MeldWindow *window)
    {
        for (MeldDoc *doc : window->docs()) {
            if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                for (int pane = 0; pane < filediff->numPanes(); ++pane)
                    filediff->textBuffer(pane)->setModified(false);
            }
        }
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
    }

    void cleanup()
    {
        discardChanges(window_.get());
        window_.reset();
    }

    void aboutWindow()
    {
        MeldApp app;
        QCOMPARE(app.handleCommandLine({"meld-qt"}), -1);
        app.showAbout();

        QPointer<QDialog> dialog;
        for (QDialog *candidate : app.window()->findChildren<QDialog *>()) {
            if (candidate->windowTitle() == QLatin1String("About Meld Qt"))
                dialog = candidate;
        }
        QVERIFY(dialog);

        QStringList texts;
        for (const QLabel *label : dialog->findChildren<QLabel *>()) {
            texts.append(label->text());
            QVERIFY(label->pixmap().isNull());
        }
        QCOMPARE(texts, (QStringList{"Meld Qt", MELD_VERSION, "Visual diff and merge tool",
                                     "Built with Qt " QT_VERSION_STR}));

        // Only Close, which closes it
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        QVERIFY(buttons);
        QCOMPARE(buttons->buttons().size(), 1);
        QCOMPARE(dialog->findChildren<QAbstractButton *>().size(), 1);
        QPushButton *close = buttons->button(QDialogButtonBox::Close);
        QVERIFY(close);
        close->click();
        QTRY_VERIFY(!dialog);
    }

    void openSingleFileComparesWithItsRepositoryVersion()
    {
        // A file outside of any repository is compared with a copy of
        // itself; the system's temporary folder isn't version controlled
        QTemporaryDir plain;
        const QString path = plain.filePath(QStringLiteral("plain.txt"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("a\n");
        file.close();

        QString error;
        // As in the original, no tab is returned, so command line labels
        // aren't applied to the comparison
        QCOMPARE(window_->openPaths({path}, false, false, true, &error), nullptr);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        auto *filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QVERIFY(filediff);
        QCOMPARE(filediff->numPanes(), 2);
        settle(window_.get());
        QCOMPARE(filediff->tabTitle(), QStringLiteral("plain.txt (repository, working)"));
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a\n"));
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("a\n"));
        QCOMPARE(filediff->textBuffer(1)->data.file, path);
    }

    void openSingleFolderShowsVersionControl()
    {
        QTemporaryDir plain;
        QString error;
        auto *vcview = qobject_cast<VcView *>(window_->openPaths({plain.path()}, false, false, true, &error));
        QVERIFY2(vcview, qPrintable(error));
        QCOMPARE(window_->currentDoc(), vcview);
        settle(window_.get());
        QCOMPARE(vcview->location(), plain.path());
        QCOMPARE(vcview->vc()->name(), QStringLiteral("None"));
    }

    void openFilesAndFolders_data()
    {
        QTest::addColumn<int>("files");
        QTest::addColumn<int>("folders");
        QTest::addColumn<QString>("expectedType");
        QTest::addColumn<int>("expectedPanes");

        QTest::newRow("two files") << 2 << 0 << "meld::FileDiff" << 2;
        QTest::newRow("three files") << 3 << 0 << "meld::FileDiff" << 3;
        QTest::newRow("two folders") << 0 << 2 << "meld::DirDiff" << 2;
        QTest::newRow("three folders") << 0 << 3 << "meld::DirDiff" << 3;
    }

    void openFilesAndFolders()
    {
        QFETCH(int, files);
        QFETCH(int, folders);
        QFETCH(QString, expectedType);
        QFETCH(int, expectedPanes);

        QStringList paths;
        for (int i = 0; i < files; ++i)
            paths.append(writeFile(QByteArray::number(i) + '\n'));
        for (int i = 0; i < folders; ++i)
            paths.append(makeFolder());

        QString error;
        MeldDoc *doc = window_->openPaths(paths, false, false, true, &error);
        QVERIFY2(doc, qPrintable(error));
        QCOMPARE(QString::fromLatin1(doc->metaObject()->className()), expectedType);
        settle(window_.get());
        QCOMPARE(doc->numPanes(), expectedPanes);
        QCOMPARE(window_->currentDoc(), doc);
        QCOMPARE(window_->tabWidget()->tabText(window_->tabWidget()->indexOf(doc)), doc->tabTitle());
    }

    void openErrors_data()
    {
        QTest::addColumn<QStringList>("paths");
        QTest::addColumn<QString>("expectedError");

        const QString file = writeFile("a\n");
        const QString folder = makeFolder();
        QTest::newRow("mixed") << QStringList{file, folder} << "Cannot compare a mixture of files and directories";
        QTest::newRow("too many") << QStringList{file, file, file, file} << "too many arguments (wanted 0-3, got 4)";
        QTest::newRow("missing") << QStringList{dir_.filePath(QStringLiteral("missing"))}
                                 << "Cannot compare a non-existent file";
    }

    void openErrors()
    {
        QFETCH(QStringList, paths);
        QFETCH(QString, expectedError);

        QString error;
        QVERIFY(!window_->openPaths(paths, false, false, true, &error));
        QCOMPARE(error, expectedError);
        QVERIFY(!window_->hasPages());
    }

    void autoMergeNeedsThreeFiles()
    {
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, window_->appendFilemerge({writeFile("a"), writeFile("b")}));
    }

    void compareButtonIsSuggestedWhenUsable()
    {
        NewDiffTab *tab = window_->appendNewComparison();
        QPushButton *compare = nullptr;
        for (QPushButton *button : tab->findChildren<QPushButton *>()) {
            if (button->text() == QLatin1String("C&ompare"))
                compare = button;
        }
        QVERIFY(compare);
        QVERIFY(!compare->isEnabled());
        // Disabled, it looks like any other disabled button...
        QCOMPARE(compare->palette().color(QPalette::Disabled, QPalette::Button),
                 tab->palette().color(QPalette::Disabled, QPalette::Button));
        QCOMPARE(compare->palette().color(QPalette::Disabled, QPalette::ButtonText),
                 tab->palette().color(QPalette::Disabled, QPalette::ButtonText));

        // ...and once it can be used, it has the accent colours (GTK's
        // suggested-action)
        for (QToolButton *button : tab->findChildren<QToolButton *>()) {
            if (button->text() == QLatin1String("File"))
                button->click();
        }
        QVERIFY(compare->isEnabled());
        QCOMPARE(compare->palette().color(QPalette::Active, QPalette::Button),
                 tab->palette().color(QPalette::Active, QPalette::Highlight));
        QCOMPARE(compare->palette().color(QPalette::Active, QPalette::ButtonText),
                 tab->palette().color(QPalette::Active, QPalette::HighlightedText));

        // The colours follow palette changes
        const QPalette original = QApplication::palette();
        QPalette changed = original;
        changed.setColor(QPalette::Highlight, QColor(0x12, 0x34, 0x56));
        QApplication::setPalette(changed);
        QTRY_COMPARE(compare->palette().color(QPalette::Active, QPalette::Button), QColor(0x12, 0x34, 0x56));
        QApplication::setPalette(original);
    }

    void newComparisonTab()
    {
        NewDiffTab *tab = window_->appendNewComparison();
        QCOMPARE(window_->currentDoc(), tab);

        const QStringList files{writeFile("a\n"), writeFile("b\n")};
        Q_EMIT tab->diffRequested(NewDiffTab::DiffType::File, files);
        QCoreApplication::processEvents();
        settle(window_.get());

        QCOMPARE(window_->docs().size(), 1);
        auto *filediff = qobject_cast<FileDiff *>(window_->currentDoc());
        QVERIFY(filediff);
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("b\n"));
        QCOMPARE(RecentComparisons::load().value(0).paths, files);
    }

    void proxyActionsFollowTheCurrentTab()
    {
        QString error;
        auto *filediff = qobject_cast<FileDiff *>(
            window_->openPaths({writeFile("a\n"), writeFile("b\n")}, false, false, true, &error));
        auto *dirdiff =
            qobject_cast<DirDiff *>(window_->openPaths({makeFolder(), makeFolder()}, false, false, true, &error));
        QVERIFY(filediff && dirdiff);
        settle(window_.get());

        ProxyAction *push = window_->viewAction(QStringLiteral("file-push-left"));
        ProxyAction *copy = window_->viewAction(QStringLiteral("folder-copy-left"));
        QVERIFY(push && copy);

        window_->tabWidget()->setCurrentWidget(dirdiff);
        QCOMPARE(copy->target(), dirdiff->action(QStringLiteral("folder-copy-left")));
        QVERIFY(!push->target());
        QVERIFY(!push->isEnabled());
        // Shared accelerators only belong to the current tab's action
        QVERIFY(push->shortcuts().isEmpty());
        QCOMPARE(copy->shortcuts(), (QList<QKeySequence>{QKeySequence(Qt::ALT | Qt::Key_Left)}));

        window_->tabWidget()->setCurrentWidget(filediff);
        QCOMPARE(push->target(), filediff->action(QStringLiteral("file-push-left")));
        QVERIFY(!copy->target());
        QVERIFY(copy->shortcuts().isEmpty());
        QCOMPARE(push->shortcuts(), (QList<QKeySequence>{QKeySequence(Qt::ALT | Qt::Key_Left)}));

        // Proxies forward triggers and state
        QSignalSpy triggered(filediff->action(QStringLiteral("refresh")), &QAction::triggered);
        window_->viewAction(QStringLiteral("refresh"))->trigger();
        QCOMPARE(triggered.count(), 1);

        ProxyAction *overview = window_->viewAction(QStringLiteral("show-overview-map"));
        QVERIFY(overview->isCheckable());
        const bool shown = filediff->action(QStringLiteral("show-overview-map"))->isChecked();
        QCOMPARE(overview->isChecked(), shown);
        overview->trigger();
        QCOMPARE(filediff->action(QStringLiteral("show-overview-map"))->isChecked(), !shown);
        filediff->action(QStringLiteral("show-overview-map"))->trigger();
        QCOMPARE(overview->isChecked(), shown);
    }

    void keyboardShortcutsReachTheCurrentTab()
    {
        const QString right = writeFile("a\nB\nc\nD\n");
        QString error;
        auto *filediff = qobject_cast<FileDiff *>(
            window_->openPaths({writeFile("a\nb\nc\nd\n"), right}, false, false, true, &error));
        QVERIFY(filediff);
        settle(window_.get());
        window_->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(window_.get()));

        SourceView *view = filediff->textView(1);
        filediff->moveCursor(1, 0);
        QTRY_VERIFY(view->hasFocus());

        // Alt+Down goes to the next change
        QTest::keyClick(view, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(view->textCursor().blockNumber(), 1);
        QTest::keyClick(view, Qt::Key_Down, Qt::AltModifier);
        QCOMPARE(view->textCursor().blockNumber(), 3);

        // Alt+Left pushes the current change to the left pane
        QTest::keyClick(view, Qt::Key_Left, Qt::AltModifier);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a\nb\nc\nD\n"));

        // Typing, then Ctrl+S saves the focused pane
        QTest::keyClicks(view, QStringLiteral("x"));
        QTest::keyClick(view, Qt::Key_S, Qt::ControlModifier);
        QFile file(right);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("a\nB\nc\nxD\n"));
        QVERIFY(!filediff->textBuffer(1)->isModified());

        // Ctrl+Z undoes across panes, most recent first
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(filediff->textBuffer(1)->text(), QStringLiteral("a\nB\nc\nD\n"));
        QTest::keyClick(view, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(filediff->textBuffer(0)->text(), QStringLiteral("a\nb\nc\nd\n"));
    }

    void tabNavigation()
    {
        QString error;
        MeldDoc *first = window_->openPaths({writeFile("a\n"), writeFile("b\n")}, false, false, true, &error);
        MeldDoc *second = window_->openPaths({writeFile("c\n"), writeFile("d\n")}, false, false, true, &error);
        QVERIFY(first && second);
        QCOMPARE(window_->currentDoc(), second);

        window_->windowAction(QStringLiteral("win.previous-tab"))->trigger();
        QCOMPARE(window_->currentDoc(), first);
        window_->windowAction(QStringLiteral("win.next-tab"))->trigger();
        QCOMPARE(window_->currentDoc(), second);
        window_->windowAction(QStringLiteral("win.next-tab"))->trigger();
        QCOMPARE(window_->currentDoc(), second);

        window_->windowAction(QStringLiteral("win.move-tab-left"))->trigger();
        QCOMPARE(window_->tabWidget()->indexOf(second), 0);
        QCOMPARE(window_->currentDoc(), second);
    }

    void closingTheLastTabLeavesAnEmptyWindow()
    {
        QString error;
        MeldDoc *doc = window_->openPaths({writeFile("a\n"), writeFile("b\n")}, false, false, true, &error);
        QVERIFY(doc);
        window_->windowAction(QStringLiteral("win.close"))->trigger();
        QVERIFY(!window_->hasPages());
        QVERIFY(!window_->windowAction(QStringLiteral("win.close"))->isEnabled());
    }

    void closingTheWindowClosesAllTabs()
    {
        QString error;
        QVERIFY(window_->openPaths({writeFile("a\n"), writeFile("b\n")}, false, false, true, &error));
        QVERIFY(window_->openPaths({makeFolder(), makeFolder()}, false, false, true, &error));
        settle(window_.get());
        QCOMPARE(window_->docs().size(), 2);

        window_->close();
        QTRY_VERIFY(!window_->hasPages());
        QTRY_VERIFY(!window_->isVisible());
    }

    void recentComparisonsAreRecorded()
    {
        RecentComparisons::clear();
        const QStringList files{writeFile("a\n"), writeFile("b\n")};
        QString error;
        QVERIFY(window_->openPaths(files, false, false, true, &error));
        const QList<Comparison> recent = RecentComparisons::load();
        QCOMPARE(recent.size(), 1);
        QCOMPARE(recent[0].type, RecentType::File);
        QCOMPARE(recent[0].paths, files);

        // Re-opening moves the comparison to the front without duplicating it
        const QStringList folders{makeFolder(), makeFolder()};
        QVERIFY(window_->openPaths(folders, false, false, true, &error));
        QVERIFY(window_->openPaths(files, false, false, true, &error));
        const QList<Comparison> updated = RecentComparisons::load();
        QCOMPARE(updated.size(), 2);
        QCOMPARE(updated[0].paths, files);
        QCOMPARE(updated[1].type, RecentType::Folder);

        MeldDoc *reopened = window_->appendRecent(updated[1]);
        QVERIFY(qobject_cast<DirDiff *>(reopened));
    }

    void comparisonFiles()
    {
        // Saved comparisons from the original Meld
        const QStringList files{writeFile("a\n"), writeFile("b\n")};
        const QString path = dir_.filePath(QStringLiteral("saved.meldcmp"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[Comparison]\ntype = File\nuris = " + QUrl::fromLocalFile(files[0]).toEncoded() + ';'
                   + QUrl::fromLocalFile(files[1]).toEncoded() + "\n\n");
        file.close();

        const Comparison comparison = RecentComparisons::readComparisonFile(path);
        QCOMPARE(comparison.type, RecentType::File);
        QCOMPARE(comparison.paths, files);

        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 RecentComparisons::readComparisonFile(dir_.filePath(QStringLiteral("missing.meldcmp"))));
    }

    void parseCommandLine_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<int>("exitStatus");
        QTest::addColumn<QString>("expectedOutput");

        QTest::newRow("no arguments") << QStringList{} << -1 << "";
        QTest::newRow("files") << QStringList{"a", "b", "c"} << -1 << "";
        QTest::newRow("too many") << QStringList{"a", "b", "c", "d"} << 2 << "too many arguments (wanted 0-3, got 4)";
        QTest::newRow("auto-merge two files") << QStringList{"--auto-merge", "a", "b"} << 2
                                              << "can’t auto-merge less than 3 files";
        QTest::newRow("diff without files") << QStringList{"--diff"} << 2
                                            << "wrong number of arguments supplied to --diff";
        QTest::newRow("diff with too many files")
            << QStringList{"--diff", "a", "b", "c", "d"} << 2 << "wrong number of arguments supplied to --diff";
        QTest::newRow("unknown option") << QStringList{"--bogus"} << 2 << "no such option: --bogus";
        QTest::newRow("unknown short option") << QStringList{"-x"} << 2 << "no such option: -x";
        QTest::newRow("missing value") << QStringList{"-o"} << 2 << "-o option requires an argument";
        QTest::newRow("unexpected value") << QStringList{"--auto-merge=yes"} << 2
                                          << "--auto-merge option does not take a value";
        QTest::newRow("ambiguous") << QStringList{"--auto"} << 2 << "ambiguous option: --auto";
        QTest::newRow("help") << QStringList{"--help"} << 0 << "--auto-compare";
        QTest::newRow("short help") << QStringList{"-h"} << 0 << "Start a 2- or 3-way folder comparison";
        QTest::newRow("version") << QStringList{"--version"} << 0 << "meld-qt " MELD_VERSION;
        QTest::newRow("patch") << QStringList{"--patch", "fix.patch", "a"} << -1 << "";
        QTest::newRow("patch help") << QStringList{"--help"} << 0 << "--patch=PATCH";
        QTest::newRow("patch usage") << QStringList{"--help"} << 0 << "Compare with a patch applied";
        QTest::newRow("patch without a file") << QStringList{"--patch", "fix.patch"} << 2
                                              << "--patch needs one file or folder to apply the patch to (got 0)";
        QTest::newRow("patch with two files") << QStringList{"--patch=fix.patch", "a", "b"} << 2
                                              << "--patch needs one file or folder to apply the patch to (got 2)";
        QTest::newRow("patch without a value") << QStringList{"a", "--patch"} << 2 << "--patch option requires an argument";
        QTest::newRow("patch and auto-merge") << QStringList{"--auto-merge", "--patch", "p", "a"} << 2
                                              << "can’t auto-merge with a patch";
        QTest::newRow("patch and comparison file") << QStringList{"--comparison-file", "c", "--patch", "p", "a"} << 2
                                                   << "can’t load a comparison file with a patch";
    }

    void parseCommandLine()
    {
        QFETCH(QStringList, arguments);
        QFETCH(int, exitStatus);
        QFETCH(QString, expectedOutput);

        const CommandLineParseResult result = meld::parseCommandLine(arguments);
        QCOMPARE(result.shouldExit ? result.exitStatus : -1, exitStatus);
        if (!expectedOutput.isEmpty())
            QVERIFY2(result.output.contains(expectedOutput), qPrintable(result.output));
        if (exitStatus == 2)
            QVERIFY(result.output.startsWith(QLatin1String("Usage:")));
    }

    void parseCommandLineOptions()
    {
        const CommandLineParseResult result = meld::parseCommandLine(
            {"-L", "Mine", "--label=Theirs", "-ao", "out.txt", "--newtab", "left", "right", "--diff", "c", "d",
             "--diff", "e", "f", "g", "--", "-dash"});
        QVERIFY(!result.shouldExit);
        const CommandLineOptions &options = result.options;
        QCOMPARE(options.labels, (QStringList{"Mine", "Theirs"}));
        QVERIFY(options.autoCompare);
        QVERIFY(options.newTab);
        QVERIFY(!options.autoMerge);
        QCOMPARE(options.outFile, QStringLiteral("out.txt"));
        QCOMPARE(options.diffs, (QList<QStringList>{{"c", "d"}, {"e", "f", "g"}}));
        QCOMPARE(options.args, (QStringList{"left", "right", "-dash"}));
    }

#ifdef MELD_SINGLE_INSTANCE
    void singleInstanceOptions()
    {
        QVERIFY(!meld::parseCommandLine({"a", "b"}).options.singleInstance);
        QCOMPARE(meld::parseCommandLine({"--single-instance", "a", "b"}).options.singleInstance, true);
        QCOMPARE(meld::parseCommandLine({"--no-single-instance"}).options.singleInstance, false);
        QCOMPARE(meld::parseCommandLine({"--single-instance", "--no-single-instance"}).options.singleInstance,
                 false);

        // The setting decides, unless the command line overrides it for
        // this invocation
        const auto forward = [](const QStringList &arguments, bool setting) {
            return shouldForwardCommandLine(meld::parseCommandLine(arguments).options, setting);
        };
        const auto receive = [](const QStringList &arguments, bool setting) {
            return shouldReceiveCommandLines(meld::parseCommandLine(arguments).options, setting);
        };
        QVERIFY(!forward({"a", "b"}, false));
        QVERIFY(forward({"a", "b"}, true));
        QVERIFY(forward({"--single-instance", "a", "b"}, false));
        QVERIFY(!forward({"--no-single-instance", "a", "b"}, true));
        QVERIFY(!forward({"--newtab", "--no-single-instance", "a", "b"}, true));
        QVERIFY(forward({"--newtab", "a", "b"}, true));
#ifdef MELD_SINGLE_INSTANCE_NETWORK
        // Every instance listens, so --newtab reaches the running instance
        QVERIFY(forward({"--newtab", "a", "b"}, false));
        QVERIFY(receive({"a", "b"}, false));
        QVERIFY(receive({"--no-single-instance", "a", "b"}, true));
#else
        // Only instances in single-instance mode use shared memory, so
        // --newtab has nothing to reach otherwise
        QVERIFY(!forward({"--newtab", "a", "b"}, false));
        QVERIFY(!receive({"a", "b"}, false));
        QVERIFY(!receive({"--newtab", "a", "b"}, false));
        QVERIFY(receive({"a", "b"}, true));
        QVERIFY(receive({"--single-instance", "a", "b"}, false));
        QVERIFY(!receive({"--no-single-instance", "a", "b"}, true));
#endif
    }

    void singleInstanceServer()
    {
#ifdef MELD_SINGLE_INSTANCE_NETWORK
        // In the runtime folder, as socket paths are short
        const QString name = QStringLiteral("meld-qt-test-%1").arg(QCoreApplication::applicationPid());
        // A socket left by an instance that ended is taken over
        {
            QFile stale(QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath(name));
            QVERIFY(stale.open(QIODevice::WriteOnly));
        }
#else
        const QString name = QStringLiteral("meld-qt-test-%1").arg(QCoreApplication::applicationPid());
        // Shared memory left by an instance that ended (kept here, as by a
        // process still attached to it) is taken over. The key is the one
        // the implementation uses.
        const QNativeIpcKey key = QSharedMemory::isKeyTypeSupported(QNativeIpcKey::Type::SystemV)
            ? QSharedMemory::platformSafeKey(name, QNativeIpcKey::Type::SystemV)
            : QSharedMemory::platformSafeKey(name);
        QSharedMemory kept(key);
        {
            SingleInstanceServer gone;
            QVERIFY(gone.listen(name));
            QVERIFY(kept.attach());
        }
#endif
        MeldApp app;
        QCOMPARE(app.handleCommandLine({"meld-qt"}), -1);
        QVERIFY(app.startSingleInstanceServer(name));
        // Only one instance receives command lines
        SingleInstanceServer other;
        QVERIFY(!other.listen(name));
        QCOMPARE(app.windows().size(), 1);
        MeldWindow *first = app.window();

        // Each invocation runs as another process would, while this one
        // handles it
        struct Result {
            std::optional<int> status;
            QString output;
        };
        const auto invoke = [&name](const QStringList &arguments, const QString &cwd) {
            auto result = std::make_shared<Result>();
            QThread *thread = QThread::create([=]() {
                result->status = forwardCommandLine(
                    QStringList{"meld-qt"} + arguments, cwd,
                    [result](const QString &text, bool) { result->output += text; }, name);
            });
            QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
            thread->start();
            return std::make_pair(result, QPointer<QThread>(thread));
        };

        // --newtab: a tab in the most recently focused window, and paths
        // relative to the invocation's folder
        const QString left = writeFile("a\n");
        const QString right = writeFile("b\n");
        const QString cwd = QFileInfo(left).absolutePath();
        auto [newTab, newTabThread] = invoke({"--newtab", QFileInfo(left).fileName(), QFileInfo(right).fileName()}, cwd);
        QTRY_COMPARE(first->docs().size(), 2);
        auto *filediff = qobject_cast<FileDiff *>(first->docs().last());
        QVERIFY(filediff);
        QTRY_COMPARE(filediff->textBuffer(1)->text(), QStringLiteral("b\n"));
        QCOMPARE(app.windows().size(), 1);
        // The invocation ends when its comparison is closed
        QTest::qWait(300);
        QVERIFY(newTabThread && newTabThread->isRunning());
        filediff->requestClose();
        QTRY_VERIFY(!newTabThread);
        QCOMPARE(newTab->status, 0);

        // Otherwise, a new window
        auto [window, windowThread] = invoke({left, right}, cwd);
        QTRY_COMPARE(app.windows().size(), 2);
        MeldWindow *second = app.windows().last();
        QVERIFY(second != first);
        QTRY_VERIFY(qobject_cast<FileDiff *>(second->currentDoc()));
        second->currentDoc()->requestClose();
        QTRY_VERIFY(!windowThread);
        QCOMPARE(window->status, 0);

        // Errors are reported to the invocation, and its window goes
        const qsizetype windows = app.windows().size();
        auto [error, errorThread] = invoke({left, makeFolder()}, cwd);
        QTRY_VERIFY(!errorThread);
        QCOMPARE(error->status, 2);
        QVERIFY2(error->output.contains(QLatin1String("Cannot compare a mixture of files and directories")),
                 qPrintable(error->output));
        QCOMPARE(app.windows().size(), windows);

        // Without comparisons, the invocation ends at once
        auto [empty, emptyThread] = invoke({}, cwd);
        QTRY_VERIFY(!emptyThread);
        QCOMPARE(empty->status, 0);
        QTRY_COMPARE(app.windows().size(), windows + 1);
        QVERIFY(qobject_cast<NewDiffTab *>(app.windows().last()->currentDoc()));

        for (MeldWindow *open : app.windows())
            discardChanges(open);
    }

    void forwardingWithoutAnInstance()
    {
        const QString name = QStringLiteral("meld-qt-test-none-%1").arg(QCoreApplication::applicationPid());
        QVERIFY(!forwardCommandLine({"meld-qt"}, QDir::currentPath(), {}, name));
    }

#ifndef MELD_SINGLE_INSTANCE_NETWORK
    void forwardingToAnInstanceThatWent()
    {
        // An instance whose heartbeat stopped doesn't receive command lines
        const QString name = QStringLiteral("meld-qt-test-went-%1").arg(QCoreApplication::applicationPid());
        auto server = std::make_unique<SingleInstanceServer>();
        QVERIFY(server->listen(name));
        server.reset();
        QVERIFY(!forwardCommandLine({"meld-qt"}, QDir::currentPath(), {}, name));
    }
#endif
#else
    void singleInstanceIsNotBuilt()
    {
        // Without single-instance support, --newtab is accepted and ignored
        const CommandLineParseResult result = meld::parseCommandLine({"--newtab", "a", "b"});
        QVERIFY(!result.shouldExit);
        QVERIFY(!shouldForwardCommandLine(result.options, true));
        QVERIFY(!shouldReceiveCommandLines(result.options, true));
        QVERIFY(meld::parseCommandLine({"--single-instance"}).shouldExit);
    }
#endif

    void autoMergeRejectsDirectories()
    {
        const CommandLineParseResult result =
            meld::parseCommandLine({"--auto-merge", makeFolder(), makeFolder(), makeFolder()});
        QCOMPARE(result.exitStatus, 2);
        QVERIFY(result.output.contains(QStringLiteral("can’t auto-merge directories")));
    }

    void applicationCommandLine()
    {
        const QStringList files{writeFile("a\n"), writeFile("b\n"), writeFile("c\n")};

        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt"}), -1);
            QVERIFY(qobject_cast<NewDiffTab *>(app.window()->currentDoc()));
        }
        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "-L", "Mine", "-L", "Theirs", files[0], files[1]}), -1);
            auto *filediff = qobject_cast<FileDiff *>(app.window()->currentDoc());
            QVERIFY(filediff);
            QCOMPARE(filediff->tabTitle(), QStringLiteral("Mine — Theirs"));
        }
        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", files[0], "@blank"}), -1);
            auto *filediff = qobject_cast<FileDiff *>(app.window()->currentDoc());
            QVERIFY(filediff);
            QVERIFY(filediff->comparison()->paths.contains(QString()));
        }
        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "--diff", files[0], files[1], "--diff", files[1], files[2]}), -1);
            QCOMPARE(app.window()->docs().size(), 2);
        }
        {
            const QString output = dir_.filePath(QStringLiteral("merged.txt"));
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "--auto-merge", "-o", output, files[0], files[1], files[2]}), -1);
            auto *filediff = qobject_cast<FileDiff *>(app.window()->currentDoc());
            QVERIFY(filediff);
            QCOMPARE(filediff->comparisonMode(), FileComparisonMode::AutoMerge);
            QCOMPARE(filediff->textBuffer(1)->data.savefile, output);
            discardChanges(app.window());
        }
        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", dir_.filePath(QStringLiteral("missing"))}), 2);
        }
        {
            MeldApp app;
            QCOMPARE(app.handleCommandLine({"meld-qt", "--version"}), 0);
            QVERIFY(!app.window());
        }
    }
};

QTEST_MAIN(TestWindow)
#include "tst_window.moc"
