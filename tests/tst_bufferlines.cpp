#include "core/meldbuffer.h"

#include <QTest>
#include <QTextBlock>

using namespace meld;

namespace {

const QString sampleText = QStringLiteral("0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10");

using Cache = std::vector<std::optional<QString>>;

Cache slice(const Cache &cache, size_t lo, size_t hi)
{
    return Cache(cache.begin() + qsizetype(lo), cache.begin() + qsizetype(std::min(hi, cache.size())));
}

Cache cached(std::initializer_list<const char *> values)
{
    Cache result;
    for (const char *v : values) {
        if (v)
            result.emplace_back(QString::fromUtf8(v));
        else
            result.emplace_back(std::nullopt);
    }
    return result;
}

QStringList capturedMessages;

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
    if (type == QtCriticalMsg)
        capturedMessages.append(message);
}

int lineStart(MeldBuffer &buffer, int line)
{
    return buffer.document()->findBlockByNumber(line).position();
}

} // namespace

class TestBufferLines : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void slicing_data()
    {
        QTest::addColumn<int>("lineStart");
        QTest::addColumn<int>("lineEnd");
        QTest::addColumn<QStringList>("expectedText");

        QTest::newRow("0:1") << 0 << 1 << QStringList{"0"};
        QTest::newRow("0:2") << 0 << 2 << QStringList{"0", "1"};
        QTest::newRow("zero-sized slice") << 9 << 9 << QStringList{};
        QTest::newRow("9:10") << 9 << 10 << QStringList{"9"};
        QTest::newRow("9:11") << 9 << 11 << QStringList{"9", "10"};
        QTest::newRow("Past the end of the buffer") << 9 << 12 << QStringList{"9", "10"};
        QTest::newRow("Waaaay past the end of the buffer") << 9 << 9999 << QStringList{"9", "10"};
        QTest::newRow("10:12") << 10 << 12 << QStringList{"10"};
        QTest::newRow("11:12") << 11 << 12 << QStringList{};
    }

    void slicing()
    {
        QFETCH(int, lineStart);
        QFETCH(int, lineEnd);
        QFETCH(QStringList, expectedText);
        MeldBuffer buffer;
        buffer.setText(sampleText);
        BufferLines lines(&buffer);
        QCOMPARE(lines.lines(lineStart, lineEnd), expectedText);
    }

    void indexOutOfRange()
    {
        MeldBuffer buffer;
        buffer.setText(sampleText);
        BufferLines lines(&buffer);
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, lines.at(11));
        QCOMPARE(lines.at(10), QStringLiteral("10"));
    }

    void cachedContents()
    {
        MeldBuffer buffer;
        buffer.setText(sampleText);
        BufferLines lines(&buffer);
        const QStringList textLines = sampleText.split(QLatin1Char('\n'));
        QCOMPARE(int(lines.cache().size()), lines.lineCount());
        QCOMPARE(lines.lineCount(), int(textLines.size()));

        // Check that without access, we have no cached contents
        QCOMPARE(lines.cache(), Cache(textLines.size()));

        // Access the lines so that they're cached
        lines.lines(0);

        Cache expected;
        for (const QString &line : textLines)
            expected.emplace_back(line);
        QCOMPARE(lines.cache(), expected);
    }

    void insertText()
    {
        MeldBuffer buffer;
        buffer.setText(sampleText);
        BufferLines lines(&buffer);
        lines.lines(0);
        QCOMPARE(slice(lines.cache(), 4, 8), cached({"4", "5", "6", "7"}));

        // Insert at the start of line 5, invalidating line 5 and adding a
        // new (uncached) line.
        buffer.insertText(lineStart(buffer, 5), QStringLiteral("hey\nthings"));
        QCOMPARE(slice(lines.cache(), 4, 8), cached({"4", nullptr, nullptr, "6"}));

        QCOMPARE(lines.lines(5, 7), (QStringList{"hey", "things5"}));
        QCOMPARE(slice(lines.cache(), 4, 8), cached({"4", "hey", "things5", "6"}));
    }

    void deleteRange()
    {
        MeldBuffer buffer;
        buffer.setText(sampleText);
        BufferLines lines(&buffer);
        lines.lines(0);
        QCOMPARE(slice(lines.cache(), 4, 8), cached({"4", "5", "6", "7"}));

        // Delete from the start of line 5 to the start of line 7,
        // invalidating line 7 but leaving its contents intact.
        buffer.deleteRange(lineStart(buffer, 5), lineStart(buffer, 7));
        QCOMPARE(slice(lines.cache(), 4, 7), cached({"4", nullptr, "8"}));

        QCOMPARE(lines.at(5), QStringLiteral("7"));
        QCOMPARE(slice(lines.cache(), 4, 7), cached({"4", "7", "8"}));
    }

    void cacheDebug()
    {
        MeldBuffer buffer;
        buffer.setText(sampleText);
        BufferLines lines(&buffer, {}, true);

        // Invalidate our line cache...
        lines.cache().emplace_back(QStringLiteral("invalid"));

        // ...and check that insertion/deletion logs an error
        capturedMessages.clear();
        const QtMessageHandler old = qInstallMessageHandler(messageHandler);
        buffer.insertText(lineStart(buffer, 5), QStringLiteral("hey"));
        QCOMPARE(capturedMessages.size(), 1);
        QVERIFY(capturedMessages.first().startsWith(QStringLiteral("Cache line count does not match")));
        capturedMessages.clear();

        buffer.deleteRange(lineStart(buffer, 5), lineStart(buffer, 7));
        QCOMPARE(capturedMessages.size(), 1);
        QVERIFY(capturedMessages.first().startsWith(QStringLiteral("Cache line count does not match")));
        qInstallMessageHandler(old);
    }

    void filteredLines()
    {
        MeldBuffer buffer;
        buffer.setText(QStringLiteral("a1\nb2\nc3"));
        BufferLines lines(&buffer, [](const QString &text, int, int) {
            QString filtered = text;
            filtered.remove(QRegularExpression(QStringLiteral("\\d")));
            return filtered;
        });
        QCOMPARE(lines.lines(0), (QStringList{"a", "b", "c"}));
    }

    void lineChangeSignals()
    {
        MeldBuffer buffer;
        buffer.setText(sampleText);
        QList<QList<int>> changes;
        connect(&buffer, &MeldBuffer::linesChanged, this,
                [&](int start, int removed, int added) { changes.append({start, removed, added}); });
        buffer.insertText(lineStart(buffer, 2), QStringLiteral("x"));
        buffer.insertText(lineStart(buffer, 3), QStringLiteral("a\nb\n"));
        buffer.deleteRange(lineStart(buffer, 3), lineStart(buffer, 5));
        QCOMPARE(changes, (QList<QList<int>>{{2, 0, 0}, {3, 0, 2}, {3, 2, 0}}));
    }
};

QTEST_MAIN(TestBufferLines)
#include "tst_bufferlines.moc"
