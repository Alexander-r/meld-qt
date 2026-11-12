// Tests for syntax highlighting (src/core/syntax)
//
// The original highlights syntax with GtkSourceView, and has no tests of its
// own for it. These are differential tests against GtkSourceView 5 using the
// same language definitions and colour schemes (see
// tools/generate_syntax_reference_data.py), and unit tests of the port.
//
// MELD_SYNTAX_REFERENCE may name other reference data, e.g., generated from
// more samples than the test data has.

#include "core/syntax/contextengine.h"
#include "core/syntax/language.h"
#include "core/syntax/stylescheme.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSet>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace meld::syntax;

namespace {

QString referencePath()
{
    const QString path = qEnvironmentVariable("MELD_SYNTAX_REFERENCE");
    return path.isEmpty() ? QStringLiteral(MELD_TEST_DATA_DIR "/syntax_reference.json") : path;
}

const QJsonObject &reference()
{
    static const QJsonObject data = [] {
        QFile file(referencePath());
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Couldn't open reference data %s", qPrintable(file.fileName()));
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return data;
}

QStringList toStringList(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &v : value.toArray())
        result.append(v.toString());
    return result;
}

QString colorName(const QColor &color)
{
    const QRgba64 rgba = color.rgba64();
    const auto channel = [](quint16 value) { return int(std::lround(value * 255.0 / 65535.0)); };
    return QString::asprintf("#%02x%02x%02x%02x", channel(rgba.red()), channel(rgba.green()), channel(rgba.blue()),
                             channel(rgba.alpha()));
}

//! A style in the reference data's format
QJsonObject styleJson(const Style &style)
{
    QJsonObject object;
    const auto color = [&object](const char *name, const std::optional<QColor> &value) {
        if (value)
            object.insert(QLatin1String(name), colorName(*value));
    };
    color("foreground", style.foreground);
    color("background", style.background);
    color("line-background", style.lineBackground);
    color("underline-color", style.underlineColor);
    if (style.bold)
        object.insert(QLatin1String("bold"), *style.bold);
    if (style.italic)
        object.insert(QLatin1String("italic"), *style.italic);
    if (style.strikethrough)
        object.insert(QLatin1String("strikethrough"), *style.strikethrough);
    if (style.underline)
        object.insert(QLatin1String("underline"), *style.underline);
    if (style.scale)
        object.insert(QLatin1String("scale"), *style.scale);
    return object;
}

QJsonObject roundScale(QJsonObject style)
{
    if (style.contains(QLatin1String("scale"))) {
        const double scale = style.value(QLatin1String("scale")).toDouble();
        style.insert(QLatin1String("scale"), std::round(scale * 1e6) / 1e6);
    }
    return style;
}

QString jsonText(const QJsonValue &value)
{
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return value.toVariant().toString();
}

QString sampleText(const QJsonObject &sample)
{
    if (sample.contains(QLatin1String("text")))
        return sample.value(QLatin1String("text")).toString();
    QFile file(QStringLiteral(MELD_TEST_DATA_DIR "/syntax/") + sample.value(QLatin1String("file")).toString());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

//! The styles of each character of a line, from the outermost: the styles
//! of the highlighted ranges in order of priority, without repeats
std::vector<QStringList> styleStacks(const ContextData &data, qsizetype length, std::vector<StyleSpan> spans)
{
    std::vector<QStringList> stacks(length);
    std::stable_sort(spans.begin(), spans.end(),
                     [](const StyleSpan &a, const StyleSpan &b) { return a.priority < b.priority; });
    for (const StyleSpan &span : spans) {
        if (span.style < 0)
            continue;
        const QString name = data.styleName(span.style);
        for (int i = std::max(span.start, 0); i < std::min<qsizetype>(span.end, length); ++i) {
            if (stacks[i].isEmpty() || stacks[i].constLast() != name)
                stacks[i].append(name);
        }
    }
    return stacks;
}

QString describeRuns(const std::vector<QStringList> &stacks)
{
    QStringList runs;
    qsizetype start = 0;
    for (qsizetype i = 1; i <= qsizetype(stacks.size()); ++i) {
        if (i == qsizetype(stacks.size()) || stacks[i] != stacks[start]) {
            if (!stacks[start].isEmpty())
                runs.append(QStringLiteral("[%1,%2) %3").arg(start).arg(i).arg(stacks[start].join(QLatin1Char('/'))));
            start = i;
        }
    }
    return runs.join(QLatin1String(", "));
}

} // namespace

class TestSyntax : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    void patternMatch_data();
    void patternMatch();
    void colors_data();
    void colors();
    void scales_data();
    void scales();
    void schemeErrors_data();
    void schemeErrors();

