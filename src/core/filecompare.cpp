#include "filecompare.h"

#include "misc.h"

#include <QFile>
#include <QHash>

#include <algorithm>

#include <chrono>
#include <filesystem>
#include <memory>
#include <new>
#include <vector>

namespace meld {

namespace {

// TODO: Get the block size from os.stat
constexpr qint64 ChunkSize = 4096;

std::filesystem::path toFsPath(const QString &path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).toStdString());
#endif
}

struct CacheResult {
    std::vector<StatItem> stats;
    FileCompareResult result;
};

QHash<QString, CacheResult> &comparisonCache()
{
    static QHash<QString, CacheResult> cache;
    return cache;
}

QString cacheKey(const QStringList &files, bool needContents, const QList<QRegularExpression> &regexes,
                 bool ignoreBlankLines)
{
    QString key = files.join(QChar(0));
    key += QChar(1);
    key += needContents ? u'1' : u'0';
    key += ignoreBlankLines ? u'1' : u'0';
    for (const QRegularExpression &regex : regexes) {
        key += QChar(2);
        key += regex.pattern();
    }
    return key;
}

std::vector<QByteArray> normalize(const std::vector<QByteArray> &contents, bool ignoreBlankLines,
                                  const QList<QRegularExpression> &regexes, bool *ok)
{
    std::vector<QByteArray> result;
    result.reserve(contents.size());
    for (const QByteArray &content : contents) {
        // For probable text files, discard newline differences to match
        QByteArray normalized;
        if (ignoreBlankLines) {
            normalized = removeBlankLines(content);
        } else {
            const QList<QByteArray> lines = splitLines(content);
            normalized = lines.join('\n');
        }

        if (!regexes.isEmpty()) {
            bool filterOk = true;
            normalized = applyTextFilters(normalized, regexes, &filterOk);
            if (!filterOk)
                *ok = false;
            if (ignoreBlankLines) {
                // We re-remove blank lines here in case applying text
                // filters has caused more lines to be blank.
                normalized = removeBlankLines(normalized);
            }
        }
        result.push_back(std::move(normalized));
    }
    return result;
}

} // namespace

std::optional<StatItem> StatItem::stat(const QString &path, bool followSymlinks)
{
    std::error_code ec;
    const std::filesystem::path fsPath = toFsPath(path);
    const std::filesystem::file_status status = followSymlinks
        ? std::filesystem::status(fsPath, ec)
        : std::filesystem::symlink_status(fsPath, ec);
    if (ec || status.type() == std::filesystem::file_type::not_found)
        return std::nullopt;

    StatItem item;
    switch (status.type()) {
    case std::filesystem::file_type::regular:
        item.kind = FileKind::Regular;
        break;
    case std::filesystem::file_type::directory:
        item.kind = FileKind::Directory;
        break;
    default:
        item.kind = FileKind::Other;
        break;
    }

    if (item.kind == FileKind::Regular) {
        const auto size = std::filesystem::file_size(fsPath, ec);
        item.size = ec ? 0 : std::int64_t(size);
    }
    ec.clear();
    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    if (!ec) {
        item.mtimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(mtime.time_since_epoch())
                           .count();
    }
    return item;
}

