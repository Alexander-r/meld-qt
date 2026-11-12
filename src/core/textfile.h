// Replaces GtkSourceFileLoader/GtkSourceFileSaver: candidate encoding
// detection, byte order marks and newline type handling.

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace meld {

enum class NewlineType {
    LF,
    CRLF,
    CR,
};

QString newlineString(NewlineType type);
QString newlineDisplayName(NewlineType type);

struct TextFileFormat {
    //! Canonical encoding name as understood by QStringConverter
    QByteArray encoding = QByteArrayLiteral("UTF-8");
    bool bom = false;
    NewlineType newline =
#ifdef Q_OS_WIN
        NewlineType::CRLF;
#else
        NewlineType::LF;
#endif

    friend bool operator==(const TextFileFormat &, const TextFileFormat &) = default;
};

struct DecodedText {
    //! Text with '\n' line separators
    QString text;
    TextFileFormat format;
};

//! Encoding names to try by default, after any user-configured ones
QList<QByteArray> defaultCandidateEncodings();
//! Encodings offered by the encoding selector
QList<QByteArray> selectableEncodings();
QString encodingDisplayName(const QByteArray &encoding);

/*!
 * Decode file contents, trying each candidate encoding in turn. A byte
 * order mark always takes priority. Returns nullopt if no candidate could
 * decode the data without errors.
 */
std::optional<DecodedText> decodeText(const QByteArray &data, const QList<QByteArray> &candidates);

//! Detect the newline type from the first line ending in `text`
std::optional<NewlineType> detectNewline(QStringView text);

//! Normalise all line endings in `text` to '\n'
QString normalizeNewlines(const QString &text);

/*!
 * Encode text (with '\n' separators) for saving. Returns nullopt if the
 * text contains characters that can't be represented in the encoding.
 */
std::optional<QByteArray> encodeText(const QString &text, const TextFileFormat &format);

} // namespace meld