    void languages();
    void languageStyles();
    void guess_data();
    void guess();
    void highlight_data();
    void highlight();
    void hereDocsEndAtTheirMarkers_data();
    void hereDocsEndAtTheirMarkers();
    void schemes();
};

void TestSyntax::initTestCase()
{
    // GtkSourceView sorts names by the locale's collation; the reference
    // data uses the C locale
    QLocale::setDefault(QLocale::c());
    QVERIFY2(!reference().isEmpty(), qPrintable(referencePath()));
}

void TestSyntax::patternMatch_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("string");
    QTest::addColumn<bool>("matches");

    QTest::newRow("literal") << "Makefile" << "Makefile" << true;
    QTest::newRow("literal mismatch") << "Makefile" << "makefile" << false;
    QTest::newRow("suffix") << "*.c" << "main.c" << true;
    QTest::newRow("suffix only") << "*.c" << ".c" << true;
    QTest::newRow("suffix mismatch") << "*.c" << "main.cc" << false;
    QTest::newRow("prefix") << "Makefile*" << "Makefile.am" << true;
    QTest::newRow("question") << "file.?" << "file.h" << true;
    QTest::newRow("question needs a character") << "file.?" << "file." << false;
    QTest::newRow("question surrogate pair") << "?.txt" << QStringLiteral(u"\U0001F600.txt") << true;
    QTest::newRow("backtracking") << "*a*b" << "xaxxab" << true;
    QTest::newRow("backtracking mismatch") << "*a*b" << "xaxxa" << false;
    QTest::newRow("stars") << "**" << "" << true;
    QTest::newRow("brackets are literal") << "[ch]" << "c" << false;
    QTest::newRow("empty pattern") << "" << "" << true;
    QTest::newRow("empty pattern, name") << "" << "x" << false;
}

void TestSyntax::patternMatch()
{
    QFETCH(QString, pattern);
    QFETCH(QString, string);
    QFETCH(bool, matches);
    QCOMPARE(patternMatchSimple(pattern, string), matches);
}

