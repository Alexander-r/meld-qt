// Tests for text filtering with dimmed ranges

#include "core/filters.h"
#include "core/meldbuffer.h"

#include <QTest>

using namespace meld;

using RangeList = std::vector<std::pair<int, int>>;
Q_DECLARE_METATYPE(RangeList)

class TestFilterText : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void filterText_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<RangeList>("ignoredRanges");
        QTest::addColumn<QString>("expectedText");

        QTest::newRow("Matching without groups") << "# asdasdasdasdsad" << RangeList{{0, 17}} << "";
        QTest::newRow("Matching with single group") << "asdasdasdasdsab" << RangeList{{1, 14}} << "ab";
        QTest::newRow("Matching with multiple groups") << "xasdyasdz" << RangeList{{1, 4}, {5, 8}} << "xyz";
        QTest::newRow("Matching with multiple partially overlapping filters")
            << "qaqxqbyqzq" << RangeList{{2, 6}, {7, 8}} << "qayzq";
        QTest::newRow("Matching with multiple fully overlapping filters")
            << "qaqxqybqzq" << RangeList{{2, 8}} << "qazq";
        QTest::newRow("Matching with and without groups, with single dominated match")
            << "# asdasdasdasdsab" << RangeList{{0, 17}} << "";
        QTest::newRow("Matching with and without groups, with partially overlapping filters")
            << "/*a*/ub" << RangeList{{0, 6}} << "b";
        QTest::newRow("Non-matching with groups") << "xasdyasdx" << RangeList{} << "xasdyasdx";
        QTest::newRow("Multiple lines with non-overlapping filters") << "#ab\na2b" << RangeList{{0, 3}, {5, 6}}
                                                                     << "\nab";
        QTest::newRow("CVS keyword") << "$Author: John Doe $" << RangeList{{8, 18}} << "$Author:$";
    }

    void filterText()
    {
        QFETCH(QString, text);
        QFETCH(RangeList, ignoredRanges);
        QFETCH(QString, expectedText);

        const QStringList filterPatterns{
            QStringLiteral("#.*"),
            QStringLiteral(R"(/\*.*\*/)"),
            QStringLiteral("a(.*)b"),
            QStringLiteral("x(.*)y(.*)z"),
            QStringLiteral(R"(\$\w+:([^\n$]+)\$)"),
        };
        QList<QRegularExpression> regexes;
        for (const QString &pattern : filterPatterns) {
            const FilterEntry f = FilterEntry::fromSettings(QStringLiteral("name"), true, pattern, FilterEntry::Regex);
            QVERIFY(f.filter);
            regexes.append(*f.filter);
        }

        MeldBuffer buffer;
        buffer.setText(text);
        const QString filtered = filterBufferText(buffer.text(), buffer.dimmedRanges(), 0, buffer.endPosition(), regexes);

        QCOMPARE(buffer.dimmedRanges().ranges(), ignoredRanges);
        QCOMPARE(filtered, expectedText);
    }

    void dimmedRangesFollowEdits()
    {
        MeldBuffer buffer;
        buffer.setText(QStringLiteral("abc # comment\ndef"));
        RangeSet &dimmed = buffer.dimmedRanges();
        dimmed.add(4, 13);
        buffer.insertText(0, QStringLiteral("XY"));
        QCOMPARE(dimmed.ranges(), (RangeList{{6, 15}}));
        // Text inserted at the end of a range doesn't extend it
        buffer.insertText(15, QStringLiteral("!"));
        QCOMPARE(dimmed.ranges(), (RangeList{{6, 15}}));
        dimmed.remove(8, 10);
        QCOMPARE(dimmed.ranges(), (RangeList{{6, 8}, {10, 15}}));
        dimmed.add(7, 12);
        QCOMPARE(dimmed.ranges(), (RangeList{{6, 15}}));
    }
};

QTEST_MAIN(TestFilterText)
#include "tst_filtertext.moc"
