#include "myers.h"

#include <algorithm>
#include <climits>
#include <unordered_set>

namespace meld {

std::pair<int, int> pySliceBounds(int length, int lo, int hi)
{
    if (lo < 0)
        lo = std::max(0, lo + length);
    if (hi < 0)
        hi = std::max(0, hi + length);
    lo = std::min(lo, length);
    hi = std::min(hi, length);
    if (hi < lo)
        hi = lo;
    return {lo, hi};
}

std::vector<DiffChunk> opcodesFromMatchingBlocks(const std::vector<MatchingBlock> &blocks)
{
    std::vector<DiffChunk> answer;
    int i = 0;
    int j = 0;
    for (const MatchingBlock &block : blocks) {
        const int ai = block.a;
        const int bj = block.b;
        std::optional<ChunkTag> tag;
        if (i < ai && j < bj)
            tag = ChunkTag::Replace;
        else if (i < ai)
            tag = ChunkTag::Delete;
        else if (j < bj)
            tag = ChunkTag::Insert;
        if (tag)
            answer.push_back({*tag, i, ai, j, bj});
        i = ai + block.size;
        j = bj + block.size;
        // The list of matching blocks is terminated by a sentinel of size 0
        if (block.size)
            answer.push_back({ChunkTag::Equal, ai, i, bj, j});
    }
    return answer;
}

std::int32_t LineInterner::intern(const QString &line)
{
    auto it = ids_.constFind(line);
    if (it != ids_.constEnd())
        return it.value();
    const std::int32_t id = std::int32_t(ids_.size());
    ids_.insert(line, id);
    return id;
}

Sequence LineInterner::intern(const QStringList &lines)
{
    Sequence seq;
    seq.reserve(lines.size());
    for (const QString &line : lines)
        seq.push_back(intern(line));
    return seq;
}

Sequence codePoints(const QString &text)
{
    Sequence seq;
    seq.reserve(text.size());
    for (char32_t cp : text.toUcs4())
        seq.push_back(std::int32_t(cp));
    return seq;
}

namespace {

int findCommonPrefix(const Sequence &a, const Sequence &b)
{
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i])
        ++i;
    return int(i);
}

int findCommonSuffix(const Sequence &a, const Sequence &b)
{
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[a.size() - 1 - i] == b[b.size() - 1 - i])
        ++i;
    return int(i);
}

//! Membership test for sequence elements, used to discard unmatched lines
class ElementSet {
public:
    explicit ElementSet(const Sequence &seq)
    {
        std::int32_t maxValue = 0;
        bool dense = true;
        for (std::int32_t v : seq) {
            if (v < 0) {
                dense = false;
                break;
            }
            maxValue = std::max(maxValue, v);
        }
        dense_ = dense && maxValue < (1 << 22);
        if (dense_) {
            flags_.assign(size_t(maxValue) + 1, 0);
            for (std::int32_t v : seq)
                flags_[size_t(v)] = 1;
        } else {
            set_.insert(seq.begin(), seq.end());
        }
    }

    bool contains(std::int32_t v) const
    {
        if (dense_)
            return v >= 0 && size_t(v) < flags_.size() && flags_[size_t(v)];
        return set_.contains(v);
    }

private:
    bool dense_ = false;
    std::vector<char> flags_;
    std::unordered_set<std::int32_t> set_;
};

void indexMatching(const Sequence &a, const Sequence &b, Sequence &matches, std::vector<int> &index)
{
    const ElementSet aset(a);
    for (int i = 0; i < int(b.size()); ++i) {
        if (aset.contains(b[size_t(i)])) {
            matches.push_back(b[size_t(i)]);
            index.push_back(i);
        }
    }
}

std::uint64_t kmerKey(const Sequence &s, size_t i)
{
    const auto part = [&](size_t k) { return std::uint64_t(std::uint32_t(s[k]) & 0x1FFFFFu); };
    return (part(i) << 42) | (part(i + 1) << 21) | part(i + 2);
}

