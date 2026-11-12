// Single-instance mode with shared memory (Qt Core)
//
// The instance in single-instance mode creates a shared memory segment of
// message slots, and polls it for the requests of later invocations, which
// poll it for the answers addressed to them. Its heartbeat in the segment
// tells whether it's still running: segments outlive instances that crash,
// and are then taken over. The segment's lock is a System V semaphore where
// possible, which the system releases if a process holding it ends.

#include "singleinstance_p.h"

#include <QDateTime>
#include <QLoggingCategory>
#include <QPointer>
#include <QRandomGenerator>
#include <QSharedMemory>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace meld {

Q_LOGGING_CATEGORY(lcSingleInstance, "meld.singleinstance")

using namespace singleinstance;

namespace {

constexpr quint32 Magic = 0x4d454c44; // "MELD"
constexpr quint32 LayoutVersion = 1;
constexpr int SlotCount = 16;
constexpr int SlotSize = 16 * 1024;
//! How often the running instance and invocations look for messages, ms
constexpr int PollInterval = 100;
constexpr int InvocationPollInterval = 50;
//! A running instance that hasn't polled for this long has gone, ms
constexpr qint64 StaleAfter = 5000;
//! Messages for invocations that don't take them in this time are dropped
constexpr qint64 UndeliveredAfter = 10000;
//! How long an invocation waits for a free slot for its request, ms
constexpr qint64 SendTimeout = 2000;

struct Slot {
    quint32 used;
    //! 0 for the running instance, or an invocation's id
    quint32 to;
    quint32 size;
    quint32 reserved;
    quint64 sequence;
    qint64 time;
    char data[SlotSize - 32];
};

struct Header {
    quint32 magic;
    quint32 version;
    //! The running instance, or 0 if there's none
    quint64 owner;
    qint64 heartbeat;
    quint32 nextId;
    quint32 reserved;
    quint64 nextSequence;
};

struct Segment {
    Header header;
    Slot messageSlots[SlotCount];
};

static_assert(std::is_trivially_copyable_v<Segment>);
static_assert(sizeof(Slot) == SlotSize);

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QNativeIpcKey nativeKey(const QString &name)
{
    // System V semaphores are released if a process holding the lock ends
    constexpr QNativeIpcKey::Type preferred = QNativeIpcKey::Type::SystemV;
    if (QSharedMemory::isKeyTypeSupported(preferred))
        return QSharedMemory::platformSafeKey(name, preferred);
    return QSharedMemory::platformSafeKey(name);
}

class Lock {
public:
    explicit Lock(QSharedMemory &memory)
        : memory_(memory)
        , locked_(memory.lock())
    {
    }
    ~Lock()
    {
        if (locked_)
            memory_.unlock();
    }
    explicit operator bool() const { return locked_; }

private:
    QSharedMemory &memory_;
    bool locked_;
};

Segment *segment(QSharedMemory &memory)
{
    return static_cast<Segment *>(memory.data());
}

bool isAlive(const Header &header)
{
    return header.magic == Magic && header.owner != 0 && now() - header.heartbeat < StaleAfter;
}

//! Put a message in a free slot; false if there's none or it's too long
bool putMessage(Segment *s, quint32 to, const QByteArray &message)
{
    if (message.size() > qsizetype(sizeof(Slot::data)))
        return false;
    for (Slot &slot : s->messageSlots) {
        if (slot.used)
            continue;
        slot.used = 1;
        slot.to = to;
        slot.size = quint32(message.size());
        slot.sequence = s->header.nextSequence++;
        slot.time = now();
        std::memcpy(slot.data, message.constData(), size_t(message.size()));
        return true;
    }
    return false;
}

//! Take the messages for a recipient, in the order they were put
QList<QByteArray> takeMessages(Segment *s, quint32 to)
{
    std::vector<Slot *> mine;
    for (Slot &slot : s->messageSlots) {
        if (slot.used && slot.to == to)
            mine.push_back(&slot);
    }
    std::sort(mine.begin(), mine.end(), [](const Slot *a, const Slot *b) { return a->sequence < b->sequence; });
    QList<QByteArray> messages;
    for (Slot *slot : mine) {
        messages.append(QByteArray(slot->data, qsizetype(slot->size)));
        slot->used = 0;
    }
    return messages;
}

} // namespace

// Only instances in single-instance mode receive command lines
const bool SingleInstanceServer::ReceivesWithoutSingleInstanceMode = false;

struct SingleInstanceServer::Private {
    QSharedMemory memory;
    QTimer poll;
    quint64 owner = 0;
    bool listening = false;
    //! Answers waiting for free slots: recipient and message
    QList<std::pair<quint32, QByteArray>> outbox;

    void stop()
    {
        listening = false;
        poll.stop();
    }

    //! Put the waiting answers in free slots, in order
    void flushOutbox(Segment *s)
    {
        while (!outbox.isEmpty()) {
            const auto &[to, message] = outbox.first();
            if (message.size() > qsizetype(sizeof(Slot::data))) {
                qCWarning(lcSingleInstance) << "Dropped an answer too long for shared memory";
            } else if (!putMessage(s, to, message)) {
                return;
            }
            outbox.removeFirst();
        }
    }
};

