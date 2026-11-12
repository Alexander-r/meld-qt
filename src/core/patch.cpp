// Reading patches and applying them in memory

#include "patch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <tuple>

namespace meld::patch {

namespace {

//! As GNU patch, which by default ignores up to two lines of context
constexpr int MaxFuzz = 2;

//! tr() for the code here that isn't in a QObject class
struct Tr {
    Q_DECLARE_TR_FUNCTIONS(meld::patch)
};

//! Lines, each with its line ending if it has one
std::vector<QByteArray> splitLines(const QByteArray &data)
{
    std::vector<QByteArray> lines;
    qsizetype start = 0;
    while (start < data.size()) {
        qsizetype end = data.indexOf('\n', start);
        end = end < 0 ? data.size() : end + 1;
        lines.push_back(data.mid(start, end - start));
        start = end;
    }
    return lines;
}

QByteArray withoutEnding(const QByteArray &line)
{
    qsizetype size = line.size();
    if (size > 0 && line.at(size - 1) == '\n')
        --size;
    if (size > 0 && line.at(size - 1) == '\r')
        --size;
    return line.left(size);
}

QByteArray ending(const QByteArray &line)
{
    return line.mid(withoutEnding(line).size());
}

//! Git quotes paths with unusual characters as C strings
QString unquote(const QByteArray &quoted)
{
    QByteArray bytes;
    for (qsizetype i = 1; i < quoted.size(); ++i) {
        const char c = quoted.at(i);
        if (c == '"')
            break;
        if (c != '\\' || i + 1 >= quoted.size()) {
            bytes.append(c);
            continue;
        }
        const char next = quoted.at(++i);
        if (next >= '0' && next <= '7') {
            int value = 0;
            int digits = 0;
            while (digits < 3 && i < quoted.size() && quoted.at(i) >= '0' && quoted.at(i) <= '7') {
                value = value * 8 + (quoted.at(i) - '0');
                ++i;
                ++digits;
            }
            --i;
            bytes.append(char(value));
            continue;
        }
        switch (next) {
        case 'n':
            bytes.append('\n');
            break;
        case 't':
            bytes.append('\t');
            break;
        case 'r':
            bytes.append('\r');
            break;
        case 'a':
            bytes.append('\a');
            break;
        case 'b':
            bytes.append('\b');
            break;
        case 'f':
            bytes.append('\f');
            break;
        case 'v':
            bytes.append('\v');
            break;
        default:
            bytes.append(next);
        }
    }
    return QString::fromUtf8(bytes);
}

//! A path in a ---/+++ line, without the timestamp after a tab; empty for
//! /dev/null
QString headerPath(const QByteArray &field)
{
    QByteArray value = withoutEnding(field);
    if (value.startsWith('"'))
        return unquote(value);
    if (const qsizetype tab = value.indexOf('\t'); tab >= 0)
        value.truncate(tab);
    value = value.trimmed();
    if (value == "/dev/null")
        return {};
    return QString::fromUtf8(value);
}

//! The paths of a "diff --git a/x b/x" line, for changes without ---/+++
//! lines (e.g., renames and binary files)
std::pair<QString, QString> gitHeaderPaths(const QByteArray &rest)
{
    const QByteArray value = withoutEnding(rest);
    if (value.startsWith('"')) {
        // Both quoted, or the first only
        qsizetype end = 1;
        while (end < value.size() && !(value.at(end) == '"' && value.at(end - 1) != '\\'))
            ++end;
        const QString oldPath = unquote(value.left(end + 1));
        const QByteArray second = value.mid(end + 1).trimmed();
        return {oldPath, second.startsWith('"') ? unquote(second) : QString::fromUtf8(second)};
    }
    // Without quotes, the paths are the same apart from their prefixes,
    // unless the file is renamed
    if ((value.size() - 1) % 2 == 0) {
        const qsizetype half = (value.size() - 1) / 2;
        const QByteArray first = value.left(half);
        const QByteArray second = value.mid(half + 1);
        const auto withoutPrefix = [](const QByteArray &p) {
            const qsizetype slash = p.indexOf('/');
            return slash < 0 ? p : p.mid(slash + 1);
        };
        if (value.at(half) == ' ' && withoutPrefix(first) == withoutPrefix(second))
            return {QString::fromUtf8(first), QString::fromUtf8(second)};
    }
    const qsizetype split = value.lastIndexOf(" b/");
    if (split > 0)
        return {QString::fromUtf8(value.left(split)), QString::fromUtf8(value.mid(split + 1))};
    return {};
}

QString gitPrefix(const QString &path)
{
    if (path.startsWith(QLatin1String("a/")) || path.startsWith(QLatin1String("b/")))
        return path.left(2);
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Reading

std::optional<Patch> parse(const QByteArray &data, QString *error)
{
    static const QRegularExpression hunkHeader(QStringLiteral(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@)"));

    const std::vector<QByteArray> lines = splitLines(data);
    const auto fail = [error](int line, const QString &message) -> std::optional<Patch> {
        if (error)
            *error = Tr::tr("Line %1 of the patch: %2").arg(line).arg(message);
        return std::nullopt;
    };

    Patch patch;
    // The git diff whose extended headers are being read
    bool inGitHeader = false;
    QString oldPrefix;
    QString newPrefix;
    // Whether the current file's ---/+++ lines were read, so hunks follow
    bool haveFile = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const QByteArray &line = lines[i];
        const QByteArray content = withoutEnding(line);
        const int lineNumber = int(i) + 1;

        if (content.startsWith("diff --git ")) {
            FilePatch file;
            file.line = lineNumber;
            std::tie(file.oldPath, file.newPath) = gitHeaderPaths(content.mid(11));
            oldPrefix = gitPrefix(file.oldPath);
            newPrefix = gitPrefix(file.newPath);
            patch.files.push_back(file);
            inGitHeader = true;
            haveFile = false;
            continue;
        }

        if (content.startsWith("--- ") && i + 1 < lines.size() && lines[i + 1].startsWith("+++ ")) {
            const bool continuesGitHeader = inGitHeader && !haveFile;
            if (!continuesGitHeader) {
                FilePatch file;
                file.line = lineNumber;
                patch.files.push_back(file);
            }
            FilePatch &file = patch.files.back();
            file.oldPath = headerPath(content.mid(4));
            file.newPath = headerPath(lines[i + 1].mid(4));
            if (file.oldPath.isEmpty())
                file.newFile = true;
            if (file.newPath.isEmpty())
                file.deletedFile = true;
            inGitHeader = continuesGitHeader;
            haveFile = true;
            ++i;
            continue;
        }

        if (inGitHeader && !haveFile) {
            FilePatch &file = patch.files.back();
            const auto field = [&content](const char *name) -> std::optional<QString> {
                const QByteArray prefix(name);
                if (!content.startsWith(prefix))
                    return std::nullopt;
                const QByteArray value = content.mid(prefix.size());
                return value.startsWith('"') ? unquote(value) : QString::fromUtf8(value);
            };
            if (content.startsWith("new file mode")) {
                file.newFile = true;
                file.oldPath.clear();
            } else if (content.startsWith("deleted file mode")) {
                file.deletedFile = true;
                file.newPath.clear();
            } else if (const auto from = field("rename from ")) {
                file.rename = true;
                file.oldPath = oldPrefix + *from;
            } else if (const auto to = field("rename to ")) {
                file.rename = true;
                file.newPath = newPrefix + *to;
            } else if (const auto from = field("copy from ")) {
                file.copy = true;
                file.oldPath = oldPrefix + *from;
            } else if (const auto to = field("copy to ")) {
                file.copy = true;
                file.newPath = newPrefix + *to;
            } else if (content.startsWith("Binary files ") || content == "GIT binary patch") {
                file.binary = true;
            } else if (!content.startsWith("index ") && !content.startsWith("old mode")
                       && !content.startsWith("new mode") && !content.startsWith("similarity index")
                       && !content.startsWith("dissimilarity index")) {
                inGitHeader = false;
            }
            continue;
        }

        if (content.startsWith("Binary files ") && content.endsWith(" differ")) {
            // From diff -r, without a git header
            continue;
        }

        if (!content.startsWith("@@ ") || !haveFile)
            continue;

        const QRegularExpressionMatch match = hunkHeader.match(QString::fromLatin1(content));
        if (!match.hasMatch())
            return fail(lineNumber, Tr::tr("malformed hunk header"));
        Hunk hunk;
        hunk.oldStart = match.captured(1).toInt();
        hunk.oldCount = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
        hunk.newStart = match.captured(3).toInt();
        hunk.newCount = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();

        int oldLeft = hunk.oldCount;
        int newLeft = hunk.newCount;
        size_t j = i + 1;
        const auto markNoNewline = [&hunk](const QByteArray &marker) {
            if (hunk.lines.empty())
                return;
            QByteArray &text = hunk.lines.back().text;
            if (text.endsWith('\n'))
                text.chop(1);
            // Patches with CRLF line endings end their lines with CRLF
            if (marker.endsWith("\r\n") && text.endsWith('\r'))
                text.chop(1);
        };
        while ((oldLeft > 0 || newLeft > 0) && j < lines.size()) {
            const QByteArray &hunkLine = lines[j];
            HunkLine entry;
            if (hunkLine == "\n" || hunkLine == "\r\n") {
                // A context line whose space was removed with trailing
                // whitespace
                entry.kind = ' ';
                entry.text = hunkLine;
            } else if (hunkLine.startsWith('\\')) {
                markNoNewline(hunkLine);
                ++j;
                continue;
            } else if (hunkLine.startsWith(' ') || hunkLine.startsWith('-') || hunkLine.startsWith('+')) {
                entry.kind = hunkLine.at(0);
                entry.text = hunkLine.mid(1);
            } else {
                return fail(int(j) + 1, Tr::tr("unexpected line in a hunk"));
            }
            if (entry.kind != '+')
                --oldLeft;
            if (entry.kind != '-')
                --newLeft;
            if (oldLeft < 0 || newLeft < 0)
                return fail(int(j) + 1, Tr::tr("the hunk has more lines than its header says"));
            hunk.lines.push_back(entry);
            ++j;
        }
        if (oldLeft > 0 || newLeft > 0)
            return fail(lineNumber, Tr::tr("the patch ends in the middle of a hunk"));
        if (j < lines.size() && lines[j].startsWith('\\')) {
            markNoNewline(lines[j]);
            ++j;
        }
        patch.files.back().hunks.push_back(std::move(hunk));
        i = j - 1;
    }

    // Patches made on Windows may separate their paths' folders with
    // backslashes. They're only taken as separators if no path has a '/',
    // as on other systems a backslash can be part of a name.
    bool anySlash = false;
    bool anyBackslash = false;
    for (const FilePatch &file : patch.files) {
        for (const QString &path : {file.oldPath, file.newPath}) {
            anySlash = anySlash || path.contains(QLatin1Char('/'));
            anyBackslash = anyBackslash || path.contains(QLatin1Char('\\'));
        }
    }
    if (anyBackslash && !anySlash) {
        for (FilePatch &file : patch.files) {
            file.oldPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
            file.newPath.replace(QLatin1Char('\\'), QLatin1Char('/'));
        }
    }

    // Git headers without changes (e.g., mode changes) change nothing here
    patch.files.erase(std::remove_if(patch.files.begin(), patch.files.end(),
                                     [](const FilePatch &f) {
                                         return f.hunks.empty() && !f.binary && !f.rename && !f.copy && !f.newFile
                                             && !f.deletedFile;
                                     }),
                      patch.files.end());
    if (patch.files.empty()) {
        if (error) {
            *error = data.contains("\n***************") ? Tr::tr("Only unified diffs can be applied; this is a context diff.")
                                                         : Tr::tr("No changes were found in the patch.");
        }
        return std::nullopt;
    }
    return patch;
}

std::optional<Patch> parseFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return std::nullopt;
    }
    return parse(file.readAll(), error);
}

// ---------------------------------------------------------------------------
// Applying

namespace {

struct HunkMatch {
    qsizetype position = 0;
    int pre = 0;
    int post = 0;
    bool loose = false;
};

/*!
 * Apply the hunks to lines. Returns the failed hunks' messages; `notes`
 * gets how the others were applied, if not as the patch says.
 */
QStringList applyHunks(const std::vector<QByteArray> &original, const std::vector<Hunk> &hunks, bool reverse,
                       QByteArray *output, QStringList *notes)
{
    QStringList failures;
    // The line ending of new lines, where the file's differ from the patch's
    QByteArray fileEnding;
    for (const QByteArray &line : original) {
        fileEnding = ending(line);
        if (!fileEnding.isEmpty())
            break;
    }

    qsizetype done = 0;
    qsizetype offset = 0;
    for (size_t h = 0; h < hunks.size(); ++h) {
        const Hunk &hunk = hunks[h];
        const char removed = reverse ? '+' : '-';
        const char added = reverse ? '-' : '+';
        const int oldStart = reverse ? hunk.newStart : hunk.oldStart;
        const int oldCount = reverse ? hunk.newCount : hunk.oldCount;

        std::vector<const HunkLine *> lines;
        for (const HunkLine &line : hunk.lines)
            lines.push_back(&line);
        std::vector<const QByteArray *> old;
        for (const HunkLine *line : lines) {
            if (line->kind != added)
                old.push_back(&line->text);
        }
        int leading = 0;
        while (leading < int(lines.size()) && lines[size_t(leading)]->kind == ' ')
            ++leading;
        int trailing = 0;
        while (trailing < int(lines.size()) - leading && lines[lines.size() - 1 - size_t(trailing)]->kind == ' ')
            ++trailing;

        // Where the hunk should be: after line oldStart for insertions
        const qsizetype expected = (oldCount == 0 ? oldStart : oldStart - 1) + offset;
        const qsizetype fileLines = qsizetype(original.size());
        // As GNU patch: fuzz ignores context lines from the end with more of
        // them, then both. A hunk with less context at one end than the other
        // is at the start or end of the file, and must still be there.
        const int context = std::max(leading, trailing);
        std::optional<HunkMatch> found;
        for (int fuzz = 0; fuzz <= std::min(MaxFuzz, context) && !found; ++fuzz) {
            const int prefixFuzz = fuzz + leading - context;
            const int suffixFuzz = fuzz + trailing - context;
            const int pre = std::max(0, prefixFuzz);
            const int post = std::max(0, suffixFuzz);
            const bool atStart = prefixFuzz < 0 && oldStart <= 1;
            const bool atEnd = suffixFuzz < 0;
            const qsizetype size = qsizetype(old.size()) - pre - post;
            const qsizetype last = fileLines - size;
            for (const bool loose : {false, true}) {
                const auto matchesAt = [&](qsizetype position) {
                    if (position < done || position > last)
                        return false;
                    for (qsizetype k = 0; k < size; ++k) {
                        const QByteArray &a = original[size_t(position + k)];
                        const QByteArray &b = *old[size_t(pre + k)];
                        if (loose ? withoutEnding(a) != withoutEnding(b) : a != b)
                            return false;
                    }
                    return true;
                };
                if (atStart || atEnd) {
                    const qsizetype position = atStart ? 0 : last;
                    if ((!atStart || !atEnd || size == fileLines) && matchesAt(position))
                        found = HunkMatch{position, pre, post, loose};
                } else {
                    const qsizetype start = std::clamp<qsizetype>(expected + pre, done, std::max(done, last));
                    for (qsizetype distance = 0; !found; ++distance) {
                        const qsizetype later = start + distance;
                        const qsizetype earlier = start - distance;
                        if (later > last && earlier < done)
                            break;
                        if (matchesAt(later))
                            found = HunkMatch{later, pre, post, loose};
                        else if (distance > 0 && matchesAt(earlier))
                            found = HunkMatch{earlier, pre, post, loose};
                    }
                }
                if (found)
                    break;
            }
        }

        if (!found) {
            failures.append(Tr::tr("Hunk #%1 doesn’t match the file at line %2.").arg(h + 1).arg(expected + 1));
            continue;
        }

        // Copy up to the hunk, then the hunk without its ignored context
        for (qsizetype k = done; k < found->position; ++k)
            output->append(original[size_t(k)]);
        qsizetype cursor = found->position;
        for (size_t k = size_t(found->pre); k < lines.size() - size_t(found->post); ++k) {
            const HunkLine *line = lines[k];
            if (line->kind == ' ') {
                output->append(original[size_t(cursor++)]);
            } else if (line->kind == removed) {
                ++cursor;
            } else {
                QByteArray text = line->text;
                if (found->loose && !fileEnding.isEmpty() && !ending(text).isEmpty())
                    text = withoutEnding(text) + fileEnding;
                output->append(text);
            }
        }
        done = cursor;

        const qsizetype shift = found->position - (expected + found->pre);
        offset += shift;
        if (notes && (offset != 0 || found->pre > 0 || found->post > 0)) {
            QString note = Tr::tr("Hunk #%1 applied at line %2").arg(h + 1).arg(found->position - found->pre + 1);
            if (offset != 0)
                note += Tr::tr(" (offset %n lines)", nullptr, offset);
            if (found->pre > 0 || found->post > 0)
                note += Tr::tr(" with fuzz %1").arg(std::max(found->pre, found->post));
            notes->append(note + QLatin1Char('.'));
        }
    }
    for (size_t k = size_t(done); k < original.size(); ++k)
        output->append(original[k]);
    return failures;
}

} // namespace

ApplyResult apply(const QByteArray &original, const FilePatch &patch)
{
    ApplyResult result;
    if (patch.binary) {
        result.error = Tr::tr("Binary changes can’t be applied.");
        return result;
    }
    if (patch.newFile && !original.isEmpty() && !patch.hunks.empty()) {
        // Unless it's already the new file
        QByteArray created;
        for (const Hunk &hunk : patch.hunks) {
            for (const HunkLine &line : hunk.lines) {
                if (line.kind != '-')
                    created.append(line.text);
            }
        }
        result.error = created == original ? Tr::tr("The patch creates this file, which is already as it would create it.")
                                           : Tr::tr("The patch creates this file, but it already exists.");
        return result;
    }

    const std::vector<QByteArray> lines = splitLines(original);
    QByteArray output;
    QStringList notes;
    const QStringList failures = applyHunks(lines, patch.hunks, false, &output, &notes);
    if (!failures.isEmpty()) {
        // Patches that were applied already apply in reverse
        QByteArray reversed;
        if (applyHunks(lines, patch.hunks, true, &reversed, nullptr).isEmpty() && !patch.hunks.empty()) {
            result.error = Tr::tr("The patch seems to be applied already: its changes are in the file.");
        } else {
            result.error = Tr::tr("%1 of %n hunks can’t be applied.", nullptr, int(patch.hunks.size())).arg(failures.size())
                + QLatin1Char('\n') + failures.join(QLatin1Char('\n'));
        }
        return result;
    }
    if (patch.deletedFile) {
        if (!output.isEmpty()) {
            result.error = Tr::tr("The patch deletes this file, but it has other contents than the patch removes.");
            return result;
        }
        result.deleted = true;
    }
    result.text = output;
    result.notes = notes;
    return result;
}

// ---------------------------------------------------------------------------
// Paths

std::optional<QString> stripPath(const QString &path, int level)
{
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= level)
        return std::nullopt;
    parts = parts.mid(level);
    parts.removeAll(QStringLiteral("."));
    if (parts.isEmpty())
        return std::nullopt;
    return parts.join(QLatin1Char('/'));
}

