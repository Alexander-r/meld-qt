#include "diffutil.h"

#include "myers.h"

#include <algorithm>
#include <climits>
#include <deque>
#include <memory>

namespace meld {

namespace {

//! Python's `lst[lo:hi] = [value] * count` for non-negative lo/hi
template<typename T>
void pySliceAssign(std::vector<T> &vec, int lo, int hi, const T &value, int count)
{
    const int length = int(vec.size());
    lo = std::min(std::max(lo, 0), length);
    hi = std::min(std::max(hi, lo), length);
    vec.erase(vec.begin() + lo, vec.begin() + hi);
    vec.insert(vec.begin() + lo, size_t(std::max(count, 0)), value);
}

DiffChunk offsetChunk(const DiffChunk &c, int o1, int o2)
{
    return {c.tag, c.start_a + o1, c.end_a + o1, c.start_b + o2, c.end_b + o2};
}

//! Offset a chunk by o1/o2 if it's after the inserted lines
OptChunk offsetAfter(const OptChunk &c, int start, int o1, int o2)
{
    if (!c)
        return std::nullopt;
    return DiffChunk{
        c->tag,
        c->start_a + (c->start_a > start ? o1 : 0),
        c->end_a + (c->end_a > start ? o1 : 0),
        c->start_b + (c->start_b > start ? o2 : 0),
        c->end_b + (c->end_b > start ? o2 : 0),
    };
}

} // namespace

OptChunk consumeBlankLines(const OptChunk &chunk, const Texts &texts, int pane1, int pane2)
{
    if (!chunk)
        return std::nullopt;

    const auto findBlankLines = [](const LineSource *txt, int lo, int hi) {
        while (lo < hi && txt->line(lo).isEmpty())
            ++lo;
        while (lo < hi && txt->line(hi - 1).isEmpty())
            --hi;
        return std::pair{lo, hi};
    };

    ChunkTag tag = chunk->tag;
    auto [c1, c2] = findBlankLines(texts[size_t(pane1)], chunk->start_a, chunk->end_a);
    auto [c3, c4] = findBlankLines(texts[size_t(pane2)], chunk->start_b, chunk->end_b);

    if (c1 == c2 && c3 == c4)
        return std::nullopt;
    if (c1 == c2 && tag == ChunkTag::Replace)
        tag = ChunkTag::Insert;
    else if (c3 == c4 && tag == ChunkTag::Replace)
        tag = ChunkTag::Delete;
    return DiffChunk{tag, c1, c2, c3, c4};
}

Differ::Differ(QObject *parent)
    : QObject(parent)
{
    // Internally, diffs are stored from text1 -> text0 and text1 -> text2.
}

Differ::~Differ() = default;

void Differ::updateMergeCache(const Texts &texts)
{
    if (numSequences_ == 3) {
        mergeCache_ = mergeDiffs(diffs_[0], diffs_[1], texts);
    } else {
        mergeCache_.clear();
        for (const DiffChunk &c : diffs_[0])
            mergeCache_.push_back({c, std::nullopt});
    }

    if (ignoreBlanks) {
        // We don't handle altering the chunk-type of conflicts in three-way
        // comparisons where e.g., pane 1 and 3 differ in blank lines
        for (MergedChunk &c : mergeCache_) {
            c = {consumeBlankLines(c.first, texts, 1, 0), consumeBlankLines(c.second, texts, 1, 2)};
        }
        std::erase_if(mergeCache_, [](const MergedChunk &c) { return !c.any(); });
    }

    // Calculate chunks that were added (in the new but not the old merge
    // cache), removed (in the old but not the new merge cache) and changed
    // (where the edit actually occurred, *and* the chunk is still around).
    // This information is used by the inline highlighting mechanism to
    // avoid re-highlighting existing chunks.
    const QSet<MergedChunk> newCache(mergeCache_.begin(), mergeCache_.end());
    ChunkChanges changes;
    for (const MergedChunk &c : std::as_const(oldMergeCache_)) {
        if (!newCache.contains(c))
            changes.removed.insert(c);
    }
    for (const MergedChunk &c : newCache) {
        if (!oldMergeCache_.contains(c))
            changes.added.insert(c);
    }
    changes.modified = changedChunks_;
    if (changes.modified && changes.removed.contains(*changes.modified))
        changes.modified.reset();

    bool mergeable0 = false;
    bool mergeable1 = false;
    for (const MergedChunk &c : mergeCache_) {
        mergeable0 = mergeable0 || (c.first && c.first->tag != ChunkTag::Conflict);
        mergeable1 = mergeable1 || (c.second && c.second->tag != ChunkTag::Conflict);
        if (mergeable0 && mergeable1)
            break;
    }
    hasMergeableChanges_ = {false, mergeable0, mergeable1, false};

    // Conflicts can only occur when there are three panes, and will always
    // involve the middle pane.
    conflicts.clear();
    for (int i = 0; i < int(mergeCache_.size()); ++i) {
        const MergedChunk &c = mergeCache_[size_t(i)];
        if ((c.first && c.first->tag == ChunkTag::Conflict)
            || (c.second && c.second->tag == ChunkTag::Conflict)) {
            conflicts.push_back(i);
        }
    }

    updateLineCache();
    Q_EMIT diffsChanged(changes);
}

void Differ::updateLineCache()
{
    // Cache a mapping from line index to per-pane chunk indices. This cache
    // exists so that the UI can quickly query for current, next and previous
    // chunks when the current cursor line changes.
    for (size_t i = 0; i < seqlength_.size() && i < lineCache_.size(); ++i) {
        // seqlength + 1 for after-last-line requests, which we do
        lineCache_[i].assign(size_t(seqlength_[i] + 1), ChunkLocation{});
    }

    const int lastChunk = int(mergeCache_.size());

    const auto findNext = [&](int diff, int seq, int current) -> std::optional<int> {
        if (seq == 1 && current + 1 < lastChunk)
            return current + 1;
        for (int j = current + 1; j < lastChunk; ++j) {
            if (mergeCache_[size_t(j)][diff])
                return j;
        }
        return std::nullopt;
    };

    std::array<std::optional<int>, 3> prev{};
    std::array<std::optional<int>, 3> next{findNext(0, 0, -1), findNext(0, 1, -1), findNext(1, 2, -1)};
    std::array<int, 3> oldEnd{0, 0, 0};

    struct SeqParam {
        int diff;
        int seq;
        bool useA;
    };
    static constexpr SeqParam seqParams[] = {{0, 0, false}, {0, 1, true}, {1, 2, false}};

    for (int i = 0; i < lastChunk; ++i) {
        const MergedChunk &c = mergeCache_[size_t(i)];
        for (const SeqParam &param : seqParams) {
            int diff = param.diff;
            const int seq = param.seq;
            if (!c[diff]) {
                if (seq == 1)
                    diff = 1;
                else
                    continue;
            }

            const DiffChunk &chunk = *c[diff];
            const int start = param.useA ? chunk.start_a : chunk.start_b;
            int end = param.useA ? chunk.end_a : chunk.end_b;
            const int last = oldEnd[size_t(seq)];
            if (start > last) {
                pySliceAssign(lineCache_[size_t(seq)], last, start,
                              ChunkLocation{std::nullopt, prev[size_t(seq)], next[size_t(seq)]},
                              start - last);
            }

            // For insert chunks, claim the subsequent line.
            if (start == end)
                end += 1;

            next[size_t(seq)] = findNext(diff, seq, i);
            pySliceAssign(lineCache_[size_t(seq)], start, end,
                          ChunkLocation{i, prev[size_t(seq)], next[size_t(seq)]}, end - start);
            prev[size_t(seq)] = i;
            oldEnd[size_t(seq)] = end;
        }
    }

    for (int seq = 0; seq < 3; ++seq) {
        const int last = oldEnd[size_t(seq)];
        const int end = int(lineCache_[size_t(seq)].size());
        if (last < end) {
            pySliceAssign(lineCache_[size_t(seq)], last, end,
                          ChunkLocation{std::nullopt, prev[size_t(seq)], next[size_t(seq)]},
                          end - last);
        }
    }
}

void Differ::changeSequence(int sequence, int startidx, int sizechange, const Texts &texts)
{
    if (sizechange < 0)
        replaceInSequence(sequence, startidx, -sizechange, 0, texts);
    else
        replaceInSequence(sequence, startidx, 0, sizechange, texts);
}

void Differ::replaceInSequence(int sequence, int startidx, int linesRemoved, int linesAdded,
                               const Texts &texts)
{
    Q_ASSERT(sequence >= 0 && sequence <= 2);
    const int sizechange = linesAdded - linesRemoved;
    if (sequence == 0 || sequence == 1)
        changeSequenceInternal(0, sequence, startidx, linesRemoved, sizechange, texts);
    if (sequence == 2 || (sequence == 1 && numSequences_ == 3))
        changeSequenceInternal(1, sequence, startidx, linesRemoved, sizechange, texts);
    if (size_t(sequence) < seqlength_.size())
        seqlength_[size_t(sequence)] += sizechange;

    // Calculate the expected differences in the chunk set if no cascading
    // changes occur, making sure to not include the changed chunk itself
    oldMergeCache_.clear();
    changedChunks_.reset();
    bool chunkChanged = false;
    for (MergedChunk c : mergeCache_) {
        OptChunk &c1 = c.first;
        OptChunk &c2 = c.second;
        if (sequence == 0) {
            if (c1 && c1->start_b <= startidx && startidx < c1->end_b)
                chunkChanged = true;
            c1 = offsetAfter(c1, startidx, 0, sizechange);
        } else if (sequence == 2) {
            if (c2 && c2->start_b <= startidx && startidx < c2->end_b)
                chunkChanged = true;
            c2 = offsetAfter(c2, startidx, 0, sizechange);
        } else {
            // Middle sequence changes alter both chunks
            if (c1 && c1->start_a <= startidx && startidx < c1->end_a)
                chunkChanged = true;
            c1 = offsetAfter(c1, startidx, sizechange, 0);
            if (numSequences_ == 3)
                c2 = offsetAfter(c2, startidx, sizechange, 0);
        }
        if (chunkChanged) {
            Q_ASSERT(!changedChunks_);
            if (!changedChunks_)
                changedChunks_ = c;
            chunkChanged = false;
        }
        oldMergeCache_.insert(c);
    }

    updateMergeCache(texts);
}

int Differ::locateChunkInternal(int whichdiffs, int sequence, int line) const
{
    // Find the index of the chunk which contains line.
    const auto &diffs = diffs_[size_t(whichdiffs)];
    for (int i = 0; i < int(diffs.size()); ++i) {
        const DiffChunk &c = diffs[size_t(i)];
        const int high = sequence != 1 ? c.end_b : c.end_a;
        if (line < high)
            return i;
    }
    return int(diffs.size());
}

bool Differ::hasChunk(int toPane, const DiffChunk &chunk) const
{
    // Return whether the pane/chunk exists in the current Differ
    const int sequence = toPane == 2 ? 1 : 0;
    const ChunkLocation location = locateChunk(1, chunk.start_a);
    if (!location.chunk)
        return false;
    return mergeCache_[size_t(*location.chunk)][sequence] == chunk;
}

OptChunk Differ::getChunk(int index, int fromPane, int toPane) const
{
    // Return the index-th change in from_pane. If to_pane is provided, then
    // only changes between from_pane and to_pane are considered, otherwise
    // all changes starting at from_pane are used.
    if (index < 0 || index >= int(mergeCache_.size()))
        return std::nullopt;
    const int sequence = (fromPane == 2 || toPane == 2) ? 1 : 0;
    OptChunk chunk = mergeCache_[size_t(index)][sequence];
    if (fromPane == 0 || fromPane == 2) {
        if (!chunk)
            return std::nullopt;
        return chunk->reversed();
    }
    if (toPane == -1 && !chunk)
        chunk = mergeCache_[size_t(index)][1];
    return chunk;
}

std::array<std::optional<int>, 3> Differ::getChunkStarts(int index) const
{
    // Return the starting lines of all chunks at an index
    if (index < 0 || index >= int(mergeCache_.size()))
        return {};
    const MergedChunk &chunks = mergeCache_[size_t(index)];
    return {
        chunks.first ? std::optional<int>(chunks.first->start_b) : std::nullopt,
        chunks.first ? std::optional<int>(chunks.first->start_a) : std::nullopt,
        chunks.second ? std::optional<int>(chunks.second->start_b) : std::nullopt,
    };
}

ChunkLocation Differ::locateChunk(int pane, int line) const
{
    // Find the index of the chunk which contains line. Returns the current,
    // previous and next chunk indices in that order.
    if (pane < 0 || pane > 2)
        return {};
    const auto &cache = lineCache_[size_t(pane)];
    int index = line;
    if (index < 0)
        index += int(cache.size());
    if (index < 0 || index >= int(cache.size()))
        return {};
    return cache[size_t(index)];
}

std::pair<bool, bool> Differ::hasMergeableChanges(int which) const
{
    return {hasMergeableChanges_[size_t(which)], hasMergeableChanges_[size_t(which + 1)]};
}

void Differ::changeSequenceInternal(int which, int sequence, int startidx, int linesRemoved,
                                    int sizechange, const Texts &texts)
{
    auto &diffs = diffs_[size_t(which)];
    std::array<int, 3> linesAdded{0, 0, 0};
    linesAdded[size_t(sequence)] = sizechange;
    int loidx = locateChunkInternal(which, sequence, startidx);
    int hiidx = linesRemoved > 0 ? locateChunkInternal(which, sequence, startidx + linesRemoved)
                                 : loidx;
    std::pair<int, int> lorange{0, 0};
    if (loidx > 0) {
        loidx -= 1;
        lorange = {diffs[size_t(loidx)].start_b, diffs[size_t(loidx)].start_a};
    }
    const int x = which * 2;
    std::pair<int, int> hirange;
    if (hiidx < int(diffs.size())) {
        hiidx += 1;
        hirange = {diffs[size_t(hiidx - 1)].end_b, diffs[size_t(hiidx - 1)].end_a};
    } else {
        hirange = {seqlength_[size_t(x)], seqlength_[1]};
    }
    const std::pair<int, int> rangex{lorange.first, hirange.first + linesAdded[size_t(x)]};
    const std::pair<int, int> range1{lorange.second, hirange.second + linesAdded[1]};
    Q_ASSERT(rangex.first <= rangex.second && range1.first <= range1.second);
    const QStringList linesx = texts[size_t(x)]->lines(rangex.first, rangex.second);
    const QStringList lines1 = texts[1]->lines(range1.first, range1.second);

    LineInterner interner;
    Sequence seq1 = interner.intern(lines1);
    Sequence seqx = interner.intern(linesx);
    MyersSequenceMatcher matcher(std::move(seq1), std::move(seqx));
    std::vector<DiffChunk> newdiffs = matcher.differenceOpcodes();
    for (DiffChunk &c : newdiffs)
        c = offsetChunk(c, range1.first, rangex.first);

    if (hiidx < int(diffs.size())) {
        for (size_t i = size_t(hiidx); i < diffs.size(); ++i)
            diffs[i] = offsetChunk(diffs[i], linesAdded[1], linesAdded[size_t(x)]);
    }
    diffs.erase(diffs.begin() + loidx, diffs.begin() + hiidx);
    diffs.insert(diffs.begin() + loidx, newdiffs.begin(), newdiffs.end());
}

std::pair<std::optional<int>, std::optional<int>> Differ::rangeFromLines(int textindex, int lo,
                                                                         int hi) const
{
    const ChunkLocation top = locateChunk(textindex, lo);
    std::optional<int> start = top.chunk;
    if (!start)
        start = top.next;
    const ChunkLocation bottom = locateChunk(textindex, hi);
    std::optional<int> end = bottom.chunk;
    if (!end)
        end = bottom.prev;
    return {start, end};
}

std::vector<DiffChunk> Differ::pairChanges(int fromindex, int toindex,
                                           std::optional<std::array<int, 4>> lines) const
{
    // Give all changes between file1 and either file0 or file2.
    int lo = 0;
    int hi = int(mergeCache_.size());
    if (lines) {
        auto [start1, end1] = rangeFromLines(fromindex, (*lines)[0], (*lines)[1]);
        auto [start2, end2] = rangeFromLines(toindex, (*lines)[2], (*lines)[3]);
        if ((!start1 || !end1) && (!start2 || !end2))
            return {};
        std::optional<int> start;
        for (const auto &s : {start1, start2}) {
            if (s && (!start || *s < *start))
                start = s;
        }
        std::optional<int> end;
        for (const auto &e : {end1, end2}) {
            if (e && (!end || *e > *end))
                end = e;
        }
        std::tie(lo, hi) = pySliceBounds(int(mergeCache_.size()), *start, *end + 1);
    }

    std::vector<DiffChunk> result;
    if (fromindex == 1) {
        const int seq = toindex / 2;
        for (int i = lo; i < hi; ++i) {
            if (const OptChunk &c = mergeCache_[size_t(i)][seq])
                result.push_back(*c);
        }
    } else {
        const int seq = fromindex / 2;
        for (int i = lo; i < hi; ++i) {
            if (const OptChunk &c = mergeCache_[size_t(i)][seq])
                result.push_back(c->reversed());
        }
    }
    return result;
}

std::vector<DiffChunk> Differ::pairedAllSingleChanges(int fromindex, int toindex) const
{
    return pairChanges(fromindex, toindex);
}

std::vector<DiffChunk> Differ::singleChanges(int textindex,
                                             std::optional<std::pair<int, int>> lines) const
{
    // Give changes for single file only. do not return 'equal' hunks.
    int lo = 0;
    int hi = int(mergeCache_.size());
    if (lines) {
        auto [start, end] = rangeFromLines(textindex, lines->first, lines->second);
        if (!start || !end)
            return {};
        std::tie(lo, hi) = pySliceBounds(int(mergeCache_.size()), *start, *end + 1);
    }

    std::vector<DiffChunk> result;
    if (textindex == 0 || textindex == 2) {
        const int seq = textindex / 2;
        for (int i = lo; i < hi; ++i) {
            if (const OptChunk &c = mergeCache_[size_t(i)][seq])
                result.push_back(c->reversed());
        }
    } else {
        for (int i = lo; i < hi; ++i) {
            const MergedChunk &cs = mergeCache_[size_t(i)];
            result.push_back(cs.first ? *cs.first : *cs.second);
        }
    }
    return result;
}

bool Differ::sequencesIdentical() const
{
    // check so that we don't call an uninitialised comparison 'identical'
    return diffs_[0].empty() && diffs_[1].empty() && initialised_;
}

Differ::MergeBounds Differ::mergeBlocks(const Using &using_) const
{
    const int lowc = std::min(using_[0].front().start_a, using_[1].front().start_a);
    const int highc = std::max(using_[0].back().end_a, using_[1].back().end_a);
    std::array<int, 2> low{};
    std::array<int, 2> high{};
    for (size_t i = 0; i < 2; ++i) {
        const DiffChunk &d0 = using_[i].front();
        low[i] = lowc - d0.start_a + d0.start_b;
        const DiffChunk &d1 = using_[i].back();
        high[i] = highc - d1.end_a + d1.end_b;
    }
    return {low[0], high[0], lowc, highc, low[1], high[1]};
}

std::vector<MergedChunk> Differ::autoMerge(Using &using_, const Texts &texts)
{
    // Automatically merge two sequences of change blocks
    const auto [l0, h0, l1, h1, l2, h2] = mergeBlocks(using_);
    ChunkTag tag;
    if (h0 - l0 == h2 - l2 && texts[0]->lines(l0, h0) == texts[2]->lines(l2, h2)) {
        if (l1 != h1 && l0 == h0)
            tag = ChunkTag::Delete;
        else if (l1 != h1)
            tag = ChunkTag::Replace;
        else
            tag = ChunkTag::Insert;
    } else {
        tag = ChunkTag::Conflict;
    }
    return {MergedChunk{DiffChunk{tag, l1, h1, l0, h0}, DiffChunk{tag, l1, h1, l2, h2}}};
}

std::vector<MergedChunk> Differ::mergeDiffs(const std::vector<DiffChunk> &seq0,
                                            const std::vector<DiffChunk> &seq1, const Texts &texts)
{
    std::array<std::deque<DiffChunk>, 2> seq{
        std::deque<DiffChunk>(seq0.begin(), seq0.end()),
        std::deque<DiffChunk>(seq1.begin(), seq1.end()),
    };
    std::vector<MergedChunk> result;
    while (!seq[0].empty() || !seq[1].empty()) {
        int highSeq;
        if (seq[0].empty()) {
            highSeq = 1;
        } else if (seq[1].empty()) {
            highSeq = 0;
        } else {
            highSeq = seq[0].front().start_a > seq[1].front().start_a ? 1 : 0;
            if (seq[0].front().start_a == seq[1].front().start_a) {
                if (seq[0].front().tag == ChunkTag::Insert)
                    highSeq = 0;
                else if (seq[1].front().tag == ChunkTag::Insert)
                    highSeq = 1;
            }
        }

        const DiffChunk highDiff = seq[size_t(highSeq)].front();
        seq[size_t(highSeq)].pop_front();
        int highMark = highDiff.end_a;
        int otherSeq = highSeq == 1 ? 0 : 1;

        Using using_;
        using_[size_t(highSeq)].push_back(highDiff);

        while (!seq[size_t(otherSeq)].empty()) {
            const DiffChunk otherDiff = seq[size_t(otherSeq)].front();
            if (highMark < otherDiff.start_a)
                break;
            if (highMark == otherDiff.start_a
                && !(highDiff.tag == ChunkTag::Insert && otherDiff.tag == ChunkTag::Insert)) {
                break;
            }

            using_[size_t(otherSeq)].push_back(otherDiff);
            seq[size_t(otherSeq)].pop_front();

            if (highMark < otherDiff.end_a) {
                std::swap(highSeq, otherSeq);
                highMark = otherDiff.end_a;
            }
        }

        if (using_[0].empty()) {
            Q_ASSERT(using_[1].size() == 1);
            result.push_back({std::nullopt, using_[1][0]});
        } else if (using_[1].empty()) {
            Q_ASSERT(using_[0].size() == 1);
            result.push_back({using_[0][0], std::nullopt});
        } else {
            const std::vector<MergedChunk> merged = autoMerge(using_, texts);
            result.insert(result.end(), merged.begin(), merged.end());
        }
    }
    return result;
}

std::generator<bool> Differ::setSequencesIter(Texts sequences)
{
    Q_ASSERT(sequences.size() <= 3);
    diffs_ = {};
    numSequences_ = int(sequences.size());
    seqlength_.clear();
    for (const LineSource *s : sequences)
        seqlength_.push_back(s->lineCount());

    for (int i = 0; i < numSequences_ - 1; ++i) {
        LineInterner interner;
        Sequence middle = interner.intern(sequences[1]->lines(0));
        Sequence other = interner.intern(sequences[size_t(i * 2)]->lines(0));
        std::unique_ptr<MyersSequenceMatcher> matcher;
        if (!syncpoints.empty()) {
            std::vector<std::pair<int, int>> points;
            for (const SyncPoint &s : syncpoints)
                points.emplace_back(s[size_t(i)].middle(), s[size_t(i)].other());
            matcher = std::make_unique<SyncPointMyersSequenceMatcher>(std::move(middle),
                                                                      std::move(other), points);
        } else {
            matcher = std::make_unique<MyersSequenceMatcher>(std::move(middle), std::move(other));
        }
        for (bool done : matcher->initialise()) {
            if (done)
                break;
            co_yield false;
        }
        diffs_[size_t(i)] = matcher->differenceOpcodes();
    }
    initialised_ = true;
    updateMergeCache(sequences);
    co_yield true;
}

void Differ::setSequences(const Texts &sequences)
{
    for (bool done : setSequencesIter(sequences)) {
        if (done)
            break;
    }
}

void Differ::clear()
{
    diffs_ = {};
    seqlength_.assign(size_t(numSequences_), 0);
    initialised_ = false;
    oldMergeCache_.clear();
    changedChunks_.reset();
    std::vector<StringListSource> empty;
    empty.resize(size_t(numSequences_));
    Texts texts;
    for (const StringListSource &s : empty)
        texts.push_back(&s);
    updateMergeCache(texts);
}

} // namespace meld
