#include "textfile.h"

#include <QCoreApplication>
#include <QStringDecoder>
#include <QStringEncoder>

namespace meld {

QString newlineString(NewlineType type)
{
    switch (type) {
    case NewlineType::LF:
        return QStringLiteral("\n");
    case NewlineType::CRLF:
        return QStringLiteral("\r\n");
    case NewlineType::CR:
        return QStringLiteral("\r");
    }
    return QStringLiteral("\n");
}

QString newlineDisplayName(NewlineType type)
{
    switch (type) {
    case NewlineType::LF:
        return QCoreApplication::translate("meld", "UNIX (LF)");
    case NewlineType::CRLF:
        return QCoreApplication::translate("meld", "DOS/Windows (CR-LF)");
    case NewlineType::CR:
        return QCoreApplication::translate("meld", "Mac OS (CR)");
    }
    return {};
}

QList<QByteArray> defaultCandidateEncodings()
{
    // Mirrors GtkSourceView's default candidates: UTF-8, the locale
    // encoding, ISO-8859-15 and UTF-16.
    QList<QByteArray> candidates{QByteArrayLiteral("UTF-8")};
    const QByteArray locale = QStringConverter::nameForEncoding(QStringConverter::System);
    if (!locale.isEmpty() && locale.compare("UTF-8", Qt::CaseInsensitive) != 0)
        candidates.append(locale);
    candidates.append(QByteArrayLiteral("ISO-8859-15"));
    candidates.append(QByteArrayLiteral("UTF-16"));
    return candidates;
}

QList<QByteArray> selectableEncodings()
{
    QList<QByteArray> result{
        "UTF-8",       "UTF-16",       "UTF-16LE",    "UTF-16BE",     "UTF-32",
        "ISO-8859-1",  "ISO-8859-15",  "ISO-8859-2",  "ISO-8859-5",   "ISO-8859-7",
        "windows-1250", "windows-1251", "windows-1252", "windows-1253", "KOI8-R",
        "Shift_JIS",   "EUC-JP",       "GB18030",     "Big5",         "EUC-KR",
    };
    result.removeIf([](const QByteArray &name) { return !QStringDecoder(name.constData()).isValid(); });
    return result;
}

QString encodingDisplayName(const QByteArray &encoding)
{
    return QString::fromLatin1(encoding);
}

std::optional<NewlineType> detectNewline(QStringView text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'\n')
            return NewlineType::LF;
        if (c == u'\r') {
            if (i + 1 < text.size() && text.at(i + 1) == u'\n')
                return NewlineType::CRLF;
            return NewlineType::CR;
        }
    }
    return std::nullopt;
}

QString normalizeNewlines(const QString &text)
{
    if (!text.contains(u'\r'))
        return text;
    QString result;
    result.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'\r') {
            result.append(u'\n');
            if (i + 1 < text.size() && text.at(i + 1) == u'\n')
                ++i;
        } else {
            result.append(c);
        }
    }
    return result;
}

namespace {

struct Bom {
    QByteArray bytes;
    QByteArray encoding;
};

std::optional<Bom> detectBom(const QByteArray &data)
{
    // Check longer marks first: the UTF-32LE mark starts with the UTF-16LE one
    static const Bom boms[] = {
        {QByteArray("\xFF\xFE\x00\x00", 4), QByteArrayLiteral("UTF-32LE")},
        {QByteArray("\x00\x00\xFE\xFF", 4), QByteArrayLiteral("UTF-32BE")},
        {QByteArray("\xEF\xBB\xBF", 3), QByteArrayLiteral("UTF-8")},
        {QByteArray("\xFF\xFE", 2), QByteArrayLiteral("UTF-16LE")},
        {QByteArray("\xFE\xFF", 2), QByteArrayLiteral("UTF-16BE")},
    };
    for (const Bom &bom : boms) {
        if (data.startsWith(bom.bytes))
            return bom;
    }
    return std::nullopt;
}

std::optional<QString> strictDecode(const QByteArray &data, const QByteArray &encoding)
{
    // Note: the Stateless flag disables Qt's tracking of invalid characters
    QStringDecoder decoder(encoding.constData(), QStringConverter::Flag::Default);
    if (!decoder.isValid())
        return std::nullopt;
    QString text = decoder.decode(data);
    if (decoder.hasError())
        return std::nullopt;
    return text;
}

} // namespace

std::optional<DecodedText> decodeText(const QByteArray &data, const QList<QByteArray> &candidates)
{
    DecodedText result;
    std::optional<QString> text;

    if (const std::optional<Bom> bom = detectBom(data)) {
        text = strictDecode(data.mid(bom->bytes.size()), bom->encoding);
        if (text) {
            result.format.encoding = bom->encoding;
            result.format.bom = true;
        }
    }

    if (!text) {
        for (const QByteArray &candidate : candidates) {
            // UTF-16 without a byte order mark only makes sense for data
            // with an even number of bytes
            if (candidate.startsWith("UTF-16") && data.size() % 2)
                continue;
            text = strictDecode(data, candidate);
            if (text) {
                result.format.encoding = candidate;
                break;
            }
        }
    }

    if (!text)
        return std::nullopt;

    if (const std::optional<NewlineType> newline = detectNewline(*text))
        result.format.newline = *newline;
    result.text = normalizeNewlines(*text);
    return result;
}

std::optional<QByteArray> encodeText(const QString &text, const TextFileFormat &format)
{
    QString output = text;
    if (format.newline != NewlineType::LF)
        output.replace(QLatin1Char('\n'), newlineString(format.newline));

    // Byte order marks only exist for Unicode encodings
    QStringConverter::Flags flags = QStringConverter::Flag::Default;
    if (format.bom && format.encoding.startsWith("UTF"))
        flags |= QStringConverter::Flag::WriteBom;
    QStringEncoder encoder(format.encoding.constData(), flags);
    if (!encoder.isValid())
        return std::nullopt;
    QByteArray bytes = encoder.encode(output);
    if (encoder.hasError())
        return std::nullopt;
    return bytes;
}

} // namespace meld
