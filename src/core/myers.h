#pragma once

#include "diffchunk.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <generator>
#include <utility>
#include <vector>

namespace meld {

//! Sequences are compared as integers: either interned line IDs (see
//! LineInterner) or Unicode code points for inline comparisons.
using Sequence = std::vector<std::int32_t>;

struct MatchingBlock {
    int a = 0;
    int b = 0;
    int size = 0;

    friend bool operator==(const MatchingBlock &, const MatchingBlock &) = default;
};

//! difflib.SequenceMatcher.get_opcodes() applied to a list of matching blocks
std::vector<DiffChunk> opcodesFromMatchingBlocks(const std::vector<MatchingBlock> &blocks);

//! Maps strings to small integers so that sequences can be compared cheaply
class LineInterner {
public:
    std::int32_t intern(const QString &line);
    Sequence intern(const QStringList &lines);

private:
    QHash<QString, std::int32_t> ids_;
};

//! Unicode code points of a string, matching Python's str indexing
Sequence codePoints(const QString &text);

/*!
 * Optimised implementation of the O(NP) algorithm described by Sun Wu,
 * Udi Manber, Gene Myers, Webb Miller ("An O(NP) Sequence Comparison
 * Algorithm", 1989), including Meld's pre- and post-processing.
 */
class MyersSequenceMatcher {
public:
    MyersSequenceMatcher(Sequence a, Sequence b);
    virtual ~MyersSequenceMatcher();

    MyersSequenceMatcher(const MyersSequenceMatcher &) = delete;
    MyersSequenceMatcher &operator=(const MyersSequenceMatcher &) = delete;

    /*!
     * Run the comparison cooperatively. The generator yields false while
     * work is still pending, and true once the result is available.
     */
    virtual std::generator<bool> initialise();

    //! Run the comparison to completion (if necessary) and return the result
    const std::vector<MatchingBlock> &matchingBlocks();
    virtual std::vector<DiffChunk> opcodes();
    std::vector<DiffChunk> differenceOpcodes();

    const Sequence &a() const { return a_; }
    const Sequence &b() const { return b_; }

protected:
    void preprocessRemovePrefixSuffix(Sequence &a, Sequence &b);
    virtual void preprocessDiscardNonmatchingLines(Sequence &a, Sequence &b);
    void preprocess(Sequence &a, Sequence &b);
    void postprocess();
    void buildMatchingBlocks(int lastsnake);
    void finishDiscard(Sequence &a, Sequence &b, Sequence &&indexedA, Sequence &&indexedB);

    Sequence a_;
    Sequence b_;
    std::vector<MatchingBlock> matchingBlocks_;
    bool ready_ = false;
    std::vector<int> aindex_;
    std::vector<int> bindex_;
    int commonPrefix_ = 0;
    int commonSuffix_ = 0;
    bool linesDiscarded_ = false;

private:
    // Snakes form linked lists in the Python implementation; here they are
    // reference counted nodes in an arena so unreachable paths are reused.
    struct Snake {
        int prev;
        int x;
        int y;
        int length;
        int refs;
    };
    int newSnake(int prev, int x, int y, int length);
    void retain(int node);
    void release(int node);

    std::vector<Snake> snakes_;
    std::vector<int> freeSnakes_;
};

//! Matcher variant used for inline (character-based) highlighting
class InlineMyersSequenceMatcher : public MyersSequenceMatcher {
public:
    using MyersSequenceMatcher::MyersSequenceMatcher;

protected:
    void preprocessDiscardNonmatchingLines(Sequence &a, Sequence &b) override;
};

//! Matcher that forces alignment of user-supplied synchronisation points
class SyncPointMyersSequenceMatcher : public MyersSequenceMatcher {
public:
    SyncPointMyersSequenceMatcher(Sequence a, Sequence b,
                                  std::vector<std::pair<int, int>> syncpoints = {});

    std::generator<bool> initialise() override;
    std::vector<DiffChunk> opcodes() override;

private:
    std::vector<std::pair<int, int>> syncpoints_;
    std::vector<std::vector<MatchingBlock>> splitMatchingBlocks_;
};

//! Python slice semantics for a sequence of length `length`
std::pair<int, int> pySliceBounds(int length, int lo, int hi);

template<typename Container>
Container pySlice(const Container &seq, int lo, int hi)
{
    auto [l, h] = pySliceBounds(int(seq.size()), lo, hi);
    return Container(seq.begin() + l, seq.begin() + h);
}

} // namespace meld
