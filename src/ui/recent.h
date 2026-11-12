// Recent comparisons are kept in the application settings instead of
// GtkRecentManager's fake single-file registrations.

#pragma once

#include "melddoc.h"

#include <QList>

namespace meld {

class RecentComparisons {
public:
    static constexpr int MaxEntries = 15;

    static QList<Comparison> load();
    static void add(const Comparison &comparison);
    static void clear();
    static QString displayName(const Comparison &comparison);

    //! Read a saved comparison file (.meldcmp, as written by the original
    //! Meld); throws std::runtime_error if the file is missing or invalid
    static Comparison readComparisonFile(const QString &path);
};

} // namespace meld
