#pragma once

#include "core/diffchunk.h"

#include <QHash>
#include <QObject>
#include <QPair>
#include <QThreadPool>

#include <functional>
#include <memory>
#include <vector>

namespace meld {

//! Inline opcodes for two texts, with offsets in UTF-16 code units
std::vector<DiffChunk> inlineOpcodes(const QString &text1, const QString &textn);

/*!
 * Simple class for caching diff results, with LRU-based eviction
 *
 * Results from the matcher are cached and timestamped, and subsequently
 * evicted based on least-recent generation/usage.
 */
class CachedSequenceMatcher : public QObject {
    Q_OBJECT

public:
    using Callback = std::function<void(const std::vector<DiffChunk> &)>;

    explicit CachedSequenceMatcher(QObject *parent = nullptr);
    ~CachedSequenceMatcher() override;

    //! Request the opcodes for a pair of texts; the callback is invoked on
    //! the main thread, immediately for cached results.
    void match(const QString &text1, const QString &textn, Callback callback);
    //! Clean the cache if necessary; sizeHint is the recommended minimum
    //! number of cache entries.
    void clean(int sizeHint);
    void stop();
    //! Wait for all queued work (for tests)
    void waitForDone();

private:
    using Key = QPair<QString, QString>;
    struct Entry {
        std::vector<DiffChunk> opcodes;
        qint64 timestamp;
    };

    void onResult(const Key &key, const std::vector<DiffChunk> &opcodes);

    QThreadPool pool_;
    QHash<Key, Entry> cache_;
    QHash<Key, std::vector<Callback>> pending_;
    std::shared_ptr<bool> alive_;
    qint64 clock_ = 0;
};

} // namespace meld
