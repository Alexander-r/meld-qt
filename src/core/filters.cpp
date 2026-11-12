#include "filters.h"

#include <QLoggingCategory>

namespace meld {

Q_LOGGING_CATEGORY(lcFilters, "meld.filters")

namespace {

bool isHexDigit(QChar c)
{
    return (c >= u'0' && c <= u'9') || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}

bool isOctDigit(QChar c)
{
    return c >= u'0' && c <= u'7';
}

bool isAsciiLetter(QChar c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

QString hexEscape(uint value)
{
    return QStringLiteral("\\x{%1}").arg(value, 0, 16);
}

std::optional<QString> translateRegex(const QString &pattern, bool bytes)
{
    QString out;
    out.reserve(pattern.size() + 8);
    const qsizetype n = pattern.size();
    bool inClass = false;
    qsizetype classStart = -1;

    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = pattern.at(i);

        if (c != u'\\') {
            if (!inClass && c == u'[') {
                inClass = true;
                classStart = i + 1;
                if (classStart < n && pattern.at(classStart) == u'^')
                    ++classStart;
            } else if (inClass && c == u']' && i != classStart) {
                inClass = false;
            }
            out.append(c);
            continue;
        }

        if (i + 1 >= n)
            return std::nullopt; // trailing backslash
        const QChar e = pattern.at(++i);

        // Python requires exactly `count` hex digits for \x, \u and \U
        const auto takeHex = [&](int count) -> std::optional<uint> {
            uint value = 0;
            for (int k = 1; k <= count; ++k) {
                if (i + k >= n || !isHexDigit(pattern.at(i + k)))
                    return std::nullopt;
                const QChar digit = pattern.at(i + k);
                const uint v = digit.isDigit() ? uint(digit.unicode() - u'0')
                                               : uint(digit.toLower().unicode() - u'a' + 10);
                value = value * 16 + v;
            }
            i += count;
            return value;
        };

        if (e == u'x') {
            const auto value = takeHex(2);
            if (!value)
                return std::nullopt; // incomplete escape
            out.append(hexEscape(*value));
            continue;
        }
        if (e == u'u' && !bytes) {
            const auto value = takeHex(4);
            if (!value)
                return std::nullopt;
            out.append(hexEscape(*value));
            continue;
        }
        if (e == u'U' && !bytes) {
            const auto value = takeHex(8);
            if (!value || *value > 0x10FFFF)
                return std::nullopt;
            out.append(hexEscape(*value));
            continue;
        }
        if (e == u'N')
            return std::nullopt; // named characters are unsupported by PCRE2

        if (inClass) {
            static const QString classEscapes = QStringLiteral("dDsSwWabfnrt");
            if (e == u'v') {
                out.append(hexEscape(0x0b));
            } else if (isOctDigit(e)) {
                // octal escape (up to three digits)
                uint value = uint(e.unicode() - u'0');
                int digits = 1;
                while (digits < 3 && i + 1 < n && isOctDigit(pattern.at(i + 1))) {
                    value = value * 8 + uint(pattern.at(++i).unicode() - u'0');
                    ++digits;
                }
                if (value > 0377)
                    return std::nullopt;
                out.append(hexEscape(value));
            } else if (e.isDigit()) {
                return std::nullopt;
            } else if (isAsciiLetter(e)) {
                if (!classEscapes.contains(e))
                    return std::nullopt; // bad escape
                out.append(u'\\');
                out.append(e);
            } else {
                out.append(u'\\');
                out.append(e);
            }
            continue;
        }

        static const QString escapes = QStringLiteral("AbBdDsSwWafnrt");
        if (e == u'Z') {
            out.append(QStringLiteral("\\z"));
        } else if (e == u'v') {
            out.append(hexEscape(0x0b));
        } else if (e == u'0') {
            // octal escape
            uint value = 0;
            int digits = 0;
            while (digits < 2 && i + 1 < n && isOctDigit(pattern.at(i + 1))) {
                value = value * 8 + uint(pattern.at(++i).unicode() - u'0');
                ++digits;
            }
            out.append(hexEscape(value));
        } else if (e.isDigit() && e.unicode() < 128) {
            // octal escape *or* decimal group reference
            QString digits(e);
            if (i + 1 < n && pattern.at(i + 1).isDigit() && pattern.at(i + 1).unicode() < 128) {
                digits.append(pattern.at(++i));
                if (isOctDigit(digits.at(0)) && isOctDigit(digits.at(1)) && i + 1 < n
                    && isOctDigit(pattern.at(i + 1))) {
                    digits.append(pattern.at(++i));
                    const uint value = digits.toUInt(nullptr, 8);
                    if (value > 0377)
                        return std::nullopt;
                    out.append(hexEscape(value));
                    continue;
                }
            }
            out.append(QStringLiteral("\\g{%1}").arg(digits));
        } else if (isAsciiLetter(e)) {
            if (!escapes.contains(e))
                return std::nullopt; // bad escape
            out.append(u'\\');
            out.append(e);
        } else {
            out.append(u'\\');
            out.append(e);
        }
    }

    if (inClass)
        return std::nullopt; // unterminated character set
    return out;
}

} // namespace

std::optional<QString> translatePythonRegex(const QString &pattern)
{
    return translateRegex(pattern, false);
}

std::optional<QRegularExpression> tryCompile(const QString &pattern,
                                             QRegularExpression::PatternOptions options)
{
    const bool bytes = !(options & QRegularExpression::UseUnicodePropertiesOption);
    const std::optional<QString> translated = translateRegex(pattern, bytes);
    if (!translated) {
        qCWarning(lcFilters).noquote() << "Error compiling regex" << pattern;
        return std::nullopt;
    }
    QRegularExpression regex(*translated, options);
    if (!regex.isValid()) {
        qCWarning(lcFilters).noquote()
            << "Error compiling regex" << pattern << ":" << regex.errorString();
        return std::nullopt;
    }
    regex.optimize();
    return regex;
}

bool pyMatch(const QRegularExpression &regex, const QString &subject)
{
    return regex
        .match(subject, 0, QRegularExpression::NormalMatch,
               QRegularExpression::AnchorAtOffsetMatchOption)
        .hasMatch();
}

QString shellToRegex(const QString &pattern)
{
    qsizetype i = 0;
    const qsizetype n = pattern.size();
    QString res;
    const auto takeChar = [&]() {
        QString c(pattern.at(i));
        if (pattern.at(i).isHighSurrogate() && i + 1 < n && pattern.at(i + 1).isLowSurrogate())
            c.append(pattern.at(++i));
        ++i;
        return c;
    };

    while (i < n) {
        const QString c = takeChar();
        if (c == u"\\") {
            if (i < n)
                res += QRegularExpression::escape(takeChar());
        } else if (c == u"*") {
            res += QStringLiteral(".*");
        } else if (c == u"?") {
            res += QStringLiteral(".");
        } else if (c == u"[") {
            const qsizetype j = pattern.indexOf(u']', i);
            if (j == -1) {
                res += QStringLiteral("\\[");
            } else {
                QString stuff = pattern.mid(i, j - i);
                i = j + 1;
                if (stuff.isEmpty()) {
                    res += QStringLiteral("\\[\\]");
                    continue;
                }
                if (stuff.at(0) == u'!')
                    stuff = QStringLiteral("^") + stuff.mid(1);
                else if (stuff.at(0) == u'^')
                    stuff = QStringLiteral("\\^") + stuff.mid(1);
                res += QStringLiteral("[%1]").arg(stuff);
            }
        } else if (c == u"{") {
            const qsizetype j = pattern.indexOf(u'}', i);
            if (j == -1) {
                res += QStringLiteral("\\{");
            } else {
                const QString stuff = pattern.mid(i, j - i);
                i = j + 1;
                QStringList parts;
                for (const QString &part : stuff.split(u','))
                    parts.append(shellToRegex(part).chopped(1));
                res += QStringLiteral("(%1)").arg(parts.join(u'|'));
            }
        } else {
            res += QRegularExpression::escape(c);
        }
    }
    return res + QStringLiteral("$");
}

std::optional<QRegularExpression> FilterEntry::compileRegex(const QString &regex, bool byteRegex)
{
    if (byteRegex) {
        // Equivalent to compiling the UTF-8 encoded pattern as a bytes regex:
        // each byte is matched as a Latin-1 character without Unicode rules.
        const QString bytesPattern = QString::fromLatin1(regex.toUtf8());
        return tryCompile(bytesPattern, QRegularExpression::MultilineOption);
    }
    return tryCompile(regex, QRegularExpression::MultilineOption
                                 | QRegularExpression::UseUnicodePropertiesOption);
}

std::optional<QRegularExpression> FilterEntry::compileShellPattern(const QString &pattern)
{
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    const QStringList bits = pattern.split(whitespace, Qt::SkipEmptyParts);
    if (bits.isEmpty()) {
        // An empty pattern would match everything, so skip it
        return std::nullopt;
    }
    QString regex;
    if (bits.size() > 1) {
        QStringList regexes;
        for (const QString &bit : bits)
            regexes.append(shellToRegex(bit).chopped(1));
        regex = QStringLiteral("(%1)$").arg(regexes.join(u'|'));
    } else {
        regex = shellToRegex(bits.first());
    }
    return tryCompile(regex, QRegularExpression::UseUnicodePropertiesOption);
}

FilterEntry FilterEntry::fromSettings(const QString &name, bool active, const QString &filterString,
                                      Type type)
{
    FilterEntry entry;
    entry.label = name;
    entry.filterString = filterString;
    if (type == Regex) {
        entry.filter = compileRegex(filterString);
        entry.byteFilter = compileRegex(filterString, true);
    } else {
        entry.filter = compileShellPattern(filterString);
    }
    entry.active = active && entry.filter.has_value();
    return entry;
}

bool FilterEntry::checkFilter(const QString &filterString, Type type)
{
    if (type == Regex)
        return compileRegex(filterString).has_value();
    return compileShellPattern(filterString).has_value();
}

} // namespace meld