void TestSyntax::colors_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("expected");

    // As gdk_rgba_parse() in GTK 4, which the original uses
    QTest::newRow("#rgb") << "#f80" << "#ff8800ff";
    QTest::newRow("#rrggbb") << "#2e3436" << "#2e3436ff";
    QTest::newRow("#RRGGBB") << "#FFF" << "#ffffffff";
    QTest::newRow("#rgba") << "#f808" << "#ff880088";
    QTest::newRow("#rrggbbaa") << "#2e343680" << "#2e343680";
    QTest::newRow("#rrrgggbbb") << "#fff000800" << "#ff0080ff";
    QTest::newRow("#rrrrggggbbbb") << "#ffff00008000" << "#ff0080ff";
    QTest::newRow("#rrrrggggbbbbaaaa") << "#ffff000080008000" << "#ff008080";
    QTest::newRow("invalid hex") << "#ggg" << "";
    QTest::newRow("hex sign") << "#+ff" << "";
    QTest::newRow("hex prefix") << "#0xf" << "";
    QTest::newRow("bad length") << "#12345" << "";
    QTest::newRow("rgb()") << "rgb(255,128,0)" << "#ff8000ff";
    QTest::newRow("rgba()") << "rgba(255, 128, 0, 0.5)" << "#ff800080";
    QTest::newRow("rgba() fraction") << "rgba(1,2,3,.5)" << "#01020380";
    QTest::newRow("percentages") << "rgb(100%,50%,0%)" << "#ff8000ff";
    QTest::newRow("percentage with space") << "rgb(100 %, 50%, 0%)" << "#ff8000ff";
    QTest::newRow("mixed") << "rgb(50%,50,0)" << "#803200ff";
    QTest::newRow("spaces") << "rgb( 1 , 2 , 3 )" << "#010203ff";
    QTest::newRow("space before (") << "rgb (1,2,3)" << "#010203ff";
    QTest::newRow("tab inside") << "rgb(\t1,2,3)" << "#010203ff";
    QTest::newRow("trailing spaces") << "rgb(1,2,3)  " << "#010203ff";
    QTest::newRow("trailing tab") << "rgb(1,2,3)\t" << "";
    QTest::newRow("leading space") << " rgb(1,2,3)" << "";
    QTest::newRow("upper case function") << "RGB(1,2,3)" << "";
    QTest::newRow("rgba() without alpha") << "rgba(1,2,3)" << "";
    QTest::newRow("rgb() with alpha") << "rgb(1,2,3,0.5)" << "";
    QTest::newRow("percentage alpha") << "rgba(1,2,3,50%)" << "";
    QTest::newRow("unclosed") << "rgb(1,2,3" << "";
    QTest::newRow("missing component") << "rgb(,2,3)" << "";
    QTest::newRow("no commas") << "rgb(1 2 3)" << "";
    QTest::newRow("junk after") << "rgb(1,2,3)x" << "";
    QTest::newRow("clamped") << "rgb(300,0,0)" << "#ff0000ff";
    QTest::newRow("clamped negative") << "rgb(-5,0,0)" << "#000000ff";
    QTest::newRow("clamped alpha") << "rgba(0,0,0,2)" << "#000000ff";
    QTest::newRow("fractional") << "rgb(1.5,2,3)" << "#020203ff";
    QTest::newRow("signs and exponents") << "rgb(+1,2e1,3)" << "#011403ff";
    QTest::newRow("hex number") << "rgb(0x10,0,0)" << "#100000ff";
    QTest::newRow("infinite") << "rgb(inf,0,0)" << "";
    QTest::newRow("out of range") << "rgb(1e400,0,0)" << "";
    QTest::newRow("name") << "red" << "#ff0000ff";
    QTest::newRow("name case") << "Red" << "#ff0000ff";
    QTest::newRow("name with spaces") << "light grey" << "#d3d3d3ff";
    QTest::newRow("CSS green") << "green" << "#008000ff";
    QTest::newRow("CSS gray") << "gray" << "#808080ff";
    QTest::newRow("transparent") << "transparent" << "";
    QTest::newRow("leading whitespace") << " #ffffff" << "";
    QTest::newRow("trailing whitespace") << "#ffffff " << "";
    QTest::newRow("unknown name") << "notacolour" << "";
    QTest::newRow("empty") << "" << "";
}

void TestSyntax::colors()
{
    QFETCH(QString, text);
    QFETCH(QString, expected);
    const std::optional<QColor> color = parseColor(text);
    QCOMPARE(color ? colorName(*color) : QString(), expected);
}

void TestSyntax::scales_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<double>("expected");

    QTest::newRow("number") << "2.5" << 2.5;
    QTest::newRow("integer") << "5" << 5.0;
    QTest::newRow("name") << "x-large" << 1.44;
    QTest::newRow("name case") << "Large" << 1.2;
    QTest::newRow("small") << "small" << 1 / 1.2;
    QTest::newRow("number prefix") << "1.5em" << 1.5;
    QTest::newRow("zero") << "0" << 0.0;
    QTest::newRow("negative") << "-1" << 0.0;
    QTest::newRow("invalid") << "huge" << 0.0;
}

