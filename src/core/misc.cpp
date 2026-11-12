#include "misc.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <deque>

namespace meld {

namespace {

//! Minimal equivalent of Python's pathlib.PurePath for name shortening
struct PurePath {
    QString anchor;
    QStringList components;
    bool windows = false;

    static PurePath parse(const QString &path, bool windows)
    {
        PurePath result;
        result.windows = windows;
        QString rest = path;
        if (windows) {
            rest.replace(QLatin1Char('/'), QLatin1Char('\\'));
            QString drive;
            if (rest.size() >= 2 && rest.at(1) == QLatin1Char(':') && rest.at(0).isLetter()) {
                drive = rest.left(2);
                rest = rest.mid(2);
            }
            QString root;
            if (rest.startsWith(QLatin1Char('\\'))) {
                root = QStringLiteral("\\");
                while (rest.startsWith(QLatin1Char('\\')))
                    rest.remove(0, 1);
            }
            result.anchor = drive + root;
            for (const QString &part : rest.split(QLatin1Char('\\'), Qt::SkipEmptyParts)) {
                if (part != QLatin1String("."))
                    result.components.append(part);
            }
        } else {
            if (rest.startsWith(QLatin1Char('/'))) {
                // POSIX allows exactly two leading slashes to be significant
                const bool doubleSlash = rest.startsWith(QLatin1String("//"))
                    && !rest.startsWith(QLatin1String("///"));
                result.anchor = doubleSlash ? QStringLiteral("//") : QStringLiteral("/");
            }
            for (const QString &part : rest.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                if (part != QLatin1String("."))
                    result.components.append(part);
            }
        }
        return result;
    }

    int partCount() const { return int(components.size()) + (anchor.isEmpty() ? 0 : 1); }

    QString key() const
    {
        const QString sep = windows ? QStringLiteral("\\") : QStringLiteral("/");
        QString text = anchor + QLatin1Char('\x01') + components.join(sep);
        return windows ? text.toCaseFolded() : text;
    }

    QString name() const { return components.isEmpty() ? QString() : components.last(); }

    std::vector<PurePath> parents() const
    {
        std::vector<PurePath> result;
        for (int k = int(components.size()) - 1; k >= 0; --k) {
            PurePath parent;
            parent.windows = windows;
            parent.anchor = anchor;
            parent.components = components.mid(0, k);
            result.push_back(parent);
        }
        return result;
    }

