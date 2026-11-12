#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QStringView>

namespace meld::vc {

//! Lines of command output, as `output.split("\n")[:-1]`: the text after the
//! last newline is dropped
inline QStringList outputLines(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    lines.removeLast();
    return lines;
}

//! Lines of text without their line endings, as Python's str.splitlines()
inline QStringList splitLines(const QString &text)
{
    QStringList lines;
    qsizetype start = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\n') || c == QLatin1Char('\r') || c == QChar(0x0b) || c == QChar(0x0c)
            || c == QChar(0x1c) || c == QChar(0x1d) || c == QChar(0x1e) || c == QChar(0x85) || c == QChar(0x2028)
            || c == QChar(0x2029)) {
            lines.append(text.mid(start, i - start));
            if (c == QLatin1Char('\r') && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n'))
                ++i;
            start = i + 1;
        }
    }
    if (start < text.size())
        lines.append(text.mid(start));
    return lines;
}

//! Decode C-style escapes (as quoted file names from git), with the
//! semantics of Python's codecs.escape_decode
inline QByteArray unescapeCString(QStringView text)
{
    QByteArray result;
    const auto isOctal = [](QChar c) { return c >= QLatin1Char('0') && c <= QLatin1Char('7'); };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= text.size()) {
            result.append(char(c.unicode() & 0xff));
            continue;
        }
        const QChar next = text.at(++i);
        switch (next.unicode()) {
        case '\n':
            break;
        case '\\':
        case '\'':
        case '"':
            result.append(char(next.unicode()));
            break;
        case 'a':
            result.append('\a');
            break;
        case 'b':
            result.append('\b');
            break;
        case 'f':
            result.append('\f');
            break;
        case 'n':
            result.append('\n');
            break;
        case 'r':
            result.append('\r');
            break;
        case 't':
            result.append('\t');
            break;
        case 'v':
            result.append('\v');
            break;
        case 'x': {
            bool ok = false;
            const int value = i + 2 < text.size() ? text.sliced(i + 1, 2).toInt(&ok, 16) : 0;
            if (ok) {
                result.append(char(value));
                i += 2;
            } else {
                result.append("\\x");
            }
            break;
        }
        default:
            if (isOctal(next)) {
                int value = next.unicode() - '0';
                for (int digits = 1; digits < 3 && i + 1 < text.size() && isOctal(text.at(i + 1)); ++digits)
                    value = value * 8 + (text.at(++i).unicode() - '0');
                result.append(char(value & 0xff));
            } else {
                result.append('\\');
                result.append(char(next.unicode() & 0xff));
            }
            break;
        }
    }
    return result;
}

} // namespace meld::vc