void TestSyntax::scales()
{
    QFETCH(QString, text);
    QFETCH(double, expected);
    const std::optional<double> scale = parseScale(text);
    if (expected == 0.0)
        QVERIFY(!scale);
    else
        QVERIFY(scale && qFuzzyCompare(*scale, expected));
}

void TestSyntax::schemeErrors_data()
{
    QTest::addColumn<QByteArray>("xml");
    QTest::addColumn<bool>("valid");

    const QByteArray start = "<style-scheme id=\"test\" name=\"Test\" version=\"1.0\">";
    const QByteArray end = "</style-scheme>";
    QTest::newRow("empty scheme") << start + end << true;
    QTest::newRow("wrong version") << QByteArray("<style-scheme id=\"test\" name=\"Test\" version=\"2.0\"/>")
                                   << false;
    QTest::newRow("wrong root") << QByteArray("<scheme id=\"test\" version=\"1.0\"/>") << false;
    QTest::newRow("style") << start + "<style name=\"def:comment\" foreground=\"#000\"/>" + end << true;
    QTest::newRow("style without name") << start + "<style foreground=\"#000\"/>" + end << false;
    QTest::newRow("use-style") << start + "<style name=\"a\" bold=\"true\"/><style name=\"b\" use-style=\"a\"/>" + end
                               << true;
    QTest::newRow("use-style before definition")
        << start + "<style name=\"b\" use-style=\"a\"/><style name=\"a\" bold=\"true\"/>" + end << false;
    QTest::newRow("use-style with attributes")
        << start + "<style name=\"a\" bold=\"true\"/><style name=\"b\" use-style=\"a\" italic=\"true\"/>" + end
        << false;
    QTest::newRow("color") << start + "<color name=\"red1\" value=\"#ff0000\"/>" + end << true;
    QTest::newRow("color without #") << start + "<color name=\"red1\" value=\"ff0000\"/>" + end << false;
    QTest::newRow("duplicate color")
        << start + "<color name=\"red1\" value=\"#ff0000\"/><color name=\"red1\" value=\"#ee0000\"/>" + end << false;
    QTest::newRow("unknown element") << start + "<colour name=\"red1\" value=\"#ff0000\"/>" + end << false;
}

void TestSyntax::schemeErrors()
{
    QFETCH(QByteArray, xml);
    QFETCH(bool, valid);
    QString error;
    const std::unique_ptr<StyleScheme> scheme = StyleScheme::fromData(xml, QStringLiteral("test.xml"), &error);
    QVERIFY2(bool(scheme) == valid, qPrintable(error));
}

void TestSyntax::languages()
{
    LanguageManager &manager = LanguageManager::instance();
    const QJsonArray expected = reference().value(QLatin1String("languages")).toArray();
    QStringList expectedIds;
    for (const QJsonValue &value : expected)
        expectedIds.append(value.toObject().value(QLatin1String("id")).toString());
    QCOMPARE(manager.languageIds(), expectedIds);

    for (const QJsonValue &value : expected) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QLatin1String("id")).toString();
        const Language *language = manager.language(id);
        QVERIFY2(language, qPrintable(id));
        QCOMPARE(language->name(), entry.value(QLatin1String("name")).toString());
        QCOMPARE(language->section(), entry.value(QLatin1String("section")).toString());
        QCOMPARE(language->hidden(), entry.value(QLatin1String("hidden")).toBool());
        QCOMPARE(language->globs(), toStringList(entry.value(QLatin1String("globs"))));
        QCOMPARE(language->mimeTypes(), toStringList(entry.value(QLatin1String("mimetypes"))));
        const QJsonObject metadata = entry.value(QLatin1String("metadata")).toObject();
        for (auto it = metadata.begin(); it != metadata.end(); ++it)
            QCOMPARE(language->metadata(it.key()), it.value().toString());

        QString error;
        const bool loads = bool(manager.contextData(id, &error));
        QVERIFY2(loads == entry.value(QLatin1String("loads")).toBool(), qPrintable(id + QLatin1String(": ") + error));
    }
}

