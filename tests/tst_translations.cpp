// Translations: choosing and installing them, their consistency with the
// English messages, and that the text the windows show is translatable

#include "core/translations.h"
#include "dirdiff.h"
#include "filediff.h"
#include "meldapp.h"
#include "meldwindow.h"
#include "preferences.h"
#include "settings.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLibraryInfo>
#include <QLineEdit>
#include <QMenu>
#include <QRegularExpression>
#include <QSet>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTranslator>
#include <QXmlStreamReader>

using namespace meld;

namespace {

const QString TranslationsFolder = QStringLiteral(MELD_TEST_DATA_DIR "/../../translations");

struct Message {
    QString context;
    QString source;
    QString disambiguation;
    bool numerus = false;
    bool finished = false;
    QStringList translations;
};

//! The messages of a .ts file, without those that have gone from the sources
QList<Message> readTs(const QString &path, QString *language = nullptr)
{
    QList<Message> messages;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return messages;
    QXmlStreamReader xml(&file);
    QString context;
    Message message;
    bool vanished = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) {
            if (xml.isEndElement() && xml.name() == QLatin1String("message") && !vanished)
                messages.append(message);
            continue;
        }
        const QStringView name = xml.name();
        if (name == QLatin1String("TS") && language) {
            *language = xml.attributes().value(QLatin1String("language")).toString();
        } else if (name == QLatin1String("name")) {
            context = xml.readElementText();
        } else if (name == QLatin1String("message")) {
            message = Message();
            message.context = context;
            message.numerus = xml.attributes().value(QLatin1String("numerus")) == QLatin1String("yes");
        } else if (name == QLatin1String("source")) {
            message.source = xml.readElementText();
        } else if (name == QLatin1String("comment")) {
            message.disambiguation = xml.readElementText();
        } else if (name == QLatin1String("translation")) {
            const QStringView type = xml.attributes().value(QLatin1String("type"));
            vanished = type == QLatin1String("vanished") || type == QLatin1String("obsolete");
            message.finished = type.isEmpty();
            if (!message.numerus)
                message.translations = {xml.readElementText()};
        } else if (name == QLatin1String("numerusform")) {
            message.translations.append(xml.readElementText());
        }
    }
    return messages;
}

QStringList tsFiles()
{
    return QDir(TranslationsFolder).entryList({QStringLiteral("meld-qt_*.ts")}, QDir::Files, QDir::Name);
}

QString key(const QString &context, const QString &source, const QString &disambiguation)
{
    return context + QChar(0x1f) + source + QChar(0x1f) + disambiguation;
}

/*!
 * Marks the translation of every message that the translations have (by its
 * context, text and disambiguation, as Qt looks it up) as ⟦message⟧, so that
 * text that isn't marked isn't translatable
 */
class MarkingTranslator : public QTranslator {
public:
    MarkingTranslator()
    {
        for (const Message &message : readTs(TranslationsFolder + QStringLiteral("/meld-qt_de.ts")))
            known_.insert(key(message.context, message.source, message.disambiguation));
    }

    QString translate(const char *context, const char *sourceText, const char *disambiguation,
                      int n) const override
    {
        Q_UNUSED(n);
        const QString source = QString::fromUtf8(sourceText);
        if (!known_.contains(key(QString::fromUtf8(context), source, QString::fromUtf8(disambiguation))))
            return QString();
        return QStringLiteral("⟦") + source + QStringLiteral("⟧");
    }

    bool isEmpty() const override { return false; }

private:
    QSet<QString> known_;
};

} // namespace