namespace {

//! Whether a patch's path stays in the folder it's applied to
bool isSafe(const QString &path)
{
    return !path.isEmpty() && !QDir::isAbsolutePath(path) && !path.split(QLatin1Char('/')).contains(QStringLiteral(".."));
}

int componentCount(const QString &path)
{
    return int(path.split(QLatin1Char('/'), Qt::SkipEmptyParts).size());
}

} // namespace

const FilePatch *findFilePatch(const Patch &patch, const QString &file, QString *error)
{
    if (patch.files.size() == 1)
        return &patch.files.front();

    // The file whose path has the most components in common with the end of
    // the given file's path
    const QStringList fileParts = QDir::fromNativeSeparators(QDir::cleanPath(file)).split(QLatin1Char('/'));
    const FilePatch *best = nullptr;
    int bestScore = 0;
    bool ambiguous = false;
    for (const FilePatch &candidate : patch.files) {
        int score = 0;
        for (const QString &path : {candidate.sourcePath(), candidate.targetPath()}) {
            const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            int common = 0;
            while (common < parts.size() && common < fileParts.size()
                   && parts.at(parts.size() - 1 - common) == fileParts.at(fileParts.size() - 1 - common)) {
                ++common;
            }
            score = std::max(score, common);
        }
        if (score > bestScore) {
            best = &candidate;
            bestScore = score;
            ambiguous = false;
        } else if (score == bestScore && score > 0) {
            ambiguous = true;
        }
    }
    if (!best || ambiguous) {
        QStringList names;
        for (const FilePatch &candidate : patch.files)
            names.append(candidate.targetPath());
        if (error) {
            *error = (best ? Tr::tr("The patch changes several files called %1:") : Tr::tr("The patch doesn’t change %1. It changes:"))
                         .arg(QFileInfo(file).fileName())
                + QLatin1Char('\n') + names.join(QLatin1Char('\n'));
        }
        return nullptr;
    }
    return best;
}

