#include "diffchunk.h"

namespace meld {

QString chunkTagName(ChunkTag tag)
{
    switch (tag) {
    case ChunkTag::Equal:
        return QStringLiteral("equal");
    case ChunkTag::Replace:
        return QStringLiteral("replace");
    case ChunkTag::Insert:
        return QStringLiteral("insert");
    case ChunkTag::Delete:
        return QStringLiteral("delete");
    case ChunkTag::Conflict:
        return QStringLiteral("conflict");
    }
    return {};
}

std::optional<ChunkTag> chunkTagFromName(QStringView name)
{
    if (name == u"equal")
        return ChunkTag::Equal;
    if (name == u"replace")
        return ChunkTag::Replace;
    if (name == u"insert")
        return ChunkTag::Insert;
    if (name == u"delete")
        return ChunkTag::Delete;
    if (name == u"conflict")
        return ChunkTag::Conflict;
    return std::nullopt;
}

ChunkTag reverseChunkTag(ChunkTag tag)
{
    switch (tag) {
    case ChunkTag::Insert:
        return ChunkTag::Delete;
    case ChunkTag::Delete:
        return ChunkTag::Insert;
    default:
        return tag;
    }
}

size_t qHash(const DiffChunk &chunk, size_t seed) noexcept
{
    return qHashMulti(seed, int(chunk.tag), chunk.start_a, chunk.end_a,
                      chunk.start_b, chunk.end_b);
}

static size_t hashOptChunk(const OptChunk &chunk, size_t seed)
{
    return chunk ? qHash(*chunk, seed) : ::qHash(-1, seed);
}

size_t qHash(const MergedChunk &chunk, size_t seed) noexcept
{
    return hashOptChunk(chunk.second, hashOptChunk(chunk.first, seed));
}

int mergedChunkOrder(const MergedChunk &chunk)
{
    if (!chunk.any())
        return 0;
    const DiffChunk &c = chunk.first ? *chunk.first : *chunk.second;
    return c.start_a;
}

QString chunkToString(const DiffChunk &chunk)
{
    return QStringLiteral("(%1, %2, %3, %4, %5)")
        .arg(chunkTagName(chunk.tag))
        .arg(chunk.start_a)
        .arg(chunk.end_a)
        .arg(chunk.start_b)
        .arg(chunk.end_b);
}

} // namespace meld
