// Differential tests of the merge support

#include "testutils.h"

#include "core/merge.h"

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

QJsonArray intArray(const std::vector<int> &values)
{
    QJsonArray array;
    for (int v : values)
        array.append(v);
    return array;
}

} // namespace

class TestMerge : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void simpleThreeWayMerge()
    {
        // Non-conflicting changes on both sides are combined
        StringListSource left({QStringLiteral("a"), QStringLiteral("B"), QStringLiteral("c"), QStringLiteral("d")});
        StringListSource base({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")});
        StringListSource right({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("D")});
        Merger merger;
        for (bool done : merger.initialize({&left, &base, &right}, {&left, &base, &right}))
            Q_UNUSED(done);
        QCOMPARE(merger.merge3Files(), QStringLiteral("a\nB\nc\nD"));
        QVERIFY(merger.unresolved.empty());
    }

    void conflictsAreMarked()
    {
        StringListSource left({QStringLiteral("a"), QStringLiteral("x"), QStringLiteral("c")});
        StringListSource base({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
        StringListSource right({QStringLiteral("a"), QStringLiteral("y"), QStringLiteral("c")});
        Merger merger;
        for (bool done : merger.initialize({&left, &base, &right}, {&left, &base, &right}))
            Q_UNUSED(done);
        QCOMPARE(merger.merge3Files(true), QStringLiteral("a\n(\?\?)b\nc"));
        QCOMPARE(merger.unresolved, std::vector<int>{1});
        QCOMPARE(merger.merge3Files(false), QStringLiteral("a\nb\nc"));
    }

    void referenceMerge()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("merge")).toArray()) {
            const QJsonObject c = value.toObject();
            const QString context = QStringLiteral("merge case %1").arg(index++);
            const std::vector<StringListSource> sources = makeSources(c[QStringLiteral("texts")].toArray());
            const Texts texts = pointers(sources);

            Merger merger;
            for (bool done : merger.initialize(texts, texts))
                Q_UNUSED(done);
            COMPARE_JSON(testutils::toJsonArray(merger.differ()->allChanges()), c[QStringLiteral("auto_merge_cache")],
                         context + QStringLiteral(" auto merge cache"));
            QCOMPARE(merger.merge3Files(true), c[QStringLiteral("merge3_marked")].toString());
            COMPARE_JSON(intArray(merger.unresolved), c[QStringLiteral("unresolved")],
                         context + QStringLiteral(" unresolved"));
            QCOMPARE(merger.merge3Files(false), c[QStringLiteral("merge3_unmarked")].toString());

            Differ plain;
            plain.setSequences(texts);
            Merger plainMerger;
            plainMerger.setDiffer(&plain);
            plainMerger.texts = texts;
            const QJsonObject merge2 = c[QStringLiteral("merge2")].toObject();
            for (auto it = merge2.begin(); it != merge2.end(); ++it) {
                const int src = it.key().at(0).digitValue();
                const int dst = it.key().at(1).digitValue();
                QCOMPARE(plainMerger.merge2Files(src, dst), it.value().toString());
            }
            QCOMPARE(plainMerger.merge3Files(false), c[QStringLiteral("plain_merge_all")].toString());

            const Texts twoTexts{texts[0], texts[1]};
            Differ plain2;
            plain2.setSequences(twoTexts);
            Merger merger2;
            merger2.setDiffer(&plain2);
            merger2.texts = twoTexts;
            const QJsonObject twoPane = c[QStringLiteral("two_pane_merge2")].toObject();
            for (auto it = twoPane.begin(); it != twoPane.end(); ++it) {
                const int src = it.key().at(0).digitValue();
                const int dst = it.key().at(1).digitValue();
                QCOMPARE(merger2.merge2Files(src, dst), it.value().toString());
            }

            if (c.contains(QStringLiteral("edit_steps"))) {
                std::vector<StringListSource> editSources = makeSources(c[QStringLiteral("edit_texts")].toArray());
                AutoMergeDiffer differ;
                for (const QJsonValue &u : c[QStringLiteral("unresolved")].toArray())
                    differ.unresolved.push_back(u.toInt());
                differ.setSequences(pointers(editSources));
                int step = 0;
                for (const QJsonValue &stepValue : c[QStringLiteral("edit_steps")].toArray()) {
                    const QJsonObject s = stepValue.toObject();
                    const QString stepContext = context + QStringLiteral(" edit step %1").arg(step++);
                    applyEdit(editSources, s[QStringLiteral("edit")].toObject());
                    const QJsonArray call = s[QStringLiteral("call")].toArray();
                    differ.changeSequence(call.at(0).toInt(), call.at(1).toInt(), call.at(2).toInt(),
                                          pointers(editSources));
                    COMPARE_JSON(intArray(differ.unresolved), s[QStringLiteral("unresolved")],
                                 stepContext + QStringLiteral(" unresolved"));
                    COMPARE_JSON(testutils::toJsonArray(differ.allChanges()), s[QStringLiteral("merge_cache")],
                                 stepContext + QStringLiteral(" merge cache"));
                }
            }
        }
        QVERIFY(index > 100);
    }
};

QTEST_GUILESS_MAIN(TestMerge)
#include "tst_merge.moc"
