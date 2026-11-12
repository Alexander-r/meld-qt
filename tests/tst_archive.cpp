// Tests for opening archives (archivehelpers.py in the original): which files
// are archives, extracting them with each supported program, and comparing
// them in the main window. The archives are made by
// tools/generate_archive_test_data.py.

#include "core/archive.h"
#include "core/meldbuffer.h"
#include "dirdiff.h"
#include "dirdifftree.h"
#include "filediff.h"
#include "meldwindow.h"
#include "pathlabel.h"
#include "recent.h"
#include "settings.h"
#include "vcview.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;

class TestArchive : public QObject {
    Q_OBJECT

private:
    // The temporary folder for extractions (TMPDIR), to see what's left
    QTemporaryDir temp_;
    // Not in the temporary folder, as comparisons of temporary files aren't
    // added to the recent comparisons
    QTemporaryDir dir_{QDir::currentPath() + QStringLiteral("/tst_archive-XXXXXX")};
    QByteArray path_;
    std::unique_ptr<MeldWindow> window_;

    static QString data(const QString &name)
    {
        return QStringLiteral(MELD_TEST_DATA_DIR "/archives/") + name;
    }

    static QByteArray read(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    QStringList leftovers() const
    {
        return QDir(temp_.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    }

    //! Extract an archive, waiting for the result; an empty root on failure
    static QString extract(const QString &archive, QString *error = nullptr)
    {
        archive::Extraction extraction(archive);
        QSignalSpy finished(&extraction, &archive::Extraction::finished);
        QSignalSpy failed(&extraction, &archive::Extraction::failed);
        extraction.start();
        if (!QTest::qWaitFor([&]() { return !finished.isEmpty() || !failed.isEmpty(); }, 30000))
            return {};
        if (!failed.isEmpty()) {
            if (error)
                *error = failed.first().first().toString();
            return {};
        }
        return finished.first().first().toString();
    }

    //! Only the given programs can be found
    void restrictPrograms(const QMap<QString, QString> &programs)
    {
        const QString bin = dir_.filePath(QStringLiteral("bin-%1").arg(QDateTime::currentMSecsSinceEpoch()));
        QDir().mkpath(bin);
        for (auto it = programs.cbegin(); it != programs.cend(); ++it)
            QFile::link(it.value(), QDir(bin).filePath(it.key()));
        qputenv("PATH", bin.toLocal8Bit());
    }

    static bool writable(const QString &path)
    {
        return QFile::permissions(path) & QFile::WriteOwner;
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

    QStringList archiveFolders(DirDiff *dirdiff) const
    {
        QStringList folders;
        for (const QString &folder : dirdiff->folders())
            folders.append(archive::formatArchivePath(folder));
        return folders;
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(temp_.isValid());
        QVERIFY(dir_.isValid());
        qputenv("TMPDIR", temp_.path().toLocal8Bit());
        QCOMPARE(QDir::cleanPath(QDir::tempPath()), QDir::cleanPath(temp_.path()));
        path_ = qgetenv("PATH");
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
        window_.reset();
        archive::removeAllExtractedArchives();
        qputenv("PATH", path_);
        QCOMPARE(leftovers(), QStringList());
    }

    // -----------------------------------------------------------------------
    // Archives

    void isArchive_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("expected");
        QTest::newRow("compressed tar") << data(QStringLiteral("left.tar.gz")) << true;
        QTest::newRow("zip") << data(QStringLiteral("right.zip")) << true;
        QTest::newRow("tar") << data(QStringLiteral("unsafe.tar")) << true;
        // Documents that are zip files aren't archives
        QTest::newRow("document") << data(QStringLiteral("document.odt")) << false;
        QTest::newRow("text") << QStringLiteral(MELD_TEST_DATA_DIR "/syntax/README") << false;
        QTest::newRow("folder") << data(QString()) << false;
        QTest::newRow("missing") << data(QStringLiteral("missing.zip")) << false;
        QTest::newRow("empty") << QString() << false;
    }

    void isArchive()
    {
        QFETCH(QString, path);
        QFETCH(bool, expected);
        QCOMPARE(archive::isArchive(path), expected);
    }

    void archiveContentTypesAreTheOriginals()
    {
        const QStringList &types = archive::archiveContentTypes();
        QCOMPARE(types.size(), 25);
        QVERIFY(types.contains(QStringLiteral("application/x-compressed-tar")));
        QVERIFY(types.contains(QStringLiteral("application/zip")));
        QVERIFY(types.contains(QStringLiteral("application/x-7z-compressed")));
    }

    void extractsArchives()
    {
        const QString root = extract(data(QStringLiteral("left.tar.gz")));
        QVERIFY(!root.isEmpty());
        QVERIFY(root.startsWith(QDir::cleanPath(temp_.path()) + QLatin1Char('/')));
        // Named as the archive, which shows in shortened paths
        QCOMPARE(QFileInfo(root).fileName(), QStringLiteral("left.tar.gz"));
        QCOMPARE(read(root + QStringLiteral("/dir/same.txt")), QByteArray("same\n"));
        QCOMPARE(read(root + QStringLiteral("/dir/changed.txt")), QByteArray("left\n"));
        QCOMPARE(read(root + QStringLiteral("/only-left.txt")), QByteArray("left only\n"));

        // Archives can't be changed
        QVERIFY(!writable(root));
        QVERIFY(!writable(root + QStringLiteral("/dir")));
        // ...but copied files keep their permissions
        QVERIFY(writable(root + QStringLiteral("/dir/same.txt")));

        QVERIFY(archive::haveExtractedArchives());
        QVERIFY(archive::isExtractedArchiveRoot(root));
        QVERIFY(archive::isExtractedArchiveRoot(root + QStringLiteral("/dir/..")));
        QVERIFY(!archive::isExtractedArchiveRoot(root + QStringLiteral("/dir")));
        QCOMPARE(archive::archiveForPath(root + QStringLiteral("/dir/same.txt")),
                 std::optional<QString>(data(QStringLiteral("left.tar.gz"))));
        QCOMPARE(archive::formatArchivePath(root), data(QStringLiteral("left.tar.gz")));
        QCOMPARE(archive::formatArchivePath(temp_.path()), QString());
        QVERIFY(!archive::archiveForPath(root + QStringLiteral("-other")));

        QCOMPARE(archive::formatArchiveRelativePath(root), QStringLiteral("left.tar.gz"));
        QCOMPARE(archive::formatArchiveRelativePath(root + QStringLiteral("/dir/same.txt")),
                 QStringLiteral("left.tar.gz — dir/same.txt"));
        QCOMPARE(archive::formatArchiveRelativePath(data(QStringLiteral("left.tar.gz"))), QString());
        QCOMPARE(archive::formatArchiveFilePath(root), data(QStringLiteral("left.tar.gz")));
        QCOMPARE(archive::formatArchiveFilePath(root + QStringLiteral("/dir/same.txt")),
                 data(QStringLiteral("left.tar.gz/dir/same.txt")));
        QCOMPARE(archive::formatArchiveFilePath(data(QStringLiteral("left.tar.gz"))), QString());

        archive::removeExtractedArchives({root});
        QVERIFY(!archive::haveExtractedArchives());
        QVERIFY(!QFileInfo::exists(root));
        QCOMPARE(leftovers(), QStringList());
    }

    void extractsWithEachProgram_data()
    {
        QTest::addColumn<QString>("archive");
        QTest::addColumn<QString>("program");
        QTest::addColumn<QStringList>("needed");

        QTest::newRow("bsdtar, tar") << QStringLiteral("left.tar.gz") << QStringLiteral("bsdtar")
                                     << QStringList{QStringLiteral("bsdtar")};
        QTest::newRow("bsdtar, zip") << QStringLiteral("right.zip") << QStringLiteral("bsdtar")
                                     << QStringList{QStringLiteral("bsdtar")};
        QTest::newRow("GNU tar") << QStringLiteral("left.tar.gz") << QStringLiteral("tar")
                                 << QStringList{QStringLiteral("tar"), QStringLiteral("gzip")};
        QTest::newRow("7-Zip") << QStringLiteral("right.zip") << QStringLiteral("7z")
                               << QStringList{QStringLiteral("7z")};
        QTest::newRow("unzip") << QStringLiteral("right.zip") << QStringLiteral("unzip")
                               << QStringList{QStringLiteral("unzip")};
    }

    void extractsWithEachProgram()
    {
        QFETCH(QString, archive);
        QFETCH(QString, program);
        QFETCH(QStringList, needed);

        QMap<QString, QString> programs;
        for (const QString &name : std::as_const(needed)) {
            const QString found = QStandardPaths::findExecutable(name);
            if (found.isEmpty())
                QSKIP(qPrintable(QStringLiteral("%1 isn't installed").arg(name)));
            programs.insert(name, found);
        }
        if (program == QLatin1String("tar")) {
            QProcess version;
            version.start(programs.value(program), {QStringLiteral("--version")});
            QVERIFY(version.waitForFinished());
            if (!version.readAllStandardOutput().contains("GNU tar"))
                QSKIP("tar isn't GNU tar");
        }
        restrictPrograms(programs);

        const QString path = data(archive);
        const std::optional<archive::Extractor> extractor = archive::extractorFor(path, temp_.path());
        QVERIFY(extractor);
        QCOMPARE(QFileInfo(extractor->program).fileName(), program);

        QString error;
        const QString root = extract(path, &error);
        QVERIFY2(!root.isEmpty(), qPrintable(error));
        QCOMPARE(read(root + QStringLiteral("/dir/same.txt")), QByteArray("same\n"));
        const bool zip = archive.endsWith(QLatin1String(".zip"));
        QCOMPARE(read(root + QStringLiteral("/dir/changed.txt")), zip ? QByteArray("right\n") : QByteArray("left\n"));
        QVERIFY(!writable(root + QStringLiteral("/dir")));
    }

    void failsWithoutAProgram()
    {
        restrictPrograms({});
        QVERIFY(!archive::extractorFor(data(QStringLiteral("right.zip")), temp_.path()));
        QString error;
        QVERIFY(extract(data(QStringLiteral("right.zip")), &error).isEmpty());
        QVERIFY2(error.contains(QLatin1String("bsdtar")), qPrintable(error));
        QVERIFY(!archive::haveExtractedArchives());
        QCOMPARE(leftovers(), QStringList());
    }

    void failedExtractionsAreRemoved()
    {
        QString error;
        QVERIFY(extract(data(QStringLiteral("broken.tar.gz")), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!archive::haveExtractedArchives());
        QCOMPARE(leftovers(), QStringList());
    }

    void membersOutsideTheArchiveAreNotWritten()
    {
        QString error;
        const QString root = extract(data(QStringLiteral("unsafe.tar")), &error);
        // The program may refuse the archive, or skip the member
        if (!root.isEmpty()) {
            QVERIFY(!QFileInfo::exists(QFileInfo(root).path() + QStringLiteral("/escaped.txt")));
            archive::removeExtractedArchives({root});
        }
        QDirIterator it(temp_.path(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            QVERIFY2(it.next().indexOf(QLatin1String("escaped")) < 0, qPrintable(it.filePath()));
        QVERIFY(!QFileInfo::exists(QFileInfo(dir_.path()).path() + QStringLiteral("/escaped.txt")));
    }

    void specialFilesAreRemovedAndFilesMadeReadable()
    {
        QString error;
        const QString root = extract(data(QStringLiteral("special.tar")), &error);
        QVERIFY2(!root.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/pipe")));
        QCOMPARE(read(root + QStringLiteral("/unreadable.txt")), QByteArray("unreadable\n"));
        QCOMPARE(read(root + QStringLiteral("/closed/inside.txt")), QByteArray("inside\n"));
        QVERIFY(!writable(root + QStringLiteral("/closed")));
    }

    void cancellingRemovesTheExtraction()
    {
        archive::Extraction extraction(data(QStringLiteral("left.tar.gz")));
        QSignalSpy finished(&extraction, &archive::Extraction::finished);
        QSignalSpy failed(&extraction, &archive::Extraction::failed);
        extraction.start();
        QVERIFY(extraction.isRunning());
        extraction.cancel();
        QVERIFY(!extraction.isRunning());
        QTest::qWait(50);
        QVERIFY(finished.isEmpty());
        QVERIFY(failed.isEmpty());
        QCOMPARE(leftovers(), QStringList());
    }

    // -----------------------------------------------------------------------
    // Comparisons

    void openingArchivesComparesTheirFolders()
    {
        const QStringList archives{data(QStringLiteral("left.tar.gz")), data(QStringLiteral("right.zip"))};
        QString error;
        auto *dirdiff = qobject_cast<DirDiff *>(window_->openPaths(archives, false, false, true, &error));
        QVERIFY2(dirdiff, qPrintable(error));
        settle();
        QCOMPARE(archiveFolders(dirdiff), archives);
        for (const QString &folder : dirdiff->folders())
            QVERIFY(archive::isExtractedArchiveRoot(folder));

        // The archives are shown, rather than where they're extracted
        QVERIFY2(dirdiff->tabTitle().contains(QLatin1String("left.tar.gz")), qPrintable(dirdiff->tabTitle()));
        QVERIFY2(dirdiff->tabTitle().contains(QLatin1String("right.zip")), qPrintable(dirdiff->tabTitle()));
        QVERIFY(dirdiff->tabTooltip().contains(archives.first()));
        TreeNode *root = dirdiff->model()->firstRoot();
        QVERIFY(root);
        QCOMPARE(root->panes[0].text, archives.at(0));
        QCOMPARE(root->panes[1].text, archives.at(1));
        QStringList labels;
        for (const PathLabel *label : dirdiff->findChildren<PathLabel *>())
            labels.append(label->displayLabel());
        QVERIFY2(labels.contains(QStringLiteral("left.tar.gz")), qPrintable(labels.join(u',')));
        QVERIFY2(labels.contains(QStringLiteral("right.zip")), qPrintable(labels.join(u',')));

        // Recorded as the archives
        QCOMPARE(dirdiff->comparison()->paths, archives);
        QCOMPARE(RecentComparisons::load().value(0).paths, archives);

        // The extracted archives go with the comparison
        const QStringList folders = dirdiff->folders();
        dirdiff->requestClose();
        QVERIFY(!archive::haveExtractedArchives());
        for (const QString &folder : folders)
            QVERIFY(!QFileInfo::exists(folder));
    }

    void archivesAndFoldersCanBeCompared()
    {
        const QString folder = dir_.filePath(QStringLiteral("folder"));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/dir")));
        QString error;
        auto *dirdiff = qobject_cast<DirDiff *>(
            window_->openPaths({folder, data(QStringLiteral("right.zip"))}, false, false, true, &error));
        QVERIFY2(dirdiff, qPrintable(error));
        QCOMPARE(dirdiff->folders().first(), folder);
        QCOMPARE(archive::formatArchivePath(dirdiff->folders().at(1)), data(QStringLiteral("right.zip")));
        QCOMPARE(dirdiff->comparison()->paths, QStringList({folder, data(QStringLiteral("right.zip"))}));
    }

    void anArchiveAloneOpensAFolderView()
    {
        QString error;
        auto *vcview =
            qobject_cast<VcView *>(window_->openPaths({data(QStringLiteral("left.tar.gz"))}, false, false, true, &error));
        QVERIFY2(vcview, qPrintable(error));
        QVERIFY(archive::haveExtractedArchives());
    }

    void archivesThatCantBeOpenedAreErrors()
    {
        QString error;
        QVERIFY(!window_->openPaths({data(QStringLiteral("broken.tar.gz")), data(QStringLiteral("right.zip"))}, false,
                                    false, true, &error));
        QVERIFY2(error.contains(data(QStringLiteral("broken.tar.gz"))), qPrintable(error));
        QVERIFY(!archive::haveExtractedArchives());
        QVERIFY(!window_->hasPages());
        // ...which the shown window shows
        auto *dialog = window_->findChild<QMessageBox *>();
        QVERIFY(dialog);
        QCOMPARE(dialog->text(), error);
        dialog->close();
    }

    void filesInArchivesAreReadOnly()
    {
        QString error;
        auto *dirdiff = qobject_cast<DirDiff *>(window_->openPaths(
            {data(QStringLiteral("left.tar.gz")), data(QStringLiteral("right.zip"))}, false, false, true, &error));
        QVERIFY2(dirdiff, qPrintable(error));
        const QStringList files{dirdiff->folders().at(0) + QStringLiteral("/dir/changed.txt"),
                                dirdiff->folders().at(1) + QStringLiteral("/dir/changed.txt")};
        auto *filediff = qobject_cast<FileDiff *>(window_->appendDiff(files));
        QVERIFY(filediff);
        settle();
        for (int pane = 0; pane < 2; ++pane)
            QVERIFY(!filediff->textBuffer(pane)->data.writable());
        QStringList labels;
        for (const PathLabel *label : filediff->findChildren<PathLabel *>())
            labels.append(label->displayLabel());
        QVERIFY2(labels.contains(QStringLiteral("left.tar.gz — dir/changed.txt")), qPrintable(labels.join(u',')));
        QVERIFY2(labels.contains(QStringLiteral("right.zip — dir/changed.txt")), qPrintable(labels.join(u',')));
        // ...and the tab by the archives
        QCOMPARE(filediff->tabTitle(), QStringLiteral("[left.tar.gz] changed.txt — [right.zip] changed.txt"));
        QVERIFY(filediff->tabTooltip().contains(data(QStringLiteral("left.tar.gz/dir/changed.txt"))));
    }

    void recentComparisonsOfArchivesExtractThemAgain()
    {
        const QStringList archives{data(QStringLiteral("left.tar.gz")), data(QStringLiteral("right.zip"))};
        auto *dirdiff = qobject_cast<DirDiff *>(window_->appendRecent({RecentType::Folder, archives}));
        QVERIFY(dirdiff);
        QCOMPARE(archiveFolders(dirdiff), archives);
    }
};

QTEST_MAIN(TestArchive)
#include "tst_archive.moc"
