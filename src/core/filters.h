#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>

#include <optional>

namespace meld {

/*!
 * Translate a Python `re` pattern to an equivalent PCRE2 pattern for
 * QRegularExpression. Returns nullopt for patterns that Python would
 * reject (e.g., incomplete \x escapes), so that filters behave the same
 * as in the original implementation.
 */
std::optional<QString> translatePythonRegex(const QString &pattern);

//! Compile a Python-syntax regex; nullopt on failure (logged, as try_compile)
std::optional<QRegularExpression> tryCompile(const QString &pattern,
                                             QRegularExpression::PatternOptions options);

//! re.match(): match anchored at the start of the subject
bool pyMatch(const QRegularExpression &regex, const QString &subject);

//! Translate a shell PATTERN to a regular expression; based on
//! fnmatch.translate(), but also handles {a,b,c}.
QString shellToRegex(const QString &pattern);

class FilterEntry {
public:
    enum Type {
        Regex = 0,
        Shell = 1,
    };

    QString label;
    bool active = false;
    std::optional<QRegularExpression> filter;
    //! Regex matching Latin-1 decoded bytes, equivalent to Python's bytes
    //! patterns. Only set for regex filters.
    std::optional<QRegularExpression> byteFilter;
    QString filterString;

    static std::optional<QRegularExpression> compileRegex(const QString &regex, bool byteRegex = false);
    static std::optional<QRegularExpression> compileShellPattern(const QString &pattern);
    static FilterEntry fromSettings(const QString &name, bool active, const QString &filterString,
                                    Type type);
    static bool checkFilter(const QString &filterString, Type type);
};

using FilterList = QList<FilterEntry>;

} // namespace meld
