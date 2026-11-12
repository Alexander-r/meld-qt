#include "testutils.h"

#include "core/diffutil.h"

#include <QSignalSpy>
#include <QTest>

using namespace meld;

namespace {

std::vector<StringListSource> makeSources(const QJsonArray &texts)
{
    std::vector<StringListSource> sources;
    for (const QJsonValue &text : texts)
        sources.emplace_back(testutils::toStringList(text));
    return sources;
}

Texts pointers(const std::vector<StringListSource> &sources)
{
    Texts texts;
    for (const StringListSource &s : sources)
        texts.push_back(&s);
    return texts;
}

QJsonValue locationJson(const ChunkLocation &loc)
{
    return QJsonArray{testutils::optIntJson(loc.chunk), testutils::optIntJson(loc.prev),
                      testutils::optIntJson(loc.next)};
}

void applyEdit(std::vector<StringListSource> &sources, const QJsonObject &edit)
{
    QStringList &lines = sources[size_t(edit[QStringLiteral("pane")].toInt())].list();
    const QString op = edit[QStringLiteral("op")].toString();
    const int line = edit[QStringLiteral("line")].toInt();
    if (op == QLatin1String("insert")) {
        const QStringList newLines = testutils::toStringList(edit[QStringLiteral("lines")]);
        for (int i = 0; i < newLines.size(); ++i)
            lines.insert(std::min<qsizetype>(line + i, lines.size()), newLines.at(i));
    } else if (op == QLatin1String("delete")) {
        lines.remove(line, edit[QStringLiteral("count")].toInt());
    } else {
        lines[line] = edit[QStringLiteral("text")].toString();
    }
}

} // namespace

