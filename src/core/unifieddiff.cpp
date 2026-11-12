#include "unifieddiff.h"

#include <algorithm>

namespace meld {

DifflibSequenceMatcher::DifflibSequenceMatcher(Sequence a, Sequence b, bool autojunk)
    : a_(std::move(a))
    , b_(std::move(b))
    , autojunk_(autojunk)
{
    for (int i = 0; i < int(b_.size()); ++i)
        b2j_[b_[size_t(i)]].push_back(i);

    // Purge popular elements that are not junk
    const int n = int(b_.size());
    if (autojunk_ && n >= 200) {
        const size_t ntest = size_t(n / 100 + 1);
        for (const auto &[elt, idxs] : b2j_) {
            if (idxs.size() > ntest)
                bpopular_.insert(elt);
        }
        for (std::int32_t elt : bpopular_)
            b2j_.erase(elt);
    }
}

MatchingBlock DifflibSequenceMatcher::findLongestMatch(int alo, int ahi, int blo, int bhi) const
{
    int besti = alo;
    int bestj = blo;
    int bestsize = 0;
    // find longest junk-free match
    std::unordered_map<int, int> j2len;
    std::unordered_map<int, int> newj2len;
    for (int i = alo; i < ahi; ++i) {
        newj2len.clear();
        const auto it = b2j_.find(a_[size_t(i)]);
        if (it != b2j_.end()) {
            for (int j : it->second) {
                // a[i] matches b[j]
                if (j < blo)
                    continue;
                if (j >= bhi)
                    break;
                const auto prev = j2len.find(j - 1);
                const int k = (prev != j2len.end() ? prev->second : 0) + 1;
                newj2len[j] = k;
                if (k > bestsize) {
                    besti = i - k + 1;
                    bestj = j - k + 1;
                    bestsize = k;
                }
            }
        }
        std::swap(j2len, newj2len);
    }

    // There is no junk function, so "junk" elements are only the popular
    // ones; neither side has isbjunk elements to extend over.
    const auto isbjunk = [](std::int32_t) { return false; };

    // Extend the best by non-junk elements on each end.
    while (besti > alo && bestj > blo && !isbjunk(b_[size_t(bestj - 1)])
           && a_[size_t(besti - 1)] == b_[size_t(bestj - 1)]) {
        --besti;
        --bestj;
        ++bestsize;
    }
    while (besti + bestsize < ahi && bestj + bestsize < bhi && !isbjunk(b_[size_t(bestj + bestsize)])
           && a_[size_t(besti + bestsize)] == b_[size_t(bestj + bestsize)]) {
        ++bestsize;
    }

    return {besti, bestj, bestsize};
}

const std::vector<MatchingBlock> &DifflibSequenceMatcher::matchingBlocks()
{
    if (ready_)
        return matchingBlocks_;

    const int la = int(a_.size());
    const int lb = int(b_.size());
    struct Range {
        int alo, ahi, blo, bhi;
    };
    std::vector<Range> queue{{0, la, 0, lb}};
    std::vector<MatchingBlock> blocks;
    while (!queue.empty()) {
        const Range r = queue.back();
        queue.pop_back();
        const MatchingBlock x = findLongestMatch(r.alo, r.ahi, r.blo, r.bhi);
        const int i = x.a;
        const int j = x.b;
        const int k = x.size;
        if (k) {
            blocks.push_back(x);
            if (r.alo < i && r.blo < j)
                queue.push_back({r.alo, i, r.blo, j});
            if (i + k < r.ahi && j + k < r.bhi)
                queue.push_back({i + k, r.ahi, j + k, r.bhi});
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const MatchingBlock &l, const MatchingBlock &r) {
        return std::tie(l.a, l.b, l.size) < std::tie(r.a, r.b, r.size);
    });

    // Collapse adjacent equal blocks
    int i1 = 0;
    int j1 = 0;
    int k1 = 0;
    std::vector<MatchingBlock> nonAdjacent;
    for (const MatchingBlock &block : blocks) {
        if (i1 + k1 == block.a && j1 + k1 == block.b) {
            k1 += block.size;
        } else {
            if (k1)
                nonAdjacent.push_back({i1, j1, k1});
            i1 = block.a;
            j1 = block.b;
            k1 = block.size;
        }
    }
    if (k1)
        nonAdjacent.push_back({i1, j1, k1});
    nonAdjacent.push_back({la, lb, 0});

    matchingBlocks_ = std::move(nonAdjacent);
    ready_ = true;
    return matchingBlocks_;
}

std::vector<DiffChunk> DifflibSequenceMatcher::opcodes()
{
    return opcodesFromMatchingBlocks(matchingBlocks());
}

std::vector<std::vector<DiffChunk>> DifflibSequenceMatcher::groupedOpcodes(int n)
{
    std::vector<DiffChunk> codes = opcodes();
    if (codes.empty())
        codes.push_back({ChunkTag::Equal, 0, 1, 0, 1});
    // Fixup leading and trailing groups if they show no changes.
    if (codes.front().tag == ChunkTag::Equal) {
        DiffChunk &c = codes.front();
        c = {c.tag, std::max(c.start_a, c.end_a - n), c.end_a, std::max(c.start_b, c.end_b - n), c.end_b};
    }
    if (codes.back().tag == ChunkTag::Equal) {
        DiffChunk &c = codes.back();
        c = {c.tag, c.start_a, std::min(c.end_a, c.start_a + n), c.start_b, std::min(c.end_b, c.start_b + n)};
    }

    const int nn = n + n;
    std::vector<std::vector<DiffChunk>> groups;
    std::vector<DiffChunk> group;
    for (DiffChunk c : codes) {
        // End the current group and start a new one whenever there is a
        // large range with no changes.
        if (c.tag == ChunkTag::Equal && c.end_a - c.start_a > nn) {
            group.push_back({c.tag, c.start_a, std::min(c.end_a, c.start_a + n), c.start_b,
                             std::min(c.end_b, c.start_b + n)});
            groups.push_back(std::move(group));
            group.clear();
            c.start_a = std::max(c.start_a, c.end_a - n);
            c.start_b = std::max(c.start_b, c.end_b - n);
        }
        group.push_back(c);
    }
    if (!group.empty() && !(group.size() == 1 && group.front().tag == ChunkTag::Equal))
        groups.push_back(std::move(group));
    return groups;
}

QStringList pySplitLines(const QString &text, bool keepEnds)
{
    QStringList lines;
    const qsizetype n = text.size();
    qsizetype start = 0;
    qsizetype i = 0;
    while (i < n) {
        const char16_t c = text.at(i).unicode();
        qsizetype eol = -1;
        switch (c) {
        case u'\r':
            eol = (i + 1 < n && text.at(i + 1) == u'\n') ? i + 2 : i + 1;
            break;
        case u'\n':
        case u'\x0b':
        case u'\x0c':
        case u'\x1c':
        case u'\x1d':
        case u'\x1e':
        case u'\x85':
        case u' ':
        case u' ':
            eol = i + 1;
            break;
        default:
            break;
        }
        if (eol >= 0) {
            lines.append(text.mid(start, (keepEnds ? eol : i) - start));
            start = eol;
            i = eol;
        } else {
            ++i;
        }
    }
    if (start < n)
        lines.append(text.mid(start));
    return lines;
}

namespace {

QString formatRangeUnified(int start, int stop)
{
    // Convert range to the "ed" format
    int beginning = start + 1; // lines start numbering with one
    const int length = stop - start;
    if (length == 1)
        return QString::number(beginning);
    if (!length)
        beginning -= 1; // empty ranges begin at line just before the range
    return QStringLiteral("%1,%2").arg(beginning).arg(length);
}

} // namespace

QString unifiedDiff(const QStringList &a, const QStringList &b, const QString &fromfile,
                    const QString &tofile, int n, const QString &lineterm)
{
    LineInterner interner;
    Sequence sa = interner.intern(a);
    Sequence sb = interner.intern(b);
    DifflibSequenceMatcher matcher(std::move(sa), std::move(sb));

    QString result;
    bool started = false;
    for (const std::vector<DiffChunk> &group : matcher.groupedOpcodes(n)) {
        if (!started) {
            started = true;
            result += QStringLiteral("--- %1%2").arg(fromfile, lineterm);
            result += QStringLiteral("+++ %1%2").arg(tofile, lineterm);
        }

        const DiffChunk &first = group.front();
        const DiffChunk &last = group.back();
        const QString file1Range = formatRangeUnified(first.start_a, last.end_a);
        const QString file2Range = formatRangeUnified(first.start_b, last.end_b);
        result += QStringLiteral("@@ -%1 +%2 @@%3").arg(file1Range, file2Range, lineterm);

        for (const DiffChunk &c : group) {
            if (c.tag == ChunkTag::Equal) {
                for (int i = c.start_a; i < c.end_a; ++i)
                    result += QLatin1Char(' ') + a.at(i);
                continue;
            }
            if (c.tag == ChunkTag::Replace || c.tag == ChunkTag::Delete) {
                for (int i = c.start_a; i < c.end_a; ++i)
                    result += QLatin1Char('-') + a.at(i);
            }
            if (c.tag == ChunkTag::Replace || c.tag == ChunkTag::Insert) {
                for (int j = c.start_b; j < c.end_b; ++j)
                    result += QLatin1Char('+') + b.at(j);
            }
        }
    }
    return result;
}

QString commonPrefix(const QStringList &strings)
{
    if (strings.isEmpty())
        return {};
    // Python compares the lexicographically smallest and largest strings
    const QString s1 = *std::min_element(strings.begin(), strings.end());
    const QString s2 = *std::max_element(strings.begin(), strings.end());
    qsizetype i = 0;
    while (i < s1.size() && i < s2.size() && s1.at(i) == s2.at(i))
        ++i;
    return s1.left(i);
}

QString formatPatch(const QStringList &texts, const QStringList &labels, int fromIndex, int toIndex)
{
    std::vector<QStringList> lineLists;
    for (const QString &text : texts) {
        QStringList lines = pySplitLines(text, true);

        // Ensure that the last line ends in a newline
        const QStringList barelines = pySplitLines(text, false);
        if (!barelines.isEmpty() && !lines.isEmpty() && barelines.last() == lines.last()) {
            // Final line lacks a line-break; add in a best guess
            QString previousLinebreak = QStringLiteral("\n");
            if (lines.size() > 1)
                previousLinebreak = lines.at(lines.size() - 2).mid(barelines.at(barelines.size() - 2).size());
            lines.last() += previousLinebreak;
        }
        lineLists.push_back(lines);
    }

    const QString prefix = commonPrefix(labels);
    const qsizetype cut = prefix.lastIndexOf(QLatin1Char('/')) + 1;
    QStringList names;
    for (const QString &label : labels)
        names.append(label.mid(cut));

    return unifiedDiff(lineLists[size_t(fromIndex)], lineLists[size_t(toIndex)], names.value(fromIndex),
                       names.value(toIndex));
}

} // namespace meld
