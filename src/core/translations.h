#pragma once

#include <QLocale>
#include <QString>
#include <QStringList>

namespace meld::translations {

/*!
 * The names of the translations to look for, best first, for languages as
 * QLocale::uiLanguages() gives them (e.g., "de-DE"), and the POSIX locale of
 * messages (LC_ALL, LC_MESSAGES or LANG, e.g., "sr_RS.UTF-8@latin").
 *
 * Names are as the translations' files have them: "de_DE", "pt_BR". Qt
 * ignores gettext's @latin and @valencia modifiers in the POSIX locale, and
 * gives them from LANGUAGE as "sr-latin" and "ca-valencia": these are
 * "sr_Latn" (as Qt names Serbian in the Latin script) and "ca_valencia".
 */
QStringList candidates(const QStringList &uiLanguages, const QString &posixLocale = QString());

//! The POSIX locale of messages, as gettext reads it from the environment
QString posixMessagesLocale();

/*!
 * Install the program's translations, and Qt's own, for the locale's
 * languages: by default the user's (QLocale::system() and the POSIX locale).
 * The English translation, which only has the plural forms of messages, is
 * always installed first, for messages a translation doesn't have. Earlier
 * translations installed by this are removed.
 *
 * \return The language of the translation installed (e.g., "de" or
 * "pt_BR"), or an empty string for English
 */
QString install(const QLocale &locale = QLocale::system());

/*!
 * The translation to use for the language setting: the language it names
 * (e.g., "de" or "pt_BR"), or none for "en"; for an empty setting, or a
 * language without a translation, the locale's, as install() chooses it.
 *
 * \return The translation's language, as its file names it, or an empty
 * string for English
 */
QString select(const QString &setting, const QLocale &locale = QLocale::system());

/*!
 * Install the translations for the language setting, as select() chooses
 * them, as install() does.
 *
 * \return The language of the translation installed, as select() gives it
 */
QString installSetting(const QString &setting, const QLocale &locale = QLocale::system());

//! The language of the translation installed last, as select() gives it
QString current();

//! The languages of the program's translations, as their files name them
QStringList available();

//! The name of a translation's language, in that language, for choosing it
QString nativeName(const QString &language);

} // namespace meld::translations
