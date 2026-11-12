// Tests for syntax highlighting in comparisons: the lazy document
// highlighter, the colours of style schemes, and the languages of the files
// being compared
//
// The original relies on GtkSourceView for these; tst_syntax compares the
// highlighting itself with GtkSourceView's.

#include "bufferselectors.h"
#include "core/meldbuffer.h"
#include "core/syntax/highlighter.h"
#include "core/syntax/language.h"
#include "core/syntax/stylescheme.h"
#include "filediff.h"
#include "meldwindow.h"
#include "patchdialog.h"
#include "settings.h"
#include "sourceview.h"
#include "statusbar.h"
#include "style.h"

#include <QApplication>
#include <QLineEdit>
#include <QPlainTextDocumentLayout>
#include <QPlainTextEdit>
#include <QScopeGuard>
#include <QScrollBar>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QStyle>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>

#include <cmath>

using namespace meld;
using namespace meld::syntax;

namespace {

const QString CSource = QStringLiteral(
    "/* A comment\n"
    " * over lines */\n"
    "#include <stdio.h>\n"
    "\n"
    "int main(void)\n"
    "{\n"
    "    const char *s = \"string \\\"quoted\\\"\"; // TODO\n"
    "    return 0x1F + 'c';\n"
    "}\n");

std::unique_ptr<QTextDocument> makeDocument(const QString &text)
{
    auto document = std::make_unique<QTextDocument>();
    document->setDocumentLayout(new QPlainTextDocumentLayout(document.get()));
    document->setPlainText(text);
    return document;
}

const StyleScheme *scheme(const QString &id)
{
    return StyleSchemeManager::instance().scheme(id);
}

const Language *language(const QString &id)
{
    return LanguageManager::instance().language(id);
}

//! Each block's formats, as text for comparisons
QStringList blockFormats(const QTextDocument &document)
{
    QStringList result;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QStringList ranges;
        for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
            QStringList properties;
            const QMap<int, QVariant> map = range.format.properties();
            for (auto it = map.begin(); it != map.end(); ++it)
                properties.append(QStringLiteral("%1=%2").arg(it.key()).arg(it.value().toString()));
            ranges.append(QStringLiteral("%1+%2{%3}").arg(range.start).arg(range.length).arg(properties.join(',')));
        }
        result.append(ranges.join(' '));
    }
    return result;
}

//! The format of the character at `column` of `line`
QTextCharFormat formatAt(const QTextDocument &document, int line, int column)
{
    const QTextBlock block = document.findBlockByNumber(line);
    for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
        if (column >= range.start && column < range.start + range.length)
            return range.format;
    }
    return {};
}

//! The format of a style of the classic scheme
QTextCharFormat styleFormat(const SyntaxHighlighter &highlighter, const QString &styleId)
{
    const std::optional<Style> style = highlighter.resolveStyle(styleId);
    return style ? SyntaxHighlighter::format(*style, QFont()) : QTextCharFormat();
}

} // namespace