    PurePath relativeTo(const PurePath &parent) const
    {
        PurePath result;
        result.windows = windows;
        result.components = components.mid(parent.components.size());
        return result;
    }
};

} // namespace

QStringList shortenNames(const QStringList &names, PathStyle style)
{
    if (names.isEmpty())
        return {};

#ifdef Q_OS_WIN
    const bool windows = style != PathStyle::Posix;
#else
    const bool windows = style == PathStyle::Windows;
#endif

    std::vector<PurePath> paths;
    for (const QString &name : names)
        paths.push_back(PurePath::parse(name, windows));

    // Identify the longest common path among the list of paths
    QHash<QString, PurePath> common;
    for (const PurePath &parent : paths[0].parents())
        common.insert(parent.key(), parent);
    for (const PurePath &path : paths) {
        QSet<QString> keys;
        for (const PurePath &parent : path.parents())
            keys.insert(parent.key());
        for (auto it = common.begin(); it != common.end();) {
            if (!keys.contains(it.key()))
                it = common.erase(it);
            else
                ++it;
        }
    }
    if (common.isEmpty())
        return names;

    PurePath commonParent = common.begin().value();
    for (const PurePath &candidate : std::as_const(common)) {
        if (candidate.partCount() > commonParent.partCount())
            commonParent = candidate;
    }

    std::vector<PurePath> relative;
    QStringList basenames;
    for (const PurePath &path : paths) {
        relative.push_back(path.relativeTo(commonParent));
        basenames.append(relative.back().name());
    }

    if (allSame(basenames)) {
        QStringList result;
        for (const PurePath &path : relative) {
            QString prefix;
            if (path.components.size() > 1 && !path.components.first().isEmpty())
                prefix = QStringLiteral("[%1] ").arg(path.components.first());
            result.append(prefix + path.name());
        }
        return result;
    }

    QStringList result;
    for (const QString &name : std::as_const(basenames))
        result.append(name.isEmpty() ? QCoreApplication::translate("meld", "[None]") : name);
    return result;
}

std::vector<Interval> mergeIntervals(std::vector<Interval> intervals)
{
    if (intervals.size() < 2)
        return intervals;

    std::sort(intervals.begin(), intervals.end());
    std::deque<Interval> pending(intervals.begin(), intervals.end());
    std::vector<Interval> merged{pending.front()};
    pending.pop_front();
    auto [currentStart, currentEnd] = merged.back();

    while (!pending.empty()) {
        const auto [newStart, newEnd] = pending.front();
        pending.pop_front();

        if (currentEnd >= newEnd)
            continue;

        if (currentEnd < newStart) {
            // Intervals do not overlap; create a new one
            merged.emplace_back(newStart, newEnd);
        } else if (currentEnd < newEnd) {
            // Intervals overlap; extend the current one
            merged.back() = {currentStart, newEnd};
        }

        std::tie(currentStart, currentEnd) = merged.back();
    }

    return merged;
}

QString applyTextFilters(const QString &text, const QList<QRegularExpression> &regexes,
                         const std::function<void(int, int)> &applyFn, bool *ok)
{
    if (ok)
        *ok = true;

    std::vector<Interval> filterRanges;
    for (const QRegularExpression &regex : regexes) {
        if (!regex.isValid() || regex.pattern().isEmpty())
            continue;

        const int groups = regex.captureCount();
        auto it = regex.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            // If there are no groups in the match, use the whole match
            if (groups == 0) {
                const int start = int(match.capturedStart(0));
                const int end = int(match.capturedEnd(0));
                if (start != end)
                    filterRanges.emplace_back(start, end);
                continue;
            }

            // If there are groups in the regex, include all groups that
            // participated in the match
            for (int i = 1; i <= groups; ++i) {
                const int start = int(match.capturedStart(i));
                const int end = int(match.capturedEnd(i));
                if (start != -1 && start != end)
                    filterRanges.emplace_back(start, end);
            }
        }
    }

    filterRanges = mergeIntervals(std::move(filterRanges));

    if (applyFn) {
        for (auto it = filterRanges.rbegin(); it != filterRanges.rend(); ++it)
            applyFn(it->first, it->second);
    }

    QString result;
    result.reserve(text.size());
    int offset = 0;
    for (const auto &[start, end] : filterRanges) {
        if (QStringView(text).mid(start, end - start).contains(QLatin1Char('\n'))) {
            if (ok)
                *ok = false;
            return text;
        }
        result.append(QStringView(text).mid(offset, start - offset));
        offset = end;
    }
    result.append(QStringView(text).mid(offset));
    return result;
}

QByteArray applyTextFilters(const QByteArray &text, const QList<QRegularExpression> &regexes, bool *ok)
{
    const QString decoded = QString::fromLatin1(text);
    return applyTextFilters(decoded, regexes, {}, ok).toLatin1();
}

Adjustment Adjustment::configure(double value, double lower, double upper, double pageSize)
{
    Adjustment adj{value, lower, upper, pageSize};
    const double maxValue = std::max(lower, upper - pageSize);
    adj.value = std::clamp(value, lower, maxValue);
    return adj;
}

double calcSyncpoint(const Adjustment &adj)
{
    const double current = adj.value;
    const double halfAScreen = adj.pageSize / 2;

    double syncpoint = 0.0;
    if (halfAScreen == 0)
        return syncpoint;

    // How far through the first half-screen our adjustment is
    const double topVal = adj.lower;
    const double firstScale = (current - topVal) / halfAScreen;
    syncpoint += 0.5 * std::min(1.0, firstScale);
    // How far through the last half-screen our adjustment is
    const double bottomVal = adj.upper - 1.5 * adj.pageSize;
    const double lastScale = (current - bottomVal) / halfAScreen;
    syncpoint += 0.5 * std::max(0.0, lastScale);
    return syncpoint;
}

// ---------------------------------------------------------------------------
// textwrap.fill()

