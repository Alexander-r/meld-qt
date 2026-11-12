#include "core/iohelpers.h"

#include <QTest>

using namespace meld;

using OptPathList = QList<std::optional<QString>>;
Q_DECLARE_METATYPE(OptPathList)

class TestIoHelpers : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void findSharedParentPath_data()
    {
        QTest::addColumn<OptPathList>("paths");
        QTest::addColumn<QString>("expectedParent");

        const auto p = [](const char *s) { return std::optional<QString>(QString::fromUtf8(s)); };
        QTest::newRow("No paths, None return") << OptPathList{} << QString();
        QTest::newRow("One path always returns its own parent") << OptPathList{p("/foo/a/b/c")} << "/foo/a/b";
        QTest::newRow("Two paths") << OptPathList{p("/foo/a"), p("/foo/b")} << "/foo";
        QTest::newRow("Three paths") << OptPathList{p("/foo/a"), p("/foo/b"), p("/foo/c")} << "/foo";
        QTest::newRow("First path is deeper") << OptPathList{p("/foo/a/asd/asd"), p("/foo/b")} << "/foo";
        QTest::newRow("Second path is deeper") << OptPathList{p("/foo/a/"), p("/foo/b/asd/asd")} << "/foo";
        QTest::newRow("Common parent is the root") << OptPathList{p("/foo/a/"), p("/bar/b/")} << "/";
        QTest::newRow("One path, one missing path") << OptPathList{p("/foo/a"), std::nullopt} << QString();
        QTest::newRow("Two paths, one missing path")
            << OptPathList{p("/foo/a"), std::nullopt, p("/foo/c")} << QString();
    }

    void findSharedParentPath()
    {
        QFETCH(OptPathList, paths);
        QFETCH(QString, expectedParent);
        const std::optional<QString> parent = meld::findSharedParentPath(paths);
        if (expectedParent.isNull())
            QVERIFY(!parent);
        else
            QCOMPARE(parent.value_or(QStringLiteral("<none>")), expectedParent);
    }

    void formatHomeRelativePath_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        QTest::newRow("in home") << "/home/hey/foo" << "~/foo";
        QTest::newRow("outside home") << "/home/hmph/foo" << "/home/hmph/foo";
    }

    void formatHomeRelativePath()
    {
        QFETCH(QString, path);
        QFETCH(QString, expected);
        QCOMPARE(meld::formatHomeRelativePath(path, QStringLiteral("/home/hey/")), expected);
    }

    void formatParentRelativePath_data()
    {
        QTest::addColumn<QString>("parent");
        QTest::addColumn<QString>("child");
        QTest::addColumn<QString>("expectedLabel");

        QTest::newRow("Child is a direct child of parent") << "/home/hey/" << "/home/hey/foo.txt"
                                                           << "…/hey/foo.txt";
        QTest::newRow("Child is a direct child of parent and parent is the root") << "/" << "/foo.txt"
                                                                                  << "/foo.txt";
        QTest::newRow("Child is a 2-depth child of parent") << "/home/hey/" << "/home/hey/project/foo.txt"
                                                            << "…/project/foo.txt";
        QTest::newRow("Child is a 3-depth child of parent") << "/home/hey/" << "/home/hey/project/package/foo.txt"
                                                            << "…/project/package/foo.txt";
        QTest::newRow("More-than-3-depth child, long immediate parent")
            << "/home/hey/" << "/home/hey/project/package/subpackage/foo.txt" << "…/project/…/subpackage/foo.txt";
        QTest::newRow("More-than-3-depth child, short immediate parent")
            << "/home/hey/" << "/home/hey/project/package/src/foo.txt" << "…/project/package/src/foo.txt";
    }

    void formatParentRelativePath()
    {
        QFETCH(QString, parent);
        QFETCH(QString, child);
        QFETCH(QString, expectedLabel);
        QCOMPARE(meld::formatParentRelativePath(parent, child, QStringLiteral("/nonexistent-home")), expectedLabel);
    }

    void formatParentRelativePathNoParent()
    {
        bool raised = false;
        try {
            meld::formatParentRelativePath(QStringLiteral("/"), QStringLiteral("/"));
        } catch (const PathError &e) {
            raised = QString::fromStdString(e.what()).contains(QStringLiteral("has no parent"));
        }
        QVERIFY(raised);
    }
};

QTEST_GUILESS_MAIN(TestIoHelpers)
#include "tst_iohelpers.moc"
