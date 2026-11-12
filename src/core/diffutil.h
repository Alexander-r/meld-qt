#pragma once

#include "diffchunk.h"
#include "linesource.h"

#include <QObject>
#include <QSet>

#include <array>
#include <functional>
#include <generator>
#include <optional>
#include <vector>

namespace meld {

//! Chunk changes between two states of the merge cache, used by inline
//! highlighting to avoid re-highlighting existing chunks.
struct ChunkChanges {
    QSet<MergedChunk> removed;
    QSet<MergedChunk> added;
    std::optional<MergedChunk> modified;
};

//! Result of a line lookup: the chunk containing the line, and the
//! previous and next chunks.
struct ChunkLocation {
    std::optional<int> chunk;
    std::optional<int> prev;
    std::optional<int> next;

    friend bool operator==(const ChunkLocation &, const ChunkLocation &) = default;
};

OptChunk consumeBlankLines(const OptChunk &chunk, const Texts &texts, int pane1, int pane2);

//! Utility class to hold diff2 or diff3 chunks
class Differ : public QObject {
    Q_OBJECT

public:
    explicit Differ(QObject *parent = nullptr);
    ~Differ() override;

    //! Line getters for a synchronisation point: one pair (middle line,
    //! other line) per two-way comparison.
    struct SyncPointLines {
        std::function<int()> middle;
        std::function<int()> other;
    };
    using SyncPoint = std::vector<SyncPointLines>;

    std::vector<SyncPoint> syncpoints;
    bool ignoreBlanks = false;
    std::vector<int> conflicts;

    //! Update the chunks after lines were inserted (sizechange > 0) or
    //! removed (sizechange < 0) in `sequence`, starting at `startidx`.
    void changeSequence(int sequence, int startidx, int sizechange, const Texts &texts);
    /*!
     * Generalised update where lines startidx..startidx+linesRemoved of the
     * old text were replaced by lines startidx..startidx+linesAdded. This
     * is needed because Qt can report combined removals and insertions.
     */
    virtual void replaceInSequence(int sequence, int startidx, int linesRemoved, int linesAdded,
                                   const Texts &texts);

    bool hasChunk(int toPane, const DiffChunk &chunk) const;
    OptChunk getChunk(int index, int fromPane, int toPane = -1) const;
    std::array<std::optional<int>, 3> getChunkStarts(int index) const;
    ChunkLocation locateChunk(int pane, int line) const;
    int diffCount() const { return int(mergeCache_.size()); }
    std::pair<bool, bool> hasMergeableChanges(int which) const;
    const std::vector<MergedChunk> &allChanges() const { return mergeCache_; }

    std::vector<DiffChunk> pairChanges(int fromindex, int toindex,
                                       std::optional<std::array<int, 4>> lines = std::nullopt) const;
    std::vector<DiffChunk> pairedAllSingleChanges(int fromindex, int toindex) const;
    std::vector<DiffChunk> singleChanges(int textindex,
                                         std::optional<std::pair<int, int>> lines = std::nullopt) const;
    bool sequencesIdentical() const;

    std::generator<bool> setSequencesIter(Texts sequences);
    //! Synchronous helper running setSequencesIter() to completion
    void setSequences(const Texts &sequences);
    void clear();

    int numSequences() const { return numSequences_; }
    const std::array<std::vector<DiffChunk>, 2> &diffs() const { return diffs_; }
    const std::vector<int> &sequenceLengths() const { return seqlength_; }

Q_SIGNALS:
    void diffsChanged(const meld::ChunkChanges &changes);

protected:
    using Using = std::array<std::vector<DiffChunk>, 2>;
    struct MergeBounds {
        int l0, h0, l1, h1, l2, h2;
    };

    MergeBounds mergeBlocks(const Using &using_) const;
    virtual std::vector<MergedChunk> autoMerge(Using &using_, const Texts &texts);
    std::vector<MergedChunk> mergeDiffs(const std::vector<DiffChunk> &seq0,
                                        const std::vector<DiffChunk> &seq1, const Texts &texts);

private:
    void updateMergeCache(const Texts &texts);
    void updateLineCache();
    int locateChunkInternal(int whichdiffs, int sequence, int line) const;
    void changeSequenceInternal(int which, int sequence, int startidx, int linesRemoved,
                                int sizechange, const Texts &texts);
    std::pair<std::optional<int>, std::optional<int>> rangeFromLines(int textindex, int lo, int hi) const;

    int numSequences_ = 0;
    std::vector<int> seqlength_{0, 0, 0};
    std::array<std::vector<DiffChunk>, 2> diffs_;
    QSet<MergedChunk> oldMergeCache_;
    std::optional<MergedChunk> changedChunks_;
    std::vector<MergedChunk> mergeCache_;
    std::array<std::vector<ChunkLocation>, 3> lineCache_;
    bool initialised_ = false;
    std::array<bool, 4> hasMergeableChanges_{false, false, false, false};
};

} // namespace meld
