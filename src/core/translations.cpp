#include "translations.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QLibraryInfo>
#include <QTranslator>

#include <memory>
#include <vector>

namespace meld::translations {

namespace {

const QString ResourceFolder = QStringLiteral(":/i18n");
const QString FilePrefix = QStringLiteral("meld-qt_");

//! The translations this installed, which it removes when it installs others
std::vector<std::unique_ptr<QTranslator>> &installed()
{
    static std::vector<std::unique_ptr<QTranslator>> translators;
    return translators;
}

//! A language as the translations' files name it
QString fileLanguage(QString name)
{
    name.replace(QLatin1Char('-'), QLatin1Char('_'));
    if (name == QLatin1String("sr_latin"))
        return QStringLiteral("sr_Latn");
    return name;
}

QString languageCode(const QString &name)
{
    return name.section(QLatin1Char('_'), 0, 0);
}

} // namespace

QStringList candidates(const QStringList &uiLanguages, const QString &posixLocale)
{
    QStringList result;
    for (const QString &language : uiLanguages) {
        const QString name = fileLanguage(language);
        if (!result.contains(name))
            result.append(name);
    }

    // The POSIX locale's modifier, e.g., "sr_RS.UTF-8@latin", comes before
    // the languages Qt made of the locale without it
    const qsizetype at = posixLocale.indexOf(QLatin1Char('@'));
    if (at > 0) {
        const QString language = languageCode(posixLocale.left(at).section(QLatin1Char('.'), 0, 0));
        const QString modifier = posixLocale.mid(at + 1);
        QString name;
        if (modifier == QLatin1String("latin"))
            name = fileLanguage(language + QLatin1String("_latin"));
        else if (modifier == QLatin1String("valencia"))
            name = language + QLatin1String("_valencia");
        if (!name.isEmpty() && !result.contains(name)) {
            qsizetype index = 0;
            while (index < result.size() && languageCode(result.at(index)) != language)
                ++index;
            result.insert(index, name);
        }
    }
    return result;
}

QString posixMessagesLocale()
{
    for (const char *variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const QString value = qEnvironmentVariable(variable);
        if (!value.isEmpty())
            return value;
    }
    return QString();
}

namespace {

QString &currentLanguage()
{
    static QString language;
    return language;
}

QString translationFile(const QString &language)
{
    return ResourceFolder + QLatin1Char('/') + FilePrefix + language + QLatin1String(".qm");
}

//! The translation for the locale's languages, or an empty string for English
QString chooseFile(const QLocale &locale)
{
    // The names are matched whole: the languages have their shorter forms
    // after them (e.g., "pt-Latn-BR pt-BR pt-Latn pt"), where shortening a
    // name as QTranslator::load() does would find "pt" before "pt_BR"
    const QString posixLocale = locale == QLocale::system() ? posixMessagesLocale() : QString();
    bool english = false;
    for (const QString &name : candidates(locale.uiLanguages(), posixLocale)) {
        const QString code = languageCode(name);
        // English is the messages' own language (as for the C locale), so
        // languages after it aren't used, apart from its variants (en_GB)
        if ((english && code != QLatin1String("en")) || code == QLatin1String("C"))
            break;
        if (name != QLatin1String("en") && QFile::exists(translationFile(name)))
            return name;
        english = english || code == QLatin1String("en");
    }
    return QString();
}

//! Install a translation (none for an empty name), returning its language
QString installFile(const QString &name)
{
    auto &translators = installed();
    for (const auto &translator : translators)
        QCoreApplication::removeTranslator(translator.get());
    translators.clear();
    currentLanguage().clear();

    const auto add = [&translators](std::unique_ptr<QTranslator> translator) {
        QCoreApplication::installTranslator(translator.get());
        translators.push_back(std::move(translator));
    };

    // The plural forms of English messages, under any other translation
    auto plurals = std::make_unique<QTranslator>();
    if (plurals->load(FilePrefix + QLatin1String("en"), ResourceFolder))
        add(std::move(plurals));

    QString language;
    auto translator = std::make_unique<QTranslator>();
    if (!name.isEmpty() && translator->load(translationFile(name))) {
        language = translator->language();
        currentLanguage() = name;
        add(std::move(translator));
    }

    // Qt's own, for its standard dialogs and buttons
    if (!language.isEmpty()) {
        auto qt = std::make_unique<QTranslator>();
        if (qt->load(QLocale(language), QStringLiteral("qt"), QStringLiteral("_"),
                     QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
            add(std::move(qt));
    }
    return language;
}

} // namespace

QString install(const QLocale &locale)
{
    return installFile(chooseFile(locale));
}

QString select(const QString &setting, const QLocale &locale)
{
    if (setting == QLatin1String("en"))
        return QString();
    if (!setting.isEmpty() && QFile::exists(translationFile(setting)))
        return setting;
    return chooseFile(locale);
}

QString installSetting(const QString &setting, const QLocale &locale)
{
    installFile(select(setting, locale));
    return current();
}

QString current()
{
    return currentLanguage();
}

QStringList available()
{
    QStringList result;
    const QStringList files = QDir(ResourceFolder).entryList({FilePrefix + QLatin1String("*.qm")}, QDir::Files);
    for (const QString &file : files)
        result.append(file.mid(FilePrefix.size()).chopped(3));
    return result;
}

QString nativeName(const QString &language)
{
    // Where Qt's names don't tell the translations apart, or are narrower
    // than the translation (Spanish, as spoken in Spain)
    static const QHash<QString, QString> names{
        {QStringLiteral("en"), QStringLiteral("English")},
        {QStringLiteral("es"), QStringLiteral("español")},
        {QStringLiteral("ca_valencia"), QStringLiteral("català (valencià)")},
        {QStringLiteral("pt"), QStringLiteral("português (Portugal)")},
        {QStringLiteral("pt_BR"), QStringLiteral("português (Brasil)")},
        {QStringLiteral("id"), QStringLiteral("Bahasa Indonesia")},
    };
    QString name = names.value(language);
    if (name.isEmpty())
        name = QLocale(language).nativeLanguageName();
    if (name.isEmpty())
        return language;
    // As at the start of a menu's item
    return name.left(1).toUpper() + name.mid(1);
}

} // namespace meld::translations