// ---------------------------------------------------------------------------
// PatchedFolder

std::shared_ptr<PatchedFolder> PatchedFolder::load(const QString &folder, const QString &patchFile, QString *error)
{
    std::optional<Patch> patch = parseFile(patchFile, error);
    if (!patch)
        return nullptr;

    auto result = std::shared_ptr<PatchedFolder>(new PatchedFolder);
    result->folder_ = QDir::cleanPath(QFileInfo(folder).absoluteFilePath());
    result->patchFile_ = QDir::cleanPath(QFileInfo(patchFile).absoluteFilePath());
    result->patch_ = std::move(*patch);
    const QDir base(result->folder_);

    // Strip the path components that find the most files to change (e.g.,
    // git's a/ and b/)
    int maxLevel = 0;
    for (const FilePatch &file : result->patch_.files)
        maxLevel = std::max(maxLevel, componentCount(file.sourcePath()) - 1);
    int bestLevel = -1;
    int bestCount = 0;
    for (int level = 0; level <= maxLevel; ++level) {
        int count = 0;
        for (const FilePatch &file : result->patch_.files) {
            if (file.newFile)
                continue;
            const std::optional<QString> path = stripPath(file.sourcePath(), level);
            if (path && isSafe(*path) && QFileInfo(base.filePath(*path)).isFile())
                ++count;
        }
        if (count > bestCount) {
            bestCount = count;
            bestLevel = level;
        }
    }
    if (bestLevel < 0) {
        // Nothing to change was found (e.g., only new files): git's
        // prefixes are still stripped
        bool prefixed = true;
        for (const FilePatch &file : result->patch_.files) {
            prefixed = prefixed && (file.oldPath.isEmpty() || gitPrefix(file.oldPath) == QLatin1String("a/"))
                && (file.newPath.isEmpty() || gitPrefix(file.newPath) == QLatin1String("b/"));
        }
        bestLevel = prefixed ? 1 : 0;
    }
    result->strip_ = bestLevel;

    const auto addError = [&result](const QString &path, const QString &message) {
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        const QString parent = slash < 0 ? QString() : path.left(slash);
        result->errors_[parent].append({path.mid(slash + 1), message});
    };
    const auto exists = [&base](const QString &path) { return QFileInfo::exists(base.filePath(path)); };

    for (const FilePatch &file : result->patch_.files) {
        const std::optional<QString> oldPath =
            file.oldPath.isEmpty() ? std::nullopt : stripPath(file.oldPath, bestLevel);
        const std::optional<QString> newPath =
            file.newPath.isEmpty() ? std::nullopt : stripPath(file.newPath, bestLevel);
        const QString name = (newPath ? *newPath : oldPath.value_or(file.targetPath()));
        if ((!oldPath && !file.oldPath.isEmpty()) || (!newPath && !file.newPath.isEmpty())
            || (oldPath && !isSafe(*oldPath)) || (newPath && !isSafe(*newPath))) {
            addError(QFileInfo(file.targetPath()).fileName(),
                     Tr::tr("The patch changes %1, which isn’t in the folder.").arg(file.targetPath()));
            continue;
        }
        if (file.binary) {
            addError(name, Tr::tr("Binary changes can’t be applied."));
            continue;
        }

        // Further changes to the same file are applied after the earlier
        const auto existing = result->targets_.find(name);
        if (existing != result->targets_.end() && !file.newFile && existing->change != Change::Deleted) {
            existing->patches.push_back(&file);
            if (file.deletedFile)
                existing->change = Change::Deleted;
            continue;
        }

        Target target;
        target.patches.push_back(&file);
        if (file.newFile) {
            if (exists(name)) {
                addError(name, Tr::tr("The patch creates this file, but it already exists."));
                continue;
            }
            target.change = Change::Added;
        } else if (file.deletedFile) {
            if (!exists(*oldPath)) {
                addError(*oldPath, Tr::tr("The patch deletes this file, but it isn’t in the folder."));
                continue;
            }
            target.change = Change::Deleted;
            target.source = *oldPath;
        } else if (file.rename || file.copy) {
            if (!exists(*oldPath)) {
                addError(*newPath, Tr::tr("The patch makes this file from %1, which isn’t in the folder.").arg(*oldPath));
                continue;
            }
            if (exists(*newPath)) {
                addError(*newPath, Tr::tr("The patch makes this file from %1, but it already exists.").arg(*oldPath));
                continue;
            }
            target.change = Change::Added;
            target.source = *oldPath;
            if (file.rename) {
                Target removed;
                removed.change = Change::Deleted;
                removed.source = *oldPath;
                result->targets_.insert(*oldPath, removed);
            }
        } else {
            // As patch, the old file if it's there, else the new one
            const QString source = oldPath && exists(*oldPath) ? *oldPath : newPath.value_or(QString());
            if (source.isEmpty() || !exists(source)) {
                addError(name, Tr::tr("The patch changes this file, but it isn’t in the folder."));
                continue;
            }
            target.source = source;
            result->targets_.insert(source, target);
            continue;
        }
        result->targets_.insert(target.change == Change::Deleted ? target.source : name, target);
    }

    for (auto it = result->targets_.cbegin(); it != result->targets_.cend(); ++it) {
        if (it->change != Change::Added)
            continue;
        QString parent = it.key();
        while (true) {
            const qsizetype slash = parent.lastIndexOf(QLatin1Char('/'));
            if (slash < 0)
                break;
            parent.truncate(slash);
            if (!QFileInfo(base.filePath(parent)).isDir())
                result->addedFolders_.insert(parent);
        }
    }
    return result;
}