class TestTranslations : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    QString writeFile(const QString &name, const QByteArray &content)
    {
        const QString path = dir_.filePath(name);
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(content);
        return path;
    }

    static void settle(MeldWindow *window)
    {
        for (int i = 0; i < 200; ++i) {
            window->runPendingTasks();
            QCoreApplication::processEvents();
            if (!window->scheduler().tasksPending())
                break;
        }
    }

    //! The text a widget and its children show, with where it is
    static void collectText(QWidget *widget, QList<std::pair<QString, QString>> &texts)
    {
        const auto add = [&texts](const QString &text, const QObject *object, const char *what) {
            if (!text.isEmpty())
                texts.append({text, QStringLiteral("%1 %2 (%3)")
                                        .arg(QString::fromUtf8(object->metaObject()->className()),
                                             object->objectName(), QLatin1String(what))});
        };
        QList<QWidget *> widgets = widget->findChildren<QWidget *>();
        widgets.prepend(widget);
        for (QWidget *w : std::as_const(widgets)) {
            // Qt's own buttons, which Qt translates
            if (qobject_cast<QTabBar *>(w->parentWidget()))
                continue;
            if (w->isWindow())
                add(w->windowTitle(), w, "window title");
            add(w->toolTip(), w, "tooltip");
            add(w->accessibleName(), w, "accessible name");
            if (auto *label = qobject_cast<QLabel *>(w))
                add(label->text(), w, "label");
            if (auto *button = qobject_cast<QAbstractButton *>(w)) {
                // Qt translates the standard buttons
                auto *box = qobject_cast<QDialogButtonBox *>(button->parentWidget());
                if (!box || box->standardButton(button) == QDialogButtonBox::NoButton)
                    add(button->text(), w, "button");
            }
            if (auto *group = qobject_cast<QGroupBox *>(w))
                add(group->title(), w, "group");
            if (auto *edit = qobject_cast<QLineEdit *>(w))
                add(edit->placeholderText(), w, "placeholder");
            if (auto *tabs = qobject_cast<QTabBar *>(w)) {
                for (int i = 0; i < tabs->count(); ++i)
                    add(tabs->tabToolTip(i), w, "tab tooltip");
            }
            if (auto *header = qobject_cast<QHeaderView *>(w); header && header->model()) {
                for (int i = 0; i < header->count(); ++i)
                    add(header->model()->headerData(i, header->orientation()).toString(), w, "header");
            }
            for (QAction *action : w->actions()) {
                if (action->text() == action->objectName())
                    continue;
                add(action->text(), action, "action");
                add(action->toolTip() != action->text().remove(QLatin1Char('&')) ? action->toolTip() : QString(),
                    action, "action tooltip");
            }
        }
        for (QAction *action : widget->findChildren<QAction *>()) {
            // Tabs' actions, which the window shows with its own labels
            if (action->text() == action->objectName())
                continue;
            add(action->text(), action, "action");
            if (action->menu())
                add(action->menu()->title(), action, "menu");
        }
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(dir_.isValid());
        MeldSettings::useSettingsFile(dir_.filePath(QStringLiteral("settings.ini")));
    }

    void cleanup()
    {
        translations::install(QLocale(QLocale::English));
    }

    // -----------------------------------------------------------------------
    // Choosing the translation

    void candidates_data()
    {
        QTest::addColumn<QStringList>("uiLanguages");
        QTest::addColumn<QString>("posixLocale");
        QTest::addColumn<QStringList>("expected");
        QTest::newRow("German") << QStringList{"de-Latn-DE", "de-DE", "de-Latn", "de"} << "de_DE.UTF-8"
                                << QStringList{"de_Latn_DE", "de_DE", "de_Latn", "de"};
        // From LANGUAGE, as Qt gives it
        QTest::newRow("LANGUAGE") << QStringList{"fr-Latn-FR", "fr-FR", "fr", "de-DE", "de"} << "de_DE.UTF-8"
                                  << QStringList{"fr_Latn_FR", "fr_FR", "fr", "de_DE", "de"};
        QTest::newRow("Serbian, Latin, LANGUAGE")
            << QStringList{"sr-latin", "sr-Cyrl-RS", "sr-RS", "sr"} << "sr_RS.UTF-8@latin"
            << QStringList{"sr_Latn", "sr_Cyrl_RS", "sr_RS", "sr"};
        // Qt ignores the POSIX locale's modifier
        QTest::newRow("Serbian, Latin, LANG")
            << QStringList{"sr-Cyrl-RS", "sr-RS", "sr-Cyrl", "sr"} << "sr_RS.UTF-8@latin"
            << QStringList{"sr_Latn", "sr_Cyrl_RS", "sr_RS", "sr_Cyrl", "sr"};
        QTest::newRow("Serbian, Cyrillic") << QStringList{"sr-Cyrl-RS", "sr-RS", "sr-Cyrl", "sr"} << "sr_RS.UTF-8"
                                           << QStringList{"sr_Cyrl_RS", "sr_RS", "sr_Cyrl", "sr"};
        QTest::newRow("Valencian, LANG") << QStringList{"ca-Latn-ES", "ca-ES", "ca"} << "ca_ES.UTF-8@valencia"
                                         << QStringList{"ca_valencia", "ca_Latn_ES", "ca_ES", "ca"};
        QTest::newRow("Valencian, LANGUAGE")
            << QStringList{"ca-valencia", "ca-Latn-ES", "ca-ES", "ca"} << "ca_ES.UTF-8@valencia"
            << QStringList{"ca_valencia", "ca_Latn_ES", "ca_ES", "ca"};
        // After languages the modifier isn't for
        QTest::newRow("modifier of a later language")
            << QStringList{"de-DE", "de", "sr-RS", "sr"} << "sr_RS.UTF-8@latin"
            << QStringList{"de_DE", "de", "sr_Latn", "sr_RS", "sr"};
        QTest::newRow("unknown modifier") << QStringList{"de-DE", "de"} << "de_DE.UTF-8@euro"
                                          << QStringList{"de_DE", "de"};
    }

    void candidates()
    {
        QFETCH(QStringList, uiLanguages);
        QFETCH(QString, posixLocale);
        QFETCH(QStringList, expected);
        QCOMPARE(translations::candidates(uiLanguages, posixLocale), expected);
    }

    void installsTheLanguage_data()
    {
        QTest::addColumn<QString>("locale");
        QTest::addColumn<QString>("language");
        QTest::newRow("German") << "de_DE" << "de";
        QTest::newRow("Austrian German") << "de_AT" << "de";
        QTest::newRow("Brazilian Portuguese") << "pt_BR" << "pt_BR";
        QTest::newRow("European Portuguese") << "pt_PT" << "pt";
        QTest::newRow("Chinese, Taiwan") << "zh_TW" << "zh_TW";
        QTest::newRow("Chinese, China") << "zh_CN" << "zh_CN";
        QTest::newRow("Serbian") << "sr_RS" << "sr";
        QTest::newRow("Serbian, Latin") << "sr_Latn_RS" << "sr_Latn";
        QTest::newRow("Norwegian Bokmål") << "nb_NO" << "nb";
        QTest::newRow("British English") << "en_GB" << "en_GB";
        QTest::newRow("American English") << "en_US" << "";
        QTest::newRow("C") << "C" << "";
        QTest::newRow("no translation") << "lv_LV" << "";
    }

    void installsTheLanguage()
    {
        QFETCH(QString, locale);
        QFETCH(QString, language);
        QCOMPARE(translations::install(QLocale(locale)), language);
    }

    void translatesMessages()
    {
        QCOMPARE(translations::install(QLocale(QStringLiteral("de_DE"))), QStringLiteral("de"));
        const QString preferences = QCoreApplication::translate("meld::MeldWindow", "&Preferences");
        QVERIFY(!preferences.isEmpty());
        QCOMPARE_NE(preferences, QStringLiteral("&Preferences"));
        // Plurals in German's forms
        QCOMPARE(QCoreApplication::translate("meld::vc", "%n branches", nullptr, 1), QStringLiteral("1 Zweig"));
        QCOMPARE(QCoreApplication::translate("meld::vc", "%n branches", nullptr, 3), QStringLiteral("3 Zweige"));
    }

    void englishHasPluralForms()
    {
        QCOMPARE(translations::install(QLocale(QLocale::English)), QString());
        QCOMPARE(QCoreApplication::translate("meld::vc", "%n unpushed commits", nullptr, 1),
                 QStringLiteral("1 unpushed commit"));
        QCOMPARE(QCoreApplication::translate("meld::vc", "%n unpushed commits", nullptr, 2),
                 QStringLiteral("2 unpushed commits"));
        QCOMPARE(QCoreApplication::translate("meld::ConflictComparison", "%n unresolved conflicts", nullptr, 1),
                 QStringLiteral("1 unresolved conflict"));
    }

    void untranslatedMessagesAreEnglish()
    {
        // In English, and with its plural forms, under a translation whose
        // messages are unfinished (Dzongkha's port-only ones are), as these
        // aren't built in
        QCOMPARE(translations::install(QLocale(QStringLiteral("dz_BT"))), QStringLiteral("dz"));
        QCOMPARE(QCoreApplication::translate("meld::ConflictComparison", "%n unresolved conflicts", nullptr, 1),
                 QStringLiteral("1 unresolved conflict"));
        QCOMPARE(QCoreApplication::translate("meld::NoSuchContext", "Not a message"), QStringLiteral("Not a message"));
    }

    void installsQtsTranslations()
    {
        const QString folder = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        if (!QFile::exists(folder + QStringLiteral("/qtbase_de.qm")))
            QSKIP("Qt's German translation isn't installed");
        translations::install(QLocale(QStringLiteral("de_DE")));
        QCOMPARE_NE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Cancel"));
        translations::install(QLocale(QLocale::English));
        QCOMPARE(QCoreApplication::translate("QPlatformTheme", "Cancel"), QStringLiteral("Cancel"));
    }

    // -----------------------------------------------------------------------
    // The language setting

    void selectsTheSettingsLanguage_data()
    {
        QTest::addColumn<QString>("setting");
        QTest::addColumn<QString>("locale");
        QTest::addColumn<QString>("language");
        QTest::newRow("system") << "" << "de_DE" << "de";
        QTest::newRow("system, English") << "" << "en_US" << "";
        QTest::newRow("a language") << "fr" << "de_DE" << "fr";
        QTest::newRow("a variant") << "pt_BR" << "pt_PT" << "pt_BR";
        QTest::newRow("Valencian") << "ca_valencia" << "en_US" << "ca_valencia";
        QTest::newRow("English") << "en" << "de_DE" << "";
        // A translation that's no longer built in, or a setting's mistake
        QTest::newRow("no translation") << "lv" << "de_DE" << "de";
        QTest::newRow("not a language") << "../de" << "de_DE" << "de";
    }

    void selectsTheSettingsLanguage()
    {
        QFETCH(QString, setting);
        QFETCH(QString, locale);
        QFETCH(QString, language);
        QCOMPARE(translations::select(setting, QLocale(locale)), language);
    }

    void installsTheSettingsLanguage()
    {
        QCOMPARE(translations::installSetting(QStringLiteral("fr"), QLocale(QStringLiteral("de_DE"))),
                 QStringLiteral("fr"));
        QCOMPARE(translations::current(), QStringLiteral("fr"));
        QCOMPARE(QCoreApplication::translate("meld::vc", "%n branches", nullptr, 3), QStringLiteral("3 branches"));
        QCOMPARE(translations::installSetting(QStringLiteral("en"), QLocale(QStringLiteral("de_DE"))), QString());
        QCOMPARE(translations::current(), QString());
        QCOMPARE(QCoreApplication::translate("meld::vc", "%n branches", nullptr, 1), QStringLiteral("1 branch"));
        QCOMPARE(translations::install(QLocale(QStringLiteral("de_DE"))), QStringLiteral("de"));
        QCOMPARE(translations::current(), QStringLiteral("de"));
    }

    void languagesHaveTheirOwnNames()
    {
        QSet<QString> names;
        for (const QString &language : translations::available()) {
            const QString name = translations::nativeName(language);
            QVERIFY2(name != language, qPrintable(language));
            QVERIFY2(!names.contains(name), qPrintable(name));
            names.insert(name);
        }
        QCOMPARE(translations::nativeName(QStringLiteral("de")), QStringLiteral("Deutsch"));
        QCOMPARE(translations::nativeName(QStringLiteral("pt_BR")), QStringLiteral("Português (Brasil)"));
        QCOMPARE(translations::nativeName(QStringLiteral("en")), QStringLiteral("English"));
    }

    void preferencesChooseTheLanguage()
    {
        MeldSettings *settings = MeldSettings::instance();
        settings->resetValue(QStringLiteral("language"));
        QCOMPARE(MeldSettings::languageSetting(), QString());
        translations::install(QLocale(QLocale::English));

        PreferencesDialog dialog;
        dialog.show();
        auto *chooser = dialog.findChild<QComboBox *>(QStringLiteral("language-chooser"));
        QVERIFY(chooser);
        QCOMPARE(chooser->count(), translations::available().size() + 1);
        QCOMPARE(chooser->currentIndex(), 0);
        QCOMPARE(chooser->itemData(0).toString(), QString());
        QLabel *restart = nullptr;
        for (QLabel *label : dialog.findChildren<QLabel *>()) {
            if (label->text().contains(QLatin1String("next started")))
                restart = label;
        }
        QVERIFY(restart);
        // English is what's installed, which is what "System Language" gives
        // only if the system's language has no translation
        const bool systemTranslated = !translations::select(QString()).isEmpty();
        QCOMPARE(restart->isVisible(), systemTranslated);

        chooser->setCurrentIndex(chooser->findData(QStringLiteral("de")));
        QCOMPARE(chooser->currentText(), QStringLiteral("Deutsch"));
        QCOMPARE(settings->stringValue(QStringLiteral("language")), QStringLiteral("de"));
        QCOMPARE(MeldSettings::languageSetting(), QStringLiteral("de"));
        QVERIFY(restart->isVisible());
        // English is what's installed
        chooser->setCurrentIndex(chooser->findData(QStringLiteral("en")));
        QVERIFY(!restart->isVisible());
        chooser->setCurrentIndex(0);
        QCOMPARE(settings->stringValue(QStringLiteral("language")), QString());
        QCOMPARE(restart->isVisible(), systemTranslated);
        settings->resetValue(QStringLiteral("language"));
    }

    // -----------------------------------------------------------------------
    // The translations

    void everyTranslationIsBuiltIn()
    {
        QStringList languages;
        for (const QString &file : tsFiles())
            languages.append(file.mid(int(qstrlen("meld-qt_"))).chopped(3));
        QVERIFY(languages.size() > 40);
        QCOMPARE(translations::available(), languages);
    }

    void translationsMatchTheirMessages_data()
    {
        QTest::addColumn<QString>("file");
        for (const QString &file : tsFiles())
            QTest::newRow(qPrintable(file)) << file;
    }

    void translationsMatchTheirMessages()
    {
        QFETCH(QString, file);
        static const QRegularExpression placeholder(QStringLiteral("%(\\d+)"));
        const auto placeholders = [](const QString &text) {
            QSet<QString> found;
            for (const QRegularExpressionMatch &m : placeholder.globalMatch(text))
                found.insert(m.captured(0));
            return found;
        };
        const auto accelerators = [](QString text) {
            return int(text.remove(QStringLiteral("&&")).count(QRegularExpression(QStringLiteral("&[^&\\s]"))));
        };

        QString language;
        const QList<Message> messages = readTs(TranslationsFolder + QLatin1Char('/') + file, &language);
        QVERIFY(!messages.isEmpty());
        QVERIFY(!language.isEmpty());
        QStringList problems;
        for (const Message &message : messages) {
            if (!message.finished)
                continue;
            const QString where = message.context + QStringLiteral(": ") + message.source;
            if (message.translations.isEmpty() || message.translations.contains(QString())) {
                problems.append(where + QStringLiteral(" is empty"));
                continue;
            }
            for (const QString &translation : message.translations) {
                // The forms of plurals may say "one" rather than %n
                if (placeholders(translation) != placeholders(message.source))
                    problems.append(where + QStringLiteral(" has other placeholders: ") + translation);
                if (accelerators(translation) > std::max(1, accelerators(message.source)))
                    problems.append(where + QStringLiteral(" has several accelerators: ") + translation);
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QLatin1Char('\n'))));
    }

    // -----------------------------------------------------------------------
    // The windows' text

    void visibleTextIsTranslatable()
    {
        MarkingTranslator marking;
        QCoreApplication::installTranslator(&marking);

        const QString left = writeFile(QStringLiteral("left/text.txt"), "one\ntwo\nthree\n");
        const QString right = writeFile(QStringLiteral("right/text.txt"), "one\n2\nthree\nfour\n");
        writeFile(QStringLiteral("left/only.txt"), "x\n");

        MeldApp app;
        QCOMPARE(app.handleCommandLine({"meld-qt"}), -1);
        MeldWindow *window = app.window();
        QVERIFY(window);
        window->appendFilediff({left, right});
        settle(window);
        window->appendDirdiff({QFileInfo(left).path(), QFileInfo(right).path()});
        settle(window);
        // Without version control: the view's widgets, and its message
        window->appendVcview(QFileInfo(left).path());
        settle(window);
        app.showAbout();
        app.showPreferences();
        for (QAction *action : window->findChildren<QAction *>()) {
            if (action->text().contains(QLatin1String("Keyboard Shortcuts"))) {
                action->trigger();
                break;
            }
        }
        // A control: text that isn't translatable, which has to be found
        QWidget control;
        auto *plain = new QLabel(QStringLiteral("Plain text"), &control);
        plain->setObjectName(QStringLiteral("control"));
        new QLabel(QCoreApplication::translate("meld::MeldWindow", "&Preferences"), &control);
        control.show();
        QCoreApplication::processEvents();

        QList<std::pair<QString, QString>> texts;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (widget->isVisible())
                collectText(widget, texts);
        }
        QVERIFY(texts.size() > 100);
        QVERIFY(std::any_of(texts.cbegin(), texts.cend(), [](const auto &text) {
            return text.first == QStringLiteral("⟦&Preferences⟧");
        }));

        // What's left is data: paths, numbers, the names of encodings, fonts
        // and colour schemes (GtkSourceView's, which it translates itself),
        // and of filters, which are settings (whose defaults are translated)
        static const QRegularExpression marked(QStringLiteral("⟦[^⟦⟧]*⟧"));
        static const QRegularExpression encoding(QStringLiteral("^([A-Z0-9_-]+|windows-\\d+|Shift_JIS|Big5)$"));
        const QFont customFont = MeldSettings::parseFontDescription(
            MeldSettings::instance()->stringValue(QStringLiteral("custom-font")));
        QStringList untranslated;
        for (const auto &[text, where] : std::as_const(texts)) {
            if (encoding.match(text).hasMatch() || where.contains(QLatin1String("custom-filter-"))
                || where.contains(QLatin1String("style-scheme-chooser")) || text.startsWith(customFont.family()))
                continue;
            // From the innermost, for translations inside translations
            QString rest = text;
            for (qsizetype size = -1; size != rest.size();) {
                size = rest.size();
                rest.remove(marked);
            }
            rest.remove(dir_.path());
            for (const char *data : {"left", "right", "text.txt", "only.txt", "Meld Qt", MELD_VERSION, QT_VERSION_STR})
                rest.remove(QLatin1String(data));
            if (rest.contains(QRegularExpression(QStringLiteral("\\p{L}{2,}"))))
                untranslated.append(QStringLiteral("%1: %2").arg(where, text));
        }
        untranslated.removeDuplicates();
        QCOMPARE(untranslated.removeAll(QStringLiteral("QLabel control (label): Plain text")), 1);
        QCoreApplication::removeTranslator(&marking);
        for (MeldWindow *w : app.windows()) {
            for (MeldDoc *doc : w->docs()) {
                if (auto *filediff = qobject_cast<FileDiff *>(doc)) {
                    for (int pane = 0; pane < filediff->numPanes(); ++pane)
                        filediff->textBuffer(pane)->setModified(false);
                }
            }
        }
        for (const QString &text : std::as_const(untranslated))
            qWarning().noquote() << "Not translatable:" << text;
        QVERIFY2(untranslated.isEmpty(),
                 qPrintable(QStringLiteral("%1 texts aren't translatable (see the warnings)").arg(untranslated.size())));
    }
};

QTEST_MAIN(TestTranslations)
#include "tst_translations.moc"