void indexMatchingKmers(const Sequence &a, const Sequence &b, Sequence &matches, std::vector<int> &index)
{
    std::unordered_set<std::uint64_t> aset;
    for (size_t i = 0; i + 2 < a.size(); ++i)
        aset.insert(kmerKey(a, i));
    int nextPossMatch = 0;
    // Start from where we can get a valid triple
    for (int i = 2; i < int(b.size()); ++i) {
        if (!aset.contains(kmerKey(b, size_t(i - 2))))
            continue;
        // Make sure we don't re-record matches from overlapping kmers
        for (int j = std::max(nextPossMatch, i - 2); j <= i; ++j) {
            matches.push_back(b[size_t(j)]);
            index.push_back(j);
        }
        nextPossMatch = i + 1;
    }
}

bool slicesEqual(const Sequence &a, int alo, int ahi, const Sequence &b, int blo, int bhi)
{
    auto [al, ah] = pySliceBounds(int(a.size()), alo, ahi);
    auto [bl, bh] = pySliceBounds(int(b.size()), blo, bhi);
    if (ah - al != bh - bl)
        return false;
    return std::equal(a.begin() + al, a.begin() + ah, b.begin() + bl);
}

} // namespace

MyersSequenceMatcher::MyersSequenceMatcher(Sequence a, Sequence b)
    : a_(std::move(a))
    , b_(std::move(b))
{
}

MyersSequenceMatcher::~MyersSequenceMatcher() = default;

const std::vector<MatchingBlock> &MyersSequenceMatcher::matchingBlocks()
{
    if (!ready_) {
        for (bool done : initialise()) {
            if (done)
                break;
        }
    }
    return matchingBlocks_;
}

std::vector<DiffChunk> MyersSequenceMatcher::opcodes()
{
    return opcodesFromMatchingBlocks(matchingBlocks());
}

std::vector<DiffChunk> MyersSequenceMatcher::differenceOpcodes()
{
    std::vector<DiffChunk> result;
    for (const DiffChunk &chunk : opcodes()) {
        if (chunk.tag != ChunkTag::Equal)
            result.push_back(chunk);
    }
    return result;
}

void MyersSequenceMatcher::preprocessRemovePrefixSuffix(Sequence &a, Sequence &b)
{
    // remove common prefix and common suffix
    commonPrefix_ = commonSuffix_ = 0;
    commonPrefix_ = findCommonPrefix(a, b);
    if (commonPrefix_ > 0) {
        a.erase(a.begin(), a.begin() + commonPrefix_);
        b.erase(b.begin(), b.begin() + commonPrefix_);
    }

    if (!a.empty() && !b.empty()) {
        commonSuffix_ = findCommonSuffix(a, b);
        if (commonSuffix_ > 0) {
            a.resize(a.size() - size_t(commonSuffix_));
            b.resize(b.size() - size_t(commonSuffix_));
        }
    }
}

void MyersSequenceMatcher::finishDiscard(Sequence &a, Sequence &b, Sequence &&indexedA,
                                         Sequence &&indexedB)
{
    // We only use the optimised result if it's worthwhile. The constant
    // represents a heuristic of how many lines constitute 'worthwhile'.
    linesDiscarded_ = (b.size() - indexedB.size() > 10) || (a.size() - indexedA.size() > 10);
    if (linesDiscarded_) {
        a = std::move(indexedA);
        b = std::move(indexedB);
    }
}

void MyersSequenceMatcher::preprocessDiscardNonmatchingLines(Sequence &a, Sequence &b)
{
    // discard lines that do not match any line from the other file
    if (a.empty() || b.empty()) {
        aindex_.clear();
        bindex_.clear();
        return;
    }

    Sequence indexedA;
    Sequence indexedB;
    aindex_.clear();
    bindex_.clear();
    indexMatching(a, b, indexedB, bindex_);
    indexMatching(b, a, indexedA, aindex_);
    finishDiscard(a, b, std::move(indexedA), std::move(indexedB));
}