void TestSyntax::languageStyles()
{
    LanguageManager &manager = LanguageManager::instance();
    const QJsonObject &data = reference();
    const QStringList probe = toStringList(data.value(QLatin1String("probe")));
    const QJsonObject fallbacks = data.value(QLatin1String("style_fallbacks")).toObject();

    for (const QJsonValue &value : data.value(QLatin1String("languages")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (!entry.value(QLatin1String("loads")).toBool())
            continue;
        const QString id = entry.value(QLatin1String("id")).toString();
        const std::shared_ptr<const ContextData> context = manager.contextData(id);
        QVERIFY2(context, qPrintable(id));

        // The language's own styles, with their names
        const QJsonObject styles = entry.value(QLatin1String("styles")).toObject();
        QStringList own;
        for (auto it = context->styles().begin(); it != context->styles().end(); ++it) {
            if (it.key().startsWith(id + QLatin1Char(':')))
                own.append(it.key());
        }
        own.sort();
        QCOMPARE(own, styles.keys());
        for (auto it = styles.begin(); it != styles.end(); ++it) {
            const QJsonArray info = it.value().toArray();
            const StyleInfo actual = context->styles().value(it.key());
            QCOMPARE(actual.name, info.at(0).toString());
            QCOMPARE(actual.mapTo, info.at(1).toString());
        }

        // The styles of all the loaded languages that map to others
        QSet<int> mapped;
        for (const QJsonValue &index : entry.value(QLatin1String("mapped_styles")).toArray())
            mapped.insert(index.toInt());
        for (int i = 0; i < probe.size(); ++i) {
            const QString expectedFallback =
                mapped.contains(i) ? fallbacks.value(probe.at(i)).toString() : QString();
            if (context->styleFallback(probe.at(i)) != expectedFallback) {
                QFAIL(qPrintable(QStringLiteral("%1: style %2 maps to '%3', expected '%4'")
                                     .arg(id, probe.at(i), context->styleFallback(probe.at(i)), expectedFallback)));
            }
        }
    }
}

void TestSyntax::guess_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("contentType");
    QTest::addColumn<QString>("expected");

    for (const QJsonValue &value : reference().value(QLatin1String("guess")).toArray()) {
        const QJsonArray item = value.toArray();
        const QString fileName = item.at(0).toString();
        const QString contentType = item.at(1).toString();
        const QByteArray name = (fileName + QLatin1Char('|') + contentType).toUtf8();
        QTest::newRow(name.constData()) << fileName << contentType << item.at(2).toString();
    }
}

void TestSyntax::guess()
{
    QFETCH(QString, fileName);
    QFETCH(QString, contentType);
    QFETCH(QString, expected);
    const Language *language = LanguageManager::instance().guessLanguage(fileName, contentType);
    QCOMPARE(language ? language->id() : QString(), expected);
}

void TestSyntax::highlight_data()
{
    QTest::addColumn<int>("index");
    const QJsonArray samples = reference().value(QLatin1String("highlight")).toArray();
    for (int i = 0; i < samples.size(); ++i) {
        const QByteArray name = samples.at(i).toObject().value(QLatin1String("name")).toString().toUtf8();
        QTest::newRow(name.constData()) << i;
    }
}

