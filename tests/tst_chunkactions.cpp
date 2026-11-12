// Qt documents don't store line terminators, so text is compared in its
// normalised form; the newline type is reapplied when saving a file.

#include "core/meldbuffer.h"
#include "core/textfile.h"

#include <QTest>

using namespace meld;

class TestChunkActions : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void deleteLastLine_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("expectedText");

        // Basic CRLF tests
        QTest::newRow("crlf 1") << "ree\r\neee" << "ree";
        QTest::newRow("crlf 2") << "ree\r\neee\r\n" << "ree\r\neee";
        // Basic CR tests
        QTest::newRow("cr 1") << "ree\neee" << "ree";
        QTest::newRow("cr 2") << "ree\neee\n" << "ree\neee";
        // Basic LF tests
        QTest::newRow("lf 1") << "ree\reee" << "ree";
        QTest::newRow("lf 2") << "ree\reee\r" << "ree\reee";
        // Mismatched newline and text
        QTest::newRow("mismatched") << "ree\r\neee" << "ree";
        // Mismatched newline types within text
        QTest::newRow("mixed 1") << "ree\r\neee\n" << "ree\r\neee";
        QTest::newRow("mixed 2") << "ree\r\neee\nqqq" << "ree\r\neee";
        QTest::newRow("mixed 3") << "ree\r\neee\nqqq\r\n" << "ree\r\neee\nqqq";
    }

    void deleteLastLine()
    {
        QFETCH(QString, text);
        QFETCH(QString, expectedText);

        MeldBuffer buffer;
        buffer.setText(text);
        const int end = buffer.lineCount();
        const int last = end - 1;
        chunkops::deleteChunk(&buffer, DiffChunk{ChunkTag::Delete, last, end, last, end});
        QCOMPARE(buffer.text(), normalizeNewlines(expectedText));
    }

    void replaceChunk()
    {
        MeldBuffer left;
        MeldBuffer right;
        left.setText(QStringLiteral("a\nb\nc\nd"));
        right.setText(QStringLiteral("a\nX\nY\nd"));
        // Replace right lines 1-3 with left lines 1-2
        chunkops::replaceChunk(&left, &right, DiffChunk{ChunkTag::Replace, 1, 2, 1, 3});
        QCOMPARE(right.text(), QStringLiteral("a\nb\nd"));
    }

    void replaceChunkAtEnd()
    {
        MeldBuffer left;
        MeldBuffer right;
        left.setText(QStringLiteral("a\nb"));
        right.setText(QStringLiteral("a"));
        // An insert of the final line: the inserted text needs a line break
        chunkops::replaceChunk(&left, &right, DiffChunk{ChunkTag::Insert, 1, 2, 1, 1});
        QCOMPARE(right.text(), QStringLiteral("a\nb"));
    }

    void copyChunkUpAndDown()
    {
        MeldBuffer left;
        left.setText(QStringLiteral("a\nb\nc"));
        MeldBuffer up;
        up.setText(QStringLiteral("a\nX\nc"));
        chunkops::copyChunk(&left, &up, DiffChunk{ChunkTag::Replace, 1, 2, 1, 2}, true);
        QCOMPARE(up.text(), QStringLiteral("a\nb\nX\nc"));

        MeldBuffer down;
        down.setText(QStringLiteral("a\nX\nc"));
        chunkops::copyChunk(&left, &down, DiffChunk{ChunkTag::Replace, 1, 2, 1, 2}, false);
        QCOMPARE(down.text(), QStringLiteral("a\nX\nb\nc"));
    }

    void copyLastLineUp()
    {
        MeldBuffer left;
        left.setText(QStringLiteral("a\nb"));
        MeldBuffer right;
        right.setText(QStringLiteral("a\nX"));
        // The source chunk ends the buffer, so a line break is added
        chunkops::copyChunk(&left, &right, DiffChunk{ChunkTag::Replace, 1, 2, 1, 2}, true);
        QCOMPARE(right.text(), QStringLiteral("a\nb\nX"));
    }

    void deleteMiddleChunk()
    {
        MeldBuffer buffer;
        buffer.setText(QStringLiteral("a\nb\nc\nd"));
        chunkops::deleteChunk(&buffer, DiffChunk{ChunkTag::Delete, 1, 3, 1, 1});
        QCOMPARE(buffer.text(), QStringLiteral("a\nd"));
    }

    void editsAreSingleUndoSteps()
    {
        MeldBuffer left;
        MeldBuffer right;
        left.setText(QStringLiteral("a\nb\nc\nd"));
        right.setText(QStringLiteral("a\nX\nY\nd"));
        UndoSequence undo;
        undo.addDocument(left.document());
        undo.addDocument(right.document());
        chunkops::replaceChunk(&left, &right, DiffChunk{ChunkTag::Replace, 1, 2, 1, 3});
        QVERIFY(undo.canUndo());
        QCOMPARE(undo.undo(), right.document());
        QCOMPARE(right.text(), QStringLiteral("a\nX\nY\nd"));
        QVERIFY(!undo.canUndo());
        QVERIFY(undo.canRedo());
        undo.redo();
        QCOMPARE(right.text(), QStringLiteral("a\nb\nd"));
    }
};

QTEST_MAIN(TestChunkActions)
#include "tst_chunkactions.moc"
