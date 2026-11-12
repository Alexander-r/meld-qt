#pragma once

#include <QHash>
#include <QString>
#include <QStringView>

#include <cstdint>
#include <optional>

namespace meld {

enum class ChunkTag : std::uint8_t {
    Equal,
    Replace,
    Insert,
    Delete,
    Conflict,
};

QString chunkTagName(ChunkTag tag);
std::optional<ChunkTag> chunkTagFromName(QStringView name);

//! The tag used when a chunk is viewed from the other side
ChunkTag reverseChunkTag(ChunkTag tag);

//! A single opcode-style change between two sequences `a` and `b`
struct DiffChunk {
    ChunkTag tag = ChunkTag::Equal;
    int start_a = 0;
    int end_a = 0;
    int start_b = 0;
    int end_b = 0;

    friend bool operator==(const DiffChunk &, const DiffChunk &) = default;

    DiffChunk reversed() const
    {
        return {reverseChunkTag(tag), start_b, end_b, start_a, end_a};
    }
};

size_t qHash(const DiffChunk &chunk, size_t seed = 0) noexcept;

using OptChunk = std::optional<DiffChunk>;

//! An entry of the Differ merge cache: the change between the middle
//! sequence and the left one, and between the middle and the right one.
struct MergedChunk {
    OptChunk first;
    OptChunk second;

    friend bool operator==(const MergedChunk &, const MergedChunk &) = default;

    bool any() const { return first.has_value() || second.has_value(); }
    const OptChunk &operator[](int i) const { return i == 0 ? first : second; }
    OptChunk &operator[](int i) { return i == 0 ? first : second; }
};

size_t qHash(const MergedChunk &chunk, size_t seed = 0) noexcept;

//! Sort key used when processing merged chunks in document order
int mergedChunkOrder(const MergedChunk &chunk);

QString chunkToString(const DiffChunk &chunk);

} // namespace meld