void TestSyntax::highlight()
{
    QFETCH(int, index);
    const QJsonObject &data = reference();
    const QJsonArray stackTable = data.value(QLatin1String("stacks")).toArray();
    const QJsonObject sample = data.value(QLatin1String("highlight")).toArray().at(index).toObject();
    const QString languageId = sample.value(QLatin1String("language")).toString();
    const QString text = sampleText(sample);
    QVERIFY2(!text.isEmpty(), "missing sample text");

    QString error;
    const std::shared_ptr<const ContextData> context = LanguageManager::instance().contextData(languageId, &error);
    QVERIFY2(context, qPrintable(error));
    const LineHighlighter highlighter(context);

    const QStringList lines = text.split(QLatin1Char('\n'));
    const QJsonArray expectedLines = sample.value(QLatin1String("lines")).toArray();
    QCOMPARE(lines.size(), expectedLines.size());

    LineStatePtr state = highlighter.initialState();
    int failures = 0;
    QString firstFailure;
    for (int line = 0; line < lines.size(); ++line) {
        std::vector<StyleSpan> spans;
        state = highlighter.highlightLine(lines.at(line), line == 0, state, spans);
        QVERIFY2(state, qPrintable(QStringLiteral("line %1 took too long").arg(line + 1)));
        const std::vector<QStringList> actual = styleStacks(*context, lines.at(line).size(), spans);

        std::vector<QStringList> expected(lines.at(line).size());
        const QJsonArray runs = expectedLines.at(line).toArray();
        for (qsizetype i = 0; i + 2 < runs.size(); i += 3) {
            const QStringList stack = toStringList(stackTable.at(runs.at(i + 2).toInt()));
            for (int c = runs.at(i).toInt(); c < runs.at(i + 1).toInt() && c < qsizetype(expected.size()); ++c)
                expected[c] = stack;
        }
        // A deliberate difference from GtkSourceView 5, whose here-doc doesn't
        // end at this marker: see hereDocsEndAtTheirMarkers()
        if (sample.value(QLatin1String("name")).toString() == QLatin1String("gtksourceview/file.sh") && line + 1 == 81
            && lines.at(line) == QLatin1String("EOF")) {
            expected.assign(3, QStringList{QStringLiteral("sh:here-doc-bound")});
        }
        if (actual != expected) {
            if (failures == 0) {
                firstFailure = QStringLiteral("line %1: %2\n    expected: %3\n    actual:   %4")
                                   .arg(line + 1)
                                   .arg(lines.at(line), describeRuns(expected), describeRuns(actual));
            }
            ++failures;
        }
    }
    if (failures) {
        QFAIL(qPrintable(QStringLiteral("%1 (%2) differs on %3 of %4 lines; first at %5")
                             .arg(sample.value(QLatin1String("name")).toString(), languageId)
                             .arg(failures)
                             .arg(lines.size())
                             .arg(firstFailure)));
    }
}

// GtkSourceView 5 finds no group for the duplicate names in these patterns, so
// its here-docs end at the next empty line rather than at their markers. This
// keeps GtkSourceView 4's behaviour (see "Differences from the original" in the
// README), so it isn't tested against the reference data.
void TestSyntax::hereDocsEndAtTheirMarkers_data()
{
    QTest::addColumn<QString>("language");
    QTest::addColumn<QString>("start");

    QTest::newRow("sh, single quotes") << "sh" << "cat <<'EOF'";
    QTest::newRow("sh, double quotes") << "sh" << "cat <<\"EOF\"";
    QTest::newRow("sh, no quotes") << "sh" << "cat <<EOF";
    QTest::newRow("perl, single quotes") << "perl" << "print <<'EOF';";
    QTest::newRow("perl, backslash") << "perl" << "print <<\\EOF;";
    QTest::newRow("perl, double quotes") << "perl" << "print <<\"EOF\";";
    QTest::newRow("perl, no quotes") << "perl" << "print <<EOF;";
    QTest::newRow("fish, single quotes") << "fish" << "cat <<'EOF'";
    QTest::newRow("fish, double quotes") << "fish" << "cat <<\"EOF\"";
    QTest::newRow("fish, no quotes") << "fish" << "cat <<EOF";
}