class TestHighlighting : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;
    std::unique_ptr<MeldWindow> window_;
    int fileCounter_ = 0;

    QString writeFile(const QByteArray &content, const QString &suffix)
    {
        const QString path = dir_.filePath(QStringLiteral("file%1%2").arg(++fileCounter_).arg(suffix));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            qFatal("Couldn't write test file");
        file.write(content);
        return path;
    }

    void settle()
    {
        for (int i = 0; i < 100; ++i) {
            window_->runPendingTasks();
            QCoreApplication::processEvents();
            if (!window_->scheduler().tasksPending())
                break;
        }
    }

    FileDiff *open(const QStringList &files)
    {
        FileDiff *doc = window_->appendFilediff(files);
        window_->tabWidget()->setCurrentWidget(doc);
        settle();
        return doc;
    }

    static QString languageId(const MeldBuffer *buffer)
    {
        return buffer->language() ? buffer->language()->id() : QString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        MeldSettings::useSettingsFile(dir_.filePath(QStringLiteral("settings.ini")));
        // A light style, whatever the system's
        QApplication::setPalette(QApplication::style()->standardPalette());
        updateTheme();
    }

    void cleanup()
    {
        if (window_) {
            for (MeldDoc *doc : window_->docs()) {
                if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                    for (int pane = 0; pane < filediff->numPanes(); ++pane)
                        filediff->textBuffer(pane)->setModified(false);
                }
            }
            window_.reset();
        }
        MeldSettings::instance()->resetValue(QStringLiteral("style-scheme"));
        MeldSettings::instance()->resetValue(QStringLiteral("highlight-syntax"));
    }

    // -----------------------------------------------------------------------
    // The document highlighter

    void formatsAreTheSchemeStyles()
    {
        auto document = makeDocument(CSource);
        SyntaxHighlighter highlighter(document.get());
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("c")));
        QVERIFY(highlighter.isActive());
        highlighter.highlightAll();
        QCOMPARE(highlighter.highlightedBlocks(), document->blockCount());

        // Styles the scheme doesn't have fall back to the ones they map to
        const QTextCharFormat comment = styleFormat(highlighter, QStringLiteral("c:comment"));
        QCOMPARE(comment.foreground().color(), QColor(Qt::blue));
        QCOMPARE(formatAt(*document, 0, 3).foreground(), comment.foreground());
        QCOMPARE(formatAt(*document, 1, 5).foreground(), comment.foreground());
        QCOMPARE(formatAt(*document, 4, 0), styleFormat(highlighter, QStringLiteral("c:type")));
        QVERIFY(formatAt(*document, 4, 0).fontWeight() == QFont::Bold);
        QCOMPARE(formatAt(*document, 6, 21).foreground(), styleFormat(highlighter, QStringLiteral("c:string")).foreground());
        // "TODO" in a comment has its own style
        const int todo = int(document->findBlockByNumber(6).text().indexOf(QLatin1String("TODO")));
        QCOMPARE(formatAt(*document, 6, todo), [&] {
            // Inner styles' attributes take precedence over outer ones'
            QTextCharFormat merged = styleFormat(highlighter, QStringLiteral("c:comment"));
            merged.merge(styleFormat(highlighter, QStringLiteral("def:note")));
            return merged;
        }());
        // Plain text has no formats
        QVERIFY(formatAt(*document, 4, 4).properties().isEmpty());
    }

    void highlightingDoesntChangeTheDocument()
    {
        auto document = makeDocument(CSource);
        const bool modified = document->isModified();
        const bool undo = document->isUndoAvailable();
        QSignalSpy changes(document.get(), &QTextDocument::contentsChange);
        QSignalSpy contents(document.get(), &QTextDocument::contentsChanged);
        SyntaxHighlighter highlighter(document.get());
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("c")));
        highlighter.highlightAll();
        highlighter.setStyleScheme(scheme(QStringLiteral("cobalt")));
        highlighter.highlightAll();
        highlighter.setHighlightSyntax(false);
        QCOMPARE(changes.count(), 0);
        QCOMPARE(contents.count(), 0);
        QCOMPARE(document->isUndoAvailable(), undo);
        QCOMPARE(document->isModified(), modified);
    }

    void editsMatchHighlightingFromScratch()
    {
        // Edits keep the highlighting of the lines whose starting state
        // didn't change; the result must be the same as highlighting the
        // edited text from the start.
        auto document = makeDocument(CSource.repeated(4));
        SyntaxHighlighter highlighter(document.get());
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("c")));
        highlighter.highlightAll();

        const QStringList insertions{QStringLiteral("/*"), QStringLiteral("*/"), QStringLiteral("\""),
                                     QStringLiteral("'"), QStringLiteral("\n"), QStringLiteral("// x\n"),
                                     QStringLiteral("#if 0\n"), QStringLiteral("#endif\n"), QStringLiteral("\\"),
                                     QStringLiteral("int ")};
        QRandomGenerator random(42);
        for (int step = 0; step < 300; ++step) {
            QTextCursor cursor(document.get());
            const int length = document->characterCount() - 1;
            cursor.setPosition(random.bounded(length + 1));
            if (random.bounded(3) == 0 && cursor.position() < length) {
                cursor.setPosition(std::min(length, cursor.position() + random.bounded(1, 12)), QTextCursor::KeepAnchor);
                cursor.removeSelectedText();
            } else {
                cursor.insertText(insertions.at(random.bounded(qsizetype(insertions.size()))));
            }
            // Highlight some, or all, of the document, as views would
            if (random.bounded(4) == 0)
                highlighter.ensureHighlighted(random.bounded(document->blockCount()));
            else
                highlighter.highlightAll();

            if (step % 10 == 0) {
                highlighter.highlightAll();
                auto fresh = makeDocument(document->toPlainText());
                SyntaxHighlighter freshHighlighter(fresh.get());
                freshHighlighter.setStyleScheme(scheme(QStringLiteral("classic")));
                freshHighlighter.setLanguage(language(QStringLiteral("c")));
                freshHighlighter.highlightAll();
                QCOMPARE(blockFormats(*document), blockFormats(*fresh));
            }
        }
    }

    void highlightingIsLazy()
    {
        QString text;
        for (int i = 0; i < 5000; ++i)
            text += QStringLiteral("int x%1 = %1; /* comment */\n").arg(i);
        auto document = makeDocument(text);
        SyntaxHighlighter highlighter(document.get());
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("c")));

        highlighter.ensureHighlighted(9);
        QCOMPARE(highlighter.highlightedBlocks(), 10);
        QVERIFY(!document->findBlockByNumber(9).layout()->formats().isEmpty());
        QVERIFY(document->findBlockByNumber(4000).layout()->formats().isEmpty());

        // The rest is highlighted in idle time
        QTRY_COMPARE_WITH_TIMEOUT(highlighter.highlightedBlocks(), document->blockCount(), 20000);
        QVERIFY(!document->findBlockByNumber(4000).layout()->formats().isEmpty());

        // Changes only need the changed lines highlighted again
        QTextCursor cursor(document->findBlockByNumber(2000));
        cursor.insertText(QStringLiteral("long "));
        QCOMPARE(highlighter.highlightedBlocks(), 2000);
        highlighter.ensureHighlighted(2000);
        QCOMPARE(highlighter.highlightedBlocks(), 2001);
        QTRY_COMPARE_WITH_TIMEOUT(highlighter.highlightedBlocks(), document->blockCount(), 20000);
    }

    void disablingRemovesFormats()
    {
        auto document = makeDocument(CSource);
        SyntaxHighlighter highlighter(document.get());
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("c")));
        highlighter.highlightAll();
        const QStringList highlighted = blockFormats(*document);
        QVERIFY(!highlighted.join(QString()).isEmpty());

        highlighter.setHighlightSyntax(false);
        QVERIFY(!highlighter.isActive());
        QVERIFY(blockFormats(*document).join(QString()).isEmpty());
        highlighter.highlightAll();
        QVERIFY(blockFormats(*document).join(QString()).isEmpty());

        highlighter.setHighlightSyntax(true);
        highlighter.highlightAll();
        QCOMPARE(blockFormats(*document), highlighted);

        // Plain text
        highlighter.setLanguage(nullptr);
        QVERIFY(!highlighter.isActive());
        QVERIFY(blockFormats(*document).join(QString()).isEmpty());
    }

    void schemeChangesRestyle()
    {
        auto document = makeDocument(CSource);
        SyntaxHighlighter highlighter(document.get());
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("c")));
        highlighter.highlightAll();
        const QBrush classic = formatAt(*document, 0, 3).foreground();

        highlighter.setStyleScheme(scheme(QStringLiteral("solarized-dark")));
        highlighter.highlightAll();
        const QBrush solarized = formatAt(*document, 0, 3).foreground();
        QVERIFY(classic != solarized);
        QCOMPARE(solarized, styleFormat(highlighter, QStringLiteral("c:comment")).foreground());
    }

    void stylesScaleTheFont()
    {
        // GtkSourceView's schemes have scaled headings, commented out
        auto document = makeDocument(QStringLiteral("<html><head><title>Title</title></head></html>\n"));
        SyntaxHighlighter highlighter(document.get());
        QFont font;
        font.setPointSizeF(10);
        highlighter.setBaseFont(font);
        highlighter.setStyleScheme(scheme(QStringLiteral("classic")));
        highlighter.setLanguage(language(QStringLiteral("html")));
        highlighter.highlightAll();
        const int title = int(document->firstBlock().text().indexOf(QLatin1String("Title<")));
        QVERIFY(!formatAt(*document, 0, title).hasProperty(QTextFormat::FontPointSize));

        const std::unique_ptr<StyleScheme> headings = StyleScheme::fromData(
            "<style-scheme id=\"headings\" name=\"Headings\" version=\"1.0\">"
            "<style name=\"def:heading0\" scale=\"5.0\" bold=\"true\"/>"
            "</style-scheme>",
            QStringLiteral("headings.xml"));
        QVERIFY(headings);
        highlighter.setStyleScheme(headings.get());
        highlighter.highlightAll();
        QCOMPARE(formatAt(*document, 0, title).fontPointSize(), 50.0);
        QCOMPARE(formatAt(*document, 0, title).fontWeight(), int(QFont::Bold));

        font.setPointSizeF(12);
        highlighter.setBaseFont(font);
        highlighter.highlightAll();
        QCOMPARE(formatAt(*document, 0, title).fontPointSize(), 60.0);
    }

    // -----------------------------------------------------------------------
    // Style schemes and comparison colours

    void defaultScheme()
    {
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("classic"));
        QCOMPARE(baseStyleScheme()->id(), QStringLiteral("meld-base"));
        QVERIFY(!currentTheme().dark);
        QCOMPARE(currentTheme().insertFill, QColor(0xd0, 0xff, 0xa3));
        // Classic has search match colours, and doesn't have Meld's
        QCOMPARE(currentTheme().searchMatch, QColor(Qt::yellow));
        QCOMPARE(colourLookupWithFallback(QStringLiteral("meld:inline"), StyleAttribute::Background),
                 QColor(0x8a, 0xc2, 0xff));
        QVERIFY(!schemeColour(QStringLiteral("meld:inline"), StyleAttribute::Background));
    }

    void darkSchemesUseMeldsDarkColours()
    {
        // Solarized has light and dark variants, so the variant for the
        // application's style is used, whichever is chosen
        QSignalSpy spy(MeldSettings::instance(), &MeldSettings::changed);
        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), QStringLiteral("solarized-dark"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("solarized-light"));
        QCOMPARE(baseStyleScheme()->id(), QStringLiteral("meld-base"));
        QVERIFY(!currentTheme().dark);
        QCOMPARE(currentTheme().insertFill, QColor(0xd0, 0xff, 0xa3));

        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Window, QColor(0x24, 0x24, 0x24));
        QApplication::setPalette(dark);
        QVERIFY(updateTheme());
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("solarized-dark"));
        QCOMPARE(baseStyleScheme()->id(), QStringLiteral("meld-dark"));
        QVERIFY(currentTheme().dark);
        QCOMPARE(currentTheme().insertFill, QColor(0x12, 0x38, 0x06));
        // The scheme's own colours
        QCOMPARE(schemeTextPalette(currentStyleScheme()).color(QPalette::Base), QColor(0x00, 0x2b, 0x36));

        QApplication::setPalette(light);
        QVERIFY(updateTheme());
        QVERIFY(!currentTheme().dark);
    }

    void meldSchemesKeepTheirTextColours()
    {
        // Meld's schemes have no variants, and get their text colours from
        // their parents, which decide whether they're dark, whatever the
        // application's style is
        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Window, QColor(0x24, 0x24, 0x24));
        dark.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
        QApplication::setPalette(dark);

        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), QStringLiteral("meld-base"));
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("meld-base"));
        QVERIFY(!currentTheme().dark);
        QVERIFY(!updateTheme());

        QApplication::setPalette(light);
        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), QStringLiteral("meld-dark"));
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("meld-dark"));
        QVERIFY(currentTheme().dark);
        QVERIFY(!updateTheme());
    }

    void classicHasADarkVariant()
    {
        // As in GtkSourceView 5, Classic's dark variant is used with dark
        // styles, and the other way around
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("classic"));
        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Window, QColor(0x24, 0x24, 0x24));
        QApplication::setPalette(dark);
        QVERIFY(updateTheme());
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("classic-dark"));
        QCOMPARE(baseStyleScheme()->id(), QStringLiteral("meld-dark"));
        QVERIFY(currentTheme().dark);
        QCOMPARE(schemeTextPalette(currentStyleScheme()).color(QPalette::Base), QColor(0x10, 0x10, 0x10));

        QApplication::setPalette(light);
        QVERIFY(updateTheme());
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("classic"));

        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), QStringLiteral("classic-dark"));
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("classic"));
        QVERIFY(!currentTheme().dark);
    }

    void currentLineHighlightStandsOutInDarkSchemes_data()
    {
        QTest::addColumn<QString>("schemeId");
        for (const char *id : {"classic-dark", "Adwaita-dark", "cobalt", "kate-dark", "oblivion", "solarized-dark",
                               "meld-dark"})
            QTest::newRow(id) << QString::fromLatin1(id);
    }

    void currentLineHighlightStandsOutInDarkSchemes()
    {
        const auto relativeLuminance = [](const QColor &colour) {
            const auto channel = [](qreal value) {
                return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
            };
            return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF())
                + 0.0722 * channel(colour.blueF());
        };
        // The WCAG contrast ratio of the highlight over a background with it
        const auto highlightContrast = [&](const QColor &background) {
            const QColor highlight = currentTheme().currentLineHighlight;
            const qreal alpha = highlight.alphaF();
            const QColor painted = QColor::fromRgbF(highlight.redF() * alpha + background.redF() * (1 - alpha),
                                                    highlight.greenF() * alpha + background.greenF() * (1 - alpha),
                                                    highlight.blueF() * alpha + background.blueF() * (1 - alpha));
            const qreal a = relativeLuminance(painted) + 0.05;
            const qreal b = relativeLuminance(background) + 0.05;
            return std::max(a, b) / std::min(a, b);
        };

        QFETCH(QString, schemeId);
        const QPalette light = QApplication::palette();
        QPalette dark = light;
        dark.setColor(QPalette::Window, QColor(0x24, 0x24, 0x24));
        QApplication::setPalette(dark);
        const auto restore = qScopeGuard([&light]() {
            QApplication::setPalette(light);
            MeldSettings::instance()->resetValue(QStringLiteral("style-scheme"));
            updateTheme();
        });
        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), schemeId);
        updateTheme();
        QCOMPARE(currentStyleScheme()->id(), schemeId);
        QVERIFY(currentTheme().dark);

        const QColor text = schemeTextPalette(currentStyleScheme()).color(QPalette::Base);
        for (const QColor &background : {text, currentTheme().insertFill, currentTheme().replaceFill,
                                         currentTheme().conflictFill}) {
            QVERIFY2(highlightContrast(background) > 1.4,
                     qPrintable(QStringLiteral("%1 over %2").arg(highlightContrast(background)).arg(background.name())));
        }
    }

    void unknownSchemesUseTheDefault()
    {
        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), QStringLiteral("no-such-scheme"));
        QCOMPARE(currentStyleScheme()->id(), QStringLiteral("classic"));
    }

    // -----------------------------------------------------------------------
    // Comparisons

    void comparisonsGuessLanguages()
    {
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));

        const QString c1 = writeFile("int a;\n", QStringLiteral(".c"));
        const QString c2 = writeFile("int b;\n", QStringLiteral(".c"));
        FileDiff *doc = open({c1, c2});
        QCOMPARE(languageId(doc->textBuffer(0)), QStringLiteral("c"));
        QCOMPARE(languageId(doc->textBuffer(1)), QStringLiteral("c"));
        QVERIFY(doc->textBuffer(0)->highlighter()->isActive());
        QCOMPARE(doc->statusBar(0)->language(), QStringLiteral("c"));
        QCOMPARE(doc->statusBar(1)->language(), QStringLiteral("c"));

        // Visible lines are highlighted when shown
        QTRY_VERIFY(doc->textBuffer(1)->highlighter()->highlightedBlocks() > 0);
        QVERIFY(!doc->textBuffer(1)->document()->firstBlock().layout()->formats().isEmpty());
    }

    void oneLanguageIsUsedForAllFiles()
    {
        window_ = std::make_unique<MeldWindow>();
        const QString py = writeFile("import os\n", QStringLiteral(".py"));
        const QString text = writeFile("import sys\n", QStringLiteral(".txt"));
        FileDiff *doc = open({py, text});
        QCOMPARE(languageId(doc->textBuffer(0)), QStringLiteral("python3"));
        QCOMPARE(languageId(doc->textBuffer(1)), QStringLiteral("python3"));

        // Different languages are kept
        const QString c = writeFile("int a;\n", QStringLiteral(".c"));
        FileDiff *mixed = open({py, c});
        QCOMPARE(languageId(mixed->textBuffer(0)), QStringLiteral("python3"));
        QCOMPARE(languageId(mixed->textBuffer(1)), QStringLiteral("c"));

        // Files without languages are plain text
        const QString other = writeFile("text\n", QStringLiteral(".txt"));
        FileDiff *plain = open({text, other});
        QCOMPARE(languageId(plain->textBuffer(0)), QString());
        QCOMPARE(languageId(plain->textBuffer(1)), QString());
        QVERIFY(!plain->textBuffer(0)->highlighter()->isActive());
    }

    void statusBarSelectsLanguages()
    {
        window_ = std::make_unique<MeldWindow>();
        const QString left = writeFile("x = 1\n", QStringLiteral(".txt"));
        const QString right = writeFile("x = 2\n", QStringLiteral(".txt"));
        FileDiff *doc = open({left, right});
        StatusBar *statusbar = doc->statusBar(0);
        auto *selector = statusbar->findChild<SourceLangSelector *>();
        QVERIFY(selector);
        QCOMPARE(statusbar->language(), QString());

        // Filtering selects the first match, which the entry activates
        selector->entry()->setText(QStringLiteral("python"));
        QTest::keyClick(selector->entry(), Qt::Key_Return);
        QCOMPARE(languageId(doc->textBuffer(0)), QStringLiteral("python3"));
        QCOMPARE(statusbar->language(), QStringLiteral("python3"));
        QCOMPARE(languageId(doc->textBuffer(1)), QString());

        // The selection moves from the entry
        selector->entry()->setText(QStringLiteral("python"));
        QTest::keyClick(selector->entry(), Qt::Key_Down);
        QTest::keyClick(selector->entry(), Qt::Key_Return);
        QCOMPARE(languageId(doc->textBuffer(0)), QStringLiteral("python"));

        // Plain text
        selector->entry()->setText(QStringLiteral("plain"));
        QTest::keyClick(selector->entry(), Qt::Key_Return);
        QCOMPARE(languageId(doc->textBuffer(0)), QString());

        // The buffer's language is shown
        doc->textBuffer(0)->setLanguage(language(QStringLiteral("rust")));
        QCOMPARE(statusbar->language(), QStringLiteral("rust"));
    }

    void largeFilesAreHighlightedWhereShown()
    {
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
        QByteArray text;
        for (int i = 0; i < 30000; ++i)
            text += "static int value" + QByteArray::number(i) + " = " + QByteArray::number(i) + "; /* comment */\n";
        const QString left = writeFile(text, QStringLiteral(".c"));
        const QString right = writeFile(text + "int end;\n", QStringLiteral(".c"));
        FileDiff *doc = open({left, right});
        SourceView *view = doc->textView(0);
        QVERIFY(doc->textBuffer(0)->highlighter()->isActive());

        // Jump to the end: the lines shown are highlighted, if not at once
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        const auto shownFormats = [view, doc]() {
            const std::vector<SourceView::VisibleLine> lines = view->visibleLines();
            if (lines.empty() || lines.front().number < 29000)
                return false;
            const QTextBlock block = doc->textBuffer(0)->document()->findBlockByNumber(lines.front().number);
            return !block.layout()->formats().isEmpty();
        };
        QTRY_VERIFY_WITH_TIMEOUT(shownFormats(), 20000);
        QTRY_COMPARE_WITH_TIMEOUT(doc->textBuffer(0)->highlighter()->highlightedBlocks(),
                                  doc->textBuffer(0)->document()->blockCount(), 20000);
    }

    void whitespaceIsShown()
    {
        window_ = std::make_unique<MeldWindow>();
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_.get()));
        const QString left = writeFile("same line\nx\n", QStringLiteral(".txt"));
        const QString right = writeFile("same line\nx  y\n", QStringLiteral(".txt"));
        FileDiff *doc = open({left, right});
        SourceView *view = doc->textView(0);

        // The newline after the first line, which Qt's own option doesn't show
        QTextCursor cursor(view->document()->firstBlock());
        cursor.movePosition(QTextCursor::EndOfBlock);
        const QRect end = view->cursorRect(cursor);
        const QRect marker(end.right() + 2, end.top(), end.height() * 2, end.height());
        const QImage hidden = view->viewport()->grab(marker).toImage();
        view->setDrawSpaces(true);
        const QImage shown = view->viewport()->grab(marker).toImage();
        QVERIFY(hidden != shown);
        view->setDrawSpaces(false);
        QCOMPARE(view->viewport()->grab(marker).toImage(), hidden);

        // Spaces in inline changes are always shown
        SourceView *other = doc->textView(1);
        QTRY_VERIFY(!doc->textBuffer(1)->inlineHighlights().isEmpty());
        // The cell of the first space after "x" is one colour unless it's marked
        QTextCursor spaces(other->document()->findBlockByNumber(1));
        spaces.setPosition(spaces.position() + 1);
        const QRect before = other->cursorRect(spaces);
        spaces.setPosition(spaces.position() + 1);
        const QRect after = other->cursorRect(spaces);
        // Inside the chunk's outlines
        const QRect cell(before.left() + 1, before.top() + 2, after.left() - before.left() - 1, before.height() - 4);
        const auto uniform = [](const QImage &image) {
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    if (image.pixel(x, y) != image.pixel(0, 0))
                        return false;
                }
            }
            return true;
        };
        QVERIFY(!uniform(other->viewport()->grab(cell).toImage()));
        doc->textBuffer(1)->inlineHighlights().clear();
        other->viewport()->repaint();
        QVERIFY(uniform(other->viewport()->grab(cell).toImage()));
    }

    void highlightSyntaxSetting()
    {
        window_ = std::make_unique<MeldWindow>();
        const QString left = writeFile("int a;\n", QStringLiteral(".c"));
        const QString right = writeFile("int b;\n", QStringLiteral(".c"));
        FileDiff *doc = open({left, right});
        SyntaxHighlighter *highlighter = doc->textBuffer(0)->highlighter();
        QVERIFY(highlighter->isActive());

        MeldSettings::instance()->setValue(QStringLiteral("highlight-syntax"), false);
        QVERIFY(!highlighter->isActive());
        MeldSettings::instance()->setValue(QStringLiteral("highlight-syntax"), true);
        QVERIFY(highlighter->isActive());

        MeldSettings::instance()->setValue(QStringLiteral("style-scheme"), QStringLiteral("cobalt-light"));
        QCOMPARE(highlighter->styleScheme(), scheme(QStringLiteral("cobalt-light")));
        QCOMPARE(doc->textView(0)->palette().color(QPalette::Base),
                 schemeTextPalette(scheme(QStringLiteral("cobalt-light"))).color(QPalette::Base));
    }

    void patchesAreHighlighted()
    {
        const QStringList texts{QStringLiteral("a\nb\n"), QStringLiteral("a\nc\n"), QString()};
        const QStringList labels{QStringLiteral("left"), QStringLiteral("right"), QString()};
        PatchDialog dialog(texts, labels, 2);
        auto *view = dialog.findChild<QPlainTextEdit *>();
        QVERIFY(view);
        QVERIFY(view->toPlainText().contains(QLatin1String("+c")));
        bool highlighted = false;
        for (QTextBlock block = view->document()->begin(); block.isValid(); block = block.next())
            highlighted = highlighted || !block.layout()->formats().isEmpty();
        QVERIFY(highlighted);
    }
};

QTEST_MAIN(TestHighlighting)

#include "tst_highlighting.moc"
