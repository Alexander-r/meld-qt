// Tests of text decoding/encoding and the difflib port

#include "testutils.h"

#include "core/textfile.h"
#include "core/unifieddiff.h"

#include <QTest>

using namespace meld;

class TestTextFormat : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void decodeUtf8()
    {
        const auto decoded = decodeText(QByteArray("h\xc3\xa9llo\r\nworld\r\n"), defaultCandidateEncodings());
        QVERIFY(decoded);
        QCOMPARE(decoded->text, QStringLiteral("héllo\nworld\n"));
        QCOMPARE(decoded->format.encoding, QByteArray("UTF-8"));
        QCOMPARE(decoded->format.newline, NewlineType::CRLF);
        QVERIFY(!decoded->format.bom);
    }

    void decodeFallsBackToLegacyEncoding()
    {
        // Invalid UTF-8; decodable as a single-byte encoding
        const auto decoded = decodeText(QByteArray("caf\xe9\n"), defaultCandidateEncodings());
        QVERIFY(decoded);
        QCOMPARE(decoded->text, QStringLiteral("café\n"));
        QVERIFY(decoded->format.encoding != QByteArray("UTF-8"));
    }

    void decodeBom()
    {
        const QByteArray data = QByteArray("\xFF\xFE", 2) + QByteArray("a\0\n\0", 4);
        const auto decoded = decodeText(data, defaultCandidateEncodings());
        QVERIFY(decoded);
        QCOMPARE(decoded->text, QStringLiteral("a\n"));
        QCOMPARE(decoded->format.encoding, QByteArray("UTF-16LE"));
        QVERIFY(decoded->format.bom);

        const auto encoded = encodeText(decoded->text, decoded->format);
        QVERIFY(encoded);
        QCOMPARE(*encoded, data);
    }

    void newlineRoundTrip_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<int>("newline");
        QTest::newRow("lf") << QByteArray("a\nb\n") << int(NewlineType::LF);
        QTest::newRow("crlf") << QByteArray("a\r\nb\r\n") << int(NewlineType::CRLF);
        QTest::newRow("cr") << QByteArray("a\rb\r") << int(NewlineType::CR);
    }

    void newlineRoundTrip()
    {
        QFETCH(QByteArray, data);
        QFETCH(int, newline);
        const auto decoded = decodeText(data, defaultCandidateEncodings());
        QVERIFY(decoded);
        QCOMPARE(int(decoded->format.newline), newline);
        QCOMPARE(decoded->text, QStringLiteral("a\nb\n"));
        QCOMPARE(encodeText(decoded->text, decoded->format).value_or(QByteArray()), data);
    }

    void unencodableCharacters()
    {
        TextFileFormat format;
        format.encoding = "ISO-8859-1";
        QVERIFY(!encodeText(QStringLiteral("snowman ☃"), format));
        QVERIFY(encodeText(QStringLiteral("café"), format));
    }

    void splitLines()
    {
        QCOMPARE(pySplitLines(QStringLiteral("a\nb\r\nc\rd"), true),
                 (QStringList{"a\n", "b\r\n", "c\r", "d"}));
        QCOMPARE(pySplitLines(QStringLiteral("a\n\n"), false), (QStringList{"a", ""}));
        QCOMPARE(pySplitLines(QString(), false), QStringList{});
    }

    void referenceUnifiedDiff()
    {
        int index = 0;
        for (const QJsonValue &value : testutils::reference().value(QStringLiteral("unified")).toArray()) {
            const QJsonObject c = value.toObject();
            const QString diff = unifiedDiff(testutils::toStringList(c[QStringLiteral("a")]),
                                             testutils::toStringList(c[QStringLiteral("b")]), QStringLiteral("a.txt"),
                                             QStringLiteral("b.txt"), c[QStringLiteral("n")].toInt());
            if (diff != c[QStringLiteral("diff")].toString())
                QFAIL(qPrintable(QStringLiteral("unified diff case %1 differs:\n%2\n---- expected ----\n%3")
                                     .arg(index)
                                     .arg(diff, c[QStringLiteral("diff")].toString())));
            ++index;
        }
        QVERIFY(index > 50);
    }

    void formatPatch()
    {
        const QString patch = meld::formatPatch({QStringLiteral("a\nb\nc"), QStringLiteral("a\nB\nc"), QString()},
                                                {QStringLiteral("/tmp/x/left.txt"), QStringLiteral("/tmp/x/right.txt"),
                                                 QStringLiteral("/tmp/x/other.txt")},
                                                0, 1);
        QCOMPARE(patch, QStringLiteral("--- left.txt\n+++ right.txt\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n"));
    }
};

QTEST_GUILESS_MAIN(TestTextFormat)
#include "tst_textformat.moc"
