#include "core/filecompare.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace meld;

Q_DECLARE_METATYPE(meld::FileCompareResult)

namespace {

constexpr int ChunkSize = 4096 * 10;

struct Entry {
    QString path;
    QByteArray contents;
    bool isDir = false;
};

QList<Entry> sampleDefinition()
{
    const QByteArray d = QByteArray(ChunkSize, 'd') + "d";
    return {
        {"a", {}, true},
        {"a/a.txt", ""},
        {"a/c", {}, true},
        {"a/c/c.txt", ""},
        {"a/d", {}, true},
        {"a/d/d.txt", d},
        {"a/e", {}, true},
        {"a/e/f", {}, true},
        {"a/e/g", {}, true},
        {"a/e/g/g.txt", "g"},
        {"a/e/h", {}, true},
        {"a/e/h/h.txt", "h"},
        {"a/e/e.txt", ""},
        {"a/crlf.txt", "foo\r\nbar\r\n"},
        {"a/crlftrailing.txt", "foo\r\nbar\r\n\r\n"},
        {"b", {}, true},
        {"b/b.txt", ""},
        {"b/c", {}, true},
        {"b/c/c.txt", ""},
        {"b/d", {}, true},
        {"b/d/d.txt", d},
        {"b/d/d.1.txt", QByteArray(ChunkSize, 'D') + "D"},
        {"b/d/d.2.txt", QByteArray(ChunkSize, 'd') + "D"},
        {"b/e", {}, true},
        {"b/e/f", {}, true},
        {"b/e/f/f.txt", ""},
        {"b/e/g", {}, true},
        {"b/e/g/g.txt", ""},
        {"b/e/h", {}, true},
        {"b/e/h/h.txt", "h"},
        {"b/e/e.txt", ""},
        {"b/lf.txt", "foo\nbar\n"},
        {"b/lftrailing.txt", "foo\nbar\n\n"},
    };
}

} // namespace

