#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace meld {

//! Return true if all elements of the list are equal
template<typename Container>
bool allSame(const Container &items)
{
    auto it = std::begin(items);
    const auto end = std::end(items);
    if (it == end)
        return true;
    const auto &sample = *it;
    for (++it; it != end; ++it) {
        if (!(*it == sample))
            return false;
    }
    return true;
}

enum class PathStyle {
    Native,
    Posix,
    Windows,
};

/*!
 * Remove common parts of a list of paths
 *
 * For example, `('/tmp/foo1', '/tmp/foo2')` would be summarised as
 * `('foo1', 'foo2')`. Paths that share a basename are distinguished by
 * prepending an indicator, e.g., `('/a/b/c', '/a/d/c')` would be summarised
 * to `['[b] c', '[d] c']`.
 */
QStringList shortenNames(const QStringList &names, PathStyle style = PathStyle::Native);

using Interval = std::pair<int, int>;

//! Merge a list of intervals, returning sorted non-overlapping intervals
std::vector<Interval> mergeIntervals(std::vector<Interval> intervals);

/*!
 * Apply text filters
 *
 * The regular expressions are applied to `text`, and every matched range
 * (or every participating group of a match) is removed from the result.
 * `applyFn` is called for each filtered interval. If a filter would remove
 * a line break, `*ok` is set to false (the original raises an assertion).
 */
QString applyTextFilters(const QString &text, const QList<QRegularExpression> &regexes,
                         const std::function<void(int, int)> &applyFn = {}, bool *ok = nullptr);

//! Byte-oriented variant, used by folder comparisons. Regexes must have
//! been compiled for Latin-1 decoded data (see FilterEntry::byteFilter).
QByteArray applyTextFilters(const QByteArray &text, const QList<QRegularExpression> &regexes,
                            bool *ok = nullptr);

//! A GtkAdjustment-like scroll model
struct Adjustment {
    double value = 0;
    double lower = 0;
    double upper = 0;
    double pageSize = 0;

    //! Equivalent of gtk_adjustment_configure(), which clamps the value
    static Adjustment configure(double value, double lower, double upper, double pageSize);
};

/*!
 * Calculate a cross-pane adjustment synchronisation point
 *
 * Our normal syncpoint is the middle of the screen. If the current position
 * is within the first half screen of a document, we scale the sync point
 * linearly back to 0.0 (top of the screen); if it's the the last half
 * screen, we again scale linearly to 1.0.
 */
double calcSyncpoint(const Adjustment &adj);

/*!
 * Wrap a paragraph to lines of at most `width` characters, joined with
 * newlines, as Python's textwrap.fill() with its default options
 *
 * Whitespace is collapsed to single spaces at line breaks, words are broken
 * at hyphens where possible, and words longer than the width are split.
 */
QString textWrapFill(const QString &text, int width);

} // namespace meld