bool StatItem::shallowEqual(const StatItem &other, std::int64_t timeResolutionNs) const
{
    if (size != other.size)
        return false;

    // Check for the ignore-timestamp configuration first
    if (timeResolutionNs == -1)
        return true;

    // 2 seconds is our current accuracy threshold (for VFAT)
    const std::int64_t difference = mtimeNs > other.mtimeNs ? mtimeNs - other.mtimeNs
                                                            : other.mtimeNs - mtimeNs;
    if (difference > 2'000'000'000)
        return false;

    if (timeResolutionNs <= 0)
        return mtimeNs == other.mtimeNs;
    return mtimeNs / timeResolutionNs == other.mtimeNs / timeResolutionNs;
}

QList<QByteArray> splitLines(const QByteArray &text)
{
    QList<QByteArray> lines;
    qsizetype start = 0;
    const qsizetype n = text.size();
    qsizetype i = 0;
    while (i < n) {
        const char c = text.at(i);
        if (c == '\n' || c == '\r') {
            lines.append(text.mid(start, i - start));
            if (c == '\r' && i + 1 < n && text.at(i + 1) == '\n')
                ++i;
            start = i + 1;
        }
        ++i;
    }
    if (start < n)
        lines.append(text.mid(start));
    return lines;
}

QByteArray removeBlankLines(const QByteArray &text)
{
    QList<QByteArray> lines = splitLines(text);
    lines.removeIf([](const QByteArray &line) { return line.isEmpty(); });
    return lines.join('\n');
}

void clearFileComparisonCache()
{
    comparisonCache().clear();
}

FileCompareResult filesSame(const QStringList &files, const QList<QRegularExpression> &byteRegexes,
                            const ComparisonArgs &args)
{
    if (allSame(files))
        return FileCompareResult::Same;

    std::vector<StatItem> stats;
    for (const QString &file : files) {
        const std::optional<StatItem> stat = StatItem::stat(file);
        if (!stat)
            return FileCompareResult::FileError;
        stats.push_back(*stat);
    }

    const bool needContents = args.ignoreBlankLines || args.applyTextFilters;
    const QList<QRegularExpression> regexes = args.applyTextFilters ? byteRegexes
                                                                    : QList<QRegularExpression>();

    // If all entries are directories, they are considered to be the same
    if (std::all_of(stats.begin(), stats.end(), [](const StatItem &s) { return s.kind == FileKind::Directory; }))
        return FileCompareResult::Same;

    // If any entries are not regular files, consider them different
    if (!std::all_of(stats.begin(), stats.end(), [](const StatItem &s) { return s.kind == FileKind::Regular; }))
        return FileCompareResult::Different;

    // Compare files superficially if the options tells us to
    if (args.shallowComparison) {
        const bool allSameTimestamp = std::all_of(stats.begin() + 1, stats.end(), [&](const StatItem &s) {
            return s.shallowEqual(stats[0], args.timeResolutionNs);
        });
        return allSameTimestamp ? FileCompareResult::DodgySame : FileCompareResult::Different;
    }

    std::vector<std::int64_t> sizes;
    for (const StatItem &s : stats)
        sizes.push_back(s.size);
    const bool sameSize = allSame(sizes);
    // If there are no text filters, unequal sizes imply a difference
    if (!needContents && !sameSize)
        return FileCompareResult::Different;

    // Check the cache before doing the expensive comparison
    const QString key = cacheKey(files, needContents, regexes, args.ignoreBlankLines);
    auto &cache = comparisonCache();
    if (auto it = cache.constFind(key); it != cache.constEnd() && it->stats == stats)
        return it->result;

    // Open files and compare bit-by-bit
    std::optional<FileCompareResult> result;
    try {
        std::vector<std::unique_ptr<QFile>> handles;
        for (const QString &file : files) {
            auto handle = std::make_unique<QFile>(file);
            if (!handle->open(QIODevice::ReadOnly))
                return FileCompareResult::FileError; // Don't cache generic errors as results
            handles.push_back(std::move(handle));
        }

        std::vector<QByteArray> firstChunks;
        bool isBin = false;
        for (size_t i = 0; i < handles.size(); ++i) {
            const qint64 chunk = std::min<qint64>(stats[i].size, ChunkSize);
            QByteArray data = handles[i]->read(chunk);
            if (data.size() != chunk)
                return FileCompareResult::FileError;
            // Rough test to see whether files are binary.
            if (data.contains('\0'))
                isBin = true;
            firstChunks.push_back(std::move(data));
        }

        // compare files chunk-by-chunk
        if (sameSize) {
            const auto differs = [](const std::vector<QByteArray> &chunks) {
                for (size_t i = 1; i < chunks.size(); ++i) {
                    if (chunks[i] != chunks[0])
                        return true;
                }
                return false;
            };
            if (differs(firstChunks)) {
                result = FileCompareResult::Different;
            } else {
                qint64 remaining = stats[0].size - std::min<qint64>(stats[0].size, ChunkSize);
                std::vector<QByteArray> chunks(handles.size());
                while (remaining > 0) {
                    const qint64 chunk = std::min<qint64>(remaining, ChunkSize * 16);
                    for (size_t i = 0; i < handles.size(); ++i) {
                        chunks[i] = handles[i]->read(chunk);
                        if (chunks[i].size() != chunk)
                            return FileCompareResult::FileError;
                    }
                    if (differs(chunks)) {
                        result = FileCompareResult::Different;
                        break;
                    }
                    remaining -= chunk;
                }
            }
        } else {
            result = FileCompareResult::Different;
        }

        // normalize and compare files again
        if (result == FileCompareResult::Different && needContents && !isBin) {
            std::vector<QByteArray> contents;
            for (size_t i = 0; i < handles.size(); ++i) {
                if (!handles[i]->seek(0))
                    return FileCompareResult::FileError;
                contents.push_back(handles[i]->readAll());
                if (contents.back().size() != stats[i].size)
                    return FileCompareResult::FileError;
            }
            bool filtersOk = true;
            const std::vector<QByteArray> normalized = normalize(contents, args.ignoreBlankLines, regexes, &filtersOk);
            result = (filtersOk && allSame(normalized)) ? FileCompareResult::SameFiltered
                                                         : FileCompareResult::Different;
        }
    } catch (const std::bad_alloc &) {
        // Files are too large; we can't apply filters
        result = allSame(stats) ? FileCompareResult::DodgySame : FileCompareResult::DodgyDifferent;
    }

    const FileCompareResult finalResult = result.value_or(FileCompareResult::Same);
    cache.insert(key, CacheResult{stats, finalResult});
    return finalResult;
}

} // namespace meld