QString PatchedFolder::realPath(const QString &path) const
{
    return path.isEmpty() ? folder_ : QDir(folder_).filePath(path);
}

PatchedFolder::Change PatchedFolder::change(const QString &path) const
{
    const auto it = targets_.constFind(path);
    return it == targets_.cend() ? Change::None : it->change;
}

bool PatchedFolder::exists(const QString &path) const
{
    switch (change(path)) {
    case Change::Modified:
    case Change::Added:
        return true;
    case Change::Deleted:
        return false;
    case Change::None:
        break;
    }
    return path.isEmpty() || addedFolders_.contains(path) || QFileInfo::exists(realPath(path));
}

bool PatchedFolder::isDir(const QString &path) const
{
    if (change(path) != Change::None)
        return false;
    return path.isEmpty() || addedFolders_.contains(path) || QFileInfo(realPath(path)).isDir();
}

bool PatchedFolder::isFile(const QString &path) const
{
    return exists(path) && !isDir(path);
}

QStringList PatchedFolder::entries(const QString &folder) const
{
    QStringList names;
    const QFileInfo info(realPath(folder));
    if (info.isDir()) {
        names = QDir(info.filePath())
                    .entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Unsorted);
    }
    const QString prefix = folder.isEmpty() ? QString() : folder + QLatin1Char('/');
    const auto addChild = [&names, &prefix](const QString &path) {
        if (!path.startsWith(prefix))
            return;
        const QString name = path.mid(prefix.size()).section(QLatin1Char('/'), 0, 0);
        if (!name.isEmpty() && !names.contains(name))
            names.append(name);
    };
    for (auto it = targets_.cbegin(); it != targets_.cend(); ++it) {
        if (it->change == Change::Added)
            addChild(it.key());
    }
    for (const QString &added : addedFolders_)
        addChild(added);
    names.removeIf([this, &prefix](const QString &name) { return change(prefix + name) == Change::Deleted; });
    return names;
}