void InlineMyersSequenceMatcher::preprocessDiscardNonmatchingLines(Sequence &a, Sequence &b)
{
    if (a.size() <= 2 && b.size() <= 2) {
        aindex_.clear();
        bindex_.clear();
        return;
    }

    Sequence indexedA;
    Sequence indexedB;
    aindex_.clear();
    bindex_.clear();
    indexMatchingKmers(a, b, indexedB, bindex_);
    indexMatchingKmers(b, a, indexedA, aindex_);
    finishDiscard(a, b, std::move(indexedA), std::move(indexedB));
}

void MyersSequenceMatcher::preprocess(Sequence &a, Sequence &b)
{
    // 1) remove common prefix and common suffix
    // 2) remove lines that do not match
    preprocessRemovePrefixSuffix(a, b);
    preprocessDiscardNonmatchingLines(a, b);
}

void MyersSequenceMatcher::postprocess()
{
    // Perform some post-processing cleanup to reduce 'chaff' and make the
    // result more human-readable. Since Myers diff is a greedy algorithm
    // backward scanning of matching chunks might reveal some smaller chunks
    // that can be combined together.
    std::vector<MatchingBlock> mb{matchingBlocks_.back()};
    int i = int(matchingBlocks_.size()) - 2;
    while (i >= 0) {
        MatchingBlock cur = matchingBlocks_[size_t(i)];
        --i;
        while (i >= 0) {
            const MatchingBlock prev = matchingBlocks_[size_t(i)];
            if (prev.b + prev.size == cur.b || prev.a + prev.size == cur.a) {
                if (slicesEqual(a_, cur.a - prev.size, cur.a, b_, cur.b - prev.size, cur.b)) {
                    cur.b -= prev.size;
                    cur.a -= prev.size;
                    cur.size += prev.size;
                    --i;
                    continue;
                }
            }
            break;
        }
        mb.push_back(cur);
    }
    std::reverse(mb.begin(), mb.end());
    matchingBlocks_ = std::move(mb);
}

void MyersSequenceMatcher::buildMatchingBlocks(int lastsnake)
{
    // Build list of matching blocks based on snakes. The resulting blocks
    // take into consideration multiple preprocessing optimizations:
    //  * add separate blocks for common prefix and suffix
    //  * shift positions and split blocks based on the list of discarded
    //    non-matching lines
    std::vector<MatchingBlock> reversed;
    while (lastsnake != -1) {
        const Snake snake = snakes_[size_t(lastsnake)];
        lastsnake = snake.prev;
        int x = snake.x;
        int y = snake.y;
        const int length = snake.length;
        if (linesDiscarded_) {
            // split snakes if needed because of discarded lines
            x += length - 1;
            y += length - 1;
            int xprev = aindex_[size_t(x)] + commonPrefix_;
            int yprev = bindex_[size_t(y)] + commonPrefix_;
            if (length > 1) {
                int newsnake = 1;
                for (int i = 1; i < length; ++i) {
                    --x;
                    --y;
                    const int xnext = aindex_[size_t(x)] + commonPrefix_;
                    const int ynext = bindex_[size_t(y)] + commonPrefix_;
                    if ((xprev - xnext != 1) || (yprev - ynext != 1)) {
                        reversed.push_back({xprev, yprev, newsnake});
                        newsnake = 0;
                    }
                    xprev = xnext;
                    yprev = ynext;
                    ++newsnake;
                }
                reversed.push_back({xprev, yprev, newsnake});
            } else {
                reversed.push_back({xprev, yprev, length});
            }
        } else {
            reversed.push_back({x + commonPrefix_, y + commonPrefix_, length});
        }
    }

    matchingBlocks_.clear();
    if (commonPrefix_)
        matchingBlocks_.push_back({0, 0, commonPrefix_});
    matchingBlocks_.insert(matchingBlocks_.end(), reversed.rbegin(), reversed.rend());
    if (commonSuffix_) {
        matchingBlocks_.push_back({int(a_.size()) - commonSuffix_,
                                   int(b_.size()) - commonSuffix_, commonSuffix_});
    }
    matchingBlocks_.push_back({int(a_.size()), int(b_.size()), 0});
    // clean-up to free memory
    aindex_ = {};
    bindex_ = {};
}

