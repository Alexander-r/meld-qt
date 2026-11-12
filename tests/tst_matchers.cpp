#include "testutils.h"

#include "core/myers.h"

#include <QTest>

using namespace meld;

namespace {

Sequence chars(const QString &text)
{
    return codePoints(text);
}

std::vector<MatchingBlock> blocks(std::initializer_list<MatchingBlock> list)
{
    return list;
}

} // namespace

namespace meld {
char *toString(const std::vector<MatchingBlock> &value)
{
    return QTest::toString(testutils::compact(testutils::blocksToJson(value)));
}
} // namespace meld

class TestMatchers : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void basicMatcher()
    {
        MyersSequenceMatcher matcher(chars(QStringLiteral("abcbdefgabcdefg")), chars(QStringLiteral("gfabcdefcd")));
        QCOMPARE(matcher.matchingBlocks(), blocks({{0, 2, 3}, {4, 5, 3}, {10, 8, 2}, {15, 10, 0}}));
    }

    void postprocessingCleanup()
    {
        MyersSequenceMatcher matcher(chars(QStringLiteral("abcfabgcd")), chars(QStringLiteral("afabcgabgcabcd")));
        QCOMPARE(matcher.matchingBlocks(), blocks({{0, 2, 3}, {4, 6, 3}, {7, 12, 2}, {9, 14, 0}}));
    }

    void inlineMatcher()
    {
        InlineMyersSequenceMatcher matcher(chars(QStringLiteral("red, blue, yellow, white")),
                                           chars(QStringLiteral("black green, hue, white")));
        QCOMPARE(matcher.matchingBlocks(), blocks({{17, 16, 7}, {24, 23, 0}}));
    }

    void syncPointMatcher0()
    {
        SyncPointMyersSequenceMatcher matcher(chars(QStringLiteral("012a3456c789")), chars(QStringLiteral("0a3412b5678")));
        QCOMPARE(matcher.matchingBlocks(),
                 blocks({{0, 0, 1}, {3, 1, 3}, {6, 7, 2}, {9, 9, 2}, {12, 11, 0}}));
    }

    void syncPointMatcher2()
    {
        SyncPointMyersSequenceMatcher matcher(chars(QStringLiteral("012a3456c789")), chars(QStringLiteral("0a3412b5678")),
                                              {{3, 6}});
        QCOMPARE(matcher.matchingBlocks(),
                 blocks({{0, 0, 1}, {1, 4, 2}, {6, 7, 2}, {9, 9, 2}, {12, 11, 0}}));
    }

    void syncPointMatcher3()
    {
        SyncPointMyersSequenceMatcher matcher(chars(QStringLiteral("012a3456c789")), chars(QStringLiteral("02a341b5678")),
                                              {{3, 2}, {8, 6}});
        QCOMPARE(matcher.matchingBlocks(),
                 blocks({{0, 0, 1}, {2, 1, 1}, {3, 2, 3}, {9, 9, 2}, {12, 11, 0}}));
    }

    void cooperativeInitialise()
    {
        // The generator must yield before completing, and complete exactly once
        MyersSequenceMatcher matcher(chars(QStringLiteral("abcdef")), chars(QStringLiteral("abXdef")));
        int pending = 0;
        int done = 0;
        for (bool finished : matcher.initialise()) {
            if (finished)
                ++done;
            else
                ++pending;
        }
        QVERIFY(pending >= 1);
        QCOMPARE(done, 1);
        QCOMPARE(matcher.opcodes().size(), size_t(3));
    }

    void referenceMyers()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("myers")).toArray()) {
            const QJsonObject c = value.toObject();
            LineInterner interner;
            Sequence a = interner.intern(testutils::toStringList(c[QStringLiteral("a")]));
            Sequence b = interner.intern(testutils::toStringList(c[QStringLiteral("b")]));
            MyersSequenceMatcher matcher(std::move(a), std::move(b));
            const QString context = QStringLiteral("myers case %1").arg(index++);
            COMPARE_JSON(testutils::blocksToJson(matcher.matchingBlocks()), c[QStringLiteral("blocks")], context);
            COMPARE_JSON(testutils::toJsonArray(matcher.opcodes()), c[QStringLiteral("opcodes")], context);
        }
        QVERIFY(index > 100);
    }

    void referenceInline()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("inline")).toArray()) {
            const QJsonObject c = value.toObject();
            InlineMyersSequenceMatcher matcher(chars(c[QStringLiteral("a")].toString()),
                                               chars(c[QStringLiteral("b")].toString()));
            const QString context = QStringLiteral("inline case %1").arg(index++);
            COMPARE_JSON(testutils::blocksToJson(matcher.matchingBlocks()), c[QStringLiteral("blocks")], context);
            COMPARE_JSON(testutils::toJsonArray(matcher.opcodes()), c[QStringLiteral("opcodes")], context);
        }
        QVERIFY(index > 100);
    }

    void referenceSyncPoint()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("syncpoint")).toArray()) {
            const QJsonObject c = value.toObject();
            LineInterner interner;
            Sequence a = interner.intern(testutils::toStringList(c[QStringLiteral("a")]));
            Sequence b = interner.intern(testutils::toStringList(c[QStringLiteral("b")]));
            std::vector<std::pair<int, int>> points;
            for (const QJsonValue &p : c[QStringLiteral("syncpoints")].toArray())
                points.emplace_back(p.toArray().at(0).toInt(), p.toArray().at(1).toInt());
            SyncPointMyersSequenceMatcher matcher(std::move(a), std::move(b), points);
            const QString context = QStringLiteral("syncpoint case %1").arg(index++);
            COMPARE_JSON(testutils::blocksToJson(matcher.matchingBlocks()), c[QStringLiteral("blocks")], context);
            COMPARE_JSON(testutils::toJsonArray(matcher.opcodes()), c[QStringLiteral("opcodes")], context);
        }
        QVERIFY(index > 50);
    }

    void largeInputsComplete()
    {
        // Two large, entirely different inputs sharing a few common lines:
        // this exercises the snake bookkeeping without a reference result.
        Sequence a;
        Sequence b;
        for (int i = 0; i < 4000; ++i) {
            a.push_back(i % 7 == 0 ? 1 : 1000 + i);
            b.push_back(i % 5 == 0 ? 1 : 100000 + i);
        }
        MyersSequenceMatcher matcher(a, b);
        const auto &result = matcher.matchingBlocks();
        QCOMPARE(result.back(), (MatchingBlock{4000, 4000, 0}));
        // Every reported block must be a genuine match
        for (const MatchingBlock &block : result) {
            for (int k = 0; k < block.size; ++k)
                QCOMPARE(a[size_t(block.a + k)], b[size_t(block.b + k)]);
        }
    }
};

QTEST_GUILESS_MAIN(TestMatchers)
#include "tst_matchers.moc"