namespace {

bool isWrapWhitespace(QChar c)
{
    return c == QLatin1Char('\t') || c == QLatin1Char('\n') || c == QChar(0x0b) || c == QChar(0x0c)
        || c == QLatin1Char('\r') || c == QLatin1Char(' ');
}

//! Expand tabs and convert all other whitespace characters to spaces
QString mungeWhitespace(const QString &text)
{
    QString result;
    result.reserve(text.size());
    qsizetype column = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char('\t')) {
            const qsizetype spaces = 8 - column % 8;
            result.append(QString(spaces, QLatin1Char(' ')));
            column += spaces;
        } else {
            result.append(isWrapWhitespace(c) ? QLatin1Char(' ') : c);
            column = (c == QLatin1Char('\n') || c == QLatin1Char('\r')) ? 0 : column + 1;
        }
    }
    return result;
}

//! Split text into indivisible chunks: words, possibly broken after
//! hyphens, and the whitespace between them
QStringList splitWrapChunks(const QString &text)
{
    // TextWrapper.wordsep_re
    static const QRegularExpression wordsepRe(
        QStringLiteral(R"(([\t\n\x0b\x0c\r ]+)"                 // any whitespace
                       R"(|(?<=[\w!"'&.,?])-{2,}(?=\w))"        // em-dash between words
                       R"(|[^\t\n\x0b\x0c\r ]+?(?:)"            // word, possibly hyphenated
                       R"(-(?:(?<=[^\d\W]{2}-)|(?<=[^\d\W]-[^\d\W]-))(?=[^\d\W]-?[^\d\W]))" // hyphenated word
                       R"(|(?=[\t\n\x0b\x0c\r ]|\z))"           // end of word
                       R"(|(?<=[\w!"'&.,?])(?=-{2,}\w))"        // em-dash
                       R"()))"),
        QRegularExpression::UseUnicodePropertiesOption);
    QStringList chunks;
    qsizetype last = 0;
    for (auto it = wordsepRe.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.capturedStart() > last)
            chunks.append(text.mid(last, match.capturedStart() - last));
        if (match.capturedLength() > 0)
            chunks.append(match.captured(1));
        last = match.capturedEnd();
    }
    if (last < text.size())
        chunks.append(text.mid(last));
    return chunks;
}

bool isBlank(const QString &chunk)
{
    return chunk.trimmed().isEmpty();
}

} // namespace

QString textWrapFill(const QString &text, int width)
{
    if (width <= 0)
        return text;

    // Arrange in reverse order so items can be efficiently popped from a
    // stack of chunks.
    QStringList chunks = splitWrapChunks(mungeWhitespace(text));
    std::reverse(chunks.begin(), chunks.end());

    QStringList lines;
    while (!chunks.isEmpty()) {
        QStringList line;
        qsizetype length = 0;

        // First chunk on line is whitespace -- drop it, unless this is the
        // very beginning of the text (ie. no lines started yet).
        if (isBlank(chunks.last()) && !lines.isEmpty())
            chunks.removeLast();

        while (!chunks.isEmpty() && length + chunks.last().size() <= width) {
            length += chunks.last().size();
            line.append(chunks.takeLast());
        }

        // The current line is full, and the next chunk is too big to fit on
        // *any* line (not just this one).
        if (!chunks.isEmpty() && chunks.last().size() > width) {
            const qsizetype spaceLeft = width - length;
            if (spaceLeft > 0) {
                // Put as much of the next chunk onto the current line as
                // will fit, breaking after the last hyphen if there are
                // non-hyphens before it
                QString &chunk = chunks.last();
                qsizetype end = spaceLeft;
                const qsizetype hyphen = chunk.left(spaceLeft).lastIndexOf(QLatin1Char('-'));
                if (hyphen > 0) {
                    const QStringView before = QStringView(chunk).left(hyphen);
                    if (std::any_of(before.begin(), before.end(), [](QChar c) { return c != QLatin1Char('-'); }))
                        end = hyphen + 1;
                }
                line.append(chunk.left(end));
                chunk.remove(0, end);
            } else if (line.isEmpty()) {
                line.append(chunks.takeLast());
            }
        }

        // If the last chunk on this line is all whitespace, drop it.
        if (!line.isEmpty() && isBlank(line.last()))
            line.removeLast();

        if (!line.isEmpty())
            lines.append(line.join(QString()));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace meld
