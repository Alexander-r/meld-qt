#include "inlinematcher.h"

#include "core/myers.h"

#include <QMetaObject>
#include <QPointer>

#include <algorithm>

namespace meld {

std::vector<DiffChunk> inlineOpcodes(const QString &text1, const QString &textn)
{
    // Match on code points (as Python's str does), then map the offsets back
    // to UTF-16 positions as used by QTextDocument.
    const auto utf16Offsets = [](const QString &text) {
        std::vector<int> offsets;
        offsets.reserve(size_t(text.size()) + 1);
        int position = 0;
        for (qsizetype i = 0; i < text.size(); ++i) {
            offsets.push_back(position);
            if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
                ++i;
                position += 2;
            } else {
                position += 1;
            }
        }
        offsets.push_back(position);
        return offsets;
    };

    InlineMyersSequenceMatcher matcher(codePoints(text1), codePoints(textn));
    std::vector<DiffChunk> opcodes = matcher.opcodes();
    const std::vector<int> offsets1 = utf16Offsets(text1);
    const std::vector<int> offsetsn = utf16Offsets(textn);
    for (DiffChunk &chunk : opcodes) {
        chunk.start_a = offsets1[size_t(chunk.start_a)];
        chunk.end_a = offsets1[size_t(chunk.end_a)];
        chunk.start_b = offsetsn[size_t(chunk.start_b)];
        chunk.end_b = offsetsn[size_t(chunk.end_b)];
    }
    return opcodes;
}

CachedSequenceMatcher::CachedSequenceMatcher(QObject *parent)
    : QObject(parent)
    , alive_(std::make_shared<bool>(true))
{
    // A single worker keeps results arriving in request order, like the
    // original implementation's single worker process.
    pool_.setMaxThreadCount(1);
}

CachedSequenceMatcher::~CachedSequenceMatcher()
{
    stop();
}

void CachedSequenceMatcher::match(const QString &text1, const QString &textn, Callback callback)
{
    const Key key{text1, textn};
    if (auto it = cache_.find(key); it != cache_.end()) {
        it->timestamp = ++clock_;
        callback(it->opcodes);
        return;
    }

    auto pendingIt = pending_.find(key);
    if (pendingIt != pending_.end()) {
        pendingIt->push_back(std::move(callback));
        return;
    }
    pending_.insert(key, {std::move(callback)});

    std::weak_ptr<bool> alive = alive_;
    QPointer<CachedSequenceMatcher> self(this);
    pool_.start([alive, self, key]() {
        if (alive.expired())
            return;
        std::vector<DiffChunk> opcodes = inlineOpcodes(key.first, key.second);
        if (alive.expired())
            return;
        QMetaObject::invokeMethod(
            self.data(),
            [alive, self, key, opcodes = std::move(opcodes)]() {
                if (!alive.expired() && self)
                    self->onResult(key, opcodes);
            },
            Qt::QueuedConnection);
    });
}

void CachedSequenceMatcher::onResult(const Key &key, const std::vector<DiffChunk> &opcodes)
{
    cache_.insert(key, Entry{opcodes, ++clock_});
    const std::vector<Callback> callbacks = pending_.take(key);
    for (const Callback &callback : callbacks)
        callback(opcodes);
}

void CachedSequenceMatcher::clean(int sizeHint)
{
    if (cache_.size() <= sizeHint * 3)
        return;
    std::vector<std::pair<qint64, Key>> items;
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        items.emplace_back(it->timestamp, it.key());
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    const qsizetype remove = qsizetype(items.size()) - qsizetype(sizeHint) * 2;
    for (qsizetype i = 0; i < remove; ++i)
        cache_.remove(items[size_t(i)].second);
}

void CachedSequenceMatcher::stop()
{
    alive_ = std::make_shared<bool>(true);
    pool_.clear();
    pool_.waitForDone();
    cache_.clear();
    pending_.clear();
}

void CachedSequenceMatcher::waitForDone()
{
    pool_.waitForDone();
}

} // namespace meld
