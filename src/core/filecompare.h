#pragma once

#include <QByteArray>
#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <cstdint>
#include <optional>

namespace meld {

enum class FileCompareResult {
    Same,
    SameFiltered,
    DodgySame,
    DodgyDifferent,
    Different,
    FileError,
};

struct ComparisonArgs {
    bool shallowComparison = false;
    //! Nanoseconds; -1 ignores timestamps entirely
    std::int64_t timeResolutionNs = 100;
    bool ignoreBlankLines = false;
    bool applyTextFilters = true;
};

enum class FileKind {
    Missing,
    Regular,
    Directory,
    Other,
};

struct StatItem {
    FileKind kind = FileKind::Missing;
    std::int64_t size = 0;
    //! Modification time in nanoseconds
    std::int64_t mtimeNs = 0;

    friend bool operator==(const StatItem &, const StatItem &) = default;

    static std::optional<StatItem> stat(const QString &path, bool followSymlinks = true);
    bool shallowEqual(const StatItem &other, std::int64_t timeResolutionNs) const;
};

//! Remove blank lines from text, and normalize line endings
QByteArray removeBlankLines(const QByteArray &text);

//! bytes.splitlines() equivalent
QList<QByteArray> splitLines(const QByteArray &text);

/*!
 * Determine whether a list of files are the same.
 *
 * Possible results are:
 *   Same: The files are the same
 *   SameFiltered: The files are identical only after filtering with 'regexes'
 *   DodgySame: The files are superficially the same (i.e., type, size, mtime)
 *   DodgyDifferent: The files are superficially different
 *   FileError: There was a problem reading one or more of the files
 *
 * `byteRegexes` are the byte filters of active text filters.
 */
FileCompareResult filesSame(const QStringList &files, const QList<QRegularExpression> &byteRegexes,
                            const ComparisonArgs &args);

void clearFileComparisonCache();

} // namespace meld
