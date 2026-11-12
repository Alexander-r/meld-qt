#include "merge.h"

#include "myers.h"

#include <deque>

namespace meld {

AutoMergeDiffer::AutoMergeDiffer(QObject *parent)
    : Differ(parent)
{
}

std::vector<MergedChunk> AutoMergeDiffer::autoMerge(Using &using_, const Texts &texts)
{
    std::vector<MergedChunk> result;
    for (const MergedChunk &merged : Differ::autoMerge(using_, texts)) {
        const DiffChunk out0 = *merged.first;
        const DiffChunk out1 = *merged.second;
        if (!(autoMergeEnabled && out0.tag == ChunkTag::Conflict)) {
            result.push_back(merged);
            continue;
        }

        // we will try to resolve more complex conflicts automatically here...
        // if possible
        const int l0 = out0.start_b;
        const int h0 = out0.end_b;
        const int l1 = out0.start_a;
        const int h1 = out0.end_a;
        const int l2 = out1.start_b;
        const int h2 = out1.end_b;
        const int len0 = h0 - l0;
        const int len1 = h1 - l1;
        const int len2 = h2 - l2;
        if ((len0 > 0 && len2 > 0) && (len0 == len1 || len2 == len1 || len1 == 0)) {
            LineInterner interner;
            Sequence s0 = interner.intern(texts[0]->lines(l0, h0));
            Sequence s2 = interner.intern(texts[2]->lines(l2, h2));
            MyersSequenceMatcher matcher(std::move(s0), std::move(s2));
            for (const DiffChunk &chunk : matcher.opcodes()) {
                int s1 = l1;
                int e1 = l1;
                if (len0 == len1) {
                    s1 += chunk.start_a;
                    e1 += chunk.end_a;
                } else if (len2 == len1) {
                    s1 += chunk.start_b;
                    e1 += chunk.end_b;
                }
                const ChunkTag tag = chunk.tag == ChunkTag::Equal ? ChunkTag::Replace : ChunkTag::Conflict;
                result.push_back({DiffChunk{tag, s1, e1, l0 + chunk.start_a, l0 + chunk.end_a},
                                  DiffChunk{tag, s1, e1, l2 + chunk.start_b, l2 + chunk.end_b}});
            }
            return result;
        }

        // some tricks to resolve even more conflicts automatically
        // unfortunately the resulting chunks cannot be used to highlight
        // changes but hey, they are good enough to merge the resulting file :)
        std::optional<ChunkTag> chunktype = using_[0][0].tag;
        for (const auto &chunkarr : using_) {
            for (const DiffChunk &chunk : chunkarr) {
                if (chunk.tag != *chunktype) {
                    chunktype.reset();
                    break;
                }
            }
            if (!chunktype)
                break;
        }

        if (chunktype != ChunkTag::Delete) {
            result.push_back(merged);
            continue;
        }

        // delete + delete -> split into delete/conflict
        std::deque<DiffChunk> using0(using_[0].begin(), using_[0].end());
        std::deque<DiffChunk> using1(using_[1].begin(), using_[1].end());
        OptChunk seq0;
        OptChunk seq1;
        int i0 = 0;
        int i1 = 0;
        int end0 = 0;
        int end1 = 0;
        while (true) {
            if (!seq0) {
                if (using0.empty())
                    break;
                seq0 = using0.front();
                using0.pop_front();
                i0 = seq0->start_a;
                end0 = seq0->end_b;
            }
            if (!seq1) {
                if (using1.empty())
                    break;
                seq1 = using1.front();
                using1.pop_front();
                i1 = seq1->start_a;
                end1 = seq1->end_b;
            }
            const int highstart = std::max(i0, i1);
            if (i0 != i1) {
                result.push_back(
                    {DiffChunk{ChunkTag::Conflict, i0 - highstart + i1, highstart,
                               seq0->start_b - highstart + i1, seq0->start_b},
                     DiffChunk{ChunkTag::Conflict, i1 - highstart + i0, highstart,
                               seq1->start_b - highstart + i0, seq1->start_b}});
            }
            const int lowend = std::min(seq0->end_a, seq1->end_a);
            if (highstart != lowend) {
                result.push_back(
                    {DiffChunk{ChunkTag::Delete, highstart, lowend, seq0->start_b, seq0->end_b},
                     DiffChunk{ChunkTag::Delete, highstart, lowend, seq1->start_b, seq1->end_b}});
            }
            i0 = i1 = lowend;
            if (lowend == seq0->end_a)
                seq0.reset();
            if (lowend == seq1->end_a)
                seq1.reset();
        }

        if (seq0) {
            result.push_back(
                {DiffChunk{ChunkTag::Conflict, i0, seq0->end_a, seq0->start_b, seq0->end_b},
                 DiffChunk{ChunkTag::Conflict, i0, seq0->end_a, end1, end1 + seq0->end_a - i0}});
        } else if (seq1) {
            result.push_back(
                {DiffChunk{ChunkTag::Conflict, i1, seq1->end_a, end0, end0 + seq1->end_a - i1},
                 DiffChunk{ChunkTag::Conflict, i1, seq1->end_a, seq1->start_b, seq1->end_b}});
        }
        return result;
    }
    return result;
}

void AutoMergeDiffer::replaceInSequence(int sequence, int startidx, int linesRemoved,
                                        int linesAdded, const Texts &texts)
{
    const int sizechange = linesAdded - linesRemoved;
    if (sequence == 1) {
        int lo = 0;
        for (int c : unresolved) {
            if (startidx <= c)
                break;
            ++lo;
        }
        if (lo < int(unresolved.size())) {
            int hi = lo;
            if (linesRemoved > 0) {
                for (size_t k = size_t(lo); k < unresolved.size(); ++k) {
                    if (startidx + linesRemoved <= unresolved[k])
                        break;
                    ++hi;
                }
            } else if (sizechange == 0 && startidx == unresolved[size_t(lo)]) {
                ++hi;
            }

            for (size_t k = size_t(hi); k < unresolved.size(); ++k)
                unresolved[k] += sizechange;
            unresolved.erase(unresolved.begin() + lo, unresolved.begin() + hi);
        }
    }

    Differ::replaceInSequence(sequence, startidx, linesRemoved, linesAdded, texts);
}

Merger::Merger()
    : ownedDiffer_(std::make_unique<AutoMergeDiffer>())
    , differ_(ownedDiffer_.get())
{
    ownedDiffer_->autoMergeEnabled = true;
}

Merger::~Merger() = default;

void Merger::setDiffer(Differ *differ)
{
    differ_ = differ;
}

std::generator<bool> Merger::initialize(Texts sequences, Texts mergeTexts)
{
    for (bool done : differ_->setSequencesIter(std::move(sequences))) {
        if (done)
            break;
        co_yield false;
    }
    texts = std::move(mergeTexts);
    co_yield true;
}

int Merger::applyChange(const LineSource *text, const DiffChunk &change, QStringList &mergedtext) const
{
    if (change.tag == ChunkTag::Insert) {
        for (int i = change.start_b; i < change.end_b; ++i)
            mergedtext.append(text->line(i));
        return 0;
    }
    if (change.tag == ChunkTag::Replace) {
        for (int i = change.start_b; i < change.end_b; ++i)
            mergedtext.append(text->line(i));
        return change.end_a - change.start_a;
    }
    return change.end_a - change.start_a;
}

QString Merger::merge3Files(bool markConflicts)
{
    unresolved.clear();
    int lastline = 0;
    int mergedline = 0;
    QStringList mergedtext;
    for (const MergedChunk &change : differ_->allChanges()) {
        int lowMark = lastline;
        if (change.first)
            lowMark = change.first->start_a;
        if (change.second)
            lowMark = std::max(lowMark, change.second->start_a);
        for (int i = lastline; i < lowMark; ++i)
            mergedtext.append(texts[1]->line(i));
        mergedline += lowMark - lastline;
        lastline = lowMark;
        if (change.first && change.second && change.first->tag == ChunkTag::Conflict) {
            const int highMark = std::max(change.first->end_a, change.second->end_a);
            if (markConflicts) {
                if (lowMark < highMark) {
                    for (int i = lowMark; i < highMark; ++i) {
                        mergedtext.append(QStringLiteral("(\?\?)") + texts[1]->line(i));
                        unresolved.push_back(mergedline);
                        ++mergedline;
                    }
                } else {
                    mergedtext.append(QStringLiteral("(\?\?)"));
                    unresolved.push_back(mergedline);
                    ++mergedline;
                }
                lastline = highMark;
            }
        } else if (change.first) {
            lastline += applyChange(texts[0], *change.first, mergedtext);
            mergedline += change.first->end_b - change.first->start_b;
        } else {
            lastline += applyChange(texts[2], *change.second, mergedtext);
            mergedline += change.second->end_b - change.second->start_b;
        }
    }
    const int baselen = texts[1]->lineCount();
    for (int i = lastline; i < baselen; ++i)
        mergedtext.append(texts[1]->line(i));

    // FIXME: We need to obtain the original line endings from the lines that
    // were merged and use those here instead of assuming '\n'.
    return mergedtext.join(QLatin1Char('\n'));
}

QString Merger::merge2Files(int fromindex, int toindex)
{
    unresolved.clear();
    int lastline = 0;
    QStringList mergedtext;
    for (const DiffChunk &change : differ_->pairChanges(toindex, fromindex)) {
        const int lowMark = change.tag == ChunkTag::Conflict ? change.end_a : change.start_a;
        for (int i = lastline; i < lowMark; ++i)
            mergedtext.append(texts[size_t(toindex)]->line(i));
        lastline = lowMark;
        if (change.tag != ChunkTag::Conflict)
            lastline += applyChange(texts[size_t(fromindex)], change, mergedtext);
    }
    const int baselen = texts[size_t(toindex)]->lineCount();
    for (int i = lastline; i < baselen; ++i)
        mergedtext.append(texts[size_t(toindex)]->line(i));

    return mergedtext.join(QLatin1Char('\n'));
}

} // namespace meld