class TestDiffutil : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void twoWayBasics()
    {
        StringListSource left({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        StringListSource right({QStringLiteral("a"), QStringLiteral("x"), QStringLiteral("c"), QStringLiteral("d")});
        Differ differ;
        QSignalSpy spy(&differ, &Differ::diffsChanged);
        differ.setSequences({&left, &right});
        QCOMPARE(spy.count(), 1);
        QCOMPARE(differ.diffCount(), 2);
        // Chunks are stored from the middle (index 1) to pane 0
        QCOMPARE(*differ.getChunk(0, 1, 0), (DiffChunk{ChunkTag::Replace, 1, 2, 1, 2}));
        QCOMPARE(*differ.getChunk(1, 0), (DiffChunk{ChunkTag::Insert, 3, 3, 3, 4}));
        QVERIFY(!differ.sequencesIdentical());
        QCOMPARE(differ.locateChunk(1, 1).chunk, std::optional<int>(0));
        QCOMPARE(differ.locateChunk(1, 0).next, std::optional<int>(0));
    }

    void identicalSequences()
    {
        StringListSource left({QStringLiteral("a"), QStringLiteral("b")});
        StringListSource right({QStringLiteral("a"), QStringLiteral("b")});
        Differ differ;
        QVERIFY(!differ.sequencesIdentical());
        differ.setSequences({&left, &right});
        QVERIFY(differ.sequencesIdentical());
        differ.clear();
        QVERIFY(!differ.sequencesIdentical());
        QCOMPARE(differ.diffCount(), 0);
    }

    void referenceDiffer()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("differ")).toArray()) {
            const QJsonObject c = value.toObject();
            const QString context = QStringLiteral("differ case %1").arg(index++);
            const std::vector<StringListSource> sources = makeSources(c[QStringLiteral("texts")].toArray());
            const int n = int(sources.size());
            Differ differ;
            differ.ignoreBlanks = c[QStringLiteral("ignore_blanks")].toBool();
            differ.setSequences(pointers(sources));

            COMPARE_JSON(testutils::toJsonArray(differ.allChanges()), c[QStringLiteral("merge_cache")], context);
            QJsonArray conflicts;
            for (int conflict : differ.conflicts)
                conflicts.append(conflict);
            COMPARE_JSON(conflicts, c[QStringLiteral("conflicts")], context + QStringLiteral(" conflicts"));
            QCOMPARE(differ.sequencesIdentical(), c[QStringLiteral("identical")].toBool());

            QJsonArray mergeable;
            for (int i = 0; i < n; ++i) {
                const auto [m0, m1] = differ.hasMergeableChanges(i);
                mergeable.append(QJsonArray{m0, m1});
            }
            COMPARE_JSON(mergeable, c[QStringLiteral("mergeable")], context + QStringLiteral(" mergeable"));

            QJsonArray lineCache;
            for (int pane = 0; pane < n; ++pane) {
                QJsonArray paneCache;
                for (int line = 0; line < sources[size_t(pane)].lineCount() + 2; ++line)
                    paneCache.append(locationJson(differ.locateChunk(pane, line)));
                lineCache.append(paneCache);
            }
            COMPARE_JSON(lineCache, c[QStringLiteral("line_cache")], context + QStringLiteral(" line cache"));

            QJsonArray single;
            for (int pane = 0; pane < n; ++pane)
                single.append(testutils::toJsonArray(differ.singleChanges(pane)));
            COMPARE_JSON(single, c[QStringLiteral("single_changes")], context + QStringLiteral(" single"));

            const QJsonObject pairs = c[QStringLiteral("pair_changes")].toObject();
            for (auto it = pairs.begin(); it != pairs.end(); ++it) {
                const int from = it.key().at(0).digitValue();
                const int to = it.key().at(1).digitValue();
                COMPARE_JSON(testutils::toJsonArray(differ.pairChanges(from, to)), it.value(),
                             context + QStringLiteral(" pair ") + it.key());
            }

            const QJsonArray chunks = c[QStringLiteral("chunks")].toArray();
            QCOMPARE(chunks.size(), differ.diffCount());
            for (int i = 0; i < differ.diffCount(); ++i) {
                const QJsonObject entry = chunks.at(i).toObject();
                const auto starts = differ.getChunkStarts(i);
                COMPARE_JSON((QJsonArray{testutils::optIntJson(starts[0]), testutils::optIntJson(starts[1]),
                                         testutils::optIntJson(starts[2])}),
                             entry[QStringLiteral("starts")], context + QStringLiteral(" starts"));
                const QJsonObject gets = entry[QStringLiteral("get")].toObject();
                for (auto it = gets.begin(); it != gets.end(); ++it) {
                    const int from = it.key().at(0).digitValue();
                    const int to = it.key().size() > 1 ? it.key().at(1).digitValue() : -1;
                    COMPARE_JSON(testutils::toJson(differ.getChunk(i, from, to)), it.value(),
                                 context + QStringLiteral(" get_chunk %1 ").arg(i) + it.key());
                }
            }

            for (const QJsonValue &rangeValue : c[QStringLiteral("ranges")].toArray()) {
                const QJsonObject range = rangeValue.toObject();
                const QJsonArray lines = range[QStringLiteral("lines")].toArray();
                if (range.contains(QStringLiteral("from"))) {
                    const std::array<int, 4> bounds{lines.at(0).toInt(), lines.at(1).toInt(), lines.at(2).toInt(),
                                                    lines.at(3).toInt()};
                    const auto result = differ.pairChanges(range[QStringLiteral("from")].toInt(),
                                                           range[QStringLiteral("to")].toInt(), bounds);
                    COMPARE_JSON(testutils::toJsonArray(result), range[QStringLiteral("result")],
                                 context + QStringLiteral(" ranged pair changes"));
                } else {
                    const auto result = differ.singleChanges(range[QStringLiteral("pane")].toInt(),
                                                             std::pair{lines.at(0).toInt(), lines.at(1).toInt()});
                    COMPARE_JSON(testutils::toJsonArray(result), range[QStringLiteral("result")],
                                 context + QStringLiteral(" ranged single changes"));
                }
            }
        }
        QVERIFY(index > 100);
    }

    void referenceDifferEdits()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("differ_edits")).toArray()) {
            const QJsonObject c = value.toObject();
            std::vector<StringListSource> sources = makeSources(c[QStringLiteral("texts")].toArray());
            Differ differ;
            std::optional<ChunkChanges> lastChanges;
            connect(&differ, &Differ::diffsChanged, this,
                    [&](const ChunkChanges &changes) { lastChanges = changes; });
            differ.setSequences(pointers(sources));

            int step = 0;
            for (const QJsonValue &stepValue : c[QStringLiteral("steps")].toArray()) {
                const QJsonObject s = stepValue.toObject();
                const QString context = QStringLiteral("differ edit case %1 step %2").arg(index).arg(step++);
                applyEdit(sources, s[QStringLiteral("edit")].toObject());
                const QJsonArray call = s[QStringLiteral("call")].toArray();
                lastChanges.reset();
                differ.changeSequence(call.at(0).toInt(), call.at(1).toInt(), call.at(2).toInt(), pointers(sources));
                QVERIFY(lastChanges);

                COMPARE_JSON(testutils::toJsonArray(differ.allChanges()), s[QStringLiteral("merge_cache")], context);
                QJsonArray removed;
                for (const MergedChunk &chunk : std::as_const(lastChanges->removed))
                    removed.append(testutils::toJson(chunk));
                QJsonArray added;
                for (const MergedChunk &chunk : std::as_const(lastChanges->added))
                    added.append(testutils::toJson(chunk));
                COMPARE_JSON(testutils::sortedByJson(removed),
                             testutils::sortedByJson(s[QStringLiteral("removed")].toArray()),
                             context + QStringLiteral(" removed"));
                COMPARE_JSON(testutils::sortedByJson(added),
                             testutils::sortedByJson(s[QStringLiteral("added")].toArray()),
                             context + QStringLiteral(" added"));
                COMPARE_JSON(lastChanges->modified ? testutils::toJson(*lastChanges->modified) : QJsonValue(),
                             s[QStringLiteral("modified")], context + QStringLiteral(" modified"));
                QJsonArray conflicts;
                for (int conflict : differ.conflicts)
                    conflicts.append(conflict);
                COMPARE_JSON(conflicts, s[QStringLiteral("conflicts")], context + QStringLiteral(" conflicts"));
            }
            ++index;
        }
        QVERIFY(index > 50);
    }

    void replaceMatchesSeparateOperations()
    {
        // A combined replacement (as reported by Qt) must yield chunks that
        // are consistent with the text, i.e. equal regions between chunks.
        StringListSource left({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d"),
                               QStringLiteral("e"), QStringLiteral("f")});
        StringListSource right({QStringLiteral("a"), QStringLiteral("x"), QStringLiteral("c"), QStringLiteral("y"),
                                QStringLiteral("e"), QStringLiteral("f")});
        Differ differ;
        differ.setSequences({&left, &right});
        QCOMPARE(differ.diffCount(), 2);

        // Replace middle lines 1..4 ("x", "c", "y", "e") with "b", "c", "d"
        right.list() = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d"),
                        QStringLiteral("f")};
        differ.replaceInSequence(1, 1, 3, 2, {&left, &right});
        // The texts are now a, b, c, d, e, f vs a, b, c, d, f
        QCOMPARE(differ.diffCount(), 1);
        QCOMPARE(*differ.getChunk(0, 1, 0), (DiffChunk{ChunkTag::Insert, 4, 4, 4, 5}));
    }
};

QTEST_GUILESS_MAIN(TestDiffutil)
#include "tst_diffutil.moc"