int MyersSequenceMatcher::newSnake(int prev, int x, int y, int length)
{
    retain(prev);
    const Snake snake{prev, x, y, length, 0};
    if (!freeSnakes_.empty()) {
        const int index = freeSnakes_.back();
        freeSnakes_.pop_back();
        snakes_[size_t(index)] = snake;
        return index;
    }
    snakes_.push_back(snake);
    return int(snakes_.size()) - 1;
}

void MyersSequenceMatcher::retain(int node)
{
    if (node != -1)
        ++snakes_[size_t(node)].refs;
}

void MyersSequenceMatcher::release(int node)
{
    while (node != -1) {
        Snake &snake = snakes_[size_t(node)];
        if (--snake.refs > 0)
            return;
        freeSnakes_.push_back(node);
        node = snake.prev;
    }
}

std::generator<bool> MyersSequenceMatcher::initialise()
{
    Sequence a = a_;
    Sequence b = b_;
    preprocess(a, b);
    const int m = int(a.size());
    const int n = int(b.size());
    const int middle = m + 1;
    int lastsnake = -1;
    const int delta = n - m + middle;
    const int dmin = std::min(middle, delta);
    const int dmax = std::max(middle, delta);

    snakes_.clear();
    freeSnakes_.clear();

    if (n > 0 && m > 0) {
        struct FurthestPoint {
            int y;
            int node;
        };
        const int size = n + m + 2;
        std::vector<FurthestPoint> fp(size_t(size), {-1, -1});
        const auto store = [&](int km, int y, int node) {
            retain(node);
            release(fp[size_t(km)].node);
            fp[size_t(km)] = {y, node};
        };
        const auto equalAt = [&](int x, int y) {
            return x >= 0 && x < m && y < n && a[size_t(x)] == b[size_t(y)];
        };

        int p = -1;
        while (true) {
            ++p;
            if (p % 100 == 0)
                co_yield false;
            // move along vertical edge
            int yv = -1;
            int node = -1;
            for (int km = dmin - p; km < delta; ++km) {
                const FurthestPoint t = fp[size_t(km + 1)];
                if (yv < t.y) {
                    yv = t.y;
                    node = t.node;
                } else {
                    ++yv;
                }
                int x = yv - km + middle;
                if (equalAt(x, yv)) {
                    int snake = x;
                    ++x;
                    ++yv;
                    while (equalAt(x, yv)) {
                        ++x;
                        ++yv;
                    }
                    snake = x - snake;
                    node = newSnake(node, x - snake, yv - snake, snake);
                }
                store(km, yv, node);
            }
            // move along horizontal edge
            int yh = -1;
            node = -1;
            for (int km = dmax + p; km > delta; --km) {
                const FurthestPoint t = fp[size_t(km - 1)];
                if (yh <= t.y) {
                    yh = t.y + 1;
                    node = t.node;
                }
                int x = yh - km + middle;
                if (equalAt(x, yh)) {
                    int snake = x;
                    ++x;
                    ++yh;
                    while (equalAt(x, yh)) {
                        ++x;
                        ++yh;
                    }
                    snake = x - snake;
                    node = newSnake(node, x - snake, yh - snake, snake);
                }
                store(km, yh, node);
            }
            // point on the diagonal that leads to the sink
            int y;
            if (yv < yh) {
                y = fp[size_t(delta + 1)].y;
                node = fp[size_t(delta + 1)].node;
            } else {
                y = fp[size_t(delta - 1)].y + 1;
                node = fp[size_t(delta - 1)].node;
            }
            int x = y - delta + middle;
            if (equalAt(x, y)) {
                int snake = x;
                ++x;
                ++y;
                while (equalAt(x, y)) {
                    ++x;
                    ++y;
                }
                snake = x - snake;
                node = newSnake(node, x - snake, y - snake, snake);
            }
            store(delta, y, node);
            if (y >= n) {
                lastsnake = node;
                break;
            }
        }
    }
    buildMatchingBlocks(lastsnake);
    postprocess();
    snakes_ = {};
    freeSnakes_ = {};
    ready_ = true;
    co_yield true;
}