void TestSyntax::hereDocsEndAtTheirMarkers()
{
    QFETCH(QString, language);
    QFETCH(QString, start);

    const auto hasStyle = [](const QStringList &stack, QLatin1String suffix) {
        return std::any_of(stack.begin(), stack.end(), [suffix](const QString &style) { return style.endsWith(suffix); });
    };

    QString error;
    const std::shared_ptr<const ContextData> context = LanguageManager::instance().contextData(language, &error);
    QVERIFY2(context, qPrintable(error));
    const LineHighlighter highlighter(context);

    // The empty line doesn't end the here-doc, and the code after it is code
    const QStringList lines{start, QStringLiteral("# body"), QString(), QStringLiteral("# body"),
                            QStringLiteral("EOF"), QStringLiteral("# code")};
    std::vector<std::vector<QStringList>> stacks;
    LineStatePtr state = highlighter.initialState();
    for (int line = 0; line < lines.size(); ++line) {
        std::vector<StyleSpan> spans;
        state = highlighter.highlightLine(lines.at(line), line == 0, state, spans);
        QVERIFY(state);
        stacks.push_back(styleStacks(*context, lines.at(line).size(), spans));
    }

    const QLatin1String comment("comment");
    QVERIFY2(!hasStyle(stacks[1].back(), comment), "the here-doc's text is highlighted as code");
    QVERIFY2(!hasStyle(stacks[3].back(), comment), "the here-doc ended at the empty line");
    QVERIFY2(hasStyle(stacks[4].front(), QLatin1String("here-doc-bound")), "the marker isn't highlighted");
    QVERIFY2(hasStyle(stacks[5].back(), comment), "the here-doc continues after its marker");
}

void TestSyntax::schemes()
{
    StyleSchemeManager &manager = StyleSchemeManager::instance();
    const QJsonObject &data = reference();
    const QStringList probe = toStringList(data.value(QLatin1String("probe")));
    const QJsonArray schemes = data.value(QLatin1String("schemes")).toArray();

    QStringList expectedIds;
    for (const QJsonValue &value : schemes)
        expectedIds.append(value.toObject().value(QLatin1String("id")).toString());
    QCOMPARE(manager.schemeIds(), expectedIds);

    for (const QJsonValue &value : schemes) {
        const QJsonObject entry = value.toObject();
        const StyleScheme *scheme = manager.scheme(entry.value(QLatin1String("id")).toString());
        QVERIFY(scheme);
        QCOMPARE(scheme->name(), entry.value(QLatin1String("name")).toString());
        QCOMPARE(scheme->description(), entry.value(QLatin1String("description")).toString());
        QCOMPARE(scheme->authors(), toStringList(entry.value(QLatin1String("authors"))));
        // The light and dark variants
        const QJsonObject metadata = entry.value(QLatin1String("metadata")).toObject();
        for (const char *name : {"variant", "light-variant", "dark-variant"})
            QCOMPARE(scheme->metadata(QLatin1String(name)), metadata.value(QLatin1String(name)).toString());

        const QJsonObject styles = entry.value(QLatin1String("styles")).toObject();
        for (const QString &styleId : probe) {
            const std::optional<Style> style = scheme->style(styleId);
            const QJsonValue expected = styles.value(styleId);
            const QString context = scheme->id() + QLatin1Char(' ') + styleId;
            QVERIFY2(bool(style) == expected.isObject(), qPrintable(context));
            if (style && roundScale(styleJson(*style)) != roundScale(expected.toObject())) {
                QFAIL(qPrintable(QStringLiteral("%1: %2, expected %3")
                                     .arg(context, jsonText(styleJson(*style)), jsonText(expected))));
            }
        }
    }
}

QTEST_GUILESS_MAIN(TestSyntax)

#include "tst_syntax.moc"