class TestFileCompare : public QObject {
    Q_OBJECT

private:
    QTemporaryDir sampleDir_;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(sampleDir_.isValid());
        for (const Entry &entry : sampleDefinition()) {
            const QString path = sampleDir_.filePath(entry.path);
            if (entry.isDir) {
                QVERIFY(QDir().mkpath(path));
            } else {
                QFile file(path);
                QVERIFY(file.open(QIODevice::WriteOnly));
                file.write(entry.contents);
            }
        }
    }

    void filesSame_data()
    {
        QTest::addColumn<QStringList>("files");
        QTest::addColumn<bool>("shallow");
        QTest::addColumn<bool>("ignoreBlankLines");
        QTest::addColumn<bool>("applyTextFilters");
        QTest::addColumn<FileCompareResult>("expected");

        using R = FileCompareResult;
        QTest::newRow("empty file list") << QStringList{} << false << true << true << R::Same;
        QTest::newRow("dirs are same") << QStringList{"a", "b"} << false << true << true << R::Same;
        QTest::newRow("dir and file are different") << QStringList{"a", "b/b.txt"} << false << true << true
                                                    << R::Different;
        QTest::newRow("shallow equal (time + size)")
            << QStringList{"a/d/d.txt", "b/d/d.1.txt"} << true << true << true << R::DodgySame;
        QTest::newRow("empty files (fastest equal, won't read files)")
            << QStringList{"a/c/c.txt", "b/c/c.txt"} << false << true << true << R::Same;
        QTest::newRow("4.1kb vs 4.1kb file (slow equal, read both until end)")
            << QStringList{"a/d/d.txt", "b/d/d.txt"} << false << true << true << R::Same;
        QTest::newRow("4.1kb vs 4.1kb file (fast different, first chunk diff)")
            << QStringList{"a/d/d.txt", "b/d/d.1.txt"} << false << true << true << R::Different;
        QTest::newRow("4.1kb vs 4.1kb file (slow different, read both until end)")
            << QStringList{"a/d/d.txt", "b/d/d.2.txt"} << false << true << true << R::Different;
        QTest::newRow("empty vs 1b file (fast different, first chunk diff)")
            << QStringList{"a/e/g/g.txt", "b/e/g/g.txt"} << false << true << true << R::Different;
        QTest::newRow("CRLF vs CRLF with trailing, ignoring blank lines")
            << QStringList{"a/crlf.txt", "a/crlftrailing.txt"} << false << true << true << R::SameFiltered;
        QTest::newRow("CRLF vs CRLF with trailing, not ignoring blank lines")
            << QStringList{"a/crlf.txt", "a/crlftrailing.txt"} << false << false << false << R::Different;
        QTest::newRow("LF vs LF with trailing, ignoring blank lines")
            << QStringList{"b/lf.txt", "b/lftrailing.txt"} << false << true << true << R::SameFiltered;
        QTest::newRow("LF vs LF with trailing, not ignoring blank lines")
            << QStringList{"b/lf.txt", "b/lftrailing.txt"} << false << false << false << R::Different;
        QTest::newRow("CRLF vs LF, ignoring blank lines")
            << QStringList{"a/crlf.txt", "b/lf.txt"} << false << true << true << R::SameFiltered;
        QTest::newRow("CRLF vs LF, not ignoring blank lines")
            << QStringList{"a/crlf.txt", "b/lf.txt"} << false << false << false << R::Different;
        QTest::newRow("CRLF with trailing vs LF with trailing, ignoring blank lines")
            << QStringList{"a/crlftrailing.txt", "b/lftrailing.txt"} << false << true << true << R::SameFiltered;
        QTest::newRow("CRLF with trailing vs LF with trailing, not ignoring blank lines")
            << QStringList{"a/crlftrailing.txt", "b/lftrailing.txt"} << false << false << false << R::Different;
    }

    void filesSame()
    {
        QFETCH(QStringList, files);
        QFETCH(bool, shallow);
        QFETCH(bool, ignoreBlankLines);
        QFETCH(bool, applyTextFilters);
        QFETCH(FileCompareResult, expected);

        QStringList paths;
        for (const QString &file : std::as_const(files))
            paths.append(sampleDir_.filePath(file));

        ComparisonArgs args;
        args.shallowComparison = shallow;
        args.timeResolutionNs = 10'000'000'000LL;
        args.ignoreBlankLines = ignoreBlankLines;
        args.applyTextFilters = applyTextFilters;
        QCOMPARE(meld::filesSame(paths, {}, args), expected);
    }

    void textFiltersDuringComparison()
    {
        QTemporaryDir dir;
        QFile one(dir.filePath(QStringLiteral("one.c")));
        QVERIFY(one.open(QIODevice::WriteOnly));
        one.write("int a; // first\n");
        one.close();
        QFile two(dir.filePath(QStringLiteral("two.c")));
        QVERIFY(two.open(QIODevice::WriteOnly));
        two.write("int a; // second comment\n");
        two.close();

        ComparisonArgs args;
        args.ignoreBlankLines = false;
        args.applyTextFilters = true;
        const QStringList files{one.fileName(), two.fileName()};
        QCOMPARE(meld::filesSame(files, {}, args), FileCompareResult::Different);
        const QRegularExpression comment(QStringLiteral("//.*"), QRegularExpression::MultilineOption);
        QCOMPARE(meld::filesSame(files, {comment}, args), FileCompareResult::SameFiltered);
    }

    void removeBlankLines_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<QByteArray>("expected");

        QTest::newRow("blank to be equal blank") << QByteArray("") << QByteArray("");
        QTest::newRow("one line with spaces") << QByteArray(" ") << QByteArray(" ");
        QTest::newRow("two lines empty 1") << QByteArray("\n") << QByteArray("");
        QTest::newRow("two lines empty 2") << QByteArray("\n ") << QByteArray(" ");
        QTest::newRow("two lines empty 3") << QByteArray(" \n") << QByteArray(" ");
        QTest::newRow("two lines empty 4") << QByteArray(" \n ") << QByteArray(" \n ");
        QTest::newRow("three lines empty 1") << QByteArray("\n\n") << QByteArray("");
        QTest::newRow("three lines empty 2") << QByteArray("\n\n ") << QByteArray(" ");
        QTest::newRow("three lines empty 3") << QByteArray("\n \n") << QByteArray(" ");
        QTest::newRow("three lines empty 4") << QByteArray("\n \n ") << QByteArray(" \n ");
        QTest::newRow("three lines empty 5") << QByteArray(" \n \n ") << QByteArray(" \n \n ");
        QTest::newRow("one line with space and content") << QByteArray(" content") << QByteArray(" content");
        QTest::newRow("empty line between content") << QByteArray("content\n\ncontent")
                                                    << QByteArray("content\ncontent");
        QTest::newRow("multiple leading and trailing newlines")
            << QByteArray("\n\n\ncontent\n\n\ncontent\n\n\n") << QByteArray("content\ncontent");
    }

    void removeBlankLines()
    {
        QFETCH(QByteArray, text);
        QFETCH(QByteArray, expected);
        QCOMPARE(meld::removeBlankLines(text), expected);
    }
};

QTEST_GUILESS_MAIN(TestFileCompare)
#include "tst_filecompare.moc"