SingleInstanceServer::SingleInstanceServer(QObject *parent)
    : QObject(parent)
    , d_(std::make_unique<Private>())
{
    d_->owner = QRandomGenerator::global()->generate64() | 1;
    d_->poll.setInterval(PollInterval);
    connect(&d_->poll, &QTimer::timeout, this, [this]() {
        QList<QByteArray> requests;
        {
            const Lock lock(d_->memory);
            if (!lock)
                return;
            Segment *s = segment(d_->memory);
            // Another instance took over, after this one seemed to have gone
            if (s->header.owner != d_->owner) {
                qCWarning(lcSingleInstance) << "Another instance receives command lines now";
                d_->stop();
                return;
            }
            s->header.heartbeat = now();
            requests = takeMessages(s, 0);
            // Drop the answers of invocations that went
            for (Slot &slot : s->messageSlots) {
                if (slot.used && slot.to != 0 && now() - slot.time > UndeliveredAfter)
                    slot.used = 0;
            }
            d_->flushOutbox(s);
        }

        for (const QByteArray &data : std::as_const(requests)) {
            QByteArray buffer = data;
            const std::optional<QJsonObject> message = takeMessage(buffer);
            if (!message)
                continue;
            const quint32 from = quint32(message->value(QStringLiteral("from")).toInteger());
            QByteArray answer;
            const std::optional<RemoteCommandLine::Request> request = parseRequest(*message, &answer);
            const auto send = [this, from](const QByteArray &data) {
                // Answers are queued in order, and sent as there's space
                d_->outbox.append({from, data});
                const Lock lock(d_->memory);
                if (!lock)
                    return;
                d_->flushOutbox(segment(d_->memory));
            };
            if (!request) {
                send(answer);
                continue;
            }
            Q_EMIT commandLineReceived(new RemoteCommandLine(*request, send, {}, this));
        }
    });
}

SingleInstanceServer::~SingleInstanceServer()
{
    if (!d_->listening)
        return;
    // Invocations waiting for this instance see that it's gone
    const Lock lock(d_->memory);
    if (lock) {
        Segment *s = segment(d_->memory);
        if (s->header.owner == d_->owner) {
            s->header.owner = 0;
            s->header.heartbeat = 0;
        }
    }
}

bool SingleInstanceServer::listen(const QString &name)
{
    if (d_->listening)
        return true;
    d_->memory.setNativeKey(nativeKey(name));
    bool created = d_->memory.create(sizeof(Segment));
    if (!created) {
        if (d_->memory.error() != QSharedMemory::AlreadyExists || !d_->memory.attach()) {
            qCWarning(lcSingleInstance) << "Couldn't share memory with other invocations:"
                                        << d_->memory.errorString();
            return false;
        }
    }
    bool anotherIsRunning = false;
    {
        const Lock lock(d_->memory);
        if (!lock)
            return false;
        Segment *s = segment(d_->memory);
        // Another instance is running; otherwise the segment is new, or left
        // by one that ended without removing it
        anotherIsRunning = !created && isAlive(s->header);
        if (!anotherIsRunning) {
            std::memset(static_cast<void *>(s), 0, sizeof(Segment));
            s->header.magic = Magic;
            s->header.version = LayoutVersion;
            s->header.owner = d_->owner;
            s->header.heartbeat = now();
            s->header.nextId = 1;
        }
    }
    // Not while locked: detach() takes the lock itself
    if (anotherIsRunning) {
        d_->memory.detach();
        return false;
    }
    d_->listening = true;
    d_->poll.start();
    return true;
}

bool SingleInstanceServer::isListening() const
{
    return d_->listening;
}

std::optional<int> forwardCommandLine(const QStringList &arguments, const QString &cwd,
                                      const std::function<void(const QString &, bool)> &print, const QString &name)
{
    QSharedMemory memory(nativeKey(name));
    if (!memory.attach())
        return std::nullopt;

    // Send the request, with this invocation's id for the answers
    quint32 id = 0;
    const qint64 start = now();
    while (true) {
        {
            const Lock lock(memory);
            if (!lock)
                return std::nullopt;
            Segment *s = segment(memory);
            if (s->header.version != LayoutVersion || !isAlive(s->header))
                return std::nullopt;
            if (id == 0) {
                id = s->header.nextId++;
                if (s->header.nextId == 0)
                    s->header.nextId = 1;
            }
            QByteArray buffer = request(arguments, cwd);
            std::optional<QJsonObject> message = takeMessage(buffer);
            message->insert(QStringLiteral("from"), qint64(id));
            if (putMessage(s, 0, encode(*message)))
                break;
        }
        if (now() - start > SendTimeout)
            return std::nullopt;
        QThread::msleep(InvocationPollInterval);
    }

    // Relay the output until the invocation ends, which may be when its
    // comparison is closed
    while (true) {
        QThread::msleep(InvocationPollInterval);
        QList<QByteArray> messages;
        bool alive = true;
        {
            const Lock lock(memory);
            if (!lock)
                return 1;
            Segment *s = segment(memory);
            messages = takeMessages(s, id);
            alive = isAlive(s->header);
        }
        for (const QByteArray &data : std::as_const(messages)) {
            QByteArray buffer = data;
            while (const std::optional<QJsonObject> message = takeMessage(buffer)) {
                if (const std::optional<int> status = handleAnswer(*message, print))
                    return status;
            }
        }
        // The running instance ended without answering
        if (!alive)
            return 1;
    }
}

} // namespace meld