SyncPointMyersSequenceMatcher::SyncPointMyersSequenceMatcher(
    Sequence a, Sequence b, std::vector<std::pair<int, int>> syncpoints)
    : MyersSequenceMatcher(std::move(a), std::move(b))
    , syncpoints_(std::move(syncpoints))
{
}

std::generator<bool> SyncPointMyersSequenceMatcher::initialise()
{
    if (syncpoints_.empty()) {
        for (bool done : MyersSequenceMatcher::initialise())
            co_yield done;
        co_return;
    }

    struct Part {
        int ai;
        int bi;
        Sequence a;
        Sequence b;
    };
    std::vector<Part> parts;
    int ai = 0;
    int bi = 0;
    for (const auto &[aj, bj] : syncpoints_) {
        parts.push_back({ai, bi, pySlice(a_, ai, aj), pySlice(b_, bi, bj)});
        ai = aj;
        bi = bj;
    }
    if (ai < int(a_.size()) || bi < int(b_.size()))
        parts.push_back({ai, bi, pySlice(a_, ai, INT_MAX), pySlice(b_, bi, INT_MAX)});

    splitMatchingBlocks_.clear();
    matchingBlocks_.clear();
    for (Part &part : parts) {
        MyersSequenceMatcher matcher(part.a, part.b);
        for (bool done : matcher.initialise()) {
            Q_UNUSED(done);
            co_yield false;
        }
        const std::vector<MatchingBlock> &blocks = matcher.matchingBlocks();
        // Note: the original implementation tries to merge the first block
        // with the previous chunk's last block here, but that code path can
        // never be taken; it is intentionally not replicated.
        std::vector<MatchingBlock> partBlocks;
        for (size_t k = 0; k + 1 < blocks.size(); ++k)
            partBlocks.push_back({part.ai + blocks[k].a, part.bi + blocks[k].b, blocks[k].size});
        matchingBlocks_.insert(matchingBlocks_.end(), partBlocks.begin(), partBlocks.end());
        // Split matching blocks each need to be terminated to get our split
        // chunks correctly created
        partBlocks.push_back({part.ai + int(part.a.size()), part.bi + int(part.b.size()), 0});
        splitMatchingBlocks_.push_back(std::move(partBlocks));
    }
    matchingBlocks_.push_back({int(a_.size()), int(b_.size()), 0});
    ready_ = true;
    co_yield true;
}

std::vector<DiffChunk> SyncPointMyersSequenceMatcher::opcodes()
{
    if (syncpoints_.empty())
        return MyersSequenceMatcher::opcodes();

    // This is difflib.SequenceMatcher.get_opcodes in which we instead
    // iterate over our internal set of split matching blocks.
    matchingBlocks();
    std::vector<DiffChunk> answer;
    int i = 0;
    int j = 0;
    for (const auto &blocks : splitMatchingBlocks_) {
        for (const MatchingBlock &block : blocks) {
            std::optional<ChunkTag> tag;
            if (i < block.a && j < block.b)
                tag = ChunkTag::Replace;
            else if (i < block.a)
                tag = ChunkTag::Delete;
            else if (j < block.b)
                tag = ChunkTag::Insert;
            if (tag)
                answer.push_back({*tag, i, block.a, j, block.b});
            i = block.a + block.size;
            j = block.b + block.size;
            if (block.size)
                answer.push_back({ChunkTag::Equal, block.a, i, block.b, j});
        }
    }
    return answer;
}

} // namespace meld
