#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace meld::syntax {

class ContextData;

//! A language definition file's metadata
class Language {
public:
    QString id() const { return id_; }
    QString name() const { return name_; }
    QString section() const { return section_; }
    bool hidden() const { return hidden_; }
    QString fileName() const { return fileName_; }
    QStringList globs() const { return splitProperty(QStringLiteral("globs")); }
    QStringList mimeTypes() const { return splitProperty(QStringLiteral("mimetypes")); }
    //! Metadata properties, e.g., "line-comment-start"
    QString metadata(const QString &name) const { return properties_.value(name); }

    //! Read a language file's metadata; returns nullptr on errors
    static std::unique_ptr<Language> fromFile(const QString &fileName, QString *error = nullptr);

private:
    QStringList splitProperty(const QString &name) const;

    QString id_;
    QString name_;
    QString section_;
    QString fileName_;
    bool hidden_ = false;
    QHash<QString, QString> properties_;
};

/*!
 * The available languages (GtkSourceLanguageManager)
 *
 * Languages are read from *.lang files in the search path; the default
 * manager uses the definitions bundled with Meld.
 */
class LanguageManager {
public:
    static LanguageManager &instance();
    explicit LanguageManager(const QStringList &searchPath);
    ~LanguageManager();

    LanguageManager(const LanguageManager &) = delete;
    LanguageManager &operator=(const LanguageManager &) = delete;

    //! All language ids, including hidden ones, sorted by language name
    QStringList languageIds() const { return ids_; }
    const Language *language(const QString &id) const;

    /*!
     * Guess the language of a file from its name and content (MIME) type,
     * as gtk_source_language_manager_guess_language() does
     *
     * Globs take precedence over the content type, which is used to pick
     * among several glob matches or to refine a match (e.g., an XML
     * dialect), or if no glob matches. Either may be empty.
     */
    const Language *guessLanguage(const QString &fileName, const QString &contentType) const;
    //! The language of a file, from its name and content type, which is
    //! guessed from its contents if needed (get_language_from_file())
    const Language *guessLanguageForFile(const QString &path) const;
    //! The language of a MIME type (get_language_from_mime_type())
    const Language *guessLanguageForMimeType(const QString &mimeType) const;

    /*!
     * The definitions for highlighting the language, including those of
     * other languages it uses
     *
     * Returns nullptr if the language can't be loaded, as GtkSourceView
     * then doesn't highlight it either. `error` describes why.
     */
    std::shared_ptr<const ContextData> contextData(const QString &id, QString *error = nullptr) const;

private:
    const Language *languageForMimeType(const QString &mimeType) const;
    const Language *languageForMimeTypePass(const QString &mimeType, bool exactMatch) const;

    std::vector<std::unique_ptr<Language>> languages_;
    QHash<QString, const Language *> byId_;
    QStringList ids_;
    mutable QHash<QString, std::shared_ptr<const ContextData>> contextData_;
    mutable QHash<QString, QString> errors_;
};

//! Whether a file name matches a glob, as g_pattern_match_simple() (only
//! '*' and '?' are special)
bool patternMatchSimple(QStringView pattern, QStringView string);

} // namespace meld::syntax
