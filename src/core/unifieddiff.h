#pragma once

#include "myers.h"

#include <QString>
#include <QStringList>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace meld {

//! difflib.SequenceMatcher (Ratcliff/Obershelp with the autojunk heuristic)
class DifflibSequenceMatcher {
public:
    DifflibSequenceMatcher(Sequence a, Sequence b, bool autojunk = true);

    MatchingBlock findLongestMatch(int alo, int ahi, int blo, int bhi) const;
    const std::vector<MatchingBlock> &matchingBlocks();
    std::vector<DiffChunk> opcodes();
    std::vector<std::vector<DiffChunk>> groupedOpcodes(int n = 3);

private:
    Sequence a_;
    Sequence b_;
    bool autojunk_;
    std::unordered_map<std::int32_t, std::vector<int>> b2j_;
    std::unordered_set<std::int32_t> bpopular_;
    std::vector<MatchingBlock> matchingBlocks_;
    bool ready_ = false;
};

//! str.splitlines(keepends) with Python's set of line boundaries
QStringList pySplitLines(const QString &text, bool keepEnds);

//! difflib.unified_diff(), joined into a single string
QString unifiedDiff(const QStringList &a, const QStringList &b, const QString &fromfile = QString(),
                    const QString &tofile = QString(), int n = 3,
                    const QString &lineterm = QStringLiteral("\n"));

//! os.path.commonprefix()
QString commonPrefix(const QStringList &strings);

/*!
 * Build the patch shown by the "Format as Patch" dialog from buffer texts
 * and labels, following meld.patchdialog.PatchDialog.update_patch().
 */
QString formatPatch(const QStringList &texts, const QStringList &labels, int fromIndex, int toIndex);

} // namespace meld
