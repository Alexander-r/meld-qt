#include "core/filters.h"

#include <QTest>

using namespace meld;

class TestFilters : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fileFilters_data()
    {
        QTest::addColumn<QStringList>("patterns");
        QTest::addColumn<QString>("filename");
        QTest::addColumn<bool>("expectedMatch");

        QTest::newRow("1") << QStringList{"*.csv"} << "foo.csv" << true;
        QTest::newRow("2") << QStringList{"*.cvs"} << "foo.csv" << false;
        QTest::newRow("3") << QStringList{"*.csv *.xml"} << "foo.csv" << true;
        QTest::newRow("4") << QStringList{"*.csv *.xml"} << "foo.xml" << true;
        QTest::newRow("5") << QStringList{"*.csv", "*.xml"} << "foo.csv" << true;
        QTest::newRow("6") << QStringList{"*.csv", "*.xml"} << "foo.xml" << true;
        QTest::newRow("7") << QStringList{"*.csv", "*.xml"} << "dumbtest" << false;
        QTest::newRow("8") << QStringList{"thing*csv"} << "thingcsv" << true;
        QTest::newRow("9") << QStringList{"thing*csv"} << "thingwhatevercsv" << true;
        QTest::newRow("10") << QStringList{"thing*csv"} << "csvthing" << false;
        // Default filter patterns
        QTest::newRow("backup ~") << QStringList{"#*# .#* ~* *~ *.{orig,bak,swp}"} << "file.txt~" << true;
        QTest::newRow("backup orig") << QStringList{"#*# .#* ~* *~ *.{orig,bak,swp}"} << "file.orig" << true;
        QTest::newRow("backup no match") << QStringList{"#*# .#* ~* *~ *.{orig,bak,swp}"} << "file.origx" << false;
        QTest::newRow("vcs") << QStringList{"_MTN .bzr .svn .svn .hg .fslckout _FOSSIL_ .fos CVS _darcs .git .svn .osc"}
                             << ".git" << true;
        QTest::newRow("vcs partial") << QStringList{".git"} << ".gitignore" << false;
        QTest::newRow("char class") << QStringList{"[!a]*"} << "abc" << false;
        QTest::newRow("char class 2") << QStringList{"[!a]*"} << "bcd" << true;
        QTest::newRow("question") << QStringList{"?.c"} << "a.c" << true;
    }

    void fileFilters()
    {
        QFETCH(QStringList, patterns);
        QFETCH(QString, filename);
        QFETCH(bool, expectedMatch);

        FilterList filters;
        for (const QString &pattern : patterns)
            filters.append(FilterEntry::fromSettings(QStringLiteral("name"), true, pattern, FilterEntry::Shell));

        // All of the dirdiff logic is "does this match any filter", so
        // that's what we test here, even if it looks a bit weird.
        bool match = false;
        for (const FilterEntry &f : std::as_const(filters))
            match = match || (f.filter && pyMatch(*f.filter, filename));
        QCOMPARE(match, expectedMatch);
    }

    void badRegexCompilation_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::newRow("Trailing wildcard") << QStringLiteral("*.foo*");
        QTest::newRow("Invalid escape") << QStringLiteral("\\xfoo");
        QTest::newRow("Unknown escape") << QStringLiteral("\\q");
        QTest::newRow("Unterminated set") << QStringLiteral("[abc");
    }

    void badRegexCompilation()
    {
        QFETCH(QString, pattern);
        const FilterEntry f = FilterEntry::fromSettings(QStringLiteral("name"), true, pattern, FilterEntry::Regex);
        QVERIFY(!f.filter);
        QVERIFY(!f.active);
    }

    void defaultTextFiltersCompile()
    {
        const QStringList patterns{
            QStringLiteral(R"(\$\w+(:[^\n$]+)?\$)"), QStringLiteral("//.*"),
            QStringLiteral(R"(/\*.*?\*/)"),          QStringLiteral(R"([ \t\r\f\v]*)"),
            QStringLiteral(R"(^[ \t\r\f\v]*)"),      QStringLiteral(R"([ \t\r\f\v]*$)"),
            QStringLiteral("#.*"),
        };
        for (const QString &pattern : patterns) {
            const FilterEntry f = FilterEntry::fromSettings(QStringLiteral("name"), true, pattern, FilterEntry::Regex);
            QVERIFY2(f.filter, qPrintable(pattern));
            QVERIFY2(f.byteFilter, qPrintable(pattern));
            QVERIFY(f.active);
        }
    }

    void pythonTranslation_data()
    {
        QTest::addColumn<QString>("pattern");
        QTest::addColumn<QString>("subject");
        QTest::addColumn<bool>("matches");

        QTest::newRow("hex escape") << QStringLiteral("\\x41") << QStringLiteral("A") << true;
        QTest::newRow("unicode escape") << QStringLiteral("\\u00e9") << QStringLiteral("é") << true;
        QTest::newRow("vertical tab only") << QStringLiteral("^\\v$") << QStringLiteral("\n") << false;
        QTest::newRow("end of string") << QStringLiteral("a\\Z") << QStringLiteral("a\n") << false;
        QTest::newRow("backreference") << QStringLiteral("(a)\\1") << QStringLiteral("aa") << true;
        QTest::newRow("octal") << QStringLiteral("\\101") << QStringLiteral("A") << true;
        QTest::newRow("unicode word") << QStringLiteral("^\\w+$") << QStringLiteral("héllo") << true;
    }

    void pythonTranslation()
    {
        QFETCH(QString, pattern);
        QFETCH(QString, subject);
        QFETCH(bool, matches);
        const auto regex = tryCompile(pattern, QRegularExpression::UseUnicodePropertiesOption);
        QVERIFY(regex);
        QCOMPARE(regex->match(subject).hasMatch(), matches);
    }

    void shellToRegexEscapes()
    {
        QCOMPARE(shellToRegex(QStringLiteral("*.c")), QStringLiteral(".*\\.c$"));
        QCOMPARE(shellToRegex(QStringLiteral("{a,b}")), QStringLiteral("(a|b)$"));
        QCOMPARE(shellToRegex(QStringLiteral("[^x]")), QStringLiteral("[\\^x]$"));
    }
};

QTEST_GUILESS_MAIN(TestFilters)
#include "tst_filters.moc"