QList<std::pair<QString, QString>> PatchedFolder::errors(const QString &folder) const
{
    return errors_.value(folder);
}

QString PatchedFolder::source(const QString &path) const
{
    const auto it = targets_.constFind(path);
    if (it == targets_.cend())
        return path;
    return it->source;
}

ApplyResult PatchedFolder::content(const QString &path) const
{
    if (!path.isEmpty() && !isSafe(path)) {
        ApplyResult result;
        result.error = Tr::tr("The file isn’t in the folder.");
        return result;
    }
    const auto it = targets_.constFind(path);
    const QString source = it == targets_.cend() ? path : it->source;

    QByteArray text;
    if (!source.isEmpty()) {
        QFile file(realPath(source));
        if (!file.open(QIODevice::ReadOnly)) {
            ApplyResult result;
            result.error = file.errorString();
            return result;
        }
        text = file.readAll();
    }
    if (it == targets_.cend()) {
        ApplyResult result;
        result.text = text;
        return result;
    }
    if (it->patches.empty()) {
        // The source of a renamed file
        ApplyResult result;
        result.text = QByteArray();
        result.deleted = true;
        return result;
    }

    ApplyResult result;
    for (const FilePatch *patch : it->patches) {
        FilePatch applied = *patch;
        // A renamed or copied file's changes apply to its source
        applied.newFile = applied.newFile && source.isEmpty();
        ApplyResult step = apply(text, applied);
        if (!step.text)
            return step;
        result.notes += step.notes;
        result.deleted = step.deleted;
        text = *step.text;
    }
    result.text = text;
    return result;
}

} // namespace meld::patch
