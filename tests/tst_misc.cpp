#include "core/misc.h"

#include <QTest>

using namespace meld;

using IntervalList = std::vector<Interval>;
Q_DECLARE_METATYPE(IntervalList)

class TestMisc : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void mergeIntervals_data()
    {
        QTest::addColumn<IntervalList>("intervals");
        QTest::addColumn<IntervalList>("expected");

        QTest::newRow("Dominated by a single range")
            << IntervalList{{1, 5}, {5, 9}, {10, 11}, {0, 20}} << IntervalList{{0, 20}};
        QTest::newRow("No overlap") << IntervalList{{1, 5}, {6, 9}, {10, 11}}
                                    << IntervalList{{1, 5}, {6, 9}, {10, 11}};
        QTest::newRow("Two overlap points between ranges")
            << IntervalList{{1, 5}, {5, 9}, {10, 12}, {11, 20}} << IntervalList{{1, 9}, {10, 20}};
        QTest::newRow("Two overlap points between ranges, out of order")
            << IntervalList{{5, 9}, {1, 5}, {11, 20}, {10, 12}} << IntervalList{{1, 9}, {10, 20}};
        QTest::newRow("Two equal ranges") << IntervalList{{1, 5}, {7, 8}, {1, 5}} << IntervalList{{1, 5}, {7, 8}};
        QTest::newRow("Three ranges overlap") << IntervalList{{1, 5}, {4, 10}, {9, 15}} << IntervalList{{1, 15}};
    }

    void mergeIntervals()
    {
        QFETCH(IntervalList, intervals);
        QFETCH(IntervalList, expected);
        QCOMPARE(meld::mergeIntervals(intervals), expected);
    }

    void calcSyncpoint_data()
    {
        QTest::addColumn<double>("value");
        QTest::addColumn<double>("pageSize");
        QTest::addColumn<double>("lower");
        QTest::addColumn<double>("upper");
        QTest::addColumn<double>("expected");

        QTest::newRow("Boring top") << 0.0 << 100.0 << 0.0 << 1000.0 << 0.0;
        QTest::newRow("Above the top!") << 0.0 << 100.0 << 100.0 << 1000.0 << 0.0;
        QTest::newRow("Normal top scaling 1") << 25.0 << 100.0 << 0.0 << 1000.0 << 0.25;
        QTest::newRow("Normal top scaling 2") << 50.0 << 100.0 << 0.0 << 1000.0 << 0.5;
        QTest::newRow("Scaling with a lower offset 1") << 25.0 << 100.0 << 25.0 << 1000.0 << 0.0;
        QTest::newRow("Scaling with a lower offset 2") << 50.0 << 100.0 << 25.0 << 1000.0 << 0.25;
        QTest::newRow("Somewhere in the middle") << 500.0 << 100.0 << 0.0 << 1000.0 << 0.5;
        QTest::newRow("Normal bottom scaling 1") << 850.0 << 100.0 << 0.0 << 1000.0 << 0.5;
        QTest::newRow("Normal bottom scaling 2") << 875.0 << 100.0 << 0.0 << 1000.0 << 0.75;
        QTest::newRow("Boring bottom") << 900.0 << 100.0 << 0.0 << 1000.0 << 1.0;
        QTest::newRow("Below the bottom!") << 1100.0 << 100.0 << 0.0 << 1000.0 << 1.0;
    }

    void calcSyncpoint()
    {
        QFETCH(double, value);
        QFETCH(double, pageSize);
        QFETCH(double, lower);
        QFETCH(double, upper);
        QFETCH(double, expected);
        const Adjustment adjustment = Adjustment::configure(value, lower, upper, pageSize);
        QCOMPARE(meld::calcSyncpoint(adjustment), expected);
    }

    void allSame_data()
    {
        QTest::addColumn<QList<int>>("list");
        QTest::addColumn<bool>("expected");

        QTest::newRow("empty") << QList<int>{} << true;
        QTest::newRow("[0]") << QList<int>{0} << true;
        QTest::newRow("[1]") << QList<int>{1} << true;
        QTest::newRow("[0, 0]") << QList<int>{0, 0} << true;
        QTest::newRow("[0, 1]") << QList<int>{0, 1} << false;
        QTest::newRow("[1, 0]") << QList<int>{1, 0} << false;
        QTest::newRow("[1, 1]") << QList<int>{1, 1} << true;
        QTest::newRow("[0, 0, 0]") << QList<int>{0, 0, 0} << true;
        QTest::newRow("[0, 0, 1]") << QList<int>{0, 0, 1} << false;
        QTest::newRow("[0, 1, 0]") << QList<int>{0, 1, 0} << false;
        QTest::newRow("[0, 1, 1]") << QList<int>{0, 1, 1} << false;
        QTest::newRow("[1, 0, 0]") << QList<int>{1, 0, 0} << false;
        QTest::newRow("[1, 0, 1]") << QList<int>{1, 0, 1} << false;
        QTest::newRow("[1, 1, 0]") << QList<int>{1, 1, 0} << false;
        QTest::newRow("[1, 1, 1]") << QList<int>{1, 1, 1} << true;
    }

    void allSame()
    {
        QFETCH(QList<int>, list);
        QFETCH(bool, expected);
        QCOMPARE(meld::allSame(list), expected);
    }

    void shortenNames_data()
    {
        QTest::addColumn<bool>("windows");
        QTest::addColumn<QStringList>("paths");
        QTest::addColumn<QStringList>("expected");

        QTest::newRow("1") << false << QStringList{"/tmp/foo1", "/tmp/foo2"} << QStringList{"foo1", "foo2"};
        QTest::newRow("2") << false << QStringList{"/tmp/foo1", "/tmp/foo2", "/tmp/foo3"}
                           << QStringList{"foo1", "foo2", "foo3"};
        QTest::newRow("3") << false << QStringList{"/tmp/bar/foo1", "/tmp/woo/foo2"} << QStringList{"foo1", "foo2"};
        QTest::newRow("4") << false << QStringList{"/tmp/bar/foo1", "/tmp/woo/foo1"}
                           << QStringList{"[bar] foo1", "[woo] foo1"};
        QTest::newRow("5") << false << QStringList{"/tmp/bar/foo1", "/tmp/woo/foo1", "/tmp/ree/foo1"}
                           << QStringList{"[bar] foo1", "[woo] foo1", "[ree] foo1"};
        QTest::newRow("6") << false << QStringList{"/tmp/bar/deep/deep", "/tmp/bar/shallow"}
                           << QStringList{"deep", "shallow"};
        QTest::newRow("7") << false << QStringList{"/tmp/bar/deep/deep/foo1", "/tmp/bar/shallow/foo1"}
                           << QStringList{"[deep] foo1", "[shallow] foo1"};
        // This case doesn't actually make much sense, so it's not that bad
        // that our output is... somewhat unclear.
        QTest::newRow("8") << false << QStringList{"/tmp/bar/subdir/subsub", "/tmp/bar/"}
                           << QStringList{"subsub", "bar"};
        QTest::newRow("9") << true << QStringList{"C:\\Users\\hmm\\bar", "C:\\Users\\hmm\\foo"}
                           << QStringList{"bar", "foo"};
        QTest::newRow("10") << true << QStringList{"C:\\Users\\bar\\hmm", "C:\\Users\\foo\\hmm"}
                            << QStringList{"[bar] hmm", "[foo] hmm"};
        // Check that paths with no commonality are handled
        QTest::newRow("11") << false << QStringList{"nothing in", "common"} << QStringList{"nothing in", "common"};
        QTest::newRow("12") << false << QStringList{"<unnamed>", "/tmp/real/path"}
                            << QStringList{"<unnamed>", "/tmp/real/path"};
    }

    void shortenNames()
    {
        QFETCH(bool, windows);
        QFETCH(QStringList, paths);
        QFETCH(QStringList, expected);
        QCOMPARE(meld::shortenNames(paths, windows ? PathStyle::Windows : PathStyle::Posix), expected);
    }

    void applyTextFiltersReportsLineBreaks()
    {
        bool ok = true;
        const QString text = QStringLiteral("a\nb");
        const QString result = meld::applyTextFilters(text, {QRegularExpression(QStringLiteral("a\\nb"))}, {}, &ok);
        QVERIFY(!ok);
        QCOMPARE(result, text);
    }
};

QTEST_GUILESS_MAIN(TestMisc)
#include "tst_misc.moc"
